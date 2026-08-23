// tsdev_lib.cpp
//
// Implementation of the shared orlib20 wrapper. See tsdev_lib.h for the API
// contract and tmp/re_artifacts/orlib_param_orders.md for the confirmed
// reverse-engineering findings this is built on.
//
// The reference probe (archive/elevated_test20.cpp) demonstrates the full flow
// end-to-end; this module encapsulates the same logic in a reusable API.

#include "tsdev_lib.h"
#include "orlib_resolver.h"
#include "com_apartment_guard.h"
#include "delete_disk_cleanup.h"

// TVS-L.13: the test-only construction seam (private ctor
// TsDevLib::TsDevLib(const wchar_t*, const TsDevLibTestConfig&) and the
// test-only factory tsdev::makeTsDevLibForTest) is defined below. The private
// ctor constructs the COM apartment guard with the injected ComApi
// (config.comApi) instead of the default real CoInitializeEx, then — on COM
// success and config.throwAfterCom == false — delegates to the shared
// initAfterCom() body so the post-COM init sequence is identical to the
// public ctor (no duplication). On COM failure it throws with the HRESULT
// and does NOTHING else (no SetDllDirectory, no orlib, no patch, no
// warehouse). On COM success + throwAfterCom it throws immediately so the
// guard's destructor balances CoUninitialize once during unwinding.
// tsdev_lib_test_hooks.h is the INTERNAL/test-only header that defines
// TsDevLibTestConfig and declares the factory; it is NOT a public header and
// is not included by src/tsdev_lib.h, so no COM configuration leaks into the
// public user API.
#include "tsdev_lib_test_hooks.h"

#include <stdexcept>
#include <cstring>
#include <cstdio>
#include <string>
#include <sstream>

// Compile-time debug logging gate. Define TSDEV_DEBUG at build time to
// enable stderr diagnostics in deleteEntry. Production builds are clean.
#ifdef TSDEV_DEBUG
#  define TSDEV_DBG(...) do { fprintf(stderr, __VA_ARGS__); fflush(stderr); } while(0)
#else
#  define TSDEV_DBG(...) do {} while(0)
#endif

namespace tsdev {

namespace {

// Support DLLs that must be pre-loaded before OrlibInit (§13).
const wchar_t* kSupportDlls[] = {
    L"ECL.dll",
    L"ORELUI20.dll",
    L"orUI20.dll",
    L"elobjmod.dll",
    L"elwinforms.dll",
    L"tschartxelobjmdl.dll",
    L"ELDebugger.dll"
};

// CELCompiler object size (§8).
const size_t kCELCompilerSize = 0x58; // 88 bytes

// Thread-check bypass patches: NOP the INT3 (0xCC → 0x90) in eleven
// dispatch helpers so they can be called from any thread, not just the
// worker thread. Each patch is a single byte.
//
// Patch-site resolution is now handled by OrlibResolver::applyPatch
// (TVS-B.5) using signature names:
//   FUN_101d0950_thread_check_INT3 (command 0x0F — load program)
//   FUN_101cf5b0_thread_check_INT3 (general dispatch helper)
//   FUN_101d1fa0_thread_check_INT3 (command 0x17 — save/commit)
//   FUN_101d0e80_thread_check_INT3 (command 0x11 — source-text load)
//   FUN_101d0c30_thread_check_INT3 (command 0x08 — type select, _LoadMasterRecsEX@16)
//   FUN_101d16c0_thread_check_INT3 (command 0x09 — item retrieval, _LoadMasterRecsEX@16)
//   FUN_101d0d90_thread_check_INT3 (command 0x07 — count by types, _LoadMasterRecsEX@16)
//   FUN_101cfce0_thread_check_INT3 (command 0x0D — COM interface get)
//   FUN_101d15c0_thread_check_INT3 (command 0x0D — local-mode record retrieval)
//   FUN_101d18c0_thread_check_INT3 (command 0x10 — BST search non-server mode)
//   FUN_101c36d0_thread_check_INT3 (verbatim name getter, non-server mode)
// The patch metadata (offsets, original/replacement bytes) is encoded
// in signatures/v9.50.01.3344.json and loaded by the resolver's signature catalog.

// Convert a UTF-8 / ANSI std::string to a std::wstring (ACP).
std::wstring toWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_ACP, 0, s.c_str(),
                                  static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(),
                        static_cast<int>(s.size()), &out[0], len);
    return out;
}

// Convert a std::wstring to a UTF-8 / ANSI std::string (ACP).
std::string toNarrow(const std::wstring& s) {
    if (s.empty()) return std::string();
    int len = WideCharToMultiByte(CP_ACP, 0, s.c_str(),
                                  static_cast<int>(s.size()),
                                  nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_ACP, 0, s.c_str(),
                        static_cast<int>(s.size()),
                        &out[0], len, nullptr, nullptr);
    return out;
}

// --- SEH-protected call wrappers ------------------------------------------
// MSVC forbids __try in functions that own C++ objects with destructors
// (C2712). These helpers contain only POD and raw pointers, so they can use
// __try/__except safely. Each returns a success flag / out-parameter.
//
// The typedefs mirror the private nested typedefs in TsDevLib; function
// pointer types are structurally identical so the members convert implicitly.

// CELCompilerCtorFn and CELCompilerDtorFn come from orlib_resolver.h
// (namespace tsdev). They are structurally compatible with the TsDevLib
// member typedefs (CELCompilerCtor_t / CELCompilerDtor_t) so the SEH
// wrappers below convert implicitly.
typedef void  (__cdecl    *StartCompilingFn)(void);
typedef void  (__fastcall *EndCompilingFn)(int param);
typedef int   (__stdcall  *VerifyModuleFn)(HWND, int, int, int, void*);

struct VerifySehResult {
    int   verifyResult;    // -1 if VerifyModule_New raised
};

bool callCtor(CELCompilerCtorFn fn, void* mem) {
    __try {
        fn(mem);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool callStartCompiling(StartCompilingFn fn) {
    __try {
        fn();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// --- Direct error array access (TVS-A.4) -----------------------------------
// _GetCompilingErrors@4 uses a std::vector ABI that is impractical to match
// across DLL boundaries. Instead, we read the global error array directly.
// The 3 data-global RVAs (error array pointer, count, HINSTANCE) are now
// resolved at construction time by OrlibResolver and stored as member
// variables (m_errArrayPtrRva, m_errCountRva, m_errHinstRva).
// The struct-layout constants below are NOT RVAs and are retained.
static constexpr size_t    kErrRecordStride = 0x129;     // 297 bytes per error record
static constexpr size_t    kErrTextOffset   = 0x29;      // null-terminated error text
static constexpr size_t    kErrResIdOffset  = 0x12;      // short: string-table resource ID
static constexpr size_t    kErrCharOffset   = 0x0A;      // int: character offset of error start in source

bool callVerify(VerifyModuleFn verifyFn,
                HWND hwnd, int whType, int progId,
                void* compilerMem, VerifySehResult* out) {
    out->verifyResult = -1;
    __try {
        out->verifyResult = verifyFn(hwnd, whType, progId, 1, compilerMem);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool callEndCompiling(EndCompilingFn fn, int param) {
    __try {
        fn(param);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool callDtor(CELCompilerDtorFn fn, void* mem) {
    __try {
        fn(mem);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// SEH-protected __fastcall call to DeleteFromTree (FUN_101e4030).
// FUN_101e4030 is __thiscall: ECX=globalLookup, [ESP+0]=whType,
// [ESP+4]=progId, [ESP+8]=flag. RET 12 (3 stack args).
// We use __fastcall with 5 args: ECX=globalLookup, EDX=unused,
// [ESP+0]=whType, [ESP+4]=progId, [ESP+8]=flag. RET 12 matches.
typedef void (__fastcall *DeleteFromTreeFn)(
    void*, int*, int, int, int);
bool callDeleteFromTree(void* fnPtr, void* globalLookup,
                        int whType, int progId, int flag) {
    DeleteFromTreeFn fn = reinterpret_cast<DeleteFromTreeFn>(fnPtr);
    __try {
        fn(globalLookup, nullptr, whType, progId, flag);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// SEH-protected call to InitGlobalLookup (__cdecl, no args).
// InitGlobalLookupFn comes from orlib_resolver.h (namespace tsdev).
bool callInitGlobalLookup(InitGlobalLookupFn fn, void** outPtr) {
    *outPtr = nullptr;
    __try {
        *outPtr = fn();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// SEH-protected call to GetWarehouse (__cdecl, 1 arg).
// GetWarehouseFn comes from orlib_resolver.h (namespace tsdev).
bool callGetWarehouse(GetWarehouseFn fn, int pathParam, void** out) {
    *out = nullptr;
    __try {
        *out = fn(pathParam);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// SEH-protected __fastcall call to FUN_101a8360 (delete orchestrator).
// __fastcall: ECX=whType, EDX=progId, no stack args, returns void.
// POD-only (no C++ objects with destructors) so __try is legal.
#pragma warning(push)
#pragma warning(disable: 4731)
bool callDeleteOrchestrator(void* fnPtr, int whType, int progId) {
    __try {
        __asm {
            mov ecx, whType
            mov edx, progId
            call fnPtr
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#pragma warning(pop)

// SEH-protected __thiscall call to FUN_101cf5b0 (command dispatcher).
// __thiscall: ECX=warehouse (this), stack=commandId (1 arg, callee cleanup
// RET 4). POD-only (no C++ objects with destructors) so __try is legal.
#pragma warning(push)
#pragma warning(disable: 4731)
bool callCmdDispatcher(void* fnPtr, void* warehouse, int cmdId) {
    __try {
        __asm {
            push cmdId
            mov ecx, warehouse
            call fnPtr
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#pragma warning(pop)

// SEH-protected call to _CloseWarehouseForCurrentThreadAndPath@4.
// __stdcall, 1 arg (wide string pointer cast to int). POD-only.
void callCloseWarehouseHelper(CloseWarehouseFn fn, int param) {
    __try {
        fn(param);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Best-effort close; ignore SEH.
    }
}

// SEH-protected __thiscall call to FUN_101d0950 (cmd 0x0F dispatch helper).
// __thiscall: ECX=warehouse (this), stack=typeArray, progId, outParam
// (3 args, callee cleanup RET 0xC). The outParam must point to a buffer of
// at least 0x280 bytes (FUN_101d0950 writes to outParam+0x26d on success).
// POD-only (no C++ objects with destructors) so __try is legal.
#pragma warning(push)
#pragma warning(disable: 4731)
bool callCmd0FDispatcher(void* fnPtr, void* warehouse, void* typeArray,
                         int progId, void* outParam) {
    __try {
        __asm {
            push outParam
            push progId
            push typeArray
            mov ecx, warehouse
            call fnPtr
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#pragma warning(pop)

// SEH-protected __stdcall call to FUN_101d43a0 (type-array builder).
// __stdcall: stack=category, outArray (2 args, callee cleanup RET 0x8).
// POD-only (no C++ objects with destructors) so __try is legal.
#pragma warning(push)
#pragma warning(disable: 4731)
bool callBuildTypeArray(void* fnPtr, int category, int* outArray) {
    __try {
        __asm {
            push outArray
            push category
            call fnPtr
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#pragma warning(pop)

// SEH-protected __fastcall call to FUN_1000dd30 (CELProgramWriter constructor).
// __fastcall: ECX=buffer (this), no stack args. POD-only (no C++ objects with
// destructors) so __try is legal.
#pragma warning(push)
#pragma warning(disable: 4731)
bool callCelProgramWriterCtor(void* fnPtr, void* buffer) {
    __try {
        __asm {
            mov ecx, buffer
            call fnPtr
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#pragma warning(pop)

// SEH-protected __thiscall call to FUN_101cfce0 (cmd 0x0D dispatch helper).
// __thiscall: ECX=warehouse (this), stack=type, param (2 args, callee cleanup
// RET 8). Returns the COM interface in EAX (or NULL on failure). The caller
// must store the returned interface back into [warehouse+0x5E] because
// FUN_101cfce0 clears [warehouse+0x5E] before returning. POD-only (no C++
// objects with destructors) so __try is legal.
#pragma warning(push)
#pragma warning(disable: 4731)
bool callCmd0DDispatcher(void* fnPtr, void* warehouse, int typeVal,
                         int param, void** outInterface) {
    void* result = nullptr;
    __try {
        __asm {
            push param
            push typeVal
            mov ecx, warehouse
            call fnPtr
            mov result, eax
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outInterface = nullptr;
        return false;
    }
    *outInterface = result;
    return true;
}
#pragma warning(pop)

// --- SEH-protected wrappers for OrlibInit, OrlibExit, FindExistPos (TVS-B.5 AC3) ---
// These three call sites were previously unprotected. Each wrapper is
// POD-only (no C++ objects with destructors) so __try/__except is legal
// under MSVC (no C2712). The typedefs (OrlibInitFn, OrlibExitFn,
// FindExistPosFn) come from orlib_resolver.h and are structurally identical
// to the TsDevLib member typedefs.

// SEH-protected call to OrlibInit (__stdcall, 2 args).
bool callOrlibInit(OrlibInitFn fn, const char* workDir, int contextId) {
    __try {
        return fn(workDir, contextId) != FALSE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// SEH-protected call to OrlibExit (__stdcall, no args).
bool callOrlibExit(OrlibExitFn fn) {
    __try {
        return fn() != FALSE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// SEH-protected call to FindExistPos (__stdcall, 2 args).
struct FindExistPosSehResult {
    int result;   // -1 if FindExistPos raised
};
bool callFindExistPos(FindExistPosFn fn, const char* name,
                      int warehouseType, FindExistPosSehResult* out) {
    out->result = -1;
    __try {
        out->result = fn(name, warehouseType);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// SEH-protected call to _LoadTextFileEX@16 (__stdcall, 4 args).
// File-scope typedef matching the TsDevLib private member typedef
// (structurally identical: HGLOBAL (__stdcall*)(void*, int, int, void*)),
// so this free helper can take a typed function pointer without accessing
// the class's private members. POD-only (no C++ objects with destructors)
// so __try/__except is legal under MSVC (no C2712).
typedef HGLOBAL (__stdcall *LoadTextFileExFn)(void* unused, int whType,
                                              int progId, void* password);
struct LoadTextFileExSehResult {
    HGLOBAL result;   // NULL if _LoadTextFileEX raised
};
bool callLoadTextFileEx(LoadTextFileExFn fn, void* unused, int whType,
                        int progId, void* password,
                        LoadTextFileExSehResult* out) {
    out->result = NULL;
    __try {
        out->result = fn(unused, whType, progId, password);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// SEH-protected call to _LoadMasterRecsEX@16 (__stdcall, 4 args).
// File-scope typedef matching the TsDevLib private member typedef
// (structurally identical: int (__stdcall*)(int, HGLOBAL*, const char*, int)),
// so this free helper can take a typed function pointer without accessing
// the class's private members. POD-only (no C++ objects with destructors)
// so __try/__except is legal under MSVC (no C2712).
typedef int (__stdcall *LoadMasterRecsExFn)(int category, HGLOBAL* outBuf,
                                            const char* path, int filterMode);
struct LoadMasterRecsExSehResult {
    int result;  // -1 if _LoadMasterRecsEX raised
};
bool callLoadMasterRecsEx(LoadMasterRecsExFn fn, int category, HGLOBAL* outBuf,
                          const char* path, int filterMode,
                          LoadMasterRecsExSehResult* out) {
    out->result = -1;
    __try {
        out->result = fn(category, outBuf, path, filterMode);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// SEH-protected call to _GetMasterRecByType@16 (__stdcall, 4 args).
// File-scope typedef matching the TsDevLib private member typedef
// (structurally identical: int (__stdcall*)(int, int, int, void*)),
// so this free helper can take a typed function pointer without accessing
// the class's private members. POD-only (no C++ objects with destructors)
// so __try/__except is legal under MSVC (no C2712).
typedef int (__stdcall *GetMasterRecByTypeFn)(int category, int subType,
                                              int index, void* outBuf);
struct GetMasterRecByTypeSehResult {
    int result;  // -1 if _GetMasterRecByType raised
};
bool callGetMasterRecByType(GetMasterRecByTypeFn fn, int category, int subType,
                            int index, void* outBuf,
                            GetMasterRecByTypeSehResult* out) {
    out->result = -1;
    __try {
        out->result = fn(category, subType, index, outBuf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// DEAD CODE — retained for reference. Superseded by callBstSearchWrapper +
// callVerbatimNameGetter. FUN_101a42d0 returns the ProgId (not the name) in
// non-server mode (DAT_1023aa8f == 0, which is our case). The new Phase 2
// calls FUN_101d05e0 + FUN_101c36d0 directly instead.
//
// SEH-protected __fastcall call to FUN_101a42d0 (find item + get verbatim name).
// __fastcall: ECX=searchName (char*), EDX=componentType (int).
// Stack args (pushed right-to-left): outVerbatimName (char**), warehouse (void*).
// Callee cleans 2 stack DWORDs (RET 0x8). Returns 0xFFFFFFFF if not found.
//
// Two return conventions depending on the global DAT_1023aa8f (server-mode flag):
//   Server mode (DAT_1023aa8f == 1): return value = warehouse pointer (success);
//       the verbatim name is written to *outVerbatimName. The search is by
//       *warehouse (an ID), NOT by name — the name is only used for a final
//       confirmation comparison. This path is NOT suitable for name-based lookup.
//   Non-server mode (DAT_1023aa8f != 1): return value = char* to the verbatim
//       stored name (success); *outVerbatimName is NOT written. The search is
//       by name via FUN_101d05e0 (BST + file-based fallback).
//
// We use the non-server path (do NOT force DAT_1023aa8f) so the search is by
// name. The caller must check both result and verbatimName: if verbatimName is
// null, the return value itself is the name pointer (non-server path).
// POD-only (no C++ objects with destructors) so __try is legal under MSVC.
struct FindItemGetNameSehResult {
    int result;         // return value of FUN_101a42d0
    char* verbatimName; // output name pointer (server mode only; null in non-server)
};
#pragma warning(push)
#pragma warning(disable: 4731)
bool callFindItemGetName(void* fnPtr, const char* searchName, int componentType,
                         void* warehouse, FindItemGetNameSehResult* out) {
    int retVal = (int)0xFFFFFFFF;
    char* outName = nullptr;
    __try {
        __asm {
            lea eax, outName           // address of local outName (char**)
            push eax                   // [ESP+4] = &outName (outVerbatimName)
            push warehouse             // [ESP+0] = warehouse
            mov ecx, searchName        // ECX = searchName (fastcall arg 1)
            mov edx, componentType     // EDX = componentType (fastcall arg 2)
            call fnPtr                 // callee cleans 8 bytes via RET 0x8
            mov retVal, eax            // capture return value
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->result = (int)0xFFFFFFFF;
        out->verbatimName = nullptr;
        return false;
    }
    out->result = retVal;
    out->verbatimName = outName;
    return true;
}
#pragma warning(pop)

// SEH-protected __thiscall call to FUN_101d05e0 (BST search wrapper).
// __thiscall: ECX=warehouse, stack=(typeFilter, name, *outNode), RET 0xC.
// Returns bool (1=found, 0=not found). Stores node pointer in *outNode.
// POD-only (no C++ objects with destructors) so __try is legal under MSVC.
#pragma warning(push)
#pragma warning(disable: 4731)
bool callBstSearchWrapper(void* fnPtr, void* warehouse, int* typeFilter,
                          const char* name, void** outNode, int* outResult) {
    *outNode = nullptr;
    *outResult = 0;
    int retVal = 0;
    __try {
        __asm {
            mov eax, outNode          ; eax = pointer to void* (where node ptr stored)
            push eax                  ; arg3: *outNode
            push name                 ; arg2: name (char*)
            push typeFilter           ; arg1: typeFilter (int* — type array ptr)
            mov ecx, warehouse        ; thiscall: ECX = warehouse
            call fnPtr                ; callee cleans 12 bytes (RET 0xC)
            mov retVal, eax           ; capture bool return value in local
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    *outResult = retVal;  // write through pointer outside asm
    return true;
}
#pragma warning(pop)

// SEH-protected __fastcall call to FUN_101c36d0 (verbatim name getter).
// __fastcall: ECX=node pointer, no stack args, RET. Returns char* to the
// verbatim stored name at node+0x1D.
// POD-only (no C++ objects with destructors) so __try is legal under MSVC.
#pragma warning(push)
#pragma warning(disable: 4731)
bool callVerbatimNameGetter(void* fnPtr, void* nodePtr, const char** outName) {
    *outName = nullptr;
    void* retVal = nullptr;
    __try {
        __asm {
            mov ecx, nodePtr          ; fastcall: ECX = node pointer
            call fnPtr                ; no stack args, RET (no cleanup)
            mov retVal, eax           ; capture char* return value
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    *outName = reinterpret_cast<const char*>(retVal);
    return true;
}
#pragma warning(pop)

// Restore `len` original bytes at an executable address. Used by the
// TsDevLib destructor to undo the thread-check patches so a
// subsequent TsDevLib instance in the same process can re-scan the (now
// original) bytes successfully. POD-only (no C++ objects with destructors)
// so __try would be legal, but no SEH guard is needed here — VirtualProtect
// + memcpy are safe; on failure we simply skip the write.
static void restoreBytes(void* addr, const unsigned char* bytes, size_t len) {
    DWORD oldProtect = 0;
    if (VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        std::memcpy(addr, bytes, len);
        VirtualProtect(addr, len, oldProtect, &oldProtect);
    }
}

// --- TS install path discovery (TVS-B.7) -----------------------------------
// Resolves the TradeStation install root at runtime via (1) an explicit
// override path, (2) the Windows registry, or (3) a conventional default
// constructed from %ProgramFiles(x86)%. Replaces the old hardcoded
// absolute TS install path constants.

// Strip a single trailing backslash (if present) from a wide path.
// Does NOT strip the backslash from a drive root like "C:\" (checks that the
// char before the trailing '\' is not ':'). This prevents double-backslashes
// in derived paths (the registry value has a trailing '\').
std::wstring normalizeTrailingBackslash(std::wstring path) {
    if (path.size() > 1 && path.back() == L'\\' &&
        path[path.size() - 2] != L':') {
        path.pop_back();
    }
    return path;
}

// Read a REG_SZ value from the registry. Returns the value as a std::wstring,
// or an empty string on failure. Uses KEY_WOW64_32KEY to access the 32-bit
// registry view (the subkey path must NOT include WOW6432Node — the flag
// handles the redirection; including both causes double-redirection).
std::wstring readRegistryRootDir(HKEY hive, const wchar_t* subkey,
                                 const wchar_t* valueName) {
    HKEY hKey = nullptr;
    LONG rc = RegOpenKeyExW(hive, subkey, 0,
                            KEY_READ | KEY_WOW64_32KEY, &hKey);
    if (rc != ERROR_SUCCESS) {
        return std::wstring();
    }
    wchar_t buf[MAX_PATH] = {0};
    DWORD bufSize = sizeof(buf);
    DWORD type = 0;
    rc = RegQueryValueExW(hKey, valueName, nullptr, &type,
                          reinterpret_cast<LPBYTE>(buf), &bufSize);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS || type != REG_SZ) {
        return std::wstring();
    }
    // bufSize is in bytes; compute the string length (excluding null terminator).
    DWORD charCount = bufSize / sizeof(wchar_t);
    if (charCount > 0 && buf[charCount - 1] == L'\0') {
        charCount--;  // exclude the null terminator
    }
    return std::wstring(buf, charCount);
}

// Construct the conventional default TS install root from the
// %ProgramFiles(x86)% environment variable + "\TradeStation 9.5". If the
// env var is not set (very unlikely on modern Windows), falls back to
// "C:\Program Files (x86)" (a Windows convention, not a TS-specific
// hardcoding — contains no "TradeStation 9.5" string).
std::wstring getConventionalDefaultRoot() {
    wchar_t pfBuf[MAX_PATH] = {0};
    DWORD pfLen = GetEnvironmentVariableW(L"ProgramFiles(x86)", pfBuf, MAX_PATH);
    std::wstring pfBase;
    if (pfLen == 0 || pfLen >= MAX_PATH) {
        pfBase = L"C:\\Program Files (x86)";
    } else {
        pfBase = std::wstring(pfBuf, pfLen);
    }
    // Strip a trailing backslash from the env var value if present.
    if (pfBase.size() > 1 && pfBase.back() == L'\\' &&
        pfBase[pfBase.size() - 2] != L':') {
        pfBase.pop_back();
    }
    return pfBase + L"\\TradeStation 9.5";
}

// Validates that <root>\Program\orlib20.dll exists. Returns true if found.
bool validateTsInstall(const std::wstring& root) {
    std::wstring orlibPath = root + L"\\Program\\orlib20.dll";
    DWORD attr = GetFileAttributesW(orlibPath.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES);
}

// Resolves the TS install root path. Priority: override → registry → default.
// Prints to stderr which mechanism was used.
std::wstring resolveTsInstallRoot(const wchar_t* overridePath) {
    // (1) Explicit override
    if (overridePath != nullptr && overridePath[0] != L'\0') {
        std::wstring root = normalizeTrailingBackslash(std::wstring(overridePath));
        std::fprintf(stderr,
                     "tsdev: TS install path resolved via --ts-path override: %ls\n",
                     root.c_str());
        fflush(stderr);
        return root;
    }

    // (2) Primary registry key: ...\TradeStation\Installing\RootDirectory
    std::wstring root = readRegistryRootDir(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\TradeStation Technologies\\TradeStation\\Installing",
        L"RootDirectory");
    if (!root.empty()) {
        root = normalizeTrailingBackslash(root);
        std::fprintf(stderr,
                     "tsdev: TS install path resolved via registry (Installing): %ls\n",
                     root.c_str());
        fflush(stderr);
        return root;
    }

    // (3) Fallback registry key: ...\TradeStation\Versions\9.05.00\RootDirectory
    root = readRegistryRootDir(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\TradeStation Technologies\\TradeStation\\Versions\\9.05.00",
        L"RootDirectory");
    if (!root.empty()) {
        root = normalizeTrailingBackslash(root);
        std::fprintf(stderr,
                     "tsdev: TS install path resolved via registry (Versions\\9.05.00): %ls\n",
                     root.c_str());
        fflush(stderr);
        return root;
    }

    // (4) Conventional default (last resort) — constructed from %ProgramFiles(x86)%
    root = getConventionalDefaultRoot();
    std::fprintf(stderr,
                 "tsdev: TS install path resolved via conventional default: %ls\n",
                 root.c_str());
    fflush(stderr);
    return root;
}

} // namespace

// ============================================================================
// Constructor / Destructor
// ============================================================================

TsDevLib::TsDevLib(const wchar_t* tsPathOverride)
    // The COM guard is constructed FIRST, in the member-initialization list,
    // so CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED) runs before any
    // effective action in the constructor body (SetDllDirectoryW,
    // LoadLibraryW, OrlibResolver, support-DLL loads, patch application,
    // OrlibInit). The default ComApi binds the real CoInitializeEx /
    // CoUninitialize from ole32.dll. If COM init fails (any HRESULT other
    // than S_OK / S_FALSE), the body below throws before touching orlib20.
    : m_comGuard(std::make_unique<ComApartmentGuard>()) {
    // 0. STA COM must be entered on this thread before any orlib20 load or
    //    init. A failed CoInitializeEx (including RPC_E_CHANGED_MODE, which
    //    indicates the thread is already in an incompatible apartment) is a
    //    hard initialization failure: do NOT proceed with orlib loading,
    //    window creation, entry creation, or ELL/XML mutation. The guard's
    //    destructor is a no-op when not initialized, so no CoUninitialize is
    //    issued on this failure path. Include the hexadecimal HRESULT for
    //    diagnostics.
    if (!m_comGuard->initialized()) {
        std::stringstream ss;
        ss << "tsdev: COM initialization failed: HRESULT 0x"
           << std::hex << std::uppercase
           << static_cast<unsigned long>(m_comGuard->initResult());
        std::fprintf(stderr, "%s\n", ss.str().c_str());
        fflush(stderr);
        throw std::runtime_error(ss.str());
    }

    // Steps 1–13 (SetDllDirectoryW → OrlibResolver → support DLLs → patches →
    // OrlibInit → hidden EDIT control) are shared with the private test-only
    // ctor via initAfterCom() so the two construction paths are behaviorally
    // identical past the COM-init step (TVS-L.13).
    initAfterCom(tsPathOverride);
}

// TVS-L.13: shared post-COM initialization body. Called by BOTH constructors
// AFTER the COM apartment guard has been constructed and verified initialized
// (S_OK / S_FALSE). Extracted from the public ctor so the private test-only
// ctor can reuse the exact same sequence without duplicating ~110 lines.
//
// If any step here throws (e.g. orlib20.dll missing, OrlibInit returned FALSE,
// patch application failed), the exception propagates out of the calling
// constructor. NOTE: when a constructor throws, the destructor BODY
// (~TsDevLib) does NOT run — only the destructors of FULLY-CONSTRUCTED
// members run, in reverse declaration order, during stack unwinding. The
// destructor body runs only when the constructor completes normally (the
// object's lifetime began). So a throw from this body is balanced solely by
// the member destructors: because m_comGuard is a fully-constructed member
// (constructed first in the member-init list), its destructor runs during
// stack unwinding and balances COM exactly once — the same unwind behavior
// the public ctor already relied on. The ~TsDevLib body's gating by
// m_orlibInitialized / m_editorHwnd / m_patchesApplied matters only for the
// NORMAL-destruction case (object fully constructed, destructor body runs);
// it does NOT participate in the constructor-throw unwind path.
void TsDevLib::initAfterCom(const wchar_t* tsPathOverride) {
    // 1. Resolve the TS install root (override → registry → conventional
    //    default). This also prints to stderr which mechanism was used.
    m_tsRoot = resolveTsInstallRoot(tsPathOverride);

    // 2. Validate that <root>\Program\orlib20.dll exists. A bad --ts-path
    //    override or a missing install is caught here with a clear error.
    if (!validateTsInstall(m_tsRoot)) {
        std::fprintf(stderr,
                     "tsdev: ERROR — orlib20.dll not found at %ls\\Program\\orlib20.dll.\n"
                     "tsdev: TS install path may be incorrect. Use --ts-path to specify the correct path.\n",
                     m_tsRoot.c_str());
        fflush(stderr);
        throw std::runtime_error(
            "tsdev: orlib20.dll not found at <root>\\Program\\orlib20.dll");
    }

    // 3. Derive member paths from the resolved root.
    m_tsProgramDir = m_tsRoot + L"\\Program";
    m_myWorkBin    = toNarrow(m_tsRoot + L"\\MyWork\\Bin");
    m_wrkPath      = toNarrow(m_tsRoot + L"\\MyWork\\MWRKArea.wrk");

    // 4. Point the loader at the TS Program directory so orlib20.dll and its
    //    dependencies resolve.
    if (!SetDllDirectoryW(m_tsProgramDir.c_str())) {
        throw std::runtime_error(
            "tsdev: SetDllDirectoryW failed for TS Program dir");
    }

    // 5. Construct OrlibResolver — loads orlib20.dll via LoadLibraryW,
    //    parses PE headers, builds signature catalog, detects version.
    //    Throws OrlibResolverError on load/PE failure (propagates as
    //    std::runtime_error — AC6).
    m_resolver = std::make_unique<OrlibResolver>(L"orlib20.dll");

    // 6. Version check — warn but do NOT abort if unknown (AC4).
    //    detectVersion() never throws; it is already cached in the resolver.
    const VersionDescriptor& vd = m_resolver->version();
    if (!vd.known || !vd.warningMessage.empty()) {
        std::fprintf(stderr, "tsdev: WARNING — %s\n",
                     vd.warningMessage.c_str());
        fflush(stderr);
    }

    // 7. Pre-load support DLLs (§13). Failures are non-fatal — log via
    //    stderr and continue; OrlibInit will fail outright if a truly
    //    required dependency is missing.
    for (const wchar_t* dll : kSupportDlls) {
        HMODULE h = LoadLibraryW(dll);
        if (!h) {
            std::fprintf(stderr,
                         "tsdev: WARNING — pre-load of %ls failed (err=%lu)\n",
                         dll, GetLastError());
        }
    }

    // 8. Resolve 7 exported functions via resolver (replaces the old
    //    GetProcAddress-based template helper).
    m_pOrlibInit      = m_resolver->resolveOrlibInit();
    m_pOrlibExit      = m_resolver->resolveOrlibExit();
    m_pFindExistPos   = m_resolver->resolveFindExistPos();
    m_pCreateEntry    = m_resolver->resolveCreateEntryEX();
    m_pSaveModuleNew  = m_resolver->resolveSaveModuleNew();
    m_pVerifyModule   = m_resolver->resolveVerifyModuleNew();
    m_pCloseWarehouse = m_resolver->resolveCloseWarehouse();
    m_loadTextFileEx  = (LoadTextFileExFn)m_resolver->resolveExport("_LoadTextFileEX@16");
    m_loadMasterRecsEx = (LoadMasterRecsExFn)m_resolver->resolveExport("_LoadMasterRecsEX@16");
    m_getMasterRecByType = (GetMasterRecByTypeFn)m_resolver->resolveExport("_GetMasterRecByType@16");

    // 9. Resolve 8 internal functions via resolver (signature scan,
    //    replaces old base-address + RVA arithmetic).
    m_pStartCompiling     = m_resolver->resolveStartCompilingProcess();
    m_pEndCompiling       = m_resolver->resolveEndCompilingProcess();
    m_pCELCompilerCtor    = m_resolver->resolveCELCompilerCtor();
    m_pCELCompilerDtor    = m_resolver->resolveCELCompilerDtor();
    m_pInitGlobalLookup   = m_resolver->resolveInitGlobalLookup();
    m_pDeleteFromTree     = m_resolver->resolveDeleteFromTree();
    m_pGetWarehouse       = m_resolver->resolveGetWarehouse();
    m_pDeleteOrchestrator = m_resolver->resolveDeleteOrchestrator();
    m_pBuildTypeArray     = m_resolver->resolveBuildTypeArray();
    m_pCelProgWriterCtor  = m_resolver->resolveCelProgramWriterCtor();
    m_pFindItemGetName    = m_resolver->resolveFindItemGetName();
    m_pBstSearchWrapper   = m_resolver->resolveBstSearchWrapper();
    m_pVerbatimNameGetter = m_resolver->resolveVerbatimNameGetter();

    // 10. Resolve 3 data-global RVAs via resolver (replaces hardcoded
    //    error-array/count/HINSTANCE RVA constants).
    m_errArrayPtrRva = m_resolver->resolveErrorArrayPtrRva();
    m_errCountRva    = m_resolver->resolveErrorCountRva();
    m_errHinstRva    = m_resolver->resolveErrorHinstRva();

    // 11. Apply the thread-check INT3 patches via the existing private method
    //     (which delegates to m_resolver->applyPatch). OrlibResolverError
    //     propagates out of the constructor on validation or VirtualProtect
    //     failure (AC6). The original FUN_10011230 finalization path is left
    //     unpatched so the real symbol/finalization flow runs end-to-end.
    applyThreadCheckPatches();

    // 12. Initialize orlib20 (SEH-protected — AC3).
    if (!callOrlibInit(m_pOrlibInit, NULL, 1)) {
        throw std::runtime_error(
            "tsdev: OrlibInit(NULL, 1) raised SEH exception or returned FALSE");
    }
    m_orlibInitialized = true;

    // 13. Create the hidden EDIT control used to hold source text.
    m_editorHwnd = createHiddenEditControl();
    if (!m_editorHwnd) {
        callOrlibExit(m_pOrlibExit);
        throw std::runtime_error("tsdev: failed to create hidden EDIT control");
    }
}

// ============================================================================
// TVS-L.13: PRIVATE test-only construction seam.
//
// This ctor is PRIVATE (declared in src/tsdev_lib.h) and the ONLY intended
// caller is the test-only free function tsdev::makeTsDevLibForTest(...)
// (declared in the INTERNAL header src/tsdev_lib_test_hooks.h, which is NOT
// part of the public user API). End users cannot call this ctor: the
// parameter type TsDevLibTestConfig is forward-declared (opaque) in the
// public header and defined only in the internal/test-only header.
//
// Contract (enforced by tests/test_tsdev_com_init_seam.cpp):
//   - Constructs the COM apartment guard with the INJECTED ComApi
//     (config.comApi) instead of the default real CoInitializeEx. This lets a
//     pure unit test drive COM init with mock function pointers and never
//     touch the test process's real COM apartment state.
//   - On COM init FAILURE (any HRESULT other than S_OK / S_FALSE, including
//     RPC_E_CHANGED_MODE and a generic E_FAIL): throws std::runtime_error
//     naming the exact hexadecimal HRESULT (uppercase, 0x%08X form, matching
//     the public ctor's diagnostic format) and does NOTHING else — no
//     SetDllDirectoryW, no orlib20 / support-DLL load, no patch application,
//     no OrlibInit, no warehouse access. The guard's destructor is a no-op
//     (m_initialized == false), so the injected CoUninitialize is NOT called.
//     COM failure is a HARD stop before every later step.
//   - On COM init SUCCESS (S_OK or S_FALSE) with config.throwAfterCom == true:
//     throws std::runtime_error IMMEDIATELY after the COM guard is
//     constructed, simulating a later-constructor exception. The guard's
//     destructor runs during stack unwinding and calls the injected
//     CoUninitialize EXACTLY once. No orlib20 / patch / warehouse side effect
//     runs (the throw happens before initAfterCom is reached).
//   - On COM init SUCCESS with config.throwAfterCom == false: proceeds with
//     the normal constructor body via initAfterCom() — identical to the
//     public ctor. This path requires a real TradeStation install and is NOT
//     exercised by the pure unit test.
// ============================================================================
TsDevLib::TsDevLib(const wchar_t* tsPathOverride,
                   const TsDevLibTestConfig& config)
    // Construct the COM guard with the INJECTED ComApi so the test controls
    // CoInitializeEx / CoUninitialize. The guard is constructed FIRST (member-
    // init list) so COM is entered before any later step, exactly as in the
    // public ctor.
    : m_comGuard(std::make_unique<ComApartmentGuard>(config.comApi)) {
    // COM-init hard-failure check. Identical diagnostic format to the public
    // ctor: "tsdev: COM initialization failed: HRESULT 0x%08X" (uppercase
    // hex). On failure the guard is NOT initialized, so its destructor is a
    // no-op — the injected CoUninitialize is never called, and NO later step
    // (SetDllDirectory / orlib / patch / warehouse) runs because we throw
    // here, before initAfterCom.
    if (!m_comGuard->initialized()) {
        std::stringstream ss;
        ss << "tsdev: COM initialization failed: HRESULT 0x"
           << std::hex << std::uppercase
           << static_cast<unsigned long>(m_comGuard->initResult());
        std::fprintf(stderr, "%s\n", ss.str().c_str());
        fflush(stderr);
        throw std::runtime_error(ss.str());
    }

    // COM succeeded (S_OK or S_FALSE). If the test requested a simulated
    // later-constructor exception, throw NOW — before any orlib20 / patch /
    // warehouse side effect. The COM guard is a fully-constructed member, so
    // its destructor runs during stack unwinding and calls the injected
    // CoUninitialize EXACTLY once, balancing the successful CoInitializeEx on
    // this thread. This is the "later-constructor exception after COM
    // success" unwind path (TVS-L.13 acceptance criterion 5).
    if (config.throwAfterCom) {
        throw std::runtime_error(
            "tsdev: injected post-COM constructor exception (TVS-L.13 test seam)");
    }

    // COM succeeded and no simulated throw: proceed with the normal
    // constructor body, shared with the public ctor via initAfterCom() so the
    // two paths are behaviorally identical past the COM-init step. This path
    // requires a real TradeStation install (orlib20.dll) and is NOT exercised
    // by the pure unit test.
    initAfterCom(tsPathOverride);
}

// ============================================================================
// TVS-L.13: test-only factory. Thin wrapper around the private ctor above.
// Returns a std::unique_ptr<TsDevLib> so the caller's ownership is explicit
// and the public ~TsDevLib() balances COM + OrlibExit on the same thread.
// Declared in the INTERNAL header src/tsdev_lib_test_hooks.h (NOT a public
// header); the public tsdev_lib.h only friend-declares it so it can reach the
// private ctor. End users never see or call this function.
//
// Defined directly here (NOT in a re-opened namespace tsdev {} block) because
// this entire TU is already inside `namespace tsdev` (opened at the top of the
// file). Re-opening it would create a nested tsdev::tsdev namespace and the
// symbol would not match the declaration in tsdev_lib_test_hooks.h.
// ============================================================================
std::unique_ptr<TsDevLib> makeTsDevLibForTest(
    const wchar_t* tsPathOverride,
    const TsDevLibTestConfig& config) {
    // Construct via the private test-only ctor and hand ownership to the
    // caller. If the ctor throws (COM failure or throwAfterCom), no object is
    // produced and the unique_ptr is never set — the COM guard's destructor
    // has already balanced COM during the ctor's stack unwinding.
    return std::unique_ptr<TsDevLib>(
        new TsDevLib(tsPathOverride, config));
}

TsDevLib::~TsDevLib() {
    if (m_editorHwnd && IsWindow(m_editorHwnd)) {
        DestroyWindow(m_editorHwnd);
        m_editorHwnd = nullptr;
    }
    // Destroy the STATIC parent AFTER the EDIT child so that destroying
    // the parent does not prematurely take the child with it.
    if (m_editorParent && IsWindow(m_editorParent)) {
        DestroyWindow(m_editorParent);
        m_editorParent = nullptr;
    }
    // Only call OrlibExit if OrlibInit actually succeeded. If the constructor
    // threw before OrlibInit (e.g. during patch application), calling
    // OrlibExit without a prior init is unsafe.
    if (m_orlibInitialized && m_pOrlibExit) {
        callOrlibExit(m_pOrlibExit);
    }
    // Restore all 11 patch sites to their original bytes AFTER OrlibExit but
    // BEFORE m_resolver (unique_ptr) is destroyed — the resolver still owns
    // the loaded orlib20.dll and its base address, so baseAddress() is valid
    // here. This returns the in-memory DLL to its original state so that a
    // subsequent TsDevLib instance in the same process can re-scan the
    // original-byte signatures (the thread-check signatures scan for 0xCC).
    // Without this restoration the second instance's scan finds the patched
    // bytes (0x90) and throws OrlibResolverError.
    if (m_patchesApplied) {
        uintptr_t base = m_resolver->baseAddress();
        // Thread-check patches: restore 1 original byte (0xCC) each. Only
        // restore sites whose RVA was actually cached before a possible
        // throw; for a site where applyPatch threw after the RVA was cached,
        // writing the original byte back is a harmless no-op (the patch was
        // never written).
        for (int i = 0; i < 11; i++) {
            if (m_threadCheckRvas[i] != 0) {
                restoreBytes(reinterpret_cast<void*>(base + m_threadCheckRvas[i]),
                             &m_threadCheckOriginalBytes[i], 1);
            }
        }
        m_patchesApplied = false;
    }
    // DLLs are released automatically at process exit; do not FreeLibrary
    // orlib20.dll here because internal threads/state may still reference it.
    // The resolver (m_resolver) owns the HMODULE and does not FreeLibrary
    // it either (matching the existing behavior).
}

// ============================================================================
// Public API
// ============================================================================

int TsDevLib::findExistPos(const std::string& name, ComponentType type) {
    FindExistPosSehResult seh;
    if (!callFindExistPos(m_pFindExistPos, name.c_str(),
                          getCategory(type), &seh)) {
        // SEH exception — treat as "not found"
        return 0;
    }
    return seh.result;
}

int TsDevLib::createEntry(ComponentType type, const std::string& name,
                          int& errCode) {
    errCode = 0;
    int progId = 0;
    __try {
        progId = m_pCreateEntry(getCategory(type), getSubType(type),
                                name.c_str(), 0, 0, &errCode);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // CreateEntryEX can raise SEH on certain malformed states; treat as
        // a hard failure (ProgId 0).
        return 0;
    }
    // TVS-L.10: createEntry no longer records the created entry in a
    // process-global (g_lastCreated is removed). The SINGLE create-path
    // cleanup owner is doInsert in the CLI, which already has the (type,
    // name, progId) triple in local scope and calls
    // requiresCreatePathCleanup() + deleteEntry(flush=false) on every
    // post-createEntry failure. This eliminates the shared mutable state
    // that previously coupled createEntry, verifyModule, and modifyEntry.
    return progId;
}

bool TsDevLib::saveSource(ComponentType type, int progId,
                          const std::string& source) {
    m_lastSource = source;
    setSourceText(m_editorHwnd, source);
    int result = 0;
    __try {
        result = m_pSaveModuleNew(getCategory(type), progId,
                                  NULL, m_editorHwnd, NULL);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return (result == 1);
}

std::string TsDevLib::getSource(ComponentType type, const std::string& name) {
    // 1. Find the progId via findExistPos (SEH-protected internally).
    int progId = findExistPos(name, type);
    if (progId <= 0) return "";  // not found

    // 1b. Check the internal-encryption flag BEFORE calling _LoadTextFileEX.
    //     _GetMasterRecByType@16 retrieves the 266-byte (0x10A) master record
    //     for this item. Bit 10 (mask 0x400) of the USHORT at byte offset
    //     0x7B indicates "internally encrypted on disk" (this is NOT a
    //     password-protection feature). When this bit is set, _LoadTextFileEX
    //     requires a non-NULL password buffer to trigger the internal
    //     decryption path (Branch C in FUN_101a0f90). TSDev passes a zeroed
    //     64-byte buffer (FUN_10108630), and any non-NULL pointer works.
    //     If _GetMasterRecByType fails (SEH exception or returns 0), we
    //     gracefully fall back to _LoadTextFileEX with NULL (the plain-text
    //     path) as before.
    unsigned char masterRec[0x10A] = {};
    GetMasterRecByTypeSehResult mrSeh;
    if (callGetMasterRecByType(m_getMasterRecByType, getCategory(type),
                               getSubType(type), progId, masterRec,
                               &mrSeh)) {
        if (mrSeh.result) {
            unsigned short flags = *(unsigned short*)(masterRec + 0x7B);
            if (flags & 0x400) {
                // Source is internally encrypted on disk. Passing a non-NULL
                // password buffer triggers Branch C in FUN_101a0f90, which
                // extracts the embedded password from the data header and
                // decrypts the source body. TSDev does the same (FUN_10108630
                // uses a zeroed 64-byte buffer). The buffer content doesn't
                // matter — any non-NULL pointer triggers the decrypt path.
                char decryptBuffer[64] = {};
                LoadTextFileExSehResult decryptSeh;
                if (!callLoadTextFileEx(m_loadTextFileEx, nullptr,
                                        getCategory(type), progId,
                                        decryptBuffer, &decryptSeh)) {
                    return "";  // SEH exception in orlib20
                }
                HGLOBAL decryptHMem = decryptSeh.result;
                if (decryptHMem == NULL) return "";  // failure (encrypted or error)

                char* decryptText = (char*)GlobalLock(decryptHMem);
                std::string decryptResult;
                if (decryptText) {
                    decryptResult = std::string(decryptText);
                    GlobalUnlock(decryptHMem);
                }
                GlobalFree(decryptHMem);
                return decryptResult;
            }
        }
    }

    // 2. Call _LoadTextFileEX@16 with NULL password (plain text items),
    //    SEH-protected via the POD-only callLoadTextFileEx helper. getSource
    //    itself holds std::string objects (with destructors), so __try cannot
    //    appear directly in this function (MSVC C2712); the helper contains
    //    only POD types and owns the __try/__except block.
    LoadTextFileExSehResult seh;
    if (!callLoadTextFileEx(m_loadTextFileEx, nullptr, getCategory(type),
                            progId, nullptr, &seh)) {
        return "";  // SEH exception in orlib20
    }
    HGLOBAL hMem = seh.result;
    if (hMem == NULL) return "";  // failure (not found or error)

    // 3. Lock, copy, free. GlobalLock returns a pointer to the ANSI source
    //    text; GlobalFree releases the HGLOBAL allocated by orlib20.
    char* text = (char*)GlobalLock(hMem);
    std::string result;
    if (text) {
        result = std::string(text);
        GlobalUnlock(hMem);
    }
    GlobalFree(hMem);
    return result;
}

EnumerateResult TsDevLib::enumerateRecords(ComponentType type,
                                           const std::string& searchName) {
    EnumerateResult result;

    int category = getCategory(type);
    int subType = getSubType(type);

    // --- Phase 1: Try _LoadMasterRecsEX@16 (COM interface cache) ---
    // This works for records that existed before this process started.
    // Newly created records may not be in the COM interface cache.
    //
    // We try twice: first WITHOUT refreshCache() (the COM interface
    // initialized by OrlibInit already has pre-existing records), then
    // WITH refreshCache() (which dispatches cmd 0x0F + cmd 0x4E to get a
    // fresh server-side interface that reflects in-session creates).
    if (m_loadMasterRecsEx) {
        for (int pass = 0; pass < 2 && !result.exists; pass++) {
            if (pass == 1) {
                refreshCache();
            }

            HGLOBAL buf = NULL;
            LoadMasterRecsExSehResult seh;
            if (callLoadMasterRecsEx(m_loadMasterRecsEx, category, &buf,
                                      (const char*)NULL, 1, &seh)) {
                int count = seh.result;

                if (count > 0 && buf != NULL) {
                    char* records = (char*)GlobalLock(buf);
                    if (records) {
                        const int kRecordSize   = 0x10A;   // 266 bytes per master record
                        const int kNameOffset   = 0x17;    // inline long name
                        const int kProgIdOffset = 0x00;    // short ProgId
                        const int kSubTypeOffset = 0xE9;   // category/subtype byte

                        for (int i = 0; i < count; i++) {
                            char* rec = records + i * kRecordSize;
                            const char* storedName = rec + kNameOffset;

                            // For category 2 (Indicator), filter by subtype so
                            // TradingApp queries only match TradingApp records,
                            // Indicator queries only match Indicator records, and
                            // ShowMe queries only match ShowMe records. Indicator
                            // now properly excludes ALL non-Indicator category-2
                            // subtypes (PaintBar, ShowMe, Expert, etc.) by
                            // accepting only raw subtypes 0x00 and 0x01.
                            // ShowMe records use raw subtype 0x03 or 0x05 (both
                            // map to power type 5 / expanded whType 0x0502).
                            if (category == 2) {
                                unsigned char recSubType =
                                    (unsigned char)rec[kSubTypeOffset];
                                if (subType == 0x14 && recSubType != 0x14) continue;
                                if (subType == 0x00 && recSubType != 0x00 && recSubType != 0x01) continue;
                                if (subType == 0x05 && recSubType != 0x05 && recSubType != 0x03) continue;
                            }

                            if (_stricmp(storedName, searchName.c_str()) == 0) {
                                result.exists = true;
                                result.storedName = std::string(storedName);
                                result.progId = *(short*)(rec + kProgIdOffset);
                                break;
                            }
                        }
                        GlobalUnlock(buf);
                    }
                }
                if (buf) GlobalFree(buf);
            }
        }
    }

    if (result.exists) {
        return result;
    }

    // --- Phase 2: Direct BST search + verbatim name getter ---
    // Phase 1 uses _LoadMasterRecsEX@16 which reads from the COM server's
    // enumeration cache. This cache is populated at warehouse initialization
    // and is NOT updated when new records are created in-session.
    //
    // FUN_101a42d0 returns the ProgId (not the name) in non-server mode
    // (DAT_1023aa8f == 0, which is our case), so we call FUN_101d05e0
    // (BST search) + FUN_101c36d0 (name getter) directly. This is the same
    // path FUN_101a42d0 takes in server mode, but we bypass the
    // server/non-server branch since FUN_101c36d0 works in both modes.
    // The stored name comes from the WAREHOUSE, not from source text
    // (HARD RULE 0).
    if (m_pBstSearchWrapper && m_pVerbatimNameGetter &&
        m_pBuildTypeArray && m_pGetWarehouse) {
        void* warehouse = nullptr;
        if (callGetWarehouse(
                reinterpret_cast<GetWarehouseFn>(m_pGetWarehouse),
                0, &warehouse) && warehouse) {

            // Build the type filter array via FUN_101d43a0 (same as
            // FUN_101a42d0 does internally). FUN_101d43a0 is __stdcall:
            // arg1=category, arg2=*outBuf, RET 0x8. Returns outBuf pointer.
            int typeFilterBuf[16] = {};
            if (callBuildTypeArray(m_pBuildTypeArray, category, typeFilterBuf)) {

                // Call FUN_101d05e0 (BST search wrapper).
                // __thiscall: ECX=warehouse, stack=(typeFilter, name, *outNode),
                // RET 0xC. Returns bool (1=found, 0=not found). Stores BST node
                // pointer in *outNode.
                void* nodePtr = nullptr;
                int bstResult = 0;
                if (callBstSearchWrapper(
                        m_pBstSearchWrapper, warehouse, typeFilterBuf,
                        searchName.c_str(), &nodePtr, &bstResult) &&
                    bstResult != 0 && nodePtr) {

                    // Call FUN_101c36d0 (verbatim name getter).
                    // __fastcall: ECX=node pointer, no stack args, RET.
                    // Returns char* to the verbatim stored name at node+0x1D.
                    const char* verbatimName = nullptr;
                    if (callVerbatimNameGetter(
                            m_pVerbatimNameGetter, nodePtr, &verbatimName) &&
                        verbatimName) {
                        result.exists = true;
                        result.storedName = std::string(verbatimName);
                        // Get ProgId separately via FindExistPos (BST).
                        result.progId = findExistPos(searchName, type);
                        if (result.progId <= 0) {
                            result.progId = 0;  // shouldn't happen, but be safe
                        }
                    }
                }
            }
        }
    }

    return result;
}

std::string TsDevLib::verifyModule(ComponentType type, int progId,
                                    CreatePathFailureStage* outStage) {
    // Allocate and zero the CELCompiler object (88 bytes).
    void* compilerMem = std::malloc(kCELCompilerSize);
    if (!compilerMem) {
        if (outStage) *outStage = CreatePathFailureStage::VerifyModule_Oom;
        return "tsdev: out of memory allocating CELCompiler object";
    }
    std::memset(compilerMem, 0, kCELCompilerSize);

    std::string errors;

    // Construct the CELCompiler object (fastcall: ecx = this).
    if (!callCtor(reinterpret_cast<CELCompilerCtorFn>(m_pCELCompilerCtor),
                  compilerMem)) {
        std::free(compilerMem);
        if (outStage) *outStage = CreatePathFailureStage::VerifyModule_SehCtor;
        return "tsdev: CELCompiler constructor raised an exception";
    }

    // Set the fields TSDev sets after construction (§8). The +0x18 field
    // must be the SAME EDIT HWND used by SaveModule_New (m_editorHwnd).
    *(reinterpret_cast<int*>(static_cast<char*>(compilerMem) + 0x10)) = 0xa02;
    *(reinterpret_cast<HWND*>(static_cast<char*>(compilerMem) + 0x18)) =
        m_editorHwnd;
    *(reinterpret_cast<int*>(static_cast<char*>(compilerMem) + 0x28)) = 6;

    // StartCompilingProcess (cdecl, no args).
    if (!callStartCompiling(
            reinterpret_cast<StartCompilingFn>(m_pStartCompiling))) {
        callDtor(reinterpret_cast<CELCompilerDtorFn>(m_pCELCompilerDtor),
                 compilerMem);
        std::free(compilerMem);
        if (outStage) *outStage = CreatePathFailureStage::VerifyModule_SehStartCompiling;
        return "tsdev: StartCompilingProcess raised an exception";
    }

    // VerifyModule_New, SEH-guarded. Error text is read directly from the
    // global error array after this call (see readErrorsFromArray).
    VerifySehResult seh;
    if (!callVerify(
            reinterpret_cast<VerifyModuleFn>(m_pVerifyModule),
            m_editorHwnd, getCategory(type), progId, compilerMem, &seh)) {
        callEndCompiling(reinterpret_cast<EndCompilingFn>(m_pEndCompiling), 0);
        callDtor(reinterpret_cast<CELCompilerDtorFn>(m_pCELCompilerDtor),
                 compilerMem);
        std::free(compilerMem);
        if (outStage) *outStage = CreatePathFailureStage::VerifyModule_SehVerify;
        return "tsdev: VerifyModule_New raised an exception";
    }

    // Build the error string from the global error array (direct access).
    if (seh.verifyResult != 1) {
        // Compile failure — read error records directly from orlib20.dll globals.
        errors = readErrorsFromArray(&m_lastSource);
        if (errors.empty()) {
            // No error text in the array (or array empty). Fall back to synthesized message.
            errors = "tsdev: VerifyModule_New returned "
                     + std::to_string(seh.verifyResult)
                     + " with no error text";
        }
        if (outStage) *outStage = CreatePathFailureStage::VerifyModule_CompileErrors;
    }

    // EndCompilingProcess (fastcall, 1 arg = 0).
    callEndCompiling(reinterpret_cast<EndCompilingFn>(m_pEndCompiling), 0);

    // Destroy the CELCompiler object.
    callDtor(reinterpret_cast<CELCompilerDtorFn>(m_pCELCompilerDtor),
             compilerMem);
    std::free(compilerMem);

    // TVS-L.10: create-path partial-entry cleanup is NO LONGER owned here.
    // The previous TD-001 branch keyed cleanup off the process-global
    // g_lastCreated (set by createEntry) and only fired when !errors.empty()
    // — i.e. it covered compile errors but only INCIDENTALLY covered the
    // OOM / SEH-ctor / SEH-StartCompiling / SEH-Verify early returns (they
    // happen to return a non-empty string), and it did NOT cover the
    // saveSource failure at all (that exit happens before verifyModule is
    // reached). The SINGLE authoritative owner is now doInsert in the CLI,
    // which calls requiresCreatePathCleanup(progId, stage) on EVERY
    // post-createEntry failure (saveSource fail + every verifyModule
    // failure mode) and routes them through one deleteEntry(flush=false)
    // call. The stage is reported back via `outStage` so doInsert can
    // record WHICH path failed. On the success (empty errors) return,
    // outStage is left untouched (the caller initializes it to a sentinel).
    return errors;
}

void TsDevLib::refreshCache() {
    if (!m_pGetWarehouse || !m_pCmdDispatcher) {
        return;
    }

    // TSDev-style cache refresh, emulating FUN_101c09f0 / FUN_101c51b0:
    //
    // Step 1: Get the warehouse. If [warehouse+0x5E] (IWHouse interface)
    //         is null, dispatch cmd 0x0F to initialize it.
    // Step 2: Set [warehouse+0x12] = 0xC and dispatch cmd 0x4E
    //         (FUN_101cf5b0) to notify the worker thread.
    //
    // Note: This refreshes the IWHouse interface at [warehouse+0x5E] but
    // does NOT refresh the COM server enumeration cache at [warehouse+0xAE]
    // used by _LoadMasterRecsEX@16. Newly created records are visible to
    // FindExistPos (which uses the in-memory BST) but not to
    // _LoadMasterRecsEX@16's get-all-records method. The two-phase
    // enumerateRecords() handles this by falling back to FindExistPos +
    // _GetMasterRecByType@16 in Phase 2 (reading the stored name from the
    // warehouse record, not from source text).

    // Step 1: Get the warehouse.
    void* warehouse = nullptr;
    if (!callGetWarehouse(
            reinterpret_cast<GetWarehouseFn>(m_pGetWarehouse), 0, &warehouse)) {
        return;
    }
    if (!warehouse) {
        return;
    }

    char* wh = reinterpret_cast<char*>(warehouse);
    void* iface5E = *reinterpret_cast<void**>(wh + 0x5E);

    // Step 1a: If [warehouse+0x5E] is null, dispatch cmd 0x0F to initialize.
    if (!iface5E && m_pCmd0FDispatcher && m_pBuildTypeArray &&
        m_pCelProgWriterCtor) {
        int typeArray[16] = {};
        if (callBuildTypeArray(m_pBuildTypeArray, 0, typeArray)) {
            // CELProgramWriter buffer (0x280 bytes, zero-initialized).
            unsigned char writerBuf[0x280] = {};
            callCelProgramWriterCtor(m_pCelProgWriterCtor, writerBuf);
            callCmd0FDispatcher(m_pCmd0FDispatcher, warehouse,
                                typeArray, 0, writerBuf);
            // Re-read [warehouse+0x5E] after cmd 0x0F.
            iface5E = *reinterpret_cast<void**>(wh + 0x5E);
        }
    }

    // If [warehouse+0x5E] is still NULL after the cmd 0x0F re-initialization
    // attempt, we cannot safely dispatch cmd 0x4E (it dereferences the
    // IWHouse interface at [warehouse+0x5E], causing ACCESS_VIOLATION on
    // NULL). This happens in serve mode after a delete operation corrupts
    // the warehouse interface. Skip the refresh — the caller
    // (enumerateRecords) will fall back to Phase 2 (findExistPos +
    // _GetMasterRecByType@16) which does not need the IWHouse interface.
    if (!iface5E) {
        TSDEV_DBG("tsdev: refreshCache — [warehouse+0x5E] is NULL after cmd 0x0F, "
                  "skipping cmd 0x4E (interface cannot be re-initialized)\n");
        return;
    }

    // Step 2: Set [warehouse+0x12] = 0xC and dispatch cmd 0x4E.
    // (The previous Step 2 — dispatching cmd 0x0D via FUN_101cfce0 to
    // obtain a fresh COM interface — has been removed. In our CLI context
    // the COM server cannot provide a fresh interface, so cmd 0x0D
    // returns null and clears [warehouse+0x5E], making it useless.
    // The new Phase 2 of enumerateRecords uses _GetMasterRecByType@16
    // instead, which dispatches cmd 0x0D via a DIFFERENT path
    // (FUN_101d15c0) and returns a single warehouse record by ProgId.)
    *reinterpret_cast<int*>(wh + 0x12) = 0xC;
    callCmdDispatcher(m_pCmdDispatcher, warehouse, 0x4E);
}

void TsDevLib::closeWarehouse() {
    __try {
        m_pCloseWarehouse(0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Best-effort flush; log SEH but do not propagate (destructor path).
        std::fprintf(stderr,
            "tsdev: WARNING — closeWarehouse raised SEH exception\n");
    }
}

bool TsDevLib::ellFileExists(ComponentType type, int progId) {
    std::string path = getEllPath(type, progId);
    DWORD attr = GetFileAttributesA(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES);
}

bool TsDevLib::xmlFileExists(ComponentType type, int progId) {
    std::string path = getXmlPath(type, progId);
    DWORD attr = GetFileAttributesA(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES);
}

std::string TsDevLib::getEllPath(ComponentType type, int progId) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s\\elmodule_%d_%d.ell",
                  m_myWorkBin.c_str(), static_cast<int>(type), progId);
    return std::string(buf);
}

std::string TsDevLib::getXmlPath(ComponentType type, int progId) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s\\elmodule_%d_%d.xml",
                  m_myWorkBin.c_str(), static_cast<int>(type), progId);
    return std::string(buf);
}

const char* TsDevLib::getMyWorkBinPath() const {
    return m_myWorkBin.c_str();
}

const char* TsDevLib::getWrkPath() const {
    return m_wrkPath.c_str();
}

bool TsDevLib::deleteEntry(ComponentType type, const std::string& name,
                           bool flush, std::string* diskCleanupErrorOut) {
    int category = getCategory(type);
    int expandedWhType = static_cast<int>(type);

    // ---- MWRKArea.wrk timestamp (before) ----
    std::string wrkPath = m_wrkPath;
    WIN32_FILE_ATTRIBUTE_DATA wrkBefore;
    BOOL wrkBeforeOk = GetFileAttributesExA(wrkPath.c_str(), GetFileExInfoStandard, &wrkBefore);
    TSDEV_DBG("tsdev: deleteEntry — MWRKArea.wrk BEFORE: ok=%d size=%lu mtime=%lu\n",
              (int)wrkBeforeOk,
              wrkBeforeOk ? wrkBefore.nFileSizeLow : 0,
              wrkBeforeOk ? wrkBefore.ftLastWriteTime.dwLowDateTime : 0);

    // 1. FindExistPos to get progId (exported, works in our process).
    //    Routed through the SEH-protected findExistPos() wrapper (AC3):
    //    direct m_pFindExistPos calls can raise SEH and must not be bypassed.
    int progId = findExistPos(name, type);
    TSDEV_DBG("tsdev: deleteEntry — FindExistPos(\"%s\", %d) = %d\n",
              name.c_str(), category, progId);
    if (progId <= 0) {
        return false;
    }

    // ------------------------------------------------------------------
    // 2. Call FUN_101a8360 — the TSDev UI delete orchestrator.
    //    __fastcall: ECX=whType, EDX=progId, no stack args.
    //
    //    This function replicates the exact UI delete path:
    //      a. GetWarehouse + PrepareWhType + FUN_101d0950 (command 0x0F)
    //         to load the program from the warehouse.
    //      b. FUN_101c26d0 to set up the program context.
    //      c. Loop through items: FUN_101c4990 (command 0x32 = get progId),
    //         FUN_101c2e90 (command 0x33 = get name), FUN_101a4f90.
    //      d. FUN_101a84e0(whType, progId, 1) — the actual delete:
    //         - FUN_101d0950 (command 0x0F) to load the program.
    //         - FUN_101c8110 to mark the program as deleted (flag bit 0 = 0).
    //         - FUN_101c28d0 for recursive child traversal.
    //         - For each child: FUN_101c8110 + CELProgramWriter cleanup.
    //         - CELProgramWriter cleanup on the main program.
    //      e. FUN_101c09f0(0xb) and FUN_101c09f0(0xc) for strategy types —
    //         sends command 0x4E to reset caches.
    //      f. FUN_101c2e90 + FUN_101c4990 to get current state.
    //      g. FUN_101d1fa0 — sends command 0x17 (SAVE/COMMIT).
    //      h. CELProgramWriter final cleanup.
    //
    //    The thread-check INT3 instructions in FUN_101d0950, FUN_101cf5b0,
    //    and FUN_101d1fa0 have been patched to NOPs (see constructor step 4b),
    //    so these functions can be called from our thread.
    // ------------------------------------------------------------------
    void* deleteOrchestrator = m_pDeleteOrchestrator;

    TSDEV_DBG("tsdev: deleteEntry — calling FUN_101a8360"
              " (deleteOrchestrator=%p, whType=%d, progId=%d)\n",
              deleteOrchestrator, category, progId);

    bool ok = callDeleteOrchestrator(deleteOrchestrator, category, progId);

    TSDEV_DBG("tsdev: deleteEntry — FUN_101a8360 returned %s\n",
              ok ? "true" : "false (SEH exception)");

    if (!ok) {
        TSDEV_DBG("tsdev: deleteEntry — delete orchestrator"
                  " raised SEH exception, aborting\n");
        return false;
    }

    // ---- MWRKArea.wrk timestamp (after orchestrator) ----
    WIN32_FILE_ATTRIBUTE_DATA wrkAfter;
    BOOL wrkAfterOk = GetFileAttributesExA(wrkPath.c_str(), GetFileExInfoStandard, &wrkAfter);
    TSDEV_DBG("tsdev: deleteEntry — MWRKArea.wrk AFTER orchestrator: ok=%d size=%lu mtime=%lu\n",
              (int)wrkAfterOk,
              wrkAfterOk ? wrkAfter.nFileSizeLow : 0,
              wrkAfterOk ? wrkAfter.ftLastWriteTime.dwLowDateTime : 0);
    if (wrkBeforeOk && wrkAfterOk) {
        if (wrkAfter.ftLastWriteTime.dwLowDateTime != wrkBefore.ftLastWriteTime.dwLowDateTime ||
            wrkAfter.ftLastWriteTime.dwHighDateTime != wrkBefore.ftLastWriteTime.dwHighDateTime) {
            TSDEV_DBG("tsdev: deleteEntry — *** MWRKArea.wrk TIMESTAMP CHANGED ***\n");
        } else {
            TSDEV_DBG("tsdev: deleteEntry — WARNING: MWRKArea.wrk timestamp UNCHANGED\n");
        }
    }

    // ------------------------------------------------------------------
    // 3. InitGlobalLookup → DeleteFromTree (update in-memory BST).
    //    These run AFTER the orchestrator so the warehouse save (command
    //    0x17) happens first. Both calls are SEH-protected via the
    //    existing callInitGlobalLookup and callDeleteFromTree wrappers.
    // ------------------------------------------------------------------
    void* globalLookup = nullptr;
    if (!callInitGlobalLookup(
            reinterpret_cast<InitGlobalLookupFn>(m_pInitGlobalLookup),
            &globalLookup)) {
        TSDEV_DBG("tsdev: deleteEntry — InitGlobalLookup raised SEH exception\n");
    } else if (!globalLookup) {
        TSDEV_DBG("tsdev: deleteEntry — InitGlobalLookup returned NULL\n");
    } else {
        TSDEV_DBG("tsdev: deleteEntry — globalLookup=%p\n", globalLookup);

        TSDEV_DBG("tsdev: deleteEntry — calling DeleteFromTree(%d, %d, 3)\n", category, progId);
        if (!callDeleteFromTree(m_pDeleteFromTree, globalLookup,
                                category, progId, 3)) {
            TSDEV_DBG("tsdev: deleteEntry — DeleteFromTree raised SEH exception\n");
        } else {
            TSDEV_DBG("tsdev: deleteEntry — DeleteFromTree done\n");
        }
    }

    // 4. CloseWarehouse (best-effort flush).
    //    Skipped when flush=false (serve/batch mode) because closing the
    //    warehouse frees the IWHouse interface ([warehouse+0x5E]), which
    //    causes subsequent enumerateRecords/refreshCache calls to crash with
    //    NULL pointer dereference in orlib20's cmd 0x4E handler. In serve/batch
    //    mode, the warehouse stays open for the session lifetime and is closed
    //    once at the end by runServe/runBatch.
    if (flush) {
        TSDEV_DBG("tsdev: deleteEntry — calling closeWarehouse()\n");
        closeWarehouse();
        TSDEV_DBG("tsdev: deleteEntry — closeWarehouse() done\n");
    } else {
        TSDEV_DBG("tsdev: deleteEntry — skipping closeWarehouse (flush=false)\n");
    }

    // 5. Delete the .ell and .xml files from disk.
    //
    // TVS-L.12: each DeleteFileA is now classified by the SINGLE decision
    // deleteDiskCleanupErrorFailsResult(). ERROR_SUCCESS and
    // ERROR_FILE_NOT_FOUND are benign (artifact gone / already absent —
    // idempotent). Any OTHER nonzero error (e.g. ERROR_ACCESS_DENIED: the
    // artifact is present but could not be removed) is a REAL cleanup failure:
    // the orchestrator has ALREADY removed the warehouse entry, but a stale
    // .ell/.xml is left on disk. In that case deleteEntry now returns false
    // (instead of unconditionally returning true) so the caller does not treat
    // a stale-artifact state as success. The residual path + Windows error are
    // surfaced via `diskCleanupErrorOut` (when provided) and via the existing
    // stderr WARNING (the "logged message" already carries path + error).
    char ellPath[512];
    char xmlPath[512];
    std::snprintf(ellPath, sizeof(ellPath), "%s\\elmodule_%d_%d.ell",
                  m_myWorkBin.c_str(), expandedWhType, progId);
    std::snprintf(xmlPath, sizeof(xmlPath), "%s\\elmodule_%d_%d.xml",
                  m_myWorkBin.c_str(), expandedWhType, progId);
    TSDEV_DBG("tsdev: deleteEntry — deleting %s and %s\n", ellPath, xmlPath);

    bool cleanupFailed = false;
    if (diskCleanupErrorOut != nullptr) diskCleanupErrorOut->clear();

    if (!DeleteFileA(ellPath)) {
        DWORD err = GetLastError();
        if (tsdev::deleteDiskCleanupErrorFailsResult(
                err, tsdev::DeleteDiskArtifactKind::Ell)) {
            // Real removal failure (e.g. ERROR_ACCESS_DENIED): the artifact is
            // present but could not be removed. Surface it as a WARNING (the
            // "logged message" carries path + Windows error) and record it for
            // the caller's Result/CliResult message via diskCleanupErrorOut.
            cleanupFailed = true;
            std::fprintf(stderr,
                "tsdev: WARNING — failed to delete %s (error=%lu)\n",
                ellPath, err);
            if (diskCleanupErrorOut != nullptr) {
                if (!diskCleanupErrorOut->empty()) *diskCleanupErrorOut += "; ";
                *diskCleanupErrorOut += std::string("failed to delete ")
                    + ellPath + " (Windows error=" + std::to_string(err) + ")";
            }
        }
        // else: the decision classified this as benign (ERROR_FILE_NOT_FOUND —
        // artifact already absent, idempotent). No WARNING, no failure. This
        // preserves the ONE case today's code already handled correctly.
    }
    if (!DeleteFileA(xmlPath)) {
        DWORD err = GetLastError();
        if (tsdev::deleteDiskCleanupErrorFailsResult(
                err, tsdev::DeleteDiskArtifactKind::Xml)) {
            cleanupFailed = true;
            std::fprintf(stderr,
                "tsdev: WARNING — failed to delete %s (error=%lu)\n",
                xmlPath, err);
            if (diskCleanupErrorOut != nullptr) {
                if (!diskCleanupErrorOut->empty()) *diskCleanupErrorOut += "; ";
                *diskCleanupErrorOut += std::string("failed to delete ")
                    + xmlPath + " (Windows error=" + std::to_string(err) + ")";
            }
        }
    }

    if (cleanupFailed) {
        TSDEV_DBG("tsdev: deleteEntry — disk cleanup failed, returning false\n");
        return false;
    }

    TSDEV_DBG("tsdev: deleteEntry — returning true\n");
    return true;
}

// ============================================================================
// modifyEntry — replace an existing component's source and recompile/re-verify.
// Composes the existing INSERT primitives (findExistPos, saveSource,
// verifyModule). No new orlib20 calls or RVAs.
// ============================================================================
Result TsDevLib::modifyEntry(ComponentType type, const std::string& name,
                             const std::string& newSource) {
    // TVS-L.10: the g_lastCreated reset that lived here is gone with the
    // global itself. modifyEntry operates on an EXISTING entry (no
    // createEntry), so it must never trigger create-path cleanup. It calls
    // verifyModule with outStage=nullptr, so no stage is recorded and the
    // create-path cleanup owner (doInsert) is never involved.
    // Structural guarantee (Ghidra-verified, see
    // docs/RE/RE_VERIFYMODULE_FINALIZATION_TRUNCATION.md):
    // On ANY modify failure (compile or post-compile finalization),
    // the .ell file is NEVER truncated or replaced. This is because
    // MDVault::SaveModule (which writes .ell) is only called when
    // FUN_10011230 returns success. On failure, only SymWriter::Save
    // (which writes .xml debugger data) may run unconditionally.
    // The .xml inconsistency is a debugger/source-map issue, not a
    // runtime linkability risk. This matches TSDev's behavior (no
    // rollback for .xml). No backup/restore is needed.
    // 1. Look up the existing component by name + type.
    int progId = findExistPos(name, type);
    TSDEV_DBG("tsdev: modifyEntry — findExistPos(\"%s\", %d) = %d\n",
              name.c_str(), getCategory(type), progId);
    if (progId <= 0) {
        return Result{false, "component not found", 0};
    }

    // 2. Replace the source in the existing entry (ProgId does NOT change).
    bool saved = saveSource(type, progId, newSource);
    TSDEV_DBG("tsdev: modifyEntry — saveSource = %s\n",
              saved ? "true" : "false");
    if (!saved) {
        return Result{false, "saveSource failed", progId};
    }

    // 3. Recompile + reverify + persist .ell via the existing flow.
    std::string errors = verifyModule(type, progId);
    if (!errors.empty()) {
        // Compilation failed — fail loudly with the error string.
        TSDEV_DBG("tsdev: modifyEntry — verifyModule errors:\n%s\n",
                  errors.c_str());
        return Result{false, errors, progId};
    }

    // 4. Success — component modified, recompiled, and persisted.
    TSDEV_DBG("tsdev: modifyEntry — success (progId=%d)\n", progId);
    return Result{true, "", progId};
}

// ============================================================================
// Helpers
// ============================================================================

HWND TsDevLib::createHiddenEditControl() {
    // A message-only parent keeps the EDIT off the desktop; the EDIT itself
    // is a child and not WS_VISIBLE.
    HWND hParent = CreateWindowW(L"STATIC", L"", WS_POPUP,
                                 0, 0, 100, 100, HWND_MESSAGE,
                                 nullptr, nullptr, nullptr);
    HWND hEdit = CreateWindowW(L"EDIT", L"",
                               WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL,
                               0, 0, 100, 100, hParent,
                               nullptr, nullptr, nullptr);
    if (!hEdit) {
        // Retry without a parent (matches the probe's fallback).
        hEdit = CreateWindowW(L"EDIT", L"",
                              ES_MULTILINE | ES_AUTOVSCROLL,
                              0, 0, 100, 100, nullptr,
                              nullptr, nullptr, nullptr);
    }
    // Store the parent so the destructor can destroy it AFTER the EDIT
    // child. If the fallback created an orphan EDIT (no parent), hParent
    // may still be a valid window — keep it so it gets cleaned up.
    m_editorParent = hParent;
    return hEdit;
}

void TsDevLib::setSourceText(HWND hwnd, const std::string& source) {
    std::wstring wide = toWide(source);
    SetWindowTextW(hwnd, wide.c_str());
}

void TsDevLib::applyThreadCheckPatches() {
    // Delegates to the resolver's applyPatch for each thread-check site.
    // The resolver handles: resolvePatchSite → validatePatchSite (throw
    // on mismatch) → idempotency check → VirtualProtect → memcpy → restore.
    // OrlibResolverError propagates to the constructor (AC6 diagnostics).
    //
    // For each site we FIRST resolve the patch-site descriptor (which scans
    // the ORIGINAL 0xCC byte) to cache the RVA and save the original byte,
    // THEN applyPatch overwrites it with 0x90. The destructor writes the
    // saved original bytes back so a second TsDevLib instance can re-scan.
    PatchSiteDescriptor tc1 = m_resolver->resolveThreadCheck1();
    m_threadCheckRvas[0] = tc1.patchRva;
    m_threadCheckOriginalBytes[0] = *reinterpret_cast<const unsigned char*>(
        m_resolver->baseAddress() + m_threadCheckRvas[0]);
    m_resolver->applyPatch("FUN_101d0950_thread_check_INT3");
    m_patchesApplied = true;
    // Cache FUN_101d0950's entry point (base + sigMatchRva) for use by
    // refreshCache(). sigMatchRva is the containing function entry RVA.
    m_pCmd0FDispatcher = reinterpret_cast<void*>(
        m_resolver->baseAddress() + tc1.sigMatchRva);

    PatchSiteDescriptor tc2 = m_resolver->resolveThreadCheck2();
    m_threadCheckRvas[1] = tc2.patchRva;
    m_threadCheckOriginalBytes[1] = *reinterpret_cast<const unsigned char*>(
        m_resolver->baseAddress() + m_threadCheckRvas[1]);
    m_resolver->applyPatch("FUN_101cf5b0_thread_check_INT3");
    m_patchesApplied = true;
    // Cache FUN_101cf5b0's entry point (base + sigMatchRva) for use by
    // refreshCache(). sigMatchRva is the containing function entry RVA.
    m_pCmdDispatcher = reinterpret_cast<void*>(
        m_resolver->baseAddress() + tc2.sigMatchRva);

    PatchSiteDescriptor tc3 = m_resolver->resolveThreadCheck3();
    m_threadCheckRvas[2] = tc3.patchRva;
    m_threadCheckOriginalBytes[2] = *reinterpret_cast<const unsigned char*>(
        m_resolver->baseAddress() + m_threadCheckRvas[2]);
    m_resolver->applyPatch("FUN_101d1fa0_thread_check_INT3");
    m_patchesApplied = true;

    // Thread-check 4: command 0x11 (source-text load, reached from
    // _LoadTextFileEX@16). No dedicated resolveThreadCheck4() convenience
    // method exists yet, so resolve the patch site directly via the public
    // resolvePatchSite() API (same underlying mechanism as the wrappers above).
    PatchSiteDescriptor tc4 = m_resolver->resolvePatchSite("FUN_101d0e80_thread_check_INT3");
    m_threadCheckRvas[3] = tc4.patchRva;
    m_threadCheckOriginalBytes[3] = *reinterpret_cast<const unsigned char*>(
        m_resolver->baseAddress() + m_threadCheckRvas[3]);
    m_resolver->applyPatch("FUN_101d0e80_thread_check_INT3");
    m_patchesApplied = true;

    // Thread-check 5: command 0x08 (type select, used by _LoadMasterRecsEX@16).
    PatchSiteDescriptor tc5 = m_resolver->resolvePatchSite("FUN_101d0c30_thread_check_INT3");
    m_threadCheckRvas[4] = tc5.patchRva;
    m_threadCheckOriginalBytes[4] = *reinterpret_cast<const unsigned char*>(
        m_resolver->baseAddress() + m_threadCheckRvas[4]);
    m_resolver->applyPatch("FUN_101d0c30_thread_check_INT3");
    m_patchesApplied = true;

    // Thread-check 6: command 0x09 (item retrieval, conditionally used by
    // _LoadMasterRecsEX@16 in headless/local mode). Applied defensively.
    PatchSiteDescriptor tc6 = m_resolver->resolvePatchSite("FUN_101d16c0_thread_check_INT3");
    m_threadCheckRvas[5] = tc6.patchRva;
    m_threadCheckOriginalBytes[5] = *reinterpret_cast<const unsigned char*>(
        m_resolver->baseAddress() + m_threadCheckRvas[5]);
    m_resolver->applyPatch("FUN_101d16c0_thread_check_INT3");
    m_patchesApplied = true;

    // Thread-check 7: command 0x07 (count by types, used by _LoadMasterRecsEX@16
    // when filterMode != 0). enumerateRecords uses filterMode=1 to include all
    // records without the filterMode=0 flag filter that excludes newly created
    // entries.
    PatchSiteDescriptor tc7 = m_resolver->resolvePatchSite("FUN_101d0d90_thread_check_INT3");
    m_threadCheckRvas[6] = tc7.patchRva;
    m_threadCheckOriginalBytes[6] = *reinterpret_cast<const unsigned char*>(
        m_resolver->baseAddress() + m_threadCheckRvas[6]);
    m_resolver->applyPatch("FUN_101d0d90_thread_check_INT3");
    m_patchesApplied = true;

    // Thread-check 8: command 0x0D (COM interface get, used by refreshCache
    // to obtain the correct [warehouse+0x5E] interface that sees newly
    // created records). The UI flow (FUN_101a8360 → FUN_101c50b0) uses
    // cmd 0x0D to get this interface; cmd 0x0F gets a different (stale)
    // interface that does not reflect in-session creates.
    PatchSiteDescriptor tc8 = m_resolver->resolvePatchSite("FUN_101cfce0_thread_check_INT3");
    m_threadCheckRvas[7] = tc8.patchRva;
    m_threadCheckOriginalBytes[7] = *reinterpret_cast<const unsigned char*>(
        m_resolver->baseAddress() + m_threadCheckRvas[7]);
    m_resolver->applyPatch("FUN_101cfce0_thread_check_INT3");
    m_patchesApplied = true;
    // Cache FUN_101cfce0's entry point for use by refreshCache().
    m_pCmd0DDispatcher = reinterpret_cast<void*>(
        m_resolver->baseAddress() + tc8.sigMatchRva);

    // Thread-check 9: command 0x0D local-mode dispatcher (FUN_101d15c0),
    // used by _GetMasterRecByType@16 to get a single warehouse record by
    // ProgId. This is a DIFFERENT function from FUN_101cfce0 (thread-check
    // 8) — FUN_101d15c0 is the local-mode path inside FUN_101cfe00, while
    // FUN_101cfce0 is the COM interface resolver. Both dispatch cmd 0x0D
    // but through different code paths. The INT3 is at offset 84.
    PatchSiteDescriptor tc9 = m_resolver->resolvePatchSite("FUN_101d15c0_thread_check_INT3");
    m_threadCheckRvas[8] = tc9.patchRva;
    m_threadCheckOriginalBytes[8] = *reinterpret_cast<const unsigned char*>(
        m_resolver->baseAddress() + m_threadCheckRvas[8]);
    m_resolver->applyPatch("FUN_101d15c0_thread_check_INT3");
    m_patchesApplied = true;

    // Thread-check 10: command 0x10 BST search non-server mode dispatcher
    // (FUN_101d18c0), used by FUN_101d05e0 (non-server BST search) via
    // FUN_101e3db0 or directly. This is the non-server-mode equivalent of
    // FUN_101d15c0 (thread-check 9) but uses command byte 0x10 instead of
    // 0x0D. Required for FUN_101a42d0 (name getter) non-server path.
    // The INT3 is at offset 294 (0x126).
    PatchSiteDescriptor tc10 = m_resolver->resolvePatchSite("FUN_101d18c0_thread_check_INT3");
    m_threadCheckRvas[9] = tc10.patchRva;
    m_threadCheckOriginalBytes[9] = *reinterpret_cast<const unsigned char*>(
        m_resolver->baseAddress() + m_threadCheckRvas[9]);
    m_resolver->applyPatch("FUN_101d18c0_thread_check_INT3");
    m_patchesApplied = true;

    // Thread-check 11: verbatim name getter (FUN_101c36d0) thread-check INT3.
    // The INT3 fires when FUN_101cfce0 returns 0 (failed to get ProgId).
    // Required for the direct FUN_101d05e0 + FUN_101c36d0 call path used by
    // enumerateRecords() Phase 2. The INT3 is at offset 0x6A (106) from
    // function entry.
    PatchSiteDescriptor tc11 = m_resolver->resolvePatchSite("FUN_101c36d0_thread_check_INT3");
    m_threadCheckRvas[10] = tc11.patchRva;
    m_threadCheckOriginalBytes[10] = *reinterpret_cast<const unsigned char*>(
        m_resolver->baseAddress() + m_threadCheckRvas[10]);
    m_resolver->applyPatch("FUN_101c36d0_thread_check_INT3");
    m_patchesApplied = true;
}

// --- readErrorsFromArray (moved from anonymous namespace, TVS-B.5) --------
// Reads error text directly from orlib20.dll's global error array.
// Must be called AFTER VerifyModule_New returns and BEFORE EndCompilingProcess.
// The 3 data-global RVAs are resolved at construction time by the resolver
// and stored as member variables. The base address comes from the resolver.
std::string TsDevLib::readErrorsFromArray(const std::string* source) {
    uintptr_t base = m_resolver->baseAddress();

    // Read error count
    uint32_t count = *reinterpret_cast<uint32_t*>(base + m_errCountRva);
    if (count == 0 || count > 1000) return "";  // sanity bound

    // Read error array pointer
    uint8_t* array = *reinterpret_cast<uint8_t**>(base + m_errArrayPtrRva);
    if (array == nullptr) return "";

    std::string result;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t* record = array + static_cast<size_t>(i) * kErrRecordStride;
        const char* text = reinterpret_cast<const char*>(record + kErrTextOffset);
        // Compute line/column from character offset (if source available)
        int charOffset = *reinterpret_cast<int*>(record + kErrCharOffset);
        std::string location;
        if (source != nullptr && charOffset > 0 &&
            static_cast<size_t>(charOffset) <= source->length()) {
            int line = 1;
            int lineStart = 0;
            for (int j = 0; j < charOffset && j < static_cast<int>(source->length()); j++) {
                if ((*source)[j] == '\n') {
                    line++;
                    lineStart = j + 1;
                }
            }
            int col = charOffset - lineStart + 1;
            location = "Line " + std::to_string(line) + ", Col " + std::to_string(col) + ": ";
        }
        if (text[0] != '\0') {
            if (!result.empty()) result += "\n";
            result += location + text;
        } else {
            // Fallback: load from string table via LoadStringA
            short resId = *reinterpret_cast<short*>(record + kErrResIdOffset);
            if (resId != 0) {
                HINSTANCE hInst = *reinterpret_cast<HINSTANCE*>(base + m_errHinstRva);
                char buf[1024];
                int len = LoadStringA(hInst, resId, buf, sizeof(buf));
                if (len > 0) {
                    if (!result.empty()) result += "\n";
                    result += location + buf;
                }
            }
        }
    }
    return result;
}

} // namespace tsdev
