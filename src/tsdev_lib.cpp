// tsdev_lib.cpp
//
// Implementation of the shared orlib20 wrapper. See tsdev_lib.h for the API
// contract and tmp/re_artifacts/orlib_param_orders.md for the confirmed
// reverse-engineering findings this is built on.
//
// The reference probe (src/elevated_test20.cpp) demonstrates the full flow
// end-to-end; this module encapsulates the same logic in a reusable API.

#include "tsdev_lib.h"

#include <stdexcept>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

// Compile-time debug logging gate. Define TSDEV_DEBUG at build time to
// enable stderr diagnostics in deleteEntry. Production builds are clean.
#ifdef TSDEV_DEBUG
#  define TSDEV_DBG(...) do { fprintf(stderr, __VA_ARGS__); fflush(stderr); } while(0)
#else
#  define TSDEV_DBG(...) do {} while(0)
#endif

namespace tsdev {

namespace {

// TradeStation paths (READ-ONLY — never modified).
const wchar_t* kTsProgramDir =
    L"C:\\Program Files (x86)\\TradeStation 9.5\\Program";
const char* kMyWorkBin =
    "C:\\Program Files (x86)\\TradeStation 9.5\\MyWork\\Bin";

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

// Internal function RVAs (relative to orlib20.dll base).
const DWORD_PTR kRvaStartCompiling    = 0x165de0;
const DWORD_PTR kRvaEndCompiling      = 0x165ed0;
const DWORD_PTR kRvaCELCompilerCtor   = 0x165a00;
const DWORD_PTR kRvaCELCompilerDtor   = 0x165ac0;
// DELETE flow internal functions (confirmed via Frida trace of TSDev.exe).
const DWORD_PTR kRvaInitGlobalLookup  = 0x1a9460;
const DWORD_PTR kRvaDeleteFromTree    = 0x1e4030;
const DWORD_PTR kRvaGetWarehouse      = 0x1a9650;

// Persistence patch (§12): bypass FUN_10011230 (SymWriter crash).
//   MOV EAX, 1   (5 bytes)  B8 01 00 00 00
//   RET 0x0C     (3 bytes)  C2 0C 00
const DWORD_PTR kRvaPersistencePatch  = 0x11230;
const unsigned char kPersistencePatchBytes[] = {
    0xB8, 0x01, 0x00, 0x00, 0x00,  // MOV EAX, 1
    0xC2, 0x0C, 0x00               // RET 0x0C
};

// Thread-check bypass patches: NOP the INT3 (0xCC → 0x90) in three
// dispatch helpers so they can be called from any thread, not just the
// worker thread. Each patch is a single byte.
//
// FUN_101d0950 (command 0x0F — load program): INT3 at RVA +0x3E
// FUN_101cf5b0 (general dispatch helper):    INT3 at RVA +0x24
// FUN_101d1fa0 (command 0x17 — save/commit): INT3 at RVA +0x28
const DWORD_PTR kRvaThreadCheck1 = 0x1d0993;  // FUN_101d0950
const DWORD_PTR kRvaThreadCheck2 = 0x1cf5d4;  // FUN_101cf5b0
const DWORD_PTR kRvaThreadCheck3 = 0x1d1fc8;  // FUN_101d1fa0

// Delete orchestrator: FUN_101a8360 — __fastcall(ECX=whType, EDX=progId).
// Loads the program via command 0x0F, marks it deleted, runs
// CELProgramWriter cleanup, sends command 0x4E (cache reset) and
// command 0x17 (save/commit). This is the function called by the
// TSDev UI delete confirmation path (FUN_10159ae0).
const DWORD_PTR kRvaDeleteOrchestrator = 0x1a8360;

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

// --- SEH-protected call wrappers ------------------------------------------
// MSVC forbids __try in functions that own C++ objects with destructors
// (C2712). These helpers contain only POD and raw pointers, so they can use
// __try/__except safely. Each returns a success flag / out-parameter.
//
// The typedefs mirror the private nested typedefs in TsDevLib; function
// pointer types are structurally identical so the members convert implicitly.

typedef void  (__fastcall *CELCompilerCtorFn)(void* thisPtr);
typedef void  (__fastcall *CELCompilerDtorFn)(void* thisPtr);
typedef void  (__cdecl    *StartCompilingFn)(void);
typedef void  (__fastcall *EndCompilingFn)(int param);
typedef int   (__stdcall  *VerifyModuleFn)(HWND, int, int, int, void*);
typedef void (__stdcall *GetErrorsFn)(char* buffer);

struct VerifySehResult {
    int   verifyResult; // -1 if VerifyModule_New raised
    char  errorBuf[4096]; // buffer filled by GetCompilingErrors
    bool  errorsCalled;   // false if GetCompilingErrors raised
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

// Runs VerifyModule_New + GetCompilingErrors under SEH. EndCompilingProcess
// and the destructor are called by the caller (also SEH-guarded) so that an
// exception in one step does not skip cleanup of the others.
bool callVerifyAndErrors(VerifyModuleFn verifyFn, GetErrorsFn errorsFn,
                         HWND hwnd, int whType, int progId,
                         void* compilerMem, VerifySehResult* out) {
    out->verifyResult = -1;
    out->errorBuf[0] = '\0';
    out->errorsCalled = false;
    __try {
        out->verifyResult = verifyFn(hwnd, whType, progId, 1, compilerMem);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    __try {
        errorsFn(out->errorBuf);
        out->errorsCalled = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->errorBuf[0] = '\0';
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
typedef void* (__cdecl *InitGlobalLookupFn)(void);
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
typedef void* (__cdecl *GetWarehouseFn)(int);
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

} // namespace

// --- Template helper definition (kept in .cpp to avoid header clutter) ---
// Resolves a decorated export from orlib20.dll, throwing on failure.
template <typename FnPtr>
FnPtr TsDevLib::resolveOrThrow(const char* decoratedName) {
    FARPROC raw = GetProcAddress(m_orlib, decoratedName);
    if (!raw) {
        throw std::runtime_error(
            std::string("tsdev: failed to resolve orlib20 export: ")
            + decoratedName);
    }
    return reinterpret_cast<FnPtr>(raw);
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

TsDevLib::TsDevLib() {
    // 1. Point the loader at the TS Program directory so orlib20.dll and its
    //    dependencies resolve.
    if (!SetDllDirectoryW(kTsProgramDir)) {
        throw std::runtime_error(
            "tsdev: SetDllDirectoryW failed for TS Program dir");
    }

    // 2. Load orlib20.dll.
    m_orlib = LoadLibraryW(L"orlib20.dll");
    if (!m_orlib) {
        DWORD err = GetLastError();
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "tsdev: LoadLibraryW(orlib20.dll) failed: %lu", err);
        throw std::runtime_error(buf);
    }
    m_orlibBase = reinterpret_cast<DWORD_PTR>(m_orlib);

    // 3. Pre-load support DLLs (§13). Failures are non-fatal — log via
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

    // 4. Apply the FUN_10011230 persistence patch (§12). Without this no
    //    .ell file is persisted.
    if (!applyPersistencePatch()) {
        throw std::runtime_error(
            "tsdev: failed to apply persistence patch (FUN_10011230)");
    }

    // 4b. Apply thread-check bypass patches. These NOP the INT3
    //     instructions in FUN_101d0950, FUN_101cf5b0, and FUN_101d1fa0
    //     so the delete orchestrator (FUN_101a8360) can call them from
    //     our thread instead of the worker thread.
    if (!applyThreadCheckPatches()) {
        throw std::runtime_error(
            "tsdev: failed to apply thread-check patches");
    }

    // 5. Resolve all exported functions.
    m_pOrlibInit      = resolveOrThrow<OrlibInit_t>("_OrlibInit@8");
    m_pOrlibExit      = resolveOrThrow<OrlibExit_t>("_OrlibExit@0");
    // NOTE: orlib20.dll exports FindExistPos UNDECORATED (not _FindExistPos@8).
    // Confirmed via dumpbin /exports. The reference probe (elevated_test20.cpp)
    // also resolves the undecorated name.
    m_pFindExistPos   = resolveOrThrow<FindExistPos_t>("FindExistPos");
    m_pCreateEntry    = resolveOrThrow<CreateEntryEX_t>("_CreateEntryEX@24");
    m_pSaveModuleNew  = resolveOrThrow<SaveModule_New_t>("_SaveModule_New@20");
    m_pVerifyModule   = resolveOrThrow<VerifyModule_New_t>("_VerifyModule_New@20");
    m_pGetErrors      = resolveOrThrow<GetCompilingErrors_t>("_GetCompilingErrors@4");
    m_pCloseWarehouse = resolveOrThrow<CloseWarehouse_t>(
        "_CloseWarehouseForCurrentThreadAndPath@4");

    // 6. Resolve internal functions by RVA.
    m_pStartCompiling = reinterpret_cast<StartCompilingProcess_t>(
        m_orlibBase + kRvaStartCompiling);
    m_pEndCompiling   = reinterpret_cast<EndCompilingProcess_t>(
        m_orlibBase + kRvaEndCompiling);
    m_pCELCompilerCtor = reinterpret_cast<CELCompilerCtor_t>(
        m_orlibBase + kRvaCELCompilerCtor);
    m_pCELCompilerDtor = reinterpret_cast<CELCompilerDtor_t>(
        m_orlibBase + kRvaCELCompilerDtor);

    // 6b. Resolve DELETE-flow internal functions by RVA.
    m_pInitGlobalLookup = reinterpret_cast<InitGlobalLookup_t>(
        m_orlibBase + kRvaInitGlobalLookup);
    m_pDeleteFromTree = reinterpret_cast<void*>(
        m_orlibBase + kRvaDeleteFromTree);
    m_pGetWarehouse = reinterpret_cast<GetWarehouse_t>(
        m_orlibBase + kRvaGetWarehouse);

    // 7. Initialize orlib20.
    if (!m_pOrlibInit(NULL, 1)) {
        throw std::runtime_error("tsdev: OrlibInit(NULL, 1) returned FALSE");
    }

    // 8. Create the hidden EDIT control used to hold source text.
    m_editorHwnd = createHiddenEditControl();
    if (!m_editorHwnd) {
        m_pOrlibExit();
        throw std::runtime_error("tsdev: failed to create hidden EDIT control");
    }
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
    if (m_pOrlibExit) {
        m_pOrlibExit();
    }
    // DLLs are released automatically at process exit; do not FreeLibrary
    // orlib20.dll here because internal threads/state may still reference it.
}

// ============================================================================
// Public API
// ============================================================================

int TsDevLib::findExistPos(const std::string& name, ComponentType type) {
    return m_pFindExistPos(name.c_str(), static_cast<int>(type));
}

int TsDevLib::createEntry(ComponentType type, const std::string& name,
                          int& errCode) {
    errCode = 0;
    int progId = 0;
    __try {
        progId = m_pCreateEntry(static_cast<int>(type), 0, name.c_str(),
                                0, 0, &errCode);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // CreateEntryEX can raise SEH on certain malformed states; treat as
        // a hard failure (ProgId 0).
        return 0;
    }
    return progId;
}

bool TsDevLib::saveSource(ComponentType type, int progId,
                          const std::string& source) {
    setSourceText(m_editorHwnd, source);
    int result = 0;
    __try {
        result = m_pSaveModuleNew(static_cast<int>(type), progId,
                                  NULL, m_editorHwnd, NULL);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return (result == 1);
}

std::string TsDevLib::verifyModule(ComponentType type, int progId) {
    // Allocate and zero the CELCompiler object (88 bytes).
    void* compilerMem = std::malloc(kCELCompilerSize);
    if (!compilerMem) {
        return "tsdev: out of memory allocating CELCompiler object";
    }
    std::memset(compilerMem, 0, kCELCompilerSize);

    std::string errors;

    // Construct the CELCompiler object (fastcall: ecx = this).
    if (!callCtor(reinterpret_cast<CELCompilerCtorFn>(m_pCELCompilerCtor),
                  compilerMem)) {
        std::free(compilerMem);
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
        return "tsdev: StartCompilingProcess raised an exception";
    }

    // VerifyModule_New(hwnd, whType, progId, flag=1, compilerObj) +
    // GetCompilingErrors, both SEH-guarded.
    VerifySehResult seh;
    if (!callVerifyAndErrors(
            reinterpret_cast<VerifyModuleFn>(m_pVerifyModule),
            reinterpret_cast<GetErrorsFn>(m_pGetErrors),
            m_editorHwnd, static_cast<int>(type), progId, compilerMem, &seh)) {
        callEndCompiling(reinterpret_cast<EndCompilingFn>(m_pEndCompiling), 0);
        callDtor(reinterpret_cast<CELCompilerDtorFn>(m_pCELCompilerDtor),
                 compilerMem);
        std::free(compilerMem);
        return "tsdev: VerifyModule_New raised an exception";
    }

    // Build the error string (empty buffer == success).
    if (seh.errorBuf[0] != '\0') {
        errors = seh.errorBuf;
    } else if (seh.verifyResult != 1) {
        // No explicit error text but VerifyModule_New did not report success.
        errors = "tsdev: VerifyModule_New returned "
                 + std::to_string(seh.verifyResult)
                 + " with no error text";
    }

    // EndCompilingProcess (fastcall, 1 arg = 0).
    callEndCompiling(reinterpret_cast<EndCompilingFn>(m_pEndCompiling), 0);

    // Destroy the CELCompiler object.
    callDtor(reinterpret_cast<CELCompilerDtorFn>(m_pCELCompilerDtor),
             compilerMem);
    std::free(compilerMem);

    return errors;
}

void TsDevLib::closeWarehouse() {
    __try {
        m_pCloseWarehouse(0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Best-effort flush; ignore SEH.
    }
}

bool TsDevLib::ellFileExists(ComponentType type, int progId) {
    std::string path = getEllPath(type, progId);
    DWORD attr = GetFileAttributesA(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES);
}

std::string TsDevLib::getEllPath(ComponentType type, int progId) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s\\elmodule_%d_%d.ell",
                  kMyWorkBin, static_cast<int>(type), progId);
    return std::string(buf);
}

std::string TsDevLib::getXmlPath(ComponentType type, int progId) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s\\elmodule_%d_%d.xml",
                  kMyWorkBin, static_cast<int>(type), progId);
    return std::string(buf);
}

bool TsDevLib::deleteEntry(ComponentType type, const std::string& name) {
    int whType = static_cast<int>(type);

    // ---- MWRKArea.wrk timestamp (before) ----
    std::string wrkPath = "C:\\Program Files (x86)\\TradeStation 9.5\\MyWork\\MWRKArea.wrk";
    WIN32_FILE_ATTRIBUTE_DATA wrkBefore;
    BOOL wrkBeforeOk = GetFileAttributesExA(wrkPath.c_str(), GetFileExInfoStandard, &wrkBefore);
    TSDEV_DBG("tsdev: deleteEntry — MWRKArea.wrk BEFORE: ok=%d size=%lu mtime=%lu\n",
              (int)wrkBeforeOk,
              wrkBeforeOk ? wrkBefore.nFileSizeLow : 0,
              wrkBeforeOk ? wrkBefore.ftLastWriteTime.dwLowDateTime : 0);

    // 1. FindExistPos to get progId (exported, works in our process)
    int progId = m_pFindExistPos(name.c_str(), whType);
    TSDEV_DBG("tsdev: deleteEntry — FindExistPos(\"%s\", %d) = %d\n",
              name.c_str(), whType, progId);
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
    void* deleteOrchestrator = reinterpret_cast<void*>(
        m_orlibBase + kRvaDeleteOrchestrator);

    TSDEV_DBG("tsdev: deleteEntry — calling FUN_101a8360"
              " (deleteOrchestrator=%p, whType=%d, progId=%d)\n",
              deleteOrchestrator, whType, progId);

    bool ok = callDeleteOrchestrator(deleteOrchestrator, whType, progId);

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

        TSDEV_DBG("tsdev: deleteEntry — calling DeleteFromTree(%d, %d, 3)\n", whType, progId);
        if (!callDeleteFromTree(m_pDeleteFromTree, globalLookup,
                                whType, progId, 3)) {
            TSDEV_DBG("tsdev: deleteEntry — DeleteFromTree raised SEH exception\n");
        } else {
            TSDEV_DBG("tsdev: deleteEntry — DeleteFromTree done\n");
        }
    }

    // 4. CloseWarehouse (best-effort flush).
    TSDEV_DBG("tsdev: deleteEntry — calling closeWarehouse()\n");
    closeWarehouse();
    TSDEV_DBG("tsdev: deleteEntry — closeWarehouse() done\n");

    // 5. Delete the .ell and .xml files from disk.
    char ellPath[512];
    char xmlPath[512];
    std::snprintf(ellPath, sizeof(ellPath), "%s\\elmodule_%d_%d.ell",
                  kMyWorkBin, whType, progId);
    std::snprintf(xmlPath, sizeof(xmlPath), "%s\\elmodule_%d_%d.xml",
                  kMyWorkBin, whType, progId);
    TSDEV_DBG("tsdev: deleteEntry — deleting %s and %s\n", ellPath, xmlPath);
    DeleteFileA(ellPath);
    DeleteFileA(xmlPath);

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
    // 1. Look up the existing component by name + type.
    int progId = findExistPos(name, type);
    TSDEV_DBG("tsdev: modifyEntry — findExistPos(\"%s\", %d) = %d\n",
              name.c_str(), static_cast<int>(type), progId);
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

bool TsDevLib::applyPersistencePatch() {
    void* patchAddr = reinterpret_cast<void*>(
        m_orlibBase + kRvaPersistencePatch);
    DWORD oldProtect = 0;
    if (!VirtualProtect(patchAddr, sizeof(kPersistencePatchBytes),
                        PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    std::memcpy(patchAddr, kPersistencePatchBytes,
                sizeof(kPersistencePatchBytes));
    DWORD dummy = 0;
    VirtualProtect(patchAddr, sizeof(kPersistencePatchBytes),
                   oldProtect, &dummy);
    return true;
}

bool TsDevLib::applyThreadCheckPatches() {
    // Patch three INT3 (0xCC) instructions to NOP (0x90) so the
    // dispatch helpers can be called from any thread.
    const DWORD_PTR patchRvas[] = {
        kRvaThreadCheck1,  // FUN_101d0950 (command 0x0F)
        kRvaThreadCheck2,  // FUN_101cf5b0 (general dispatch)
        kRvaThreadCheck3,  // FUN_101d1fa0 (command 0x17)
    };
    for (DWORD_PTR rva : patchRvas) {
        void* addr = reinterpret_cast<void*>(m_orlibBase + rva);
        DWORD oldProtect = 0;
        if (!VirtualProtect(addr, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            std::fprintf(stderr, "tsdev: applyThreadCheckPatches — "
                         "VirtualProtect failed at RVA 0x%lX (err=%lu)\n",
                         (unsigned long)rva, GetLastError());
            fflush(stderr);
            return false;
        }
        // Verify the byte is either unpatched (0xCC) or already patched (0x90).
        unsigned char* p = static_cast<unsigned char*>(addr);
        if (*p == 0x90) {
            // Already patched — idempotent, skip.
            std::fprintf(stderr, "tsdev: applyThreadCheckPatches — "
                         "RVA 0x%lX already patched (0x90), skipping\n",
                         (unsigned long)rva);
            fflush(stderr);
            DWORD dummy = 0;
            VirtualProtect(addr, 1, oldProtect, &dummy);
            continue;
        }
        if (*p != 0xCC) {
            // Unexpected byte — abort patching to avoid corrupting code.
            std::fprintf(stderr, "tsdev: applyThreadCheckPatches — "
                         "ERROR: byte at RVA 0x%lX is 0x%02X, expected 0xCC or 0x90\n",
                         (unsigned long)rva, (unsigned)*p);
            fflush(stderr);
            DWORD dummy = 0;
            VirtualProtect(addr, 1, oldProtect, &dummy);
            return false;
        }
        *p = 0x90;  // NOP
        DWORD dummy = 0;
        VirtualProtect(addr, 1, oldProtect, &dummy);
        std::fprintf(stderr, "tsdev: applyThreadCheckPatches — patched RVA 0x%lX\n",
                     (unsigned long)rva);
        fflush(stderr);
    }
    return true;
}

} // namespace tsdev
