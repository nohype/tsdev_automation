# Enumerating stored item names by type - mechanism RE + live proof

Date: 2026-11-01. Subagent task: find and PROVE the mechanism to enumerate stored
item names by type (verbatim, case-preserved, incl. user items). No repo source
modified; all artifacts under tmp\. ASCII only.

All evidence files (tmp\):
  exp_orlib20.txt / exp_orelui20.txt  - full export tables (dumpbin /exports)
  tlbdump_full.txt                    - WHServer typelib {7CEEA906} full member dump
  wh_enum.cpp / wh_enum.exe           - live probe (Candidate 2)
  enum_run_base.txt                   - fresh full enumeration, all 5 type values
  enum_scan.txt                       - subtype-family scan
  enum_walk_st4.txt / enum_walk_st18.txt - PaintBar / ProbabilityMap walks
  wh_enum_progslist_*.bin             - ProgramsList raw blobs (all 5 types)
  verify_names_results.txt            - cross-check (a): every name vs tsdev_cli exists
  probe_lifecycle_results.txt         - cross-check (b): zz_ probe lifecycle
  diff_counts2.py / analyze_blob3.py  - count-drift and blob-layout analysis
  enum_empty_type.txt                 - empty-type behavior (typeVal=256)

===============================================================================
1. CANDIDATE-BY-CANDIDATE VERDICTS
-------------------------------------------------------------------------------
CANDIDATE 1 - orlib20 / ORELUI20 export surface + WHServer typelib.  PARTIAL:
  NOT sufficient as an export-based name enumerator, but the typelib dump is
  what unlocked Candidate 2.

  1a. orlib20.dll exports (636 names; tmp\exp_orlib20.txt): NO export enumerates
      item names. Master-record family (_LoadMasterRecsEX@16 ord 568,
      _GetMasterRecsEX@8, _GetMasterRecsBuf@12, _GetMasterRecByType@16 ord 510,
      _GetMasterRecByName@12 ord 509, _GetNumofMasterRecsEX@4 ord 521) returns
      0x10A-byte records that contain NO name (root-cause-proven earlier).
      Name-consuming exports are lookups only: FindExistPos (ord 426,
      RVA 0x001A4230) name->position/ProgId; _FindExistPosSubType@12 (ord 454);
      FindAvlPos (ord 452). CountUniqueNames (ord 114, RVA 0x00107D50) and
      GetSystemCount (ord 185, RVA 0x00107D30) are adjacent EL-runtime builtins
      (system/strategy-count domain), not warehouse name enumeration.
      _GetBuiltInCategoryNames@8 (ord 474) returns category names, not items.

  1b. ORELUI20.dll exports (2146 names; tmp\exp_orelui20.txt): the UI list
      loader IS exported: ord 1291 `?LoadList@CELFileOpenDialog@@IAEXHPAVCAnalysisTypeInfo@@@Z`
      (RVA 0x00012290), ctor ord 17/18. But its required CAnalysisTypeInfo
      ctor is NOT exported and in-process driving is unproven -> defer to
      Candidate 3 (not needed; see below).

  1c. WHServer.exe typelib {7CEEA906-D59E-11D3-8394-00C04F7BDF10} v1.0,
      11 typeinfos (tmp\tlbdump_full.txt, produced by tmp\tlbdump.exe via
      LoadRegTypeLib + ITypeInfo). Clean COM metadata, zero RE. Enumeration-
      relevant members with full signatures:
        IWHouse (IID 7CEEA903, cFuncs=58):
          ProgramsList        memid 0x14 PROPERTYGET (I2 Type) -> VARIANT
          ProgramsCountByType memid 0x19 PROPERTYGET (I2 Type) -> I4
          NextId              memid 0x07 PROPERTYGET (I2 Type) -> I4
          ProgramById         memid 0x04 PROPERTYGET (I2 Type, I4 Id) -> DISPATCH
          ProgramByName       memid 0x05 PROPERTYGET (I2 Type, BSTR Name) -> DISPATCH
          MasterRecsByType    memid 0x15 PROPERTYGET (I2 Type) -> VARIANT
          CreateProgram 0x02, RemoveProgram 0x08, RenameProgram 0x1C,
          FlushCache 0x18, ClearVerifyFlagByType 0x1F, ... (maintenance)
        IProgram (IID 7CEEA904, cFuncs=48):
          Name  memid 0x02 PROPERTYGET () -> BSTR        <- THE name member
          Id    memid 0x04 PROPERTYGET () -> I4
          Type  memid 0x01 PROPERTYGET () -> I2
          Source 0x05, Callers 0x06, Callees 0x07, Code 0x09, ...
        IControl (IID 7CEEA90A, cFuncs=8): WHouseEx memid 0x01 PROPERTYGET
          (BSTR DirName, I4 Create) -> DISPATCH
      Verdict: the typelib provides everything needed -> Candidate 2.

CANDIDATE 2 - WHServer IWHouse COM enumeration.  ***WINNER - PROVEN LIVE***
  See section 2 for the exact invocation shape and section 3 for the proof.

CANDIDATE 3 - ORELUI20 CELFileOpenDialog::LoadList.  NOT PURSUED.
  Stopped per task rule: a higher-level candidate (2) is proven sufficient.
  (LoadList would additionally need the non-exported CAnalysisTypeInfo ctor,
  internal CELTechInfo/CMap layout parsing, and unproven in-process loadability
  of a UI DLL -> strictly more binary internals than Candidate 2.)

CANDIDATE 4 - orlib20 BST name-index in-order walk.  NOT PURSUED (same reason).

===============================================================================
2. WINNER - EXACT INVOCATION SHAPE (for future C++ production code)
-------------------------------------------------------------------------------
NO hardcoded RVAs and NO hardcoded dispids: all COM members are resolved by
name via IDispatch::GetIDsOfNames against the WHServer typeinfo. The only
non-typelib constant is the custom factory IID + its vtable slot (a COM
interface, KB Sec 17, already the project's proven activation path), and the
stable CLSIDs/IIDs.

Type values (the I2 "Type" argument) = the EXPANDED whType of
src/tsdev_lib.h ComponentType, verified 1:1 against exists:
  Strategy=0  Function=1  Indicator=2  ShowMe=1282 (0x0502)  TradingApp=5122 (0x1402)

Activation (verbatim from the live-proven KB Sec 17 chain):
  CoInitialize(NULL)                                   // own STA, own thread
  CoGetClassObject(CLSID_Control{7CEEA90C-D59E-11D3-8394-00C04F7BDF10},
                   CLSCTX_ALL, NULL,
                   IID_Factory{7CEEACBC-D59E-11D3-8394-00C04F7BDF10}, &factory)
  // factory vtable slot 8 (CreateInstanceLic-style; custom factory contract):
  typedef HRESULT (__stdcall *FactoryCreateFn)(
      IUnknown* self, unsigned a0 /*0*/, unsigned a1 /*0*/,
      const IID* riid /*&IID_IControl{7CEEA90A-...}*/,
      BSTR name /*"<268435478>,<ror(((GetTickCount>>12)&0xFFFF|0x160000)^0x1256abef,6)>"*/,
      void** outObj /*IControl*/, BSTR* outName /*assigned name out*/);
  QI(outObj, IID_IDispatch) -> IControl IDispatch
  IControl::WHouseEx (GetIDsOfNames("WHouseEx"), DISPATCH_PROPERTYGET,
                      rgvarg[0]=VT_I4 Create=1,
                      rgvarg[1]=VT_BSTR DirName="C:\Program Files (x86)\TradeStation 9.5\MyWork")
      -> VT_DISPATCH = IWHouse IDispatch
  NOTE: rgvarg is in REVERSE parameter order (Create last param first).
  NOTE: plain CoCreateInstance(CLSID_Control) is NOT viable (live hr=0x80040112)
  and CLSID_WHouse{7CEEA907} is not registered (hr=0x80040154); the custom
  factory path above is the working, proven shape.

Enumeration (pure IDispatch, names resolved via GetIDsOfNames):
  IWHouse::ProgramsCountByType (DISPATCH_PROPERTYGET, rgvarg[0]=VT_I2 Type) -> I4
  IWHouse::NextId              (DISPATCH_PROPERTYGET, VT_I2 Type) -> I4
  for (I4 Id = 1; Id < NextId; ++Id) {
      IWHouse::ProgramById (DISPATCH_PROPERTYGET,
          rgvarg[0]=VT_I4 Id, rgvarg[1]=VT_I2 Type) -> VT_DISPATCH IProgram
      if (null) continue;
      IProgram::Name (DISPATCH_PROPERTYGET, no args) -> BSTR   // verbatim name
      IProgram::Id   (optional) -> I4                          // == ProgId
  }
  Completeness: walk yields exactly ProgramsCountByType rows per type (proven
  for all 5 types); Id space is sparse (e.g. strategy 761 rows in 1..10569),
  null returns are normal, zero Invoke errors observed.

ALTERNATIVE within Candidate 2 (1 call, but binary layout): IWHouse::ProgramsList
  (DISPATCH_PROPERTYGET, VT_I2 Type) -> VT_ARRAY|VT_EMPTY (SAFEARRAY of UI1).
  Live-validated record layout (all 5 types, exact parse + full buffer
  consumption + 1:1 id/name match vs the ProgramById walk):
    [u32 count][per record: u16 type(expanded whType), u32 id,
                u16 nameLenInclNul, name bytes + NUL, 22 trailing bytes]
  Names are plaintext ASCII. This is faster (1 call) but decodes an
  UNDOCUMENTED record layout (the 22 trailing bytes are opaque) - it is a
  binary-internal contract. RECOMMENDATION: production uses the ProgramById
  walk (typelib-only); ProgramsList is a validated fallback if performance
  ever demands it (walk cost measured: function 0.2 s, strategy 4.2 s).

Apartment/threading: all calls on the creating STA thread succeeded with zero
  RPC_E_WRONG_THREAD (each client creates its own IWHouse via the chain).
  A live WHServer.exe is required (SCM auto-respawns; pid stable during runs).

===============================================================================
3. LIVE PROOF TRANSCRIPTS
-------------------------------------------------------------------------------
3.0 Baselines
  - No TradeStation desktop, no WHServer running at start; no TSCrashReport.
  - Popup audit after EVERY action (probe script logs POPUP-AUDIT lines):
    all clean. No crash dialogs, no error/warning windows at any point.
  - IMPORTANT: no frida-spawn of tsdev_cli was used anywhere (per mandate);
    all RE evidence is static/typelib or in-process tmp\ diagnostics.

3.1 Fresh full enumeration (tmp\enum_run_base.txt, fresh process)
  typeVal=0   strategy:    ProgramsCountByType=761  walk found=761 (err=0)
  typeVal=1   function:    ProgramsCountByType=84   walk found=84  (err=0)
  typeVal=2   indicator:   ProgramsCountByType=314  walk found=314 (err=0)
  typeVal=1282 showme:     ProgramsCountByType=61   walk found=61  (err=0)
  typeVal=5122 tradingapp: ProgramsCountByType=9    walk found=9   (err=0)
  Names are verbatim + case-preserved (e.g. "BTA_ELITE_SmartCode_GEN3",
  "bounce2.0", "RCI (Spearman's RCC)", "52 Wk Channel", "%Chg"), incl. user
  items and names with spaces/&/</>/apostrophes. Name sets across the 5 type
  values are disjoint except one legal cross-type homonym (a name used both as
  strategy and indicator).

3.2 Cross-check (a) - every name vs tsdev_cli exists (tmp\verify_names_results.txt)
  Stratified spot-check, 33 names across strategy/function/showme
  (head/mid/tail + random; tmp\spotcheck.py): 33/33 OK - exists=True,
  stored_name == enumerated name (byte-exact, case-sensitive),
  prog_id == IProgram::Id. Examples:
    exists strategy "BTA_ELITE_SmartCode_GEN3" -> progId=994 == walk id
    exists function  "AB_NextLabel"            -> progId=1   == walk id
    exists showme    "C_Abandoned Baby"        -> progId=301 == walk id
    exists indicator "%Chg"                    -> progId=1   == walk id
  => exists ProgId numbering == IProgram::Id numbering (verified identity).
  EXHAUSTIVE SCAN (tmp\verify_names_results.txt): all 1229 enumerated names
  (strategy 761 + function 84 + indicator 314 + showme 61 + tradingapp 9) run
  through `tsdev_cli exists <type> <name> --json`, case-sensitive:
    SUMMARY total=1229 ok=1229 fail=0
  Every name resolves exists=true with byte-identical stored_name and
  prog_id == IProgram::Id. Zero mismatches.

3.3 Cross-check (b) - zz_ probe lifecycle (tmp\probe_lifecycle_results.txt,
    parsed transcripts tmp\probe_walk_*.txt)
  Probes: zz_enumprobe_strat_20261101195618 (strategy),
          zz_enumprobe_fn_20261101195618 (function).
  1. PRE exists (collision avoidance): exists=False / exists=False.
  2. INSERT strategy exit=0 prog_id=10570 (elmodule_0_10570.ell);
     INSERT function  exit=0 prog_id=195   (elmodule_1_195.ell). JSON success.
  3. GT after insert: exists=True prog_id=10570 / 195, stored_name == probe
     names verbatim.
  4. FRESH-process enumeration (wh_enum.exe walk, one process per type):
       post_insert strategy walk: 762 rows incl.
         (10570, 'zz_enumprobe_strat_20261101195618')   <- VERBATIM, id==10570
       post_insert function walk: 85 rows incl.
         (195, 'zz_enumprobe_fn_20261101195618')        <- VERBATIM, id==195
     (count moves +1 exactly: 761->762, 84->85)
  5. DELETE strategy exit=0 prog_id=10570; DELETE function exit=0 prog_id=195.
  6. FRESH-process enumeration:
       post_delete strategy walk: 761 rows, probe ABSENT (762->761)
       post_delete function walk: 84 rows,  probe ABSENT (85->84)
  7. CLEANUP: exists=False / exists=False (both probes gone; verified again
     after the script via the walk transcripts).
  8. Popup audit after every action: no TSCrashReport/TSCrash process, no
     error/warning dialog at any point. (The audit regex also matched a File
     Explorer window title that contains the path "...\TradeStation 9.5...";
     benign, not a dialog - regex false positive, no action needed.)
  This also reproduces the row-count drift mechanism live: the winning
  mechanism's count tracks insert/delete +1/-1 immediately and exactly.

3.4 Cross-check (c) - count consistency + the 762->761 drift EXPLAINED
  Winning mechanism counts (3 independent COM runs): 761/84/314/61/9.
  tsdev_cli list (master-rec rows) TODAY, both 19:09 snapshot and now:
  761/84/314/61/9 - identical to the COM counts.
  The TVS-1-era raw master-rec dump (tmp\tvs1_bulk_dump_out2.txt, 18:47) had:
    strategy 762, function 85, cat-2 family 415.
  Set diff vs the live COM enumeration (tmp\diff_counts2.py):
    strategy: extra row progId=10568 (nothing else differs)
    function: extra row progId=193  (nothing else differs)
    cat-2:    415 = 314 (indicator) + 61 (showme) + 9 (tradingapp)
                    + 29 (subtype 4 = PaintBar, tmp\enum_walk_st4.txt:
                          "Custom PaintBar", "Count Criteria PB", ...)
                    +  2 (subtype 18 = ProbabilityMap, tmp\enum_walk_st18.txt:
                          "Normal Distribution", "Normal Range")
  Conclusions:
    - 762->761 drift = ONE stale master-record row per probed type (progIds
      10568/193) left in the _LoadMasterRecsEX buffer right after the TVS-1
      probe insert/delete churn (18:23-18:25 window). Neither id resolves in
      any name lookup (then or now) and both are gone from all later snapshots
      (19:09 list run and now: 761/84). The name-index-based COM enumeration
      NEVER counted them in any run. Master-record row counts are therefore
      NOT trustworthy mid-churn; the winning mechanism's counts are.
    - The cat-2 "415 vs 384" difference is NOT drift: it is subtype coverage.
      Master-rec cat-2 enumerates ALL cat-2 subtypes; the 5 supported types
      cover st=0/5/20 only. Scan (tmp\enum_scan.txt) accounting:
        cat0 st0=761  cat1 st0=84  cat2 st0=314 st4=29 st5=61 st18=2 st20=9
        (also visible: cat3 st0=5, cat4 st0=414 - outside the 5 types)

===============================================================================
4. STABILITY ASSESSMENT (what a future TSDev/orlib20 update can break)
-------------------------------------------------------------------------------
Robust (typed COM contract, name-resolved):
  - All IWHouse/IProgram members used (ProgramsCountByType, NextId,
    ProgramById, Name) are IDispatch members looked up by NAME via
    GetIDsOfNames from the WHServer typeinfo; no dispids, no RVAs.
    Breaks only if TradeStation removes/renames these typelib members.
  - Type values = expanded whType; validated against exists. Could silently
    mis-filter if TradeStation re-encodes subtypes (detectable: walk count
    would stop equalling ProgramsCountByType or exists cross-check fails).
Fragile-ish (versioned but non-typelib):
  - The custom factory: IID {7CEEACBC...} + vtable slot 8 signature (KB Sec 17).
    It is a COM interface (not an RVA) and is already the project's proven
    activation path, but its contract is not in the typelib. If a TS update
    changes the factory interface, activation breaks LOUDLY (HRESULT fail).
    Mitigation: activation is one isolated function; only the IIDs/signature
    would need re-proving.
  - ProgramsList blob layout (if ever used): undocumented; MUST be validated
    at runtime (count field == parsed records, fully consumed buffer, names
    cross-check) or avoided. Recommendation stands: avoid in production.
Not used at all: binary RVAs, BST internals, ORELUI20 UI internals, master
  record parsing. orlib20 is not even loaded by the winning mechanism.

===============================================================================
5. IMPLEMENTER CAVEATS (verified) + STATUS: CLOSED
-------------------------------------------------------------------------------
- Apartment: IControl/IWHouse/IProgram proxies are apartment-bound. Create the
  factory, the Control, and the IWHouse via the chain ON ONE STA thread and
  call them from that same thread (KB Sec 17: QI on those proxies from any
  other apartment -> RPC_E_WRONG_THREAD 0x8001010E). One IWHouse per client
  process/thread is fine; each activation returns a fresh reference.
- Server liveness: a WHServer.exe must be activatable. CoGetClassObject goes
  through SCM: if the server is dead, SCM respawns it automatically (observed
  pid stable during runs; no desktop required -- all proofs ran with the
  TradeStation desktop CLOSED). All calls are synchronous RPC; no timeouts or
  errors observed across ~35,000 Invoke calls in total.
- Error behavior: ProgramById on absent ids returns VT_EMPTY/null (not an
  error); a walk over the whole id range completed with err=0 every run.
  DISP_E_EXCEPTION never observed. A null IProgram pointer is the ONLY
  not-found signal.
- Empty type (verified with typeVal=256 = cat 0 subtype 1): exit 0,
  ProgramsCountByType=0, walk found=0 rows with zero errors,
  ProgramsList returns a 4-byte blob (u32 count=0). Empty is SUCCESS.
- NextId semantics: per-CATEGORY id bound (Type & 0xFF), NOT per-subtype.
  Observed: NextId(0)=10570/10571 (cat 0), NextId(1)=195 (cat 1),
  NextId(2)=NextId(1282)=NextId(5122)=3548 (shared cat-2 id range),
  NextId(256)=10571 (cat-0 range despite empty subtype). Ids are sparse and
  the bound grows monotonically (deleted ids are not reused immediately).
  Walk bound = NextId(type) - 1; completeness is asserted by
  (walk rows == ProgramsCountByType).
- Type arg is VT_I2 (signed short); 5122 fits. Pass the EXPANDED whType
  (subType<<8 | category) exactly as src/tsdev_lib.h ComponentType values.
- DISPPARAMS args are in REVERSE parameter order (ProgramById: rgvarg[0]=Id,
  rgvarg[1]=Type; WHouseEx: rgvarg[0]=Create, rgvarg[1]=DirName).
- Names: IProgram::Name returns a BSTR (VT_BSTR) -- free with SysFreeString.
  Names are case-preserved verbatim; they may contain spaces, '&', '<', '>',
  apostrophes, '%', '#' etc. Do not parse/normalize. One legal homonym exists
  across types (same name as strategy and indicator) -- names are per-type.
- IProgram::Name read via the typelib member only; the object+0x46 wide-string
  offset (KB note) was NOT needed and NOT used.

STATUS: all deliverable sections complete.
  Cross-check (a): 1229/1229 ok, 0 fail (tmp\verify_names_results.txt).
  Cross-check (b): probe lifecycle proven (tmp\probe_lifecycle_results.txt,
    tmp\probe_walk_*.txt); final exists=0 for both probes (re-verified at close).
  Popup audit: clean at every step and at close (no TSCrashReport, no error
    or warning dialogs at any time).