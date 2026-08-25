// tsdev_lib.h
//
// Shared wrapper module for orlib20.dll — the "engine" for the tsdev_automation
// CLI. Encapsulates the confirmed orlib20 operation set (init, name lookup,
// entry creation, source save, compile/verify, warehouse close) behind a clean,
// RAII C++ API.
//
// All findings are CONFIRMED and documented in
// tmp/re_artifacts/orlib_param_orders.md (gitignored). The reference probe is
// archive/elevated_test20.cpp.
//
// This module is 32-bit only (orlib20.dll is a 32-bit PE).

#ifndef TSDEV_LIB_H
#define TSDEV_LIB_H

#include <string>
#include <memory>
#include <windows.h>

#include "create_path_cleanup.h"  // CreatePathFailureStage (TVS-L.10 verifyModule out-param)

class ComApartmentGuard;   // forward declaration (global namespace, matches com_apartment_guard.h)

// TVS-L.13 test-only construction seam (GREEN — implemented).
//
// TsDevLibTestConfig is an OPAQUE, forward-declared struct from the point of
// view of this PUBLIC header. Its definition lives ONLY in the internal/test-
// only header src/tsdev_lib_test_hooks.h, which is NOT included here and is
// NOT part of the public user API. The definition carries the injected COM
// API (ComApi) and a flag that simulates a later-constructor exception
// immediately after a successful COM init — but NONE of those COM details
// appear in this public header, so no COM configuration (ComApi,
// CoInitializeEx function-pointer types, <objbase.h>) leaks into the public
// user API. Users see only an opaque struct name and a PRIVATE constructor
// they cannot call.
struct TsDevLibTestConfig;  // defined in src/tsdev_lib_test_hooks.h (test-only)

namespace tsdev {

class OrlibResolver;       // forward declaration (defined in orlib_resolver.h)

// Confirmed warehouse types (see orlib_param_orders.md §1 and
// tradingapp_whtype.md). The enum value is the EXPANDED whType
// (= (subType << 8) | category). For types with sub-type 0
// (Strategy, Indicator, Function), expanded whType == category.
// For TradingApp, expanded whType (5122) != category (2).
enum class ComponentType : int {
    Strategy    = 0,
    Indicator   = 2,
    Function    = 4,
    TradingApp  = 5122,  // 0x1402 = (subType 0x14 << 8) | category 2
    ShowMe      = 1282   // 0x0502 = (subType 0x05 << 8) | category 2
};

// Decompose an expanded whType (the enum value) into its category and
// sub-type components. The orlib20 API functions take the CATEGORY whType
// (low 8 bits), while the .ell filename uses the full expanded whType.
// For Strategy/Indicator/Function (sub-type 0), category == expanded whType.
// For TradingApp (sub-type 0x14), category (2) != expanded whType (5122).
inline int getCategory(ComponentType type) {
    return static_cast<int>(type) & 0xFF;
}
inline int getSubType(ComponentType type) {
    return static_cast<int>(type) >> 8;
}

// Generic operation result. For compile/verify, prefer verifyModule()'s
// returned error string as the authoritative success indicator.
struct Result {
    bool        success = false;
    std::string message;
    int         progId  = 0;  // valid when success is true (where applicable)
};

// Result of a case-insensitive enumeration-based existence query.
struct EnumerateResult {
    bool        exists = false;
    std::string storedName;  // verbatim stored name (empty if not found)
    int         progId  = 0; // ProgId (0 if not found)
};

// RAII wrapper for an orlib20 session.
//
// Constructor:  loads orlib20.dll, pre-loads support DLLs, applies the
//               thread-check INT3 patches, resolves exports, calls
//               OrlibInit(NULL, 1), creates a hidden EDIT control.
//               Throws std::runtime_error on fatal failure.
// Destructor:   calls OrlibExit(), destroys the EDIT control.
//
// Not copyable, not movable (owns DLL handles and live window objects).
//
// Same-thread contract (HARD, TVS-L.2):
//   TsDevLib must be constructed, used, and destroyed on a single thread.
//   The COM guard (m_comGuard) initializes STA COM on the owning thread via
//   CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED) as the very first
//   effective action of the constructor — BEFORE any SetDllDirectoryW,
//   LoadLibraryW, OrlibResolver construction, support-DLL load, patch
//   application, or OrlibInit — and balances it with CoUninitialize as the
//   last effective action of the destructor — AFTER OrlibExit, hidden-window
//   destruction, and patch restoration. CoInitializeEx / CoUninitialize are
//   thread-affine: balancing COM on a different thread from the one that
//   entered the apartment is undefined behavior. The owning TsDevLib is
//   non-movable and lives on a single 64 MiB worker thread; the guard shares
//   that lifetime. If COM initialization fails (any HRESULT other than S_OK
//   or S_FALSE, including RPC_E_CHANGED_MODE), the constructor throws with
//   the HRESULT in the message and performs NO orlib loading, NO window
//   creation, NO entry creation, and NO ELL/XML mutation.
class TsDevLib {
public:
    explicit TsDevLib(const wchar_t* tsPathOverride = nullptr);
    ~TsDevLib();

    TsDevLib(const TsDevLib&)            = delete;
    TsDevLib& operator=(const TsDevLib&) = delete;
    TsDevLib(TsDevLib&&)                 = delete;
    TsDevLib& operator=(TsDevLib&&)      = delete;

    // --- TVS-L.13 test-only construction seam (GREEN — implemented) ---
    // PRIVATE constructor + friend factory. This is NOT part of the public
    // user API: it is private, and the only caller intended is the test-only
    // free function tsdev::makeTsDevLibForTest(...) declared in the INTERNAL
    // header src/tsdev_lib_test_hooks.h (NOT a public header). The parameter
    // type TsDevLibTestConfig is forward-declared above and defined in the
    // internal header, so this declaration does NOT pull ComApi / COM headers
    // into the public API.
    //
    // Contract (IMPLEMENTED in src/tsdev_lib.cpp; enforced by
    // tests/test_tsdev_com_init_seam.cpp):
    //   - Constructs the COM apartment guard with the injected ComApi
    //     (config.comApi) instead of the default real CoInitializeEx.
    //   - If the injected CoInitializeEx returns anything other than S_OK /
    //     S_FALSE (e.g. RPC_E_CHANGED_MODE, E_FAIL), throws std::runtime_error
    //     with the HRESULT in the message and performs NO SetDllDirectoryW,
    //     NO orlib20 load, NO patch application, NO OrlibInit, and NO warehouse
    //     access — COM failure is a hard stop before every later step.
    //   - If config.throwAfterCom is true AND COM init succeeded, throws
    //     std::runtime_error immediately after the COM guard is constructed
    //     (simulating a later-constructor exception). The COM guard's
    //     destructor runs during stack unwinding and balances COM exactly
    //     once (calls the injected CoUninitialize once). No orlib20 / patch /
    //     warehouse side effect runs.
    //   - Otherwise (COM success, throwAfterCom == false) proceeds with the
    //     normal constructor body (SetDllDirectoryW, OrlibResolver, support
    //     DLLs, patches, OrlibInit, hidden EDIT control) — identical to the
    //     public constructor above.
    explicit TsDevLib(const wchar_t* tsPathOverride,
                      const TsDevLibTestConfig& config);

    // Friend the test-only factory so it can reach the private constructor.
    // The factory is declared in src/tsdev_lib_test_hooks.h and is the SOLE
    // intended caller of the private ctor. Users have no path to it.
    friend std::unique_ptr<TsDevLib> makeTsDevLibForTest(
        const wchar_t* tsPathOverride,
        const TsDevLibTestConfig& config);

    // Look up an existing component by name.
    // Returns ProgId (>0) if found, 0 if not found.
    int findExistPos(const std::string& name, ComponentType type);

    // Create a new entry in the warehouse.
    // Returns ProgId on success, 0 on failure (inspect errCode for detail).
    int createEntry(ComponentType type, const std::string& name, int& errCode);

    // Save source text into an entry (does NOT persist to disk).
    // Sets the source into the hidden EDIT control and calls SaveModule_New.
    // isVerified: true when re-saving an already-verified item (modify path);
    //   false for new items (insert path). Controls flags bit 4 (is_verified)
    //   per TSDev FUN_00452290. See docs/RE/RE_M2_SAVEMODULE_NEW.md.
    // Returns true on success.
    bool saveSource(ComponentType type, int progId, const std::string& source,
                    bool isVerified = false);

    // Retrieves EasyLanguage source text for an existing item by name + type.
    // Internally encrypted items are decrypted automatically, matching TSDev.
    // Returns the source text as a string. Returns empty string on failure
    // (not found or internal error).
    std::string getSource(ComponentType type, const std::string& name);

    // Case-insensitive existence query. Uses a two-phase approach:
    //   Phase 1: _LoadMasterRecsEX@16 (COM interface cache) — works for
    //     records that existed before this process started. Searches
    //     records case-insensitively via _stricmp.
    //   Phase 2: FUN_101d05e0 (BST search wrapper, __thiscall:
    //     ECX=warehouse, stack=(typeFilter, name, *outNode); RET 0xC) +
    //     FUN_101c36d0 (verbatim name getter, __fastcall: ECX=node pointer,
    //     no stack args; RET — returns char* from the warehouse node at
    //     offset +0x1D). FUN_101d05e0 finds the BST node for the item name;
    //     FUN_101c36d0 reads the verbatim stored name directly from that
    //     node. (FUN_101a42d0 was the original candidate but was found to
    //     return the ProgId — not the verbatim name — in non-server mode,
    //     so it cannot be used to get the verbatim name; it is retained as
    //     m_pFindItemGetName for reference only.) The stored name comes
    //     from the WAREHOUSE, not from source text (HARD RULE 0).
    //     ProgId is obtained separately via FindExistPos.
    // For TradingApp (category 2, subtype 0x14), Phase 1 filters by
    // rec[0xE9].
    EnumerateResult enumerateRecords(ComponentType type,
                                     const std::string& searchName);

    // Compile and verify an entry. Persists the .ell file on success.
    // Creates a CELCompiler object, sets its fields, calls
    // StartCompilingProcess + VerifyModule_New + EndCompilingProcess, then
    // destroys the compiler object.
    // Returns the compilation error string (empty string == success).
    // Uses the internal hidden EDIT control (m_editorHwnd) — the same
    // control saveSource uses — for both VerifyModule_New and the
    // CELCompiler +0x18 field.
    //
    // TVS-L.10: verifyModule no longer owns create-path partial-entry
    // cleanup (that was the TD-001 incidental !errors.empty() branch keyed
    // off the process-global g_lastCreated, which is now removed). The
    // SINGLE create-path cleanup owner is doInsert in the CLI, which calls
    // requiresCreatePathCleanup() after every post-createEntry failure.
    // To let doInsert report WHICH verify-internal path failed (and to make
    // coverage of every failure mode explicit), verifyModule optionally
    // writes the failure stage to `*outStage`:
    //   * nullptr (default) — stage not reported (used by modifyEntry, which
    //     operates on an existing entry and must never trigger create-path
    //     cleanup).
    //   * non-null  — set to the matching CreatePathFailureStage enumerator
    //     on every non-empty (failure) return path: OOM, SEH in ctor /
    //     StartCompiling / VerifyModule_New, and compile errors. Left
    //     untouched on the success (empty-string) return.
    std::string verifyModule(ComponentType type, int progId,
                             CreatePathFailureStage* outStage = nullptr);

    // Refresh the warehouse cache so that the cached _LoadMasterRecsEX@16
    // enumeration sees items created/modified/deleted in the same process.
    // If [warehouse+0x5E] (COM interface) is NULL, dispatches cmd 0x0F to
    // initialize it. Then sets [warehouse+0x12] = 0xC and dispatches
    // cmd 0x4E via FUN_101cf5b0.
    // Defensive: skips silently on any failure. SEH-protected. Should be
    // called before enumerateRecords().
    void refreshCache();

    // Close/flush the warehouse for the current thread. Safe to call once at
    // the end of a batch of operations; the destructor does NOT call this.
    // NOTE: When called with NULL (0), the warehouse may not actually be
    // closed due to an internal path-mismatch in orlib20. This is acceptable
    // for the delete flow — the deletion is persisted via OrlibExit (in the
    // destructor) and the in-memory BST is updated via DeleteFromTree.
    void closeWarehouse();

    // Delete an existing component. Returns true if deleted, false if not
    // found OR if a delete-path disk-cleanup failure occurred (TVS-L.12). The
    // flow (confirmed via Frida trace of live TSDev.exe):
    //   FindExistPos(name, whType) →
    //   FUN_101a8360 (delete orchestrator: __fastcall ECX=whType, EDX=progId)
    //     — loads program, marks deleted, runs CELProgramWriter cleanup,
    //       sends command 0x4E (cache reset) and command 0x17 (save/commit) →
    //   InitGlobalLookup → DeleteFromTree(globalLookup, whType, progId, 3) →
    //   closeWarehouse (best-effort flush) →
    //   OrlibExit (in destructor, persists the deletion) →
    //   delete .ell/.xml files via Windows API.
    //
    // TVS-L.12: step 5 (delete .ell/.xml) consults
    // deleteDiskCleanupErrorFailsResult() for EACH DeleteFileA call. A real
    // removal failure (e.g. ERROR_ACCESS_DENIED — artifact present but could
    // not be removed) now makes deleteEntry return false instead of
    // unconditionally returning true. ERROR_SUCCESS and ERROR_FILE_NOT_FOUND
    // remain non-failures (idempotent absence is not a failure). When false is
    // returned due to a disk-cleanup failure, the orchestrator has ALREADY run
    // (the warehouse entry was removed); only the on-disk artifact(s) remain.
    //
    // `diskCleanupErrorOut` (optional, may be nullptr): when provided and
    // deleteEntry returns false due to a disk-cleanup failure, it is populated
    // with a human-readable description of the residual artifact(s) — the path
    // and the Windows error code for each DeleteFileA that failed the
    // decision. It is left untouched for non-cleanup failures (not found /
    // orchestrator SEH) so a caller can distinguish "warehouse entry removed,
    // disk cleanup failed" (non-empty) from an earlier failure (empty). This
    // mirrors the existing createEntry(..., int& errCode) out-parameter
    // pattern; it is a diagnostic channel only and does not introduce a new
    // error code or hierarchy.
    bool deleteEntry(ComponentType type, const std::string& name,
                     bool flush = true,
                     std::string* diskCleanupErrorOut = nullptr);

    // Modify an existing component's EasyLanguage source and recompile/re-verify.
    // Flow (reuses the existing INSERT primitives directly — no new orlib20
    // calls, no new RVAs):
    //   findExistPos(name, type) → lookup existing ProgId
    //     - if not found (progId <= 0): return Result{success=false,
    //       message="component not found"}
    //   saveSource(type, progId, newSource) → replace source in the existing
    //     entry (the ProgId does NOT change — no createEntry)
    //   verifyModule(type, progId) → recompile + reverify + persist .ell
    //     - if verifyModule returns a non-empty error string (compilation
    //       failed): return Result{success=false, message=<error>,
    //       progId=progId} — fail loudly with the error
    //     - if verifyModule returns an empty string (success): return
    //       Result{success=true, message="", progId=progId}
    // The .ell file is updated on disk via verifyModule's existing
    // persistence path (the real finalization path runs end-to-end).
    Result modifyEntry(ComponentType type, const std::string& name,
                       const std::string& newSource);

    // Check whether the .ell file for a given entry exists on disk.
    bool ellFileExists(ComponentType type, int progId);

    // Check whether the .xml (debug symbols) file for a given entry exists on disk.
    bool xmlFileExists(ComponentType type, int progId);

    // Return the full path of the .ell file for a given entry.
    std::string getEllPath(ComponentType type, int progId);

    // Return the full path of the .xml (debug symbols) file for a given entry.
    std::string getXmlPath(ComponentType type, int progId);

    // Return the discovered MyWork\Bin path (narrow, for .ell/.xml file paths).
    const char* getMyWorkBinPath() const;

    // Return the discovered MWRKArea.wrk path (narrow, for debug logging / tests).
    const char* getWrkPath() const;

private:
    // --- COM apartment guard (TVS-L.2) ---
    // Declared FIRST among the members that participate in the init/teardown
    // sequence so it is constructed first (CoInitializeEx runs before any
    // SetDllDirectoryW / LoadLibraryW / OrlibResolver / patch / OrlibInit)
    // and destructed LAST (CoUninitialize runs after the destructor body's
    // OrlibExit + hidden-window destruction + patch restoration, and after
    // every other member's destructor — members are destructed in reverse
    // declaration order). PIMPL via unique_ptr so the ComApartmentGuard
    // definition (com_apartment_guard.h) is not required in this header.
    // The default ComApi binds the real CoInitializeEx / CoUninitialize.
    std::unique_ptr<ComApartmentGuard> m_comGuard;

    // --- OrlibResolver (TVS-B.5) ---
    std::unique_ptr<OrlibResolver> m_resolver;  // owns the DLL load + all resolution
    void*      m_pDeleteOrchestrator = nullptr;  // Target 8, resolved by signature scan
    uintptr_t  m_errArrayPtrRva      = 0;        // data global, resolved by instruction extraction
    uintptr_t  m_errCountRva         = 0;        // data global, resolved by instruction extraction
    uintptr_t  m_errHinstRva         = 0;        // data global, resolved by instruction extraction
    uintptr_t  m_threadCheckRvas[11] = {};       // RVAs of the 11 thread-check patch sites,
                                                 // cached BEFORE patching for restoration.
    unsigned char m_threadCheckOriginalBytes[11] = {};  // original byte (0xCC) at each
                                                       // thread-check site, saved BEFORE
                                                       // patching for restoration.
    bool       m_patchesApplied = false;         // true once at least one patch has
                                                 // been applied; set after EACH
                                                 // successful patch (TD-002) so the
                                                 // destructor restores whatever was
                                                 // actually applied. Each restoration
                                                 // is guarded by its cached RVA.
    bool       m_orlibInitialized = false;       // true once OrlibInit succeeds; gates
                                                 // OrlibExit in the destructor so a
                                                 // partially-constructed instance does not
                                                 // call OrlibExit without a prior init.

    HWND       m_editorHwnd   = nullptr;  // hidden EDIT control for source text
    HWND       m_editorParent = nullptr;  // STATIC parent of the EDIT control
    std::string m_lastSource;             // source saved by saveSource (for error line/col computation)

    // --- TS install path (TVS-B.7) ---
    std::wstring m_tsRoot;       // normalized install root, e.g. C:\...\TradeStation 9.5
    std::wstring m_tsProgramDir; // <root>\Program (wide, for SetDllDirectoryW)
    std::string  m_myWorkBin;    // <root>\MyWork\Bin (narrow, for .ell/.xml paths)
    std::string  m_wrkPath;      // <root>\MyWork\MWRKArea.wrk (narrow, for debug logging)

    // --- Resolved orlib20 exports (all __stdcall, decorated) ---
    typedef BOOL (__stdcall *OrlibInit_t)(const char* workDirPath, int contextId);
    typedef BOOL (__stdcall *OrlibExit_t)(void);
    typedef int  (__stdcall *FindExistPos_t)(const char* name, int warehouseType);
    typedef int  (__stdcall *CreateEntryEX_t)(int whType, int secondary,
                                              const char* name, int param4,
                                              char param5, int* errCode);
    typedef int  (__stdcall *SaveModule_New_t)(int warehouseType, int progId,
                                               void* arg3, HWND editorHwnd,
                                               void* arg5);
    typedef int  (__stdcall *VerifyModule_New_t)(HWND hwnd, int warehouseType,
                                                 int progId, int flag,
                                                 void* compilerObj);
    typedef void (__stdcall *CloseWarehouse_t)(int param);
    typedef HGLOBAL (__stdcall *LoadTextFileExFn)(void* unused, int whType,
                                                  int progId, void* password);
    typedef int (__stdcall *LoadMasterRecsExFn)(int category, HGLOBAL* outBuf,
                                                const char* path, int filterMode);
    // _GetMasterRecByType@16 — __stdcall, 4 params (category, subtype,
    // index=ProgId, outBuf). Returns 1 on success (record found and copied
    // to outBuf), 0 on failure (not found or warehouse unavailable).
    // The record is 0x10A bytes with the stored name at offset 0x17.
    // Dispatches cmd 0x0D via FUN_101d15c0 (local mode) — requires the
    // FUN_101d15c0_thread_check_INT3 patch.
    typedef int (__stdcall *GetMasterRecByTypeFn)(int category, int subType,
                                                  int index, void* outBuf);

    OrlibInit_t          m_pOrlibInit       = nullptr;
    OrlibExit_t          m_pOrlibExit       = nullptr;
    FindExistPos_t       m_pFindExistPos    = nullptr;
    CreateEntryEX_t      m_pCreateEntry     = nullptr;
    SaveModule_New_t     m_pSaveModuleNew   = nullptr;
    VerifyModule_New_t   m_pVerifyModule    = nullptr;
    CloseWarehouse_t     m_pCloseWarehouse  = nullptr;
    LoadTextFileExFn     m_loadTextFileEx   = nullptr;
    LoadMasterRecsExFn   m_loadMasterRecsEx = nullptr;
    GetMasterRecByTypeFn m_getMasterRecByType = nullptr;

    // --- Resolved internal functions (by RVA from orlib20 base) ---
    // RVAs are resolved at runtime via signature scan; see
    // signatures/v9.50.01.3344.json for the per-target RVAs.
    // StartCompilingProcess: __cdecl, no args
    typedef void (__cdecl *StartCompilingProcess_t)(void);
    // EndCompilingProcess: __fastcall, 1 arg
    typedef void (__fastcall *EndCompilingProcess_t)(int param);
    // Workstream M slice M.1: CELCompiler alloc+ctor and dtor+free are now
    // resolved by orlib20 ORDINAL (66 / 67), not by signature scan of the
    // raw ctor/dtor. The typedefs are declared locally here (structurally
    // identical to tsdev::CELCompilerAllocFn / tsdev::CELCompilerDestroyFn
    // in orlib_resolver.h — function pointer types with the same signature
    // convert implicitly). See tmp/m1_ghidra_findings.md §1 (R-M.1.1).
    //   Ordinal 66: __cdecl, no args, returns void* (alloc + construct).
    //   Ordinal 67: __stdcall, one arg (void* p), returns void (dtor + free).
    typedef void* (__cdecl *CELCompilerAlloc_t)(void);
    typedef void  (__stdcall *CELCompilerDestroy_t)(void*);
    // Workstream M slice M.3: 0x40A library-change notification sender
    // resolved by orlib20 ORDINAL 57 (FUN_101526f0). __stdcall, RET 8
    // (2 dword args): (unsigned short category, unsigned short progId).
    // Internally packs lParam=(category<<16)|(progId&0xFFFF), wParam=0,
    // msg=0x40A, calls tslib!PosMessageToClients. Always returns 1 (the
    // return value is NOT a delivery guarantee — see
    // docs/RE/RE_M3_0x40A_NOTIFICATION.md §3.1). The typedef is declared
    // locally here (structurally identical to tsdev::NotifyLibChangedFn in
    // orlib_resolver.h). See docs/RE/RE_M3_0x40A_NOTIFICATION.md §1.2.
    typedef void (__stdcall *NotifyLibChanged_t)(unsigned short category,
                                                 unsigned short progId);
    // InitGlobalLookup: __cdecl, no args
    // Returns pointer to the global lookup object (used as `this` for
    // DeleteFromTree). Confirmed via Frida trace.
    typedef void* (__cdecl *InitGlobalLookup_t)(void);
    // DeleteFromTree: __thiscall (ecx=globalLookup, stack: whType, progId,
    // flag). Internally calls RemoveMetaDataModules when
    // flag != 0. MSVC C++ has no direct __thiscall function-pointer type, so
    // this is stored as void* and invoked via inline asm in a POD-only helper.
    // (See callDeleteFromTree in the .cpp.)
    // GetWarehouse: __cdecl, 1 arg (path, 0=default).
    // Returns pointer to the per-thread warehouse object.
    // Used by FindExistPos and _GetMasterRecByName internally.
    typedef void* (__cdecl *GetWarehouse_t)(int pathParam);

    StartCompilingProcess_t m_pStartCompiling = nullptr;
    EndCompilingProcess_t   m_pEndCompiling   = nullptr;
    // Workstream M slice M.1: CELCompiler alloc/destroy resolved by ordinal
    // (66/67) instead of signature scan. Stored as the local typedefs
    // (CELCompilerAlloc_t / CELCompilerDestroy_t), which are structurally
    // identical to tsdev::CELCompilerAllocFn / tsdev::CELCompilerDestroyFn.
    CELCompilerAlloc_t      m_pCELCompilerAlloc   = nullptr;
    CELCompilerDestroy_t    m_pCELCompilerDestroy = nullptr;
    // Workstream M slice M.3: 0x40A notification sender resolved by ordinal
    // (57). Stored as the local typedef (NotifyLibChanged_t), which is
    // structurally identical to tsdev::NotifyLibChangedFn. nullptr in
    // production if ordinal 57 resolution failed (notification silently
    // skipped — see docs/RE/RE_M3_0x40A_NOTIFICATION.md §3.1).
    NotifyLibChanged_t      m_pNotifyLibChanged   = nullptr;
    InitGlobalLookup_t      m_pInitGlobalLookup = nullptr;
    void*                   m_pDeleteFromTree   = nullptr;
    GetWarehouse_t          m_pGetWarehouse     = nullptr;
    // FUN_101cf5b0 — the generic command dispatcher (__thiscall:
    // ECX=warehouse, stack=commandId). Already INT3-patched at offset 36.
    // Stored as void* and invoked via inline asm in a POD-only helper
    // (callCmdDispatcher) because MSVC has no direct __thiscall function-
    // pointer type. Used by refreshCache() to dispatch cmd 0x4E.
    void*                   m_pCmdDispatcher    = nullptr;
    // FUN_101d0950 — the cmd 0x0F dispatch helper (__thiscall:
    // ECX=warehouse, stack=typeArray, progId, &outParam). Already INT3-patched
    // (FUN_101d0950_thread_check_INT3). Stored as void* and invoked via
    // inline asm in callCmd0FDispatcher. Used by refreshCache() to set
    // [warehouse+0x5E] (COM interface) before dispatching cmd 0x4E.
    void*                   m_pCmd0FDispatcher   = nullptr;
    // FUN_101d43a0 — the type-array builder (__stdcall: 2 stack args —
    // category, outArray). Stored as void* and invoked via inline asm in
    // callBuildTypeArray. Used by refreshCache() to build the type array
    // needed by the cmd 0x0F dispatch.
    void*                   m_pBuildTypeArray    = nullptr;
    // FUN_1000dd30 — the CELProgramWriter constructor (__fastcall:
    // ECX=buffer). Stored as void* and invoked via inline asm in
    // callCelProgramWriterCtor. Used by refreshCache() to initialize the
    // output buffer before calling FUN_101d0950 (cmd 0x0F dispatch).
    void*                   m_pCelProgWriterCtor = nullptr;
    // FUN_101cfce0 — the cmd 0x0D dispatch helper (__thiscall:
    // ECX=warehouse, stack=type, param). Already INT3-patched
    // (FUN_101cfce0_thread_check_INT3). Stored as void* and invoked via
    // inline asm in callCmd0DDispatcher. Used by refreshCache() to obtain
    // the correct COM interface (from the server-side cache) at
    // [warehouse+0x5E], which cmd 0x08 (type select, used by
    // _LoadMasterRecsEX@16) reads to enumerate records. The cmd 0x0F
    // interface is stale after create/modify; cmd 0x0D gets a fresh one.
    void*                   m_pCmd0DDispatcher   = nullptr;
    // FUN_101a42d0 — find item by name + get verbatim stored name
    // (__fastcall: ECX=searchName, EDX=componentType, stack=warehouse,
    // outVerbatimName; RET 0x8). Stored as void* and invoked via inline asm
    // in callFindItemGetName. SUPERSEDED / DEAD CODE (retained for
    // reference only): was the original Phase 2 candidate but was found to
    // return the ProgId (not the verbatim name) in non-server mode, so it
    // cannot be used to get the verbatim name. enumerateRecords() Phase 2
    // now uses FUN_101d05e0 (m_pBstSearchWrapper) + FUN_101c36d0
    // (m_pVerbatimNameGetter) instead.
    void*                   m_pFindItemGetName   = nullptr;
    // FUN_101d05e0 — BST search wrapper (non-server mode)
    // (__thiscall: ECX=warehouse, stack=(typeFilter, name, *outNode);
    // RET 0xC). Stored as void* and invoked via inline asm in
    // callBstSearchWrapper. Used by enumerateRecords() Phase 2 to find the
    // BST node for a given item name.
    void*                   m_pBstSearchWrapper   = nullptr;
    // FUN_101c36d0 — verbatim name getter (__fastcall: ECX=node pointer,
    // no stack args; RET). Stored as void* and invoked via inline asm in
    // callVerbatimNameGetter. Used by enumerateRecords() Phase 2 to get the
    // verbatim stored name from the BST node found by FUN_101d05e0.
    void*                   m_pVerbatimNameGetter = nullptr;

    // --- Workstream M slice M.1: test-only compiler-ready hook ---
    // Optional callback populated from TsDevLibTestConfig::onCompilerReady
    // during construction via makeTsDevLibForTest (the private test-only
    // ctor). When non-null, verifyModule() invokes it immediately BEFORE the
    // _VerifyModule_New call (after StartCompilingProcess and after the
    // +0x00/+0x04 result-field pre-zeroing), giving a test live read access
    // to the 0x58-byte CELCompiler object state. Stored as a plain
    // function pointer (NOT a TsDevLibTestConfig member) so the public
    // header does not need to include tsdev_lib_test_hooks.h or know the
    // struct definition. Always nullptr for the public ctor (production).
    //
    // Signature: void(*)(void* compilerMem, int componentType, int progId)
    //   compilerMem    — pointer to the live CELCompiler object
    //   componentType  — static_cast<int>(type) being verified
    //   progId         — the ProgId passed to verifyModule
    void (*m_onCompilerReady)(void* compilerMem, int componentType,
                              int progId) = nullptr;

    // --- Workstream M slice M.2: test-only save-module-ready hook ---
    // Optional callback populated from
    // TsDevLibTestConfig::onSaveModuleNewReady during construction via
    // makeTsDevLibForTest (the private test-only ctor). When non-null,
    // saveSource() invokes it immediately BEFORE the _SaveModule_New@20
    // call, capturing the flags word (arg3) and password buffer (arg5) so a
    // test can assert the TSDev-parity values. Stored as a plain function
    // pointer (NOT a TsDevLibTestConfig member) so the public header does
    // not need to include tsdev_lib_test_hooks.h or know the struct
    // definition. Always nullptr for the public ctor (production).
    //
    // Signature:
    //   void(*)(const unsigned int* flags, const char* passwordBuf,
    //           int passwordBufSize, int componentType, int progId)
    //   flags          — pointer to the flags word (arg3); NULL in the
    //                    current production code (RED state).
    //   passwordBuf    — pointer to the 64-byte password buffer (arg5);
    //                    NULL in the current production code (RED state).
    //   passwordBufSize— buffer size (0 while production passes NULL).
    //   componentType  — static_cast<int>(type) being saved.
    //   progId         — the ProgId passed to saveSource.
    void (*m_onSaveModuleNewReady)(const unsigned int*, const char*,
                                   int, int, int) = nullptr;

    // --- Workstream M slice M.3: test-only notify-ready hook ---
    // Optional callback populated from TsDevLibTestConfig::onNotifyReady
    // during construction via makeTsDevLibForTest (the private test-only
    // ctor). When non-null, verifyModule() (on the success path) and
    // deleteEntry() (on the success path, gated category != 4) invoke it
    // immediately BEFORE the 0x40A notification would be sent via orlib20
    // Ordinal_57, capturing the (msg, wParam, lParam) tuple plus the
    // affected componentType and progId so a test can assert the
    // TSDev-parity packing (msg=0x40A, wParam=0,
    // lParam=(category<<16)|(ProgId&0xFFFF)). Stored as a plain function
    // pointer (NOT a TsDevLibTestConfig member) so the public header does
    // not need to include tsdev_lib_test_hooks.h or know the struct
    // definition. Always nullptr for the public ctor (production).
    //
    // Signature:
    //   void(*)(UINT msg, WPARAM wParam, LPARAM lParam,
    //           int componentType, int progId)
    //   msg           — 0x040A (the message ID about to be posted).
    //   wParam        — 0.
    //   lParam        — (category << 16) | (ProgId & 0xFFFF).
    //   componentType — static_cast<int>(type) of the affected module.
    //   progId        — the ProgId of the affected module.
    void (*m_onNotifyReady)(UINT, WPARAM, LPARAM, int, int) = nullptr;

    // --- Helpers ---
    HWND createHiddenEditControl();
    void setSourceText(HWND hwnd, const std::string& source);
    void applyThreadCheckPatches();
    std::string readErrorsFromArray(const std::string* source);

    // TVS-L.13: shared post-COM initialization body. Called by BOTH the
    // public ctor and the private test-only ctor AFTER the COM apartment guard
    // has been constructed and verified initialized (S_OK / S_FALSE). This
    // avoids duplicating the ~110-line init sequence (SetDllDirectoryW,
    // OrlibResolver, support DLLs, patches, OrlibInit, hidden EDIT control)
    // across the two constructors. It is a private implementation detail —
    // not part of the public API — and exists solely to keep the two
    // construction paths behaviorally identical past the COM-init step.
    void initAfterCom(const wchar_t* tsPathOverride);
};

} // namespace tsdev

#endif // TSDEV_LIB_H
