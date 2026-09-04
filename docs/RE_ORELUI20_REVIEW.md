# TVS #7 Adversarial Review — ORELUI20.dll Enumeration + Delete

Independent adversarial review of the TVS #7 research deliverable
(`docs/RE_ORELUI20.md` + SSOT additions in `docs/KNOWLEDGE_BASE.md` +
`docs/WORKING_MEMORY.md` update), checked against the TVS #7 task and the
overarching project requirements in `AGENTS.md`.

## Verdict: APPROVE

The deliverable meets the TVS #7 task and project requirements. All checklist
items PASS or NOTE (no FAIL). No required fixes. Three non-blocking
recommendations follow.

---

## Findings by category

### A. Scope compliance

- **A1 — PASS.** `ORELUI20.dll` imported into Ghidra; `ORLIB20.dll` was NOT
  imported. Justified in `RE_ORELUI20_STATIC.md` §4: the invocation recipe is
  fully characterized from the `ORELUI20.dll` call sites (DeleteItem's
  decompilation shows the exact `Ordinal_59`/`Ordinal_17` arguments), so
  importing `ORLIB20.dll` would add RE work (purpose a) without changing the
  recipe. Correct minimization.
- **A2 — PASS.** Enumeration surface characterized via exported ordinals
  (Ordinal_17 ctor, Ordinal_1291 LoadList, Ordinal_166 dtor). The one
  non-exported surface (`FUN_10044820`) is necessary and explicitly flagged as
  version-brittle (`RE_ORELUI20.md` §2.7). Per-item data (verbatim name, ProgId,
  type, subtype, record id) characterized at `RE_ORELUI20.md` §2.4.
- **A3 — PASS.** Delete routine located and characterized: calling convention
  `__thiscall`, params `(this, CELTechInfo*)`, `RET 0x4`, preconditions
  (not built-in type 4, not open in QE), live dialog NOT required
  (`RE_ORELUI20.md` §3.2–3.5).
- **A4 — PASS.** Enumeration Frida-verified from within the TSDev.exe process:
  2958 items across 9 types; 5 test items found with correct verbatim names +
  ProgIds (`RE_ORELUI20_DYNAMIC.md` §2.5–2.6).
- **A5 — PASS.** Delete Frida-verified for TradingApp + Function + Indicator +
  ShowMe (minimal bar exceeded: TradingApp mandatory + 3 others). Strategy
  honestly documented as NOT Frida-tested due to `tsdevctl insert --type
  Strategy` timeout — explicitly labeled a tsdevctl production issue, not a
  Frida failure (`RE_ORELUI20.md` §3.6; `RE_ORELUI20_DYNAMIC.md` §3.3).
- **A6 — PASS.** Per-operation assessment confirmed/corrected by RE findings
  (`RE_ORELUI20.md` §4): delete/list/exists YES; getsrc/insert/modify/upsert NO.
- **A7 — PASS.** No production code changes. `git status` shows only
  `docs/KNOWLEDGE_BASE.md` (modified), `docs/WORKING_MEMORY.md` (modified), and
  three new untracked doc files (`RE_ORELUI20.md`, `RE_ORELUI20_STATIC.md`,
  `RE_ORELUI20_DYNAMIC.md`). No source/build files touched.
- **A8 — PASS.** Dialog-check rule followed. `RE_ORELUI20_DYNAMIC.md` §4 logs
  exactly one crash — caused by `tsdevctl delete --type Strategy` (production
  bug), NOT by any Frida operation. The crash was read, dismissed, and
  corrective action taken (TSDev restarted PID 16600, cleanup completed via
  Frida). "No dialogs or crashes occurred during any Frida enumeration or
  delete operation."

### B. Correctness

- **B1 — PASS.** Every claim cites A's Ghidra finding or B's Frida result. No
  speculation found. Spot-checks below (3 claims verified against source files
  + 3 independently confirmed via Ghidra MCP).
- **B2 — PASS.** All 3 corrections reflected in final recipes in both
  `RE_ORELUI20.md` (§2.1 steps 3–4, §2.2 table) AND `KNOWLEDGE_BASE.md`
  (lines 365–379). Stale static versions are NOT propagated — the static file
  retains them as the working input (correct; it is subtask A's record).
  - Correction 1 (CAnalysisTypeInfo ctor 4 params): `RE_ORELUI20.md` §2.1 step 3
    + KB line 367–371. Independently confirmed via Ghidra: `FUN_10044820`
    signature is `(void * this, undefined4, undefined4, undefined4)` = 4 params.
  - Correction 2 (LoadList per-type with packed `DAT_1008fd04` values):
    `RE_ORELUI20.md` §2.1 step 3 + KB lines 359–363.
  - Correction 3 (CMapWordToPtr CAssoc swapped: value@+0x04, key@+0x08):
    `RE_ORELUI20.md` §2.1 step 4 + KB lines 373–379.
- **B3 — PASS.** Delete recipe correct and Frida-verified. DeleteItem ordinal
  587 (0x24A), `__thiscall bool(this, CELTechInfo*)`, `RET 0x4`. Per-type table
  in `RE_ORELUI20.md` §3.6 matches `RE_ORELUI20_DYNAMIC.md` §3.4 exactly
  (Function true, Indicator true, ShowMe true, TradingApp true, Strategy N/A).
  Independently confirmed via Ghidra: `DeleteItem` @ `0x10010dd0` signature
  `bool DeleteItem(CELFileOpenDialog * this, CELTechInfo * param_1)`.
- **B4 — PASS.** "No document-state manipulation needed" is evidence-backed.
  `RE_ORELUI20.md` §3.4 + §3.3: DeleteItem's decompiled logic (A §2.3) shows it
  reads type/subtype/recordId from the `CELTechInfo*` and calls `Ordinal_59` +
  `Ordinal_17` (ORLIB20.dll) — no TSDev document object is touched. B §3.2
  confirmed this dynamically. The contrast with the 7-coupling-point destructor
  path is factual (referenced from `RE_DELETE_OPEN_DIALOG.md`).
- **B5 — PASS.** Strategy production bugs recorded as OUT-OF-SCOPE-BUT-FLAGGED
  in all three locations: `RE_ORELUI20.md` §5 (with blockquote "production bug
  — not fixed in TVS #7 (research only); flagged for next implementation TVS"),
  `KNOWLEDGE_BASE.md` lines 505–521 (same blockquote), `WORKING_MEMORY.md`
  lines 63–67. Not silently dropped, not disguised as research limitations.
- **B6 — PASS.** Alternative direct path (`Ordinal_59` + `Ordinal_17` without
  DeleteItem) is documented but explicitly labeled "Not Frida-tested"
  (`RE_ORELUI20.md` §3.1; `RE_ORELUI20_DYNAMIC.md` §5 "NOT TESTED — redundant";
  KB line 477–482 "not Frida-tested, documented as fallback"). Honest labeling.
  Acceptable: `DeleteItem` (the recommended path) is fully verified for 4 types;
  the alternative is redundant (DeleteItem already calls these ordinals) and is
  presented only as a fallback.

### C. Quality

- **C1 — PASS (NOTE).** SSOT discipline upheld: `KNOWLEDGE_BASE.md` holds the
  proven facts (lines 324–524, "ORELUI20.dll + ORLIB20.dll ordinals" section);
  `RE_ORELUI20.md` explicitly references the SSOT (lines 8–11) and frames
  itself as "consolidated reasoning + evidence appendix"; `WORKING_MEMORY.md`
  holds current state only. No drift detected between the three (type tables,
  counts, per-type delete results, field offsets all match). NOTE:
  `RE_ORELUI20.md` does reproduce the type-category table (§2.3) and
  CELTechInfo layout (§2.4) inline, which is a mild SSOT tension — but the
  document explicitly defers canonical authority to the SSOT and the values are
  consistent. Acceptable for a recipe document an implementer follows.
- **C2 — PASS.** Purpose (a) minimized RE: research stopped at highest-level
  access points (exported ordinals). Internal details documented (CELTechInfo
  offsets, CAssoc layout, CAnalysisTypeInfo ctor) are the minimum needed for
  the recipe — the CMap cannot be walked without the CAssoc layout, and
  CAnalysisTypeInfo cannot be constructed without the ctor signature. Not
  over-documented.
- **C3 — PASS.** Purpose (b): recommended surfaces are exported ordinals
  (Ordinal_17, 1291, 166, 587). The single non-exported surface
  (`FUN_10044820`) is explicitly flagged as version-brittle in `RE_ORELUI20.md`
  §2.7 ("may change across TSDev/TradeStation versions — flag as
  version-brittle per purpose b") and KB lines 365–366. KB also notes the
  module base must be resolved at runtime, not hardcoded (lines 332–335).
- **C4 — PASS.** Process boundary (purpose d): all RE and Frida work was within
  the TSDev.exe process. No separate process was injected/instrumented. The
  `tsdevctl` commands that ran as separate processes are the existing production
  CLI (not RE work), and the one crash originated from `tsdevctl`, not from
  crossing a process boundary in the RE work.
- **C5 — PASS.** No hacks: no errors suppressed, no problems masked. The
  Strategy insert timeout and the `tsdevctl delete --type Strategy` crash were
  honestly documented and flagged, not hidden. Deletions were confirmed via
  re-enumeration when `tsdevctl exists` timed out (transparent reporting, not
  masking).
- **C6 — PASS.** Writing is factual and objective. No flattery or affirmation
  language found.

### D. Completeness

- **D1 — PASS.** Invocation recipes for both enumeration (`RE_ORELUI20.md`
  §2.1, 6 steps) and delete (§3.1, 3 steps) are present and sufficient for an
  implementer: calling conventions, parameter layouts, field offsets, type
  table, and preconditions all specified.
- **D2 — PASS.** Per-operation assessment (§4) answers the motivating question
  (which ops benefit from the module-calling approach): delete/list/exists YES,
  getsrc/insert/modify/upsert NO, each with one-line evidence.
- **D3 — PASS (NOTE).** No real gaps. Calling conventions, preconditions, and
  the runtime-base resolution concept ("module base + offset", KB lines
  332–335) are stated. NOTE (non-blocking, legitimately deferred to
  implementation): the exact Windows API call to resolve the runtime base
  (e.g. `GetModuleHandleW(L"ORELUI20.dll")`) and the exact method to extract a
  string from an MFC `CString` are not spelled out — but these are standard
  Windows/MFC knowledge, not research gaps.

---

## Spot-check evidence

### Claims verified against source files

1. **DeleteItem ordinal 587, `__thiscall bool(this, CELTechInfo*)`, `RET 0x4`.**
   - `RE_ORELUI20.md` §3.2 table: "Export ordinal 587 (0x24A)" with evidence
     "B §3.1: `dumpbin /EXPORTS ORELUI20.dll` →
     `587  24A 00010DD0 ?DeleteItem@CELFileOpenDialog@@IAE_NPAVCELTechInfo@@@Z`".
   - `RE_ORELUI20_DYNAMIC.md` §3.1: "DeleteItem export ordinal = 587 (0x24A).
     Confirmed via `dumpbin /EXPORTS ORELUI20.dll`:
     `587  24A 00010DD0 ?DeleteItem@CELFileOpenDialog@@IAE_NPAVCELTechInfo@@@Z`".
   - `RE_ORELUI20_STATIC.md` §2.2: "Calling convention: `__thiscall` — `this`
     in `ECX`, one explicit stack parameter (`CELTechInfo*`), callee cleans
     stack (`RET 0x4`)."
   - Match: consistent across all three files.

2. **2958 items enumerated across 9 types.**
   - `RE_ORELUI20.md` §2.8: "2958 items enumerated across 9 types (B §2.5)".
   - `RE_ORELUI20_DYNAMIC.md` §2.5: "Total items enumerated across all types:
     2958" with per-type table (Strategy 1655, Function 92, Indicator 244,
     ActivityBar 5, Function-built-in 852, PaintBar 29, ShowMe 65,
     ProbabilityMap 2, TradingApp 14).
   - `KNOWLEDGE_BASE.md` line 426–430: same counts.
   - Match: consistent. (Sum check: 1655+92+244+5+852+29+65+2+14 = 2958. Correct.)

3. **Per-type delete results table.**
   - `RE_ORELUI20.md` §3.6 table: Function true/CONFIRMED_DELETED,
     Indicator true/CONFIRMED_DELETED, ShowMe true/CONFIRMED_DELETED,
     TradingApp true/CONFIRMED_DELETED, Strategy N/A (insert failed).
   - `RE_ORELUI20_DYNAMIC.md` §3.4 table: identical rows and results.
   - Match: consistent.

### Claims independently confirmed via Ghidra MCP

4. **DeleteItem @ `0x10010dd0`.** Ghidra `get_function_by_address(0x10010dd0)`
   → name "DeleteItem", signature
   `bool DeleteItem(CELFileOpenDialog * this, CELTechInfo * param_1)`.
   Confirms the deliverable's identity + signature claim.

5. **LoadList @ `0x10012290`.** Ghidra → name "LoadList", signature
   `void LoadList(CELFileOpenDialog * this, int param_1, CAnalysisTypeInfo * param_2)`.
   Confirms the `__thiscall LoadList(this, int, CAnalysisTypeInfo*)` recipe.

6. **FUN_10044820 (CAnalysisTypeInfo ctor) takes 4 params.** Ghidra → name
   "FUN_10044820", signature
   `undefined4 * FUN_10044820(void * this, undefined4 param_1, undefined4 param_2, undefined4 param_3)`.
   This is `this` + 3 explicit params = 4 total, confirming correction B-1
   (A's static recipe claimed 1 param). The deliverable's corrected recipe is
   accurate; the stale static version was NOT propagated to the final recipes.

---

## Recommendations (non-blocking)

1. **Reduce inline SSOT restatement.** `RE_ORELUI20.md` §2.3 and §2.4 reproduce
   the type-category table and CELTechInfo layout that the SSOT holds. Consider
   replacing these inline tables with a brief reference ("see SSOT section X
   for the full table") to strengthen SSOT discipline. Low priority — the
   current state has no drift and the inline form is convenient for
   implementers following the recipe.

2. **Spell out runtime-base resolution.** The recipe says "compute its address
   from the ORELUI20.dll module base + offset" (§2.7) but does not name the
   Windows API (`GetModuleHandleW`). The next implementation TVS should
   document the concrete call; this is an implementation detail, not a
   research gap.

3. **Characterize the `DAT_1008fd04` read method for production.** The recipe
   reads the packed type values from `DAT_1008fd04` (a static data address in
   ORELUI20.dll). Production code will need to resolve this address at runtime
   the same way as `FUN_10044820` (module base + offset `0x8fd04`). This is
   implied but not explicitly stated; the implementation TVS should note it as
   a second version-brittle offset alongside `0x44820`.
