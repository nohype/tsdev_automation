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

#include <stdexcept>
#include <cstring>
#include <cstdio>
#include <string>

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

// Persistence patch (§12): bypass FUN_10011230 (SymWriter crash).
//   MOV EAX, 1   (5 bytes)  B8 01 00 00 00
//   RET 0x0C     (3 bytes)  C2 0C 00
// Patch-site resolution is now handled by OrlibResolver::applyPatch
// (TVS-B.5). These byte arrays are retained as patch CONTENT (not RVAs)
// for use by validatePersistencePatchBytes() and tests.
const unsigned char kPersistencePatchBytes[] = {
    0xB8, 0x01, 0x00, 0x00, 0x00,  // MOV EAX, 1
    0xC2, 0x0C, 0x00               // RET 0x0C
};

// Original 8 bytes at the persistence patch site (FUN_10011230 entry, pre-patch).
// Bytes 6-7 are the low 2 bytes of the SEH handler address (image-base-
// relative, subject to ASLR relocation) and are wildcarded in validation.
const unsigned char kPersistenceOriginalBytes[] = {
    0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0xCD, 0xD3
};
const unsigned char kPersistenceOriginalMask[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00  // 1=check, 0=wildcard
};

// TD-001: tracks the most recently created warehouse entry so verifyModule
// can auto-clean it up on compile failure. Set by createEntry, consumed and
// cleared by verifyModule, and cleared at the start of modifyEntry (modify
// operates on an existing entry and must never delete it on a compile
// failure). TsDevLib is thread-affine and used one instance per process, so
// file-local state is safe here.
struct LastCreatedEntry {
    int  progId   = 0;
    int  category = 0;
    char name[128] = {};  // Fixed-size buffer (component names are short);
                          // std::string would require C++ unwinding, which is
                          // incompatible with the __try/__except in createEntry.
} g_lastCreated;

// Thread-check bypass patches: NOP the INT3 (0xCC → 0x90) in three
// dispatch helpers so they can be called from any thread, not just the
// worker thread. Each patch is a single byte.
//
// Patch-site resolution is now handled by OrlibResolver::applyPatch
// (TVS-B.5) using signature names:
//   FUN_101d0950_thread_check_INT3 (command 0x0F — load program)
//   FUN_101cf5b0_thread_check_INT3 (general dispatch helper)
//   FUN_101d1fa0_thread_check_INT3 (command 0x17 — save/commit)
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

// Restore `len` original bytes at an executable address. Used by the
// TsDevLib destructor to undo the persistence + thread-check patches so a
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

TsDevLib::TsDevLib(const wchar_t* tsPathOverride) {
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

    // 10. Resolve 3 data-global RVAs via resolver (replaces hardcoded
    //    error-array/count/HINSTANCE RVA constants).
    m_errArrayPtrRva = m_resolver->resolveErrorArrayPtrRva();
    m_errCountRva    = m_resolver->resolveErrorCountRva();
    m_errHinstRva    = m_resolver->resolveErrorHinstRva();

    // 11. Apply patches via existing private methods (which delegate to
    //    m_resolver->applyPatch). OrlibResolverError propagates out of
    //    the constructor on validation or VirtualProtect failure (AC6).
    applyPersistencePatch();
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
    // Restore all 4 patch sites to their original bytes AFTER OrlibExit but
    // BEFORE m_resolver (unique_ptr) is destroyed — the resolver still owns
    // the loaded orlib20.dll and its base address, so baseAddress() is valid
    // here. This returns the in-memory DLL to its original state so that a
    // subsequent TsDevLib instance in the same process can re-scan the
    // original-byte signatures (the persistence patch signature scans for
    // 55 8B EC 6A FF 68...; the thread-check signatures scan for 0xCC).
    // Without this restoration the second instance's scan finds the patched
    // bytes (B8 01 00 00 00 C2 0C 00 / 0x90) and throws OrlibResolverError.
    if (m_patchesApplied) {
        uintptr_t base = m_resolver->baseAddress();
        // Persistence patch: restore 8 original bytes. Guarded by the cached
        // RVA so a site whose resolve threw before caching (RVA still 0) is
        // skipped — restoring to base+0 would corrupt the DLL header.
        if (m_persistencePatchRva != 0) {
            restoreBytes(reinterpret_cast<void*>(base + m_persistencePatchRva),
                         m_persistenceOriginalBytes, 8);
        }
        // Thread-check patches: restore 1 original byte (0xCC) each. Only
        // restore sites whose RVA was actually cached before a possible
        // throw; for a site where applyPatch threw after the RVA was cached,
        // writing the original byte back is a harmless no-op (the patch was
        // never written).
        for (int i = 0; i < 3; i++) {
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
    if (progId > 0) {
        g_lastCreated.progId = progId;
        g_lastCreated.category = getCategory(type);
        strncpy_s(g_lastCreated.name, sizeof(g_lastCreated.name),
                  name.c_str(), _TRUNCATE);
    } else {
        g_lastCreated = {};
    }
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
    }

    // EndCompilingProcess (fastcall, 1 arg = 0).
    callEndCompiling(reinterpret_cast<EndCompilingFn>(m_pEndCompiling), 0);

    // Destroy the CELCompiler object.
    callDtor(reinterpret_cast<CELCompilerDtorFn>(m_pCELCompilerDtor),
             compilerMem);
    std::free(compilerMem);

    // TD-001: auto-cleanup a freshly-created entry on compile failure. Only
    // fires when this verifyModule targets the entry most recently created by
    // createEntry (and not since cleared), so modifyEntry and standalone
    // verify calls are unaffected. Best-effort: cleanup failures are swallowed
    // so they never mask the original compile error.
    if (!errors.empty() && g_lastCreated.progId == progId &&
        g_lastCreated.category == getCategory(type)) {
        std::fprintf(stderr,
            "tsdev: compile failed — cleaning up partial warehouse entry "
            "\"%s\" (ProgId=%d)\n", g_lastCreated.name, progId);
        try {
            deleteEntry(type, g_lastCreated.name);
        } catch (...) {
        }
        g_lastCreated = {};
    } else if (errors.empty()) {
        g_lastCreated = {};
    }

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

bool TsDevLib::deleteEntry(ComponentType type, const std::string& name) {
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
    TSDEV_DBG("tsdev: deleteEntry — calling closeWarehouse()\n");
    closeWarehouse();
    TSDEV_DBG("tsdev: deleteEntry — closeWarehouse() done\n");

    // 5. Delete the .ell and .xml files from disk.
    char ellPath[512];
    char xmlPath[512];
    std::snprintf(ellPath, sizeof(ellPath), "%s\\elmodule_%d_%d.ell",
                  m_myWorkBin.c_str(), expandedWhType, progId);
    std::snprintf(xmlPath, sizeof(xmlPath), "%s\\elmodule_%d_%d.xml",
                  m_myWorkBin.c_str(), expandedWhType, progId);
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
    g_lastCreated = {};
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

bool TsDevLib::validatePersistencePatchBytes(const unsigned char* expected,
                                              const unsigned char* mask, size_t len) {
    // Use the cached persistence patch RVA (resolved in applyPersistencePatch
    // BEFORE the original bytes were overwritten). We must NOT re-scan the
    // signature here: resolvePersistencePatch() searches for the ORIGINAL
    // unpatched bytes, which have already been replaced by the patch — a
    // post-patch scan finds 0 matches and throws OrlibResolverError.
    if (m_persistencePatchRva == 0) {
        std::fprintf(stderr, "tsdev: validatePersistencePatchBytes — "
                     "ERROR: persistence patch RVA was not cached (constructor failed?)\n");
        fflush(stderr);
        return false;
    }
    const unsigned char* p = reinterpret_cast<const unsigned char*>(
        m_resolver->baseAddress() + m_persistencePatchRva);
    for (size_t i = 0; i < len; i++) {
        if (mask[i] && p[i] != expected[i]) {
            std::fprintf(stderr, "tsdev: validatePersistencePatchBytes — "
                         "ERROR: byte at RVA 0x%llX offset %zu is 0x%02X, expected 0x%02X\n",
                         (unsigned long long)m_persistencePatchRva, i,
                         (unsigned)p[i], (unsigned)expected[i]);
            fflush(stderr);
            return false;
        }
    }
    return true;
}

void TsDevLib::applyPersistencePatch() {
    // Cache the persistence patch RVA BEFORE applying the patch. The
    // signature scan in resolvePersistencePatch() searches for the ORIGINAL
    // unpatched bytes (55 8B EC 6A FF 68...); once applyPatch() overwrites
    // them with B8 01 00 00 00 C2 0C 00, a later re-scan finds 0 matches and
    // throws OrlibResolverError. validatePersistencePatchBytes() relies on
    // this cached RVA to read the patched bytes without re-scanning.
    PatchSiteDescriptor desc = m_resolver->resolvePersistencePatch();
    m_persistencePatchRva = desc.patchRva;

    // Save the ACTUAL 8 original bytes at the patch site BEFORE overwriting
    // them. kPersistenceOriginalBytes wildcards out positions 6-7 (the low
    // 2 bytes of the SEH handler address, which are ASLR-relocated), so it
    // cannot be used to restore — we need the real on-disk bytes. The
    // destructor writes these back so a second TsDevLib instance in the same
    // process can re-scan the original-byte signature successfully.
    const unsigned char* site = reinterpret_cast<const unsigned char*>(
        m_resolver->baseAddress() + m_persistencePatchRva);
    std::memcpy(m_persistenceOriginalBytes, site, 8);

    // Delegates to the resolver's applyPatch, which handles:
    //   resolvePatchSite → validatePatchSite (throw on mismatch)
    //   → idempotency check → VirtualProtect → memcpy → restore.
    // OrlibResolverError propagates to the constructor (AC6 diagnostics).
    m_resolver->applyPatch("FUN_10011230_persistence_patch");
    // Mark applied immediately so the destructor restores this patch even
    // if a subsequent thread-check patch throws (partial-construction gap,
    // TD-002). The destructor guards each site by its cached RVA.
    m_patchesApplied = true;
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

    PatchSiteDescriptor tc2 = m_resolver->resolveThreadCheck2();
    m_threadCheckRvas[1] = tc2.patchRva;
    m_threadCheckOriginalBytes[1] = *reinterpret_cast<const unsigned char*>(
        m_resolver->baseAddress() + m_threadCheckRvas[1]);
    m_resolver->applyPatch("FUN_101cf5b0_thread_check_INT3");
    m_patchesApplied = true;

    PatchSiteDescriptor tc3 = m_resolver->resolveThreadCheck3();
    m_threadCheckRvas[2] = tc3.patchRva;
    m_threadCheckOriginalBytes[2] = *reinterpret_cast<const unsigned char*>(
        m_resolver->baseAddress() + m_threadCheckRvas[2]);
    m_resolver->applyPatch("FUN_101d1fa0_thread_check_INT3");
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
