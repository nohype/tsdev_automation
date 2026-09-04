#include "shared.h"

#include <windows.h>
#include <cstring>

// Enumerates the TSDev library by driving ORELUI20.dll's own enumeration
// surface inside the TSDev.exe process (the same code path the Open dialog
// uses). Recipe: docs/RE_ORELUI20.md §2.1 (TVS #7 research deliverable,
// including the three dynamic-verification corrections). Facts SSOT:
// docs/KNOWLEDGE_BASE.md, "ORELUI20.dll + ORLIB20.dll ordinals".
//
// Version-brittle surfaces (AGENTS.md purpose b) — the CLI verifies the
// ORELUI20.dll SHA-256 (kOreluiKnownSha256 in tsdevctl/main.cpp) BEFORE
// injecting this DLL, so the two non-exported surfaces below are only used
// on the verified build (TSDev 9.5, ORELUI20.dll build Nov 30 2018) and may
// change across TradeStation/TSDev versions:
//   - CAnalysisTypeInfo ctor (FUN_10044820): module base + 0x44820 (NOT exported).
//   - Type-category table (DAT_1008fd04, 13 uint32 entries): module base + 0x8fd04.
// All other surfaces are exported ordinals (stable): Ordinal_17 = dialog
// constructor, Ordinal_1291 = LoadList, Ordinal_166 = dialog destructor.
static const unsigned long kCatiCtorRva     = 0x44820;  // VERSION-BRITTLE (FUN_10044820)
static const unsigned long kTypeTableRva    = 0x8fd04;  // VERSION-BRITTLE (DAT_1008fd04)
static const unsigned long kTypeTableEntries = 13;

// The dialog constructor initializes fields up to this+0x3dc; the recipe
// requires a buffer of at least ~0x3e0 bytes. The per-type open-document
// counter map (std::map<std::string,long>) is not needed for enumeration; a
// zero-initialized 0x40-byte buffer suffices (dynamically verified).
static const size_t kDialogBytes = 0x400;
static const size_t kCounterMapBytes = 0x40;
static const size_t kCatiBytes = 0x28;  // sizeof(CAnalysisTypeInfo)

#if defined(_M_IX86)
// The ORELUI20.dll surfaces are __thiscall (this in ECX, remaining args on
// the stack, callee cleans the stack). MSVC cannot form __thiscall function
// pointers directly, so the standard x86 emulation is used: __fastcall passes
// the first two arguments in ECX/EDX, so __fastcall(this, dummy, ...) puts
// `this` in ECX and the real arguments on the stack with identical callee
// stack cleanup (EDX is scratch for __thiscall callees). Same pattern as
// delete_hook's ThiscallVoidFn.
typedef void (__fastcall *DlgCtorFn)(void* thisptr, void* edx_dummy, void* counter_map, void* wnd);
typedef void (__fastcall *ThiscallVoidFn)(void* thisptr);
typedef void* (__fastcall *CatiCtorFn)(void* thisptr, void* edx_dummy, int category,
                                       int subtype, int index);
typedef void (__fastcall *LoadListFn)(void* thisptr, void* edx_dummy, unsigned long packed,
                                      void* cati);
#else
// x64 fallbacks — never used (the DLL is only loaded into 32-bit TSDev.exe),
// but the code must compile for the build to succeed.
typedef void (*DlgCtorFn)(void* thisptr, void* counter_map, void* wnd);
typedef void (*ThiscallVoidFn)(void* thisptr);
typedef void* (*CatiCtorFn)(void* thisptr, int category, int subtype, int index);
typedef void (*LoadListFn)(void* thisptr, unsigned long packed, void* cati);
#endif

namespace {

// Reads an MFC CString field (32-bit MFC140): the CString object is a single
// wchar_t* to a CStringData buffer; the character data starts at that pointer
// and the length lives at data-0x0C (CStringData::nDataLength; x86 MFC140
// layout: pStringMgr@-0x10, nDataLength@-0x0C, nAllocLength@-0x08, nRefs@-0x04,
// data@+0x00). Returns false when the length is implausible — the caller then
// treats the record as unreadable instead of guessing.
bool read_cstring_field(const void* cstring_field, const wchar_t** out_data, size_t* out_len) {
    const wchar_t* data = *static_cast<const wchar_t* const*>(cstring_field);
    if (!data) {
        *out_data = L"";
        *out_len = 0;
        return true;
    }
    const long len = *(reinterpret_cast<const long*>(reinterpret_cast<const char*>(data) - 0x0C));
    if (len < 0 || len > (1 << 20)) return false;
    *out_data = data;
    *out_len = static_cast<size_t>(len);
    return true;
}

// Appends one CELTechInfo record to the shared mapping. Returns false when
// the record capacity is exhausted (truncated_flag set; the caller stops and
// still signals done).
bool append_record(EnumShared* shared, const unsigned char* cel) {
    if (shared->count >= static_cast<long>(sizeof(EnumShared::records) / sizeof(EnumRecord))) {
        shared->truncated_flag = 1;
        return false;
    }
    const wchar_t* name = nullptr;
    const wchar_t* progid = nullptr;
    size_t name_len = 0;
    size_t progid_len = 0;
    // CELTechInfo layout (dynamically confirmed, KB): category@+0x08,
    // subtype@+0x0a, record id@+0x0c, name CString@+0x10, ProgId CString@+0x1c.
    if (!read_cstring_field(cel + 0x10, &name, &name_len)) return false;
    if (!read_cstring_field(cel + 0x1c, &progid, &progid_len)) return false;
    EnumRecord& rec = shared->records[shared->count];
    bool truncated = copy_fixed_wstr(rec.name, sizeof(rec.name) / sizeof(rec.name[0]), name);
    truncated = copy_fixed_wstr(rec.progid, sizeof(rec.progid) / sizeof(rec.progid[0]), progid)
                || truncated;
    rec.category = *reinterpret_cast<const unsigned short*>(cel + 0x08);
    rec.subtype = *reinterpret_cast<const unsigned short*>(cel + 0x0a);
    rec.record_id = *reinterpret_cast<const unsigned short*>(cel + 0x0c);
    if (truncated) shared->truncated_flag = 1;
    ++shared->count;
    return true;
}

// Walks a populated CMapWordToPtr (at cati+0x0c) and appends every
// CELTechInfo record. Returns false when the enumeration must stop
// (capacity overflow or an implausible map/CString layout).
bool walk_type_map(EnumShared* shared, const unsigned char* map) {
    // CMapWordToPtr layout: +0x00 vtable, +0x04 m_pHashTable (CAssoc**),
    // +0x08 m_nHashTableSize.
    void* const* hash_table = static_cast<void* const*>(*reinterpret_cast<void* const*>(map + 0x04));
    const unsigned long hash_size = *reinterpret_cast<const unsigned long*>(map + 0x08);
    if (!hash_table || hash_size == 0) return true;  // empty map: nothing to enumerate
    if (hash_size > (1u << 20)) return false;        // implausible layout — refuse to walk
    for (unsigned long i = 0; i < hash_size; ++i) {
        // Correction B-3: the CAssoc layout is SWAPPED from standard MFC —
        // { pNext@0x00, value@0x04 = CELTechInfo*, key@0x08 = recordId }.
        // (Standard MFC { pNext, key@4, value@8 } reads garbage values.)
        const unsigned char* assoc = static_cast<const unsigned char*>(hash_table[i]);
        while (assoc) {
            const unsigned char* cel = *reinterpret_cast<unsigned char* const*>(assoc + 0x04);
            if (cel && !append_record(shared, cel)) return false;
            assoc = *reinterpret_cast<unsigned char* const*>(assoc);
        }
        if (shared->truncated_flag) return false;
    }
    return true;
}

}  // namespace

// Exported parameterless entry, called by the CLI via CreateRemoteThread
// (call_remote_function). Contract (mirrors ReArmHook's contract): returns
// nonzero when setup succeeded — the full enumeration then runs inside this
// remote thread and overall completion is governed by the done-event wait —
// and 0 on setup failure (results are marshalled via the file mapping; the
// CLI reads them after the done event).
#if defined(_M_IX86)
#pragma comment(linker, "/EXPORT:RunEnum=_RunEnum@4")
#endif
extern "C" __declspec(dllexport) DWORD WINAPI RunEnum(LPVOID) {
    const unsigned long pid = GetCurrentProcessId();

    wchar_t map_name[128];
    enum_map_name(pid, map_name);
    const HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, map_name);
    if (!mapping) return 0;
    EnumShared* shared = static_cast<EnumShared*>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(EnumShared)));
    if (!shared) {
        CloseHandle(mapping);
        return 0;
    }

    wchar_t event_name[128];
    enum_done_event_name(pid, event_name);
    const HANDLE done_event = OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name);

    shared->count = 0;
    shared->status = EnumPending;
    shared->crash_flag = 0;
    shared->truncated_flag = 0;

    // Resolve ORELUI20.dll in-process (it is loaded into TSDev.exe; no
    // LoadLibrary — the module must already be present).
    const HMODULE orelui = GetModuleHandleW(L"ORELUI20.dll");
    const unsigned char* orelui_base = reinterpret_cast<const unsigned char*>(orelui);
    // Exported surfaces by ordinal (stable); the two non-exported surfaces
    // are computed from the module base (version-brittle, see file header).
    const FARPROC dlg_ctor_proc = orelui ? GetProcAddress(orelui, MAKEINTRESOURCEA(17)) : nullptr;
    const FARPROC load_list_proc = orelui ? GetProcAddress(orelui, MAKEINTRESOURCEA(1291)) : nullptr;
    const FARPROC dlg_dtor_proc = orelui ? GetProcAddress(orelui, MAKEINTRESOURCEA(166)) : nullptr;
    if (!orelui_base || !dlg_ctor_proc || !load_list_proc || !dlg_dtor_proc) {
        shared->status = EnumError;
        if (done_event) SetEvent(done_event);
        UnmapViewOfFile(shared);
        CloseHandle(mapping);
        return 0;
    }
    void* const dlg_ctor = reinterpret_cast<void*>(dlg_ctor_proc);
    void* const load_list = reinterpret_cast<void*>(load_list_proc);
    void* const dlg_dtor = reinterpret_cast<void*>(dlg_dtor_proc);
    void* const cati_ctor = const_cast<unsigned char*>(orelui_base) + kCatiCtorRva;

    __try {
        alignas(4) unsigned char counter_map[kCounterMapBytes] = {};
        alignas(4) unsigned char dlg[kDialogBytes] = {};
        alignas(4) unsigned char cati[kCatiBytes];

        // Headless construction: NULL CWnd* parent — no dialog window is
        // created and no message pump runs (dynamically verified), so TSDev
        // visibility is never changed (R7). The second (EDX) argument is the
        // __fastcall dummy standing in for __thiscall's register-based this.
        const DlgCtorFn dlg_ctor_fn = reinterpret_cast<DlgCtorFn>(dlg_ctor);
        dlg_ctor_fn(dlg, nullptr, counter_map, nullptr);

        for (unsigned long i = 0; i < kTypeTableEntries && !shared->truncated_flag; ++i) {
            // VERSION-BRITTLE table read (DAT_1008fd04, 13 packed uint32 entries).
            const unsigned long packed =
                *reinterpret_cast<const unsigned long*>(const_cast<unsigned char*>(orelui_base)
                                                        + kTypeTableRva + i * 4);
            const int category = static_cast<int>(packed & 0xff);
            const int subtype = static_cast<int>((packed >> 8) & 0xff);
            // Correction B-1: the CAnalysisTypeInfo ctor takes 4 parameters
            // (this, category, subtype, index); the subtype lands at
            // cati+0x04 where LoadList applies it as the power-type filter.
            memset(cati, 0, kCatiBytes);
            const CatiCtorFn cati_ctor_fn = reinterpret_cast<CatiCtorFn>(cati_ctor);
            cati_ctor_fn(cati, nullptr, category, subtype, static_cast<int>(i));
            // Correction B-2: LoadList is called per-type with the packed
            // value from the table (type=0 does not bypass the filter).
            const LoadListFn load_list_fn = reinterpret_cast<LoadListFn>(load_list);
            load_list_fn(dlg, nullptr, packed, cati);
            // LoadList populates the CMapWordToPtr at cati+0x0c.
            if (!walk_type_map(shared, cati + 0x0c)) break;
        }

        // Destroy the dialog (dynamically verified: no crash on destruction).
        const ThiscallVoidFn dtor_fn = reinterpret_cast<ThiscallVoidFn>(dlg_dtor);
        dtor_fn(dlg);

        shared->status = EnumDone;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        shared->crash_flag = 1;
        shared->status = EnumError;
    }

    // Completion is signalled via the done event regardless of outcome; on
    // would-be overflow truncated_flag is set and the enumeration stops.
    if (done_event) SetEvent(done_event);

    UnmapViewOfFile(shared);
    CloseHandle(mapping);
    if (done_event) CloseHandle(done_event);
    return 1;
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    // All work happens in RunEnum (called via CreateRemoteThread), never in
    // DllMain — keeps first load and subsequent loads identical.
    (void)reason;
    return TRUE;
}
