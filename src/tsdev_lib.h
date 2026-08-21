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

namespace tsdev {

class OrlibResolver;  // forward declaration (defined in orlib_resolver.h)

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
//               FUN_10011230 persistence patch, resolves exports, calls
//               OrlibInit(NULL, 1), creates a hidden EDIT control.
//               Throws std::runtime_error on fatal failure.
// Destructor:   calls OrlibExit(), destroys the EDIT control.
//
// Not copyable, not movable (owns DLL handles and live window objects).
class TsDevLib {
public:
    explicit TsDevLib(const wchar_t* tsPathOverride = nullptr);
    ~TsDevLib();

    TsDevLib(const TsDevLib&)            = delete;
    TsDevLib& operator=(const TsDevLib&) = delete;
    TsDevLib(TsDevLib&&)                 = delete;
    TsDevLib& operator=(TsDevLib&&)      = delete;

    // Look up an existing component by name.
    // Returns ProgId (>0) if found, 0 if not found.
    int findExistPos(const std::string& name, ComponentType type);

    // Create a new entry in the warehouse.
    // Returns ProgId on success, 0 on failure (inspect errCode for detail).
    int createEntry(ComponentType type, const std::string& name, int& errCode);

    // Save source text into an entry (does NOT persist to disk).
    // Sets the source into the hidden EDIT control and calls SaveModule_New.
    // Returns true on success.
    bool saveSource(ComponentType type, int progId, const std::string& source);

    // Retrieves EasyLanguage source text for an existing item by name + type.
    // Returns the source text as a string. Returns empty string on failure
    // (not found, password-protected, or internal error).
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
    std::string verifyModule(ComponentType type, int progId);

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
    // found. The flow (confirmed via Frida trace of live TSDev.exe):
    //   FindExistPos(name, whType) →
    //   FUN_101a8360 (delete orchestrator: __fastcall ECX=whType, EDX=progId)
    //     — loads program, marks deleted, runs CELProgramWriter cleanup,
    //       sends command 0x4E (cache reset) and command 0x17 (save/commit) →
    //   InitGlobalLookup → DeleteFromTree(globalLookup, whType, progId, 3) →
    //   closeWarehouse (best-effort flush) →
    //   OrlibExit (in destructor, persists the deletion) →
    //   delete .ell/.xml files via Windows API.
    bool deleteEntry(ComponentType type, const std::string& name,
                     bool flush = true);

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
    // persistence path (the FUN_10011230 patch is applied in the constructor).
    Result modifyEntry(ComponentType type, const std::string& name,
                       const std::string& newSource);

    // Check whether the .ell file for a given entry exists on disk.
    bool ellFileExists(ComponentType type, int progId);

    // Return the full path of the .ell file for a given entry.
    std::string getEllPath(ComponentType type, int progId);

    // Return the full path of the .xml (debug symbols) file for a given entry.
    std::string getXmlPath(ComponentType type, int progId);

    // Return the discovered MyWork\Bin path (narrow, for .ell/.xml file paths).
    const char* getMyWorkBinPath() const;

    // Return the discovered MWRKArea.wrk path (narrow, for debug logging / tests).
    const char* getWrkPath() const;

    // Validate that bytes at the persistence patch RVA match the expected
    // pattern (with mask). Returns true if match, false + stderr on mismatch.
    // Read-only — does not patch. Used by applyPersistencePatch() and tests.
    bool validatePersistencePatchBytes(const unsigned char* expected,
                                       const unsigned char* mask, size_t len);

private:
    // --- OrlibResolver (TVS-B.5) ---
    std::unique_ptr<OrlibResolver> m_resolver;  // owns the DLL load + all resolution
    void*      m_pDeleteOrchestrator = nullptr;  // Target 8, resolved by signature scan
    uintptr_t  m_errArrayPtrRva      = 0;        // data global, resolved by instruction extraction
    uintptr_t  m_errCountRva         = 0;        // data global, resolved by instruction extraction
    uintptr_t  m_errHinstRva         = 0;        // data global, resolved by instruction extraction
    uintptr_t  m_persistencePatchRva = 0;        // persistence patch site RVA, cached BEFORE
                                                 // the patch is applied (the signature scan
                                                 // for original bytes fails post-patch)
    unsigned char m_persistenceOriginalBytes[8] = {};  // ACTUAL 8 original bytes at the
                                                       // persistence patch site (including the
                                                       // ASLR-sensitive positions 6-7 that
                                                       // kPersistenceOriginalBytes wildcards
                                                       // out), saved BEFORE patching so the
                                                       // destructor can restore them.
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
    // CELCompiler constructor: __fastcall (ecx=this)
    typedef void* (__fastcall *CELCompilerCtor_t)(void* thisPtr);
    // CELCompiler destructor: __fastcall (ecx=this)
    typedef void (__fastcall *CELCompilerDtor_t)(void* thisPtr);
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
    CELCompilerCtor_t       m_pCELCompilerCtor = nullptr;
    CELCompilerDtor_t       m_pCELCompilerDtor = nullptr;
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

    // --- Helpers ---
    HWND createHiddenEditControl();
    void setSourceText(HWND hwnd, const std::string& source);
    void applyPersistencePatch();
    void applyThreadCheckPatches();
    std::string readErrorsFromArray(const std::string* source);
};

} // namespace tsdev

#endif // TSDEV_LIB_H
