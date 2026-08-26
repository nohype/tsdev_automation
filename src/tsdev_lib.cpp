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

// CELCompiler object size — confirmed 0x58 (88 bytes) by Ghidra static
// analysis of the ordinal-66 alloc wrapper (FUN_10165f70 calls
// operator_new(0x58)). Workstream M slice M.1: no longer used for allocation
// (ordinal 66 allocates internally), retained as a documented constant for
// the confirmed object size. See tmp/m1_ghidra_findings.md §1.
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

// CELCompilerAllocFn and CELCompilerDestroyFn come from orlib_resolver.h
// (namespace tsdev). Workstream M slice M.1 replaced the old
// signature-scanned CELCompilerCtorFn / CELCompilerDtorFn (raw ctor/dtor)
// with ordinal-based alloc/destroy wrappers (orlib20 ordinals 66/67).
//   callAlloc: invokes ordinal 66 (__cdecl, no args, returns void*).
//   callDestroy: invokes ordinal 67 (__stdcall, one arg void* p).
typedef void  (__cdecl    *StartCompilingFn)(void);
typedef void  (__fastcall *EndCompilingFn)(int param);
typedef int   (__stdcall  *VerifyModuleFn)(HWND, int, int, int, void*);

struct VerifySehResult {
    int   verifyResult;    // -1 if VerifyModule_New raised
};

// Workstream M slice M.1: SEH-protected call to ordinal 66 (alloc+ctor).
// __cdecl, no args, returns void* (the constructed object, or NULL on alloc
// failure). On SEH exception returns false and sets *outPtr=nullptr. POD-only
// (no C++ objects with destructors) so __try is legal.
bool callAlloc(CELCompilerAllocFn fn, void** outPtr) {
    *outPtr = nullptr;
    __try {
        *outPtr = fn();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outPtr = nullptr;
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

// Workstream M slice M.1: SEH-protected call to ordinal 67 (dtor+free).
// __stdcall, one arg (void* p), returns void. The wrapper destructs AND frees
// the object (operator delete(p, 0x58)) — the caller must NOT std::free the
// pointer afterwards (that would double-free). POD-only (no C++ objects with
// destructors) so __try is legal.
bool callDestroy(CELCompilerDestroyFn fn, void* mem) {
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

// SEH-protected contract-change comparison (M.4).
// Replicates TSDev.exe FUN_004d9040: compares two warehouse master records
// field-by-field. Returns 1 = no change, 0 = changed.
// SEH-protected because record pointers come from orlib20 and may be invalid.
// POD-only (no C++ objects) for __try/__except legality.
int compareMasterRecsContractSeh(const char* oldRec, const char* newRec) {
    // Null check
    if (!oldRec || !newRec) return 0;

    int result = 0;  // default: changed
    __try {
        // 1. +0xd6: pointer equality (NOT dereferenced)
        if (*(int*)(oldRec + 0xd6) != *(int*)(newRec + 0xd6)) return 0;

        // 2. +0x15: low byte, mask 0x7E (bits 1-6)
        unsigned char old15 = *(unsigned char*)(oldRec + 0x15);
        unsigned char new15 = *(unsigned char*)(newRec + 0x15);
        if (((old15 ^ new15) & 0x7E) != 0) return 0;

        // 3. +0x7f: ushort, mask 0xB007 (bits 0,1,2,12,13,15)
        unsigned short old7f = *(unsigned short*)(oldRec + 0x7f);
        unsigned short new7f = *(unsigned short*)(newRec + 0x7f);
        if (((old7f ^ new7f) & 0xB007) != 0) return 0;

        // 4. +0x81: ushort, mask 0xF900 (bits 8,11,12,13,14,15)
        unsigned short old81 = *(unsigned short*)(oldRec + 0x81);
        unsigned short new81 = *(unsigned short*)(newRec + 0x81);
        if (((old81 ^ new81) & 0xF900) != 0) return 0;

        // 5. +0x29: int element count, full equality
        int count = *(int*)(oldRec + 0x29);
        if (count != *(int*)(newRec + 0x29)) return 0;

        // 6. +0x4f: dereference both, compare array contents at stride 0xb5
        int* oldArr = *(int**)(oldRec + 0x4f);
        int* newArr = *(int**)(newRec + 0x4f);
        if (!oldArr || !newArr) return 0;  // null array = changed

        for (int i = 0; i < count; i++) {
            const char* oldElem = (const char*)oldArr + (i * 0xb5);
            const char* newElem = (const char*)newArr + (i * 0xb5);
            // elem+0xd: int* pointer equality
            if (*(int*)(oldElem + 0xd) != *(int*)(newElem + 0xd)) return 0;
            // elem+0x11: int* pointer equality
            if (*(int*)(oldElem + 0x11) != *(int*)(newElem + 0x11)) return 0;
            // elem+0x15: int* pointer equality
            if (*(int*)(oldElem + 0x15) != *(int*)(newElem + 0x15)) return 0;
            // elem+0x83: uint, bit 2 only (mask 0x4)
            unsigned int old83 = *(unsigned int*)(oldElem + 0x83);
            unsigned int new83 = *(unsigned int*)(newElem + 0x83);
            if (((old83 ^ new83) & 4) != 0) return 0;
            // elem+0x87: short, full equality
            if (*(short*)(oldElem + 0x87) != *(short*)(newElem + 0x87)) return 0;
            // elem+0xb1: int* pointer equality
            if (*(int*)(oldElem + 0xb1) != *(int*)(newElem + 0xb1)) return 0;
        }

        result = 1;  // all fields match = no change
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;  // SEH = treat as changed
    }
    return result;
}

// SEH-protected call to GetDependencies (M.4). POD-only.
// File-scope typedef matching the TsDevLib private member typedef
// (structurally identical: HGLOBAL (__stdcall*)(int, int, int, int*)),
// so this free helper can take a typed function pointer without accessing
// the class's private members.
typedef HGLOBAL (__stdcall *GetDependenciesFn)(int whType, int progId,
                                                int elementIndex,
                                                int* outDepCount);
struct GetDependenciesSehResult {
    HGLOBAL handle;  // NULL if no deps or SEH
    int depCount;    // number of dependency ProgIds in the handle
};
bool callGetDependencies(GetDependenciesFn fn, int whType, int progId,
                         int elementIndex,
                         GetDependenciesSehResult* out) {
    out->handle = NULL;
    out->depCount = 0;
    __try {
        out->handle = fn(whType, progId, elementIndex, &out->depCount);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// ============================================================================
// Workstream M slice M.4: linker-based reverse-dependency helpers.
// ============================================================================

// SEH-protected call to Ordinal_28 (partial-record copy, RVA 0x1A03F0).
// __stdcall, 4 args (category, progId, flag, outBuf). Returns 1 on success
// and writes a 0x10A-byte partial record into outBuf. POD-only.
typedef unsigned int (__stdcall *Ordinal28Fn)(int category, int progId,
                                              int flag, void* outBuf);
struct Ord28SehResult { unsigned int result; };
bool callOrd28(Ordinal28Fn fn, int category, int progId, int flag,
               void* outBuf, Ord28SehResult* out) {
    out->result = 0xFFFFFFFFu;
    __try { out->result = fn(category, progId, flag, outBuf); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

// SEH-protected call to Ordinal_70 (release refcounted record, RVA 0x15DF30).
// __stdcall, 2 args (rec, flag). POD-only.
typedef int (__stdcall *Ordinal70Fn)(void* rec, int flag);
struct Ord70SehResult { int result; };
bool callOrd70(Ordinal70Fn fn, void* rec, int flag, Ord70SehResult* out) {
    out->result = -1;
    __try { out->result = fn(rec, flag); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

// callLinkerManual — calls the linker with ALL 4 args on the stack
// (as __stdcall would) AND loads ECX=type, EDX=id (as __fastcall would),
// using inline assembly so we can manually restore ESP regardless of whether
// the callee did RET 0x10 (stdcall, popped 16) or RET 0x8 (fastcall, popped
// 8). This makes the call robust to calling-convention mismatch.
//
// NOTE: This function is NOT itself SEH-protected (inline asm + SEH in the
// same function is rejected by MSVC C2712/C2181). The caller wraps it.
//
// COPIED EXACTLY from tests/diag_m4_deplist_dump.cpp.
void* callLinkerManual(void* fn, int whType, int whId, int whErrFlag,
                       void* whPartialBuf) {
    void* retVal = nullptr;
    __asm {
        mov  esi, esp
        push whPartialBuf
        push whErrFlag
        push whId
        push whType
        mov  ecx, whType
        mov  edx, whId
        call fn
        mov  retVal, eax
        mov  esp, esi
    }
    return retVal;
}

struct LinkerSehResult { void* ptr; bool seh; };
bool callLinkerSafe(void* fn, int type, int id, int errorFlag,
                    void* partialBuf, LinkerSehResult* out) {
    out->ptr = nullptr;
    out->seh = false;
    __try {
        out->ptr = callLinkerManual(fn, type, id, errorFlag, partialBuf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->seh = true;
        return false;
    }
    return true;
}

// SEH-protected raw readers. POD-only so __try/__except is legal.
bool sehReadInt(const void* p, int* out) {
    *out = 0;
    __try { *out = *(const int*)p; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}
bool sehReadShort(const void* p, short* out) {
    *out = 0;
    __try { *out = *(const short*)p; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}
bool sehReadByte(const void* p, unsigned char* out) {
    *out = 0;
    __try { *out = *(const unsigned char*)p; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}
bool sehReadPtr(const void* p, void** out) {
    *out = nullptr;
    __try { *out = *(void* const*)p; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}
bool sehReadUInt(const void* p, unsigned int* out) {
    *out = 0;
    __try { *out = *(const unsigned int*)p; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}
bool sehMemcpy(const void* src, void* dst, size_t n) {
    __try { std::memcpy(dst, src, n); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

// Dep-list entry. The layout is:
//   +0x00: type   (dword)
//   +0x04: length (byte) — total entry length including this header
//   +0x05: sourceLine (int)
//   +0x09: sourceCol  (int)
//   +0x0d: payload; for type 0xfa3 / 0xfa4 the first 4 bytes are the
//          function reference (ProgId).
struct DepListEntry {
    unsigned int type;
    unsigned char length;
    int funcRef;
};

// Parse a dep-list buffer. Stops on zero-length entries or truncated entries.
// The buffer is assumed to be a local copy (already SEH-protected during copy).
std::vector<DepListEntry> parseDepList(const unsigned char* buf, size_t bufLen) {
    std::vector<DepListEntry> entries;
    size_t off = 0;
    while (off + 5 <= bufLen) {
        DepListEntry e;
        e.type   = *(const unsigned int*)(buf + off + 0);
        e.length = *(const unsigned char*)(buf + off + 4);
        if (e.length == 0) {
            break;
        }
        if (off + e.length > bufLen) {
            break;
        }
        if (e.length >= 13 && off + 13 + sizeof(int) <= bufLen) {
            e.funcRef = *(const int*)(buf + off + 13);
        } else {
            e.funcRef = 0;
        }
        entries.push_back(e);
        off += e.length;
    }
    return entries;
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

    // M.4: GetDependencies (ordinal 487) and _DeleteDependencyList@4 (ordinal 397).
    // These are named exports, resolved by name.
    m_getDependencies = (GetDependenciesFn)m_resolver->resolveExport("GetDependencies");
    m_deleteDependencyList = (DeleteDependencyListFn)m_resolver->resolveExport("_DeleteDependencyList@4");

    // Workstream M slice M.4: linker-based reverse-dependency enumeration.
    // Ordinal_28 (RVA 0x1A03F0), Ordinal_70 (RVA 0x15DF30), and ordinal 85
    // (the linker, FUN_101a3340). The linker is a __fastcall/__stdcall hybrid
    // and is invoked via an inline-asm wrapper.
    m_ord28 = reinterpret_cast<Ordinal28Fn>(m_resolver->resolveExportByOrdinal(28));
    m_ord70 = reinterpret_cast<Ordinal70Fn>(m_resolver->resolveExportByOrdinal(70));
    m_linkerRaw = (void*)m_resolver->resolveExportByOrdinal(85);

    // 9. Resolve 8 internal functions via resolver (signature scan,
    //    replaces old base-address + RVA arithmetic).
    //    Workstream M slice M.1: CELCompiler alloc/destroy are now resolved
    //    by orlib20 ORDINAL (66/67), not by signature scan of the raw
    //    ctor/dtor. See tmp/m1_ghidra_findings.md §1 (R-M.1.1).
    m_pStartCompiling     = m_resolver->resolveStartCompilingProcess();
    m_pEndCompiling       = m_resolver->resolveEndCompilingProcess();
    m_pCELCompilerAlloc   = m_resolver->resolveCELCompilerAlloc();
    m_pCELCompilerDestroy = m_resolver->resolveCELCompilerDestroy();
    // Workstream M slice M.3: resolve the 0x40A notification sender
    // (orlib20 ordinal 57 = FUN_101526f0). Used by verifyModule() (success
    // path) and deleteEntry() (success path, category != 4) to post the
    // library-change notification, matching TSDev's Frida-confirmed
    // ordering. See docs/RE/RE_M3_0x40A_NOTIFICATION.md §1.2 / §2.5.
    m_pNotifyLibChanged   = m_resolver->resolveNotifyLibChanged();
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
    //
    // Workstream M slice M.1: copy the optional onCompilerReady test hook
    // from the config BEFORE initAfterCom() so it is in place by the time
    // any verifyModule() call runs. The public ctor leaves m_onCompilerReady
    // as nullptr (its default), so production code is unaffected.
    m_onCompilerReady = config.onCompilerReady;
    // Workstream M slice M.2: copy the optional onSaveModuleNewReady test
    // hook from the config BEFORE initAfterCom() so it is in place by the
    // time any saveSource() call runs. The public ctor leaves
    // m_onSaveModuleNewReady as nullptr (its default), so production code is
    // unaffected.
    m_onSaveModuleNewReady = config.onSaveModuleNewReady;
    // Workstream M slice M.3: copy the optional onNotifyReady test hook
    // from the config BEFORE initAfterCom() so it is in place by the time
    // any verifyModule() or deleteEntry() call runs. The public ctor leaves
    // m_onNotifyReady as nullptr (its default), so production code is
    // unaffected. No invocation of this hook is added anywhere in this RED
    // phase — the GREEN phase will add the actual 0x40A send (and the hook
    // call) to verifyModule() and deleteEntry().
    m_onNotifyReady = config.onNotifyReady;
    m_onReverseDependentRevalidationReady = config.onReverseDependentRevalidationReady;
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
                          const std::string& source, bool isVerified) {
    m_lastSource = source;
    setSourceText(m_editorHwnd, source);
    // --- Workstream M slice M.2: invoke the optional test-only
    // save-module-ready hook immediately BEFORE the _SaveModule_New@20 call,
    // capturing the flags word (arg3) and password buffer (arg5) so a test
    // can assert the TSDev-parity values. m_onSaveModuleNewReady is nullptr
    // in production (public ctor), so this is a pure test seam with no
    // production side effect. ---
    // TSDev parity: FUN_00452290 forces IsProtected+IsProtectedPlus (bits 5+6)
    // for all items, using internal default password "!p$Kn@aSus" (0x00567b40).
    // Category bits 0-2 from type_word & 0x7. Bit 4 (is_verified) set when
    // re-saving an already-verified item (modify path). See docs/RE/RE_M2_SAVEMODULE_NEW.md.
    static const char kInternalDefaultPassword[] = "!p$Kn@aSus";  // TSDev internal default at 0x00567b40, NOT a user secret
    unsigned int saveFlags = (getCategory(type) & 0x7) | 0x60;
    if (isVerified) {
        saveFlags |= 0x10;  // bit 4 = is_verified (FUN_0044f720 returns 1 for re-saves)
    }
    char savePasswordBuf[64] = {};
    memcpy(savePasswordBuf, kInternalDefaultPassword, sizeof(kInternalDefaultPassword) - 1);  // 10 bytes, rest already zeroed
    if (m_onSaveModuleNewReady) {
        m_onSaveModuleNewReady(&saveFlags, savePasswordBuf, 64,
                               static_cast<int>(type), progId);
    }
    int result = 0;
    __try {
        result = m_pSaveModuleNew(getCategory(type), progId,
                                  &saveFlags, m_editorHwnd, savePasswordBuf);
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
    // Workstream M slice M.1 — TSDev-parity CELCompiler lifecycle.
    //
    // TSDev (FUN_00451800 / FUN_004D9480) allocates+constructs the
    // 0x58-byte CELCompiler via orlib20 ordinal export 66 (FUN_10165f70 =
    // operator_new(0x58) + ctor FUN_10165a00) and destructs+frees via
    // ordinal 67 (FUN_10165fd0 = dtor FUN_10165ac0 + operator
    // delete(p, 0x58)). The CLI MUST NOT separately malloc/memset the
    // object (ordinal 66 does both) and MUST NOT std::free the pointer
    // after ordinal 67 (that would double-free).
    //
    // Field layout written by TSDev (FUN_00451800 at 0x00451ac4-0x00451af9),
    // from warehouse type mapping (NOT source-text parsing):
    //   +0x08 = category (whType low 8 bits)  = getCategory(type)
    //   +0x0C = subtype                      = getSubType(type)
    //   +0x10 = ProgId                        = progId
    //   +0x18 = notification HWND             = m_editorHwnd
    //   +0x28 = NOT WRITTEN by TSDev (left as uninitialized heap; ctor does NOT zero it).
    // After StartCompilingProcess, before _VerifyModule_New, FUN_004D9480
    // zeroes +0x00 and +0x04 (the result fields). See
    // tmp/m1_ghidra_findings.md §2-3 (R-M.1.2 / R-M.1.3 / R-M.1.4 / R-M.1.5).

    // --- 1. Allocate + construct via ordinal 66 (no malloc, no memset) ---
    void* compilerMem = nullptr;
    if (!callAlloc(reinterpret_cast<CELCompilerAllocFn>(m_pCELCompilerAlloc),
                   &compilerMem)) {
        // SEH inside the alloc+ctor wrapper. No object was produced (the
        // wrapper allocates internally; on SEH we treat it as a ctor
        // failure). Nothing to free — ordinal 66 owns its own allocation.
        if (outStage) *outStage = CreatePathFailureStage::VerifyModule_SehCtor;
        return "tsdev: CELCompiler alloc+ctor (ordinal 66) raised an exception";
    }
    if (!compilerMem) {
        // Ordinal 66 returned NULL — operator_new(0x58) failed inside the
        // wrapper. Preserve the OOM stage/error behavior.
        if (outStage) *outStage = CreatePathFailureStage::VerifyModule_Oom;
        return "tsdev: out of memory allocating CELCompiler object";
    }

    std::string errors;

    // --- 2. Write the TSDev-parity fields (warehouse type mapping) ---
    //   +0x08 = category, +0x0C = subtype, +0x10 = ProgId, +0x18 = HWND.
    //   +0x28 is NOT written (TSDev does not write it; ctor does NOT zero it — uninitialized heap).
    char* base = static_cast<char*>(compilerMem);
    *(reinterpret_cast<int*>(base + 0x08)) = getCategory(type);
    *(reinterpret_cast<int*>(base + 0x0C)) = getSubType(type);
    *(reinterpret_cast<int*>(base + 0x10)) = progId;
    *(reinterpret_cast<HWND*>(base + 0x18)) = m_editorHwnd;

    // --- 3. StartCompilingProcess (cdecl, no args) ---
    if (!callStartCompiling(
            reinterpret_cast<StartCompilingFn>(m_pStartCompiling))) {
        // Destroy via ordinal 67 (dtor + free). NO std::free — ordinal 67
        // already calls operator delete(p, 0x58).
        callDestroy(reinterpret_cast<CELCompilerDestroyFn>(m_pCELCompilerDestroy),
                    compilerMem);
        if (outStage) *outStage = CreatePathFailureStage::VerifyModule_SehStartCompiling;
        return "tsdev: StartCompilingProcess raised an exception";
    }

    // --- 4. Zero the result fields (+0x00, +0x04) AFTER StartCompilingProcess
    //     and BEFORE _VerifyModule_New, matching FUN_004D9480. These are the
    //     cumulative compile-result fields that VerifyModule_New populates;
    //     TSDev zeroes them here so stale state from a prior compile does not
    //     leak into this one. ---
    *(reinterpret_cast<int*>(base + 0x00)) = 0;
    *(reinterpret_cast<int*>(base + 0x04)) = 0;

    // --- 5. Invoke the optional test-only compiler-ready hook immediately
    //     BEFORE _VerifyModule_New. In TSDev this is the point where
    //     FUN_004D9480 has just zeroed +0x00/+0x04 and is about to call
    //     FUN_004D91E0 -> _VerifyModule_New. The hook receives the live
    //     compiler object so a test can assert the TSDev-parity field
    //     layout. m_onCompilerReady is nullptr in production (public ctor),
    //     so this is a pure test seam with no production side effect. ---
    if (m_onCompilerReady) {
        m_onCompilerReady(compilerMem, static_cast<int>(type), progId);
    }

    // --- 6. VerifyModule_New, SEH-guarded. Error text is read directly from
    //     the global error array after this call (see readErrorsFromArray).
    //     Args unchanged: HWND=m_editorHwnd, category=getCategory(type),
    //     ProgId=progId, flag=1, compiler=compilerMem. ---
    VerifySehResult seh;
    if (!callVerify(
            reinterpret_cast<VerifyModuleFn>(m_pVerifyModule),
            m_editorHwnd, getCategory(type), progId, compilerMem, &seh)) {
        callEndCompiling(reinterpret_cast<EndCompilingFn>(m_pEndCompiling), 0);
        // Destroy via ordinal 67 (dtor + free). NO std::free.
        callDestroy(reinterpret_cast<CELCompilerDestroyFn>(m_pCELCompilerDestroy),
                    compilerMem);
        if (outStage) *outStage = CreatePathFailureStage::VerifyModule_SehVerify;
        return "tsdev: VerifyModule_New raised an exception";
    }

    // --- 7. Build the error string from the global error array ---
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

    // --- 7b. M.4: Reverse-dependent revalidation (TSDev FUN_004D91E0 parity).
    //     Only on verify success AND category in 0/1/4 (matching TSDev's gate
    //     at 0x004d92b2-0x004d92bd). The traversal revalidates all reverse
    //     dependents (callers) of the just-verified module. EndCompilingProcess
    //     is deferred until AFTER the traversal (matching TSDev's
    //     FUN_004D9480 which calls EndCompilingProcess after FUN_004D91E0
    //     returns). The compiler object stays alive during the traversal. ---
    if (seh.verifyResult == 1) {
        int rootCategory = getCategory(type);
        if (rootCategory == 0 || rootCategory == 1 || rootCategory == 4) {
            // Contract-change check: compare old vs new master record.
            // The old record was captured before saveSource() (if applicable).
            bool contractChanged = true;  // default: assume changed
            if (m_hasOldMasterRec) {
                // Read the new master record.
                char newRec[0x10A] = {};
                GetMasterRecByTypeSehResult gmSeh;
                if (callGetMasterRecByType(m_getMasterRecByType,
                        rootCategory, getSubType(type), progId, newRec, &gmSeh)
                    && gmSeh.result == 1) {
                    int cmpResult = compareMasterRecsContractSeh(
                        (const char*)m_oldMasterRec, (const char*)newRec);
                    contractChanged = (cmpResult == 0);  // 0 = changed
                }
                m_hasOldMasterRec = false;  // reset for next operation
            }

            // Traverse reverse dependents with explicit visited-set.
            std::set<int> visited;
            visited.insert(progId);  // root is already visited
            traverseReverseDependents(type, progId, compilerMem,
                                      contractChanged, visited, 0);
        }
    }

    // --- 8. EndCompilingProcess (fastcall, 1 arg = 0) ---
    callEndCompiling(reinterpret_cast<EndCompilingFn>(m_pEndCompiling), 0);

    // --- 8b. M.3: Post-success 0x40A library-change notification
    //     (TSDev parity). TSDev's FUN_00453f70 calls orlib20 Ordinal_57
    //     AFTER EndCompilingProcess on verify success (Frida-confirmed
    //     ordering: verify success → EndCompilingProcess →
    //     ShowRecalculationWarning → 0x40A send → compiler destruction).
    //     The send is GATED on verify success (seh.verifyResult == 1) —
    //     failed compile/verify does NOT send 0x40A (R-M.3.2). The hook
    //     fires immediately BEFORE the actual 0x40A post (the same point
    //     at which TSDev calls Ordinal_57), capturing the (msg, wParam,
    //     lParam) tuple plus the affected componentType and progId so a
    //     test can assert the TSDev-parity packing. If Ordinal_57 failed
    //     to resolve (m_pNotifyLibChanged == nullptr), the notification is
    //     silently skipped (R-M.3.3 error semantics — no crash, no
    //     failure), but the hook still fires (it captures the intent to
    //     send). See docs/RE/RE_M3_0x40A_NOTIFICATION.md §2.5 / §3.1. ---
    if (seh.verifyResult == 1) {
        unsigned short category = static_cast<unsigned short>(getCategory(type));
        unsigned short progIdWord = static_cast<unsigned short>(progId & 0xFFFF);
        LPARAM lParam = (static_cast<LPARAM>(category) << 16) | progIdWord;
        if (m_onNotifyReady) {
            m_onNotifyReady(0x40A, 0, lParam, static_cast<int>(type), progId);
        }
        if (m_pNotifyLibChanged) {
            m_pNotifyLibChanged(category, progIdWord);
        }
    }

    // --- 9. Destroy + free via ordinal 67. NO std::free — ordinal 67 already
    //     calls operator delete(p, 0x58). Calling std::free here would
    //     double-free. ---
    callDestroy(reinterpret_cast<CELCompilerDestroyFn>(m_pCELCompilerDestroy),
                compilerMem);

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

std::vector<TsDevLib::ReverseDependent> TsDevLib::enumerateReverseDependents(
    int rootProgId, int rootCategory) {
    std::vector<ReverseDependent> result;
    (void)rootCategory;  // rootCategory is unused; we scan all categories.

    if (!m_linkerRaw || !m_ord28 || !m_ord70 || !m_loadMasterRecsEx) {
        return result;  // exports not resolved — no deps
    }

    // Refresh cache so newly created records are visible.
    refreshCache();

    // Enumerate all categories (0-7). For each candidate, get the linked
    // record via the linker (ordinal 85) and scan each element's dep-list
    // for a forward reference to rootProgId (type 0xfa3 or 0xfa4).
    const int kRecordSize = 0x10A;
    const int kProgIdOffset = 0x00;   // short ProgId
    const int kSubTypeOffset = 0xE9;  // byte subtype
    const size_t kElemSize = 0xb5;
    const size_t kMaxDepListCopy = 8192;

    for (int cat = 0; cat <= 7; cat++) {
        HGLOBAL buf = NULL;
        LoadMasterRecsExSehResult lmrSeh;
        if (!callLoadMasterRecsEx(m_loadMasterRecsEx, cat, &buf,
                                  (const char*)NULL, 1, &lmrSeh)) {
            continue;  // SEH — skip this category
        }
        int count = lmrSeh.result;
        if (count <= 0 || buf == NULL) {
            if (buf) GlobalFree(buf);
            continue;
        }

        char* records = (char*)GlobalLock(buf);
        if (!records) {
            GlobalFree(buf);
            continue;
        }

        for (int i = 0; i < count; i++) {
            char* rec = records + (size_t)i * kRecordSize;

            short candidateProgId = 0;
            unsigned char candidateSubType = 0;
            if (!sehReadShort(rec + kProgIdOffset, &candidateProgId) ||
                !sehReadByte(rec + kSubTypeOffset, &candidateSubType)) {
                continue;
            }
            if (candidateProgId <= 0) continue;
            if ((int)candidateProgId == rootProgId) continue;  // skip root

            // Step 1: get the partial record via Ordinal_28.
            unsigned char partialBuf[kRecordSize] = {};
            Ord28SehResult ord28r;
            if (!callOrd28(m_ord28, cat, (int)candidateProgId, /*flag=*/0,
                           partialBuf, &ord28r)) {
                continue;
            }
            if (ord28r.result != 1) continue;

            // Step 2: call the linker (ordinal 85) to get the linked record.
            LinkerSehResult linkr;
            if (!callLinkerSafe(m_linkerRaw, cat, (int)candidateProgId,
                                /*errorFlag=*/0, partialBuf, &linkr)) {
                continue;
            }
            void* linkedRec = linkr.ptr;
            if (!linkedRec) continue;

            // Step 3: walk the element array and scan dep-lists.
            bool isReverseDep = false;
            int elemCount = 0;
            void* elemArray = nullptr;
            if (!sehReadInt((const char*)linkedRec + 0x29, &elemCount) ||
                !sehReadPtr((const char*)linkedRec + 0x4f, &elemArray)) {
                // Release the record even if the read failed.
                Ord70SehResult r70;
                callOrd70(m_ord70, linkedRec, 0, &r70);
                continue;
            }

            unsigned char depBuf[kMaxDepListCopy] = {};
            if (elemCount > 0 && elemArray) {
                for (int ei = 0; ei < elemCount && !isReverseDep; ei++) {
                    const unsigned char* elem =
                        (const unsigned char*)elemArray + (size_t)ei * kElemSize;

                    unsigned int depListSize = 0;
                    void* depListPtr = nullptr;
                    if (!sehReadUInt(elem + 0x73, &depListSize) ||
                        !sehReadPtr(elem + 0x7b, &depListPtr)) {
                        continue;  // skip this element on SEH
                    }

                    if (!depListPtr || depListSize == 0) continue;

                    size_t copySize = depListSize < kMaxDepListCopy
                                          ? depListSize
                                          : kMaxDepListCopy;
                    if (!sehMemcpy(depListPtr, depBuf, copySize)) {
                        continue;  // skip this element on SEH during copy
                    }

                    std::vector<DepListEntry> entries =
                        parseDepList(depBuf, copySize);
                    for (const auto& e : entries) {
                        if (e.type == 0xFA3 || e.type == 0xFA4) {
                            if (e.funcRef == rootProgId) {
                                isReverseDep = true;
                                break;
                            }
                        }
                    }
                }
            }

            // Step 4: release the linked record via Ordinal_70.
            Ord70SehResult r70;
            callOrd70(m_ord70, linkedRec, 0, &r70);

            if (isReverseDep) {
                result.push_back({(int)candidateProgId, cat,
                                  (int)candidateSubType});
            }
        }

        GlobalUnlock(buf);
        GlobalFree(buf);
    }

    return result;
}

void TsDevLib::traverseReverseDependents(
    ComponentType rootType, int rootProgId,
    void* compilerMem,
    bool contractChanged,
    std::set<int>& visited,
    int traversalDepth) {

    int rootCategory = getCategory(rootType);
    int rootComponentType = static_cast<int>(rootType);

    // Fire traversal-started sentinel ONLY at depth 0.
    if (traversalDepth == 0 && m_onReverseDependentRevalidationReady) {
        m_onReverseDependentRevalidationReady(
            rootComponentType, rootProgId, rootCategory,
            0, 0, 0,  // dependent fields = 0 for sentinel
            -1,  // revalidationTriggered = -1 (sentinel)
            -1,  // revalidationResult = -1
            -1); // traversalDepth = -1
    }

    // If exports not resolved, cannot enumerate.
    if (!m_linkerRaw || !m_ord28 || !m_ord70 || !m_loadMasterRecsEx) {
        return;
    }

    // Enumerate reverse dependents.
    auto dependents = enumerateReverseDependents(rootProgId, rootCategory);

    char* compilerBase = static_cast<char*>(compilerMem);

    for (const auto& dep : dependents) {
        // Cycle safety: check visited-set.
        if (visited.count(dep.progId)) {
            if (!contractChanged) {
                // Already visited AND contract did NOT change -> skip.
                if (m_onReverseDependentRevalidationReady) {
                    m_onReverseDependentRevalidationReady(
                        rootComponentType, rootProgId, rootCategory,
                        dep.category, dep.progId, dep.category,
                        0,   // revalidationTriggered = 0 (skipped)
                        -1,  // revalidationResult = -1 (not run)
                        traversalDepth);
                }
                continue;
            }
            // Contract changed -> reverify even if visited.
        }

        // Mark as visited BEFORE reverifying (prevents infinite recursion).
        visited.insert(dep.progId);

        // Reverify via _VerifyModule_New@20 with flag=0 (non-primary).
        // The compiler object's +0x08/+0x0C/+0x10 stay as the root's values.
        // The dependent's (category, progId) are passed as args.
        VerifySehResult vSeh;
        bool ok = callVerify(
            reinterpret_cast<VerifyModuleFn>(m_pVerifyModule),
            m_editorHwnd, dep.category, dep.progId,
            compilerMem, &vSeh);

        int revalidationTriggered = 1;
        int revalidationResult = -1;
        if (!ok) {
            // SEH exception during verify -- treat as failure.
            revalidationResult = 0;
        } else {
            revalidationResult = (vSeh.verifyResult == 1) ? 1 : 0;
        }

        // AND the dependent result into compiler+0x04 (aggregate).
        // compiler+0x00 (root result) is NOT touched (flag=0, not primary).
        int currentAggregate = *(reinterpret_cast<int*>(compilerBase + 0x04));
        *(reinterpret_cast<int*>(compilerBase + 0x04)) =
            currentAggregate & revalidationResult;

        // Fire per-dependent hook event.
        if (m_onReverseDependentRevalidationReady) {
            m_onReverseDependentRevalidationReady(
                rootComponentType, rootProgId, rootCategory,
                dep.category, dep.progId, dep.category,
                revalidationTriggered,
                revalidationResult,
                traversalDepth);
        }

        // Recurse into the dependent (find its reverse dependents).
        // Only recurse for categories 0/1/4 (matching TSDev's gate).
        if (dep.category == 0 || dep.category == 1 || dep.category == 4) {
            ComponentType depType = ComponentType::Function;  // default
            if (dep.category == 0) depType = ComponentType::Strategy;
            else if (dep.category == 4) depType = ComponentType::Function;
            // category 1 is also Function subtype
            traverseReverseDependents(depType, dep.progId, compilerMem,
                                      contractChanged, visited,
                                      traversalDepth + 1);
        }
    }
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

    // ------------------------------------------------------------------
    // 6. M.3: Post-delete 0x40A library-change notification (TSDev parity).
    //    TSDev's FUN_10159ae0 sends 0x40A after a successful delete, GATED
    //    on category != 4 (Function deletes do NOT send 0x40A — see
    //    docs/RE/RE_M3_0x40A_NOTIFICATION.md §2.4). The hook fires
    //    immediately BEFORE the actual 0x40A post, capturing the
    //    (msg, wParam, lParam) tuple plus the affected componentType and
    //    progId so a test can assert the TSDev-parity packing. If
    //    Ordinal_57 failed to resolve (m_pNotifyLibChanged == nullptr),
    //    the notification is silently skipped (R-M.3.3 error semantics),
    //    but the hook still fires (it captures the intent to send).
    //
    //    NOTE: this deleteEntry() implementation calls FUN_101a8360 (the
    //    TSDev UI delete orchestrator), NOT FUN_10159dd0 / FUN_10159ae0
    //    (the orlib20 delete export that internally sends 0x40A). The
    //    orchestrator path does NOT emit 0x40A on its own, so an explicit
    //    send is required here for TSDev parity. This is NOT a duplicate
    //    send — no orlib20 function called below emits 0x40A.
    // ------------------------------------------------------------------
    if (category != 4) {  // Function deletes do NOT send 0x40A
        unsigned short categoryWord = static_cast<unsigned short>(category);
        unsigned short progIdWord = static_cast<unsigned short>(progId & 0xFFFF);
        LPARAM lParam = (static_cast<LPARAM>(categoryWord) << 16) | progIdWord;
        if (m_onNotifyReady) {
            m_onNotifyReady(0x40A, 0, lParam, static_cast<int>(type), progId);
        }
        if (m_pNotifyLibChanged) {
            m_pNotifyLibChanged(categoryWord, progIdWord);
        }
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
    // M.2: pass isVerified=true — modify operates on a previously-verified
    // item, so flags bit 4 (is_verified) must be set per TSDev FUN_00452290.
    bool saved = saveSource(type, progId, newSource, true);
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
