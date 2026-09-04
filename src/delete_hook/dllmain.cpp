#include "shared.h"

#include <windows.h>
#include <cstring>

// Version-specific addresses for TSDev 9.5 (build Nov 30 2018).
// The CLI verifies the TSDev.exe SHA-256 before injecting, so these are
// safe to hardcode — a mismatched build is rejected before injection.
static void* const kFnOpen   = reinterpret_cast<void*>(0x004944b0);
static void* const kFnClearSaved = reinterpret_cast<void*>(0x0044f3d0);
static const int  kModifiedOffset          = 0x17c;
static const int  kVtableCloseOffset       = 0x8c;
static const int  kVtableLoaderOffset      = 0x1F4;
static const int  kVtableClearSavedOffset  = 0x1B4;
static const unsigned short kTradingAppType = 0x1402;

// Original first 5 bytes of FUN_004944b0: push ebp; mov ebp,esp; push -1
static constexpr unsigned char kOriginalBytes[5] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF };

static DeleteShared* g_shared = nullptr;
static HANDLE        g_mapping = nullptr;
static HANDLE        g_done_event = nullptr;
static void*         g_trampoline = nullptr;
static bool          g_hooked = false;

// FUN_004944b0 is __thiscall with 4 params: this (ECX) + 3 stack args.
//   void* __thiscall FUN_004944b0(void* this, CStringT name, int type, int flags)
//   Returns: document pointer (void*)
//   RET 0xC (pops 3 stack args = 12 bytes)
// The function destroys its CStringT copy before returning. MFC CStringT uses
// reference counting, so the buffer may survive if the caller holds a reference,
// but we copy the name before calling the original as a safety measure.

#if defined(_M_IX86)
// thiscall passes `this` in ECX. __fastcall passes the first arg in ECX,
// which is equivalent for a function whose only parameter is `this`.
typedef void (__fastcall *ThiscallVoidFn)(void* thisPtr);
#else
// x64 has a single calling convention; the DLL is only loaded in x86, but
// the code must compile in x64 for the build to succeed.
typedef void (*ThiscallVoidFn)(void* thisPtr);
#endif

// Saved copy of the item name (copied before the original function destroys it).
static wchar_t g_saved_name[256] = {};

// Re-entry guard: the CLI re-sends the pipe command every 3 seconds while
// waiting for the done event, and each re-send calls FUN_004944b0, firing the
// hook. If a previous do_delete is still running (e.g. during the loader or
// clear-saved call) when a new pipe-send arrives, the hook could fire again on
// a new document. The hook runs in TSDev's main thread, so there's no true
// concurrency, but nested message processing can re-enter; this flag prevents
// that. Checked after name_matches so non-matching opens still pass through.
static volatile long g_delete_in_progress = 0;

static bool name_matches(const wchar_t* a, const wchar_t* b) {
    if (!a || !b) return false;
    return wcscmp(a, b) == 0;
}

static void unhook() {
    if (!g_hooked) return;
    DWORD old_protect = 0;
    if (VirtualProtect(kFnOpen, 5, PAGE_EXECUTE_READWRITE, &old_protect)) {
        memcpy(kFnOpen, kOriginalBytes, 5);
        VirtualProtect(kFnOpen, 5, old_protect, &old_protect);
        FlushInstructionCache(GetCurrentProcess(), kFnOpen, 5);
    }
    g_hooked = false;
}

static void cleanup() {
    unhook();
    if (g_trampoline) {
        VirtualFree(g_trampoline, 0, MEM_RELEASE);
        g_trampoline = nullptr;
    }
    if (g_shared) {
        UnmapViewOfFile(g_shared);
        g_shared = nullptr;
    }
    if (g_mapping) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
    }
    if (g_done_event) {
        CloseHandle(g_done_event);
        g_done_event = nullptr;
    }
}

// C helper called from the naked hook after the original function returns.
// Uses the destructor path: sets up the three conditions the CELDocument
// destructor (FUN_0044a640) requires to call FUN_0044c0f0 (library deletion),
// then calls OnCloseDocument which triggers the destructor. The destructor
// handles both deletion and document cleanup in the correct order, then
// frees the doc object.
//
// The three destructor conditions (all must be met):
//   1. doc+0x174 != 0 — set by vtable[0x1F4] (loader) if the item exists
//   2. !CELProperties::IsSaved — set by vtable[0x1B4] which calls
//      CELProperties::SetSaved(false) on the properties at doc+0x210
//   3. doc+0x17C != 0 — set directly (modified flag)
//
// After OnCloseDocument returns, the doc pointer is INVALID (freed by the
// destructor). Do NOT access doc after the close call.
static void __cdecl do_delete(void* doc) {
    if (!g_shared || !doc) return;
    if (!name_matches(g_saved_name, g_shared->target_name)) return;
    if (g_delete_in_progress) return;
    g_delete_in_progress = 1;

    __try {
        void** vtable = *reinterpret_cast<void***>(doc);

        // 1. Call vtable[0x1F4] (loader / FUN_0044f8e0) — sets doc+0x174 = 1
        //    if the item exists in the library. The loader clears doc+0x17C
        //    and doc+0x174 at the start, then sets doc+0x174 = 1 if found.
        //    Must be called BEFORE setting doc+0x17C (it clears it).
        const ThiscallVoidFn loader =
            reinterpret_cast<ThiscallVoidFn>(vtable[g_shared->vtable_loader_offset / sizeof(void*)]);
        loader(doc);

        // 2. Call vtable[0x1B4] (FUN_00458160 for CELFunctionDoc) — calls
        //    FUN_0044f3d0 which calls CELProperties::SetSaved(false) on the
        //    properties at doc+0x210. This makes the destructor's IsSaved
        //    check return false. IsSaved is in an external DLL and checks
        //    an internal CELProperties field set by SetSaved; the field
        //    offset is not visible from TSDev.exe static analysis.
        //
        //    TradingApp (CELApplicationDoc, type 0x1402) has an empty stub
        //    at vtable[0x1B4] (FUN_00413900) that does NOT call SetSaved.
        //    For this type, call FUN_0044f3d0 directly. See
        //    docs/RE_DELETE_TRADINGAPP.md for the full analysis.
        const unsigned char type_lo = *(reinterpret_cast<unsigned char*>(doc) + 0x1e8);
        const unsigned char type_hi = *(reinterpret_cast<unsigned char*>(doc) + 0x1ea);
        const unsigned short doc_type = static_cast<unsigned short>((type_hi << 8) | type_lo);
        if (doc_type == kTradingAppType) {
            const ThiscallVoidFn clear_saved_direct =
                reinterpret_cast<ThiscallVoidFn>(kFnClearSaved);
            clear_saved_direct(doc);
        } else {
            vtable = *reinterpret_cast<void***>(doc);
            const ThiscallVoidFn clear_saved =
                reinterpret_cast<ThiscallVoidFn>(vtable[g_shared->vtable_clear_saved_offset / sizeof(void*)]);
            clear_saved(doc);
        }

        // 3. Set doc+0x17C = 1 (modified flag). Must be AFTER the loader
        //    (which clears it). The destructor requires this to be non-zero.
        *reinterpret_cast<volatile long*>(static_cast<char*>(doc) + g_shared->modified_offset) = 1;

        // 4. Call vtable[0x8c] (OnCloseDocument / FUN_004508e0) — closes the
        //    document, destroys views, and calls delete this (the destructor).
        //    The destructor sees doc+0x174=1, !IsSaved=true, doc+0x17C=1,
        //    and calls FUN_0044c0f0 (library deletion). After this call,
        //    doc is freed — do NOT access doc below.
        vtable = *reinterpret_cast<void***>(doc);
        const ThiscallVoidFn close_doc =
            reinterpret_cast<ThiscallVoidFn>(vtable[g_shared->vtable_close_offset / sizeof(void*)]);
        close_doc(doc);

        g_shared->status = DeleteDone;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_shared->crash_flag = 1;
        g_shared->status = DeleteError;
    }

    g_delete_in_progress = 0;

    if (g_done_event) SetEvent(g_done_event);
    unhook();
}

#if defined(_M_IX86)
// Naked hook entry point — preserves the __thiscall convention.
// On entry: ECX = this, stack = [ret_addr, param2, param3, param4, ...]
// param2 is the CStringT's m_pszData (wchar_t* pointer).
// The original function destroys the CStringT (freeing the buffer) before
// returning, so we must copy the name BEFORE calling the trampoline.
static __declspec(naked) void hook_open_doc() {
    __asm {
        // Save callee-saved registers used during the name copy
        push esi
        push edi
        // Stack: [edi, esi, ret_addr, param2, param3, param4, ...]
        // param2 is at ESP+12

        // Copy param2 (wchar_t* name) to g_saved_name before the original frees it
        mov eax, [esp+12]       // eax = param2 (wchar_t*)
        lea esi, g_saved_name   // esi = destination buffer
        xor edi, edi            // edi = byte offset counter
    copy_loop:
        mov dx, [eax + edi]
        mov [esi + edi], dx
        test dx, dx
        jz copy_done
        add edi, 2
        cmp edi, 510            // 255 * 2 = max bytes (leave room for NUL)
        jb copy_loop
    copy_done:
        pop edi
        pop esi
        // Stack restored: [ret_addr, param2, param3, param4, ...]
        // ECX still = this

        // Push copies of the 3 stack args for the trampoline call.
        // The trampoline's RET 0xC will consume these copies, leaving
        // the original args intact for our own RET 0xC.
        push [esp+12]           // param4
        push [esp+12]           // param3 (shifted by 1 push)
        push [esp+12]           // param2 (shifted by 2 pushes)

        // Call the trampoline (original function) — ECX=this, args on stack
        mov eax, g_trampoline
        call eax
        // EAX = doc pointer (return value)
        // Stack: [ret_addr, param2, param3, param4, ...] (copies consumed)

        // Call do_delete(doc) — checks name, performs deletion if match
        push eax                // save doc pointer (return value)
        push eax                // arg for do_delete
        call do_delete
        add esp, 4              // clean up do_delete arg (cdecl)
        pop eax                 // restore doc pointer

        // Return to caller — RET 0xC pops ret_addr + 3 stack args
        ret 0Ch
    }
}
#else
// x64 stub — never used (DLL is only loaded in 32-bit TSDev.exe)
static void hook_open_doc() {}
#endif

static bool install_hook() {
    // Verify the original bytes match before patching.
    if (memcmp(kFnOpen, kOriginalBytes, 5) != 0) return false;

    // Allocate a trampoline: original 5 bytes + JMP back to fn+5.
    g_trampoline = VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_trampoline) return false;

    unsigned char* tramp = static_cast<unsigned char*>(g_trampoline);
    memcpy(tramp, kOriginalBytes, 5);
    // JMP rel32 to kFnOpen + 5
    tramp[5] = 0xE9;
    const intptr_t rel = reinterpret_cast<intptr_t>(kFnOpen) + 5 -
                         reinterpret_cast<intptr_t>(tramp + 5) - 5;
    memcpy(tramp + 6, &rel, 4);

    // Write JMP to hook_open_doc at the function entry.
    DWORD old_protect = 0;
    if (!VirtualProtect(kFnOpen, 5, PAGE_EXECUTE_READWRITE, &old_protect))
        return false;

    unsigned char patch[5];
    patch[0] = 0xE9;
    const intptr_t hook_rel = reinterpret_cast<intptr_t>(&hook_open_doc) -
                              reinterpret_cast<intptr_t>(kFnOpen) - 5;
    memcpy(patch + 1, &hook_rel, 4);
    memcpy(kFnOpen, patch, 5);

    VirtualProtect(kFnOpen, 5, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), kFnOpen, 5);
    g_hooked = true;
    return true;
}

// Exported function called by the CLI via CreateRemoteThread after
// LoadLibraryW. This makes first load and subsequent loads identical:
// DllMain just loads the DLL; ReArmHook does all the work (map shared
// memory, open the done event, install the hook). On a second delete the
// DLL is already loaded (DllMain is not called again), so the CLI calls
// ReArmHook to re-arm the hook.
#if defined(_M_IX86)
#pragma comment(linker, "/EXPORT:ReArmHook=_ReArmHook@4")
#endif
extern "C" __declspec(dllexport) DWORD WINAPI ReArmHook(LPVOID) {
    const unsigned long pid = GetCurrentProcessId();

    if (g_hooked) unhook();
    if (g_shared) {
        UnmapViewOfFile(g_shared);
        g_shared = nullptr;
    }
    if (g_mapping) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
    }
    if (g_done_event) {
        CloseHandle(g_done_event);
        g_done_event = nullptr;
    }

    wchar_t map_name[128];
    wsprintfW(map_name, L"Local\\TSDevAutomation.Delete.%lu", pid);
    g_mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, map_name);
    if (!g_mapping) return 0;
    g_shared = static_cast<DeleteShared*>(
        MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(DeleteShared)));
    if (!g_shared) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
        return 0;
    }

    wchar_t event_name[128];
    wsprintfW(event_name, L"Local\\TSDevAutomation.DeleteDone.%lu", pid);
    g_done_event = OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name);

    // The CLI fills in addresses; copy them into the shared struct as
    // a fallback in case they were not set (they should always be set).
    if (!g_shared->fn_open)   g_shared->fn_open   = kFnOpen;
    if (!g_shared->modified_offset)           g_shared->modified_offset           = kModifiedOffset;
    if (!g_shared->vtable_close_offset)       g_shared->vtable_close_offset       = kVtableCloseOffset;
    if (!g_shared->vtable_loader_offset)      g_shared->vtable_loader_offset      = kVtableLoaderOffset;
    if (!g_shared->vtable_clear_saved_offset) g_shared->vtable_clear_saved_offset = kVtableClearSavedOffset;

    g_shared->status = DeletePending;
    g_shared->crash_flag = 0;

    if (!install_hook()) {
        g_shared->status = DeleteError;
        if (g_done_event) SetEvent(g_done_event);
        cleanup();
        return 0;
    }
    return 1;
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) return TRUE;
    if (reason == DLL_PROCESS_DETACH) cleanup();
    return TRUE;
}
