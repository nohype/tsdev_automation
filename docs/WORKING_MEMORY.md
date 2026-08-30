# Working Memory — TSDev Automation

Live, real-time state for the PM. Keep minimal; drop stale/superseded facts.
Do not restate what the code itself documents.

## Current objective

**TS Desktop interaction acceptance.** The six operations (Insert/Delete/Modify/
Exists/Get-source/Upsert) are implemented in C++ CLI + Python `TsDevClient` with
batch (manifest + stdin) support. The user has clarified that production
acceptance REQUIRES proper interaction with a running TS Desktop, matching
TSDev.exe:
- (a) all operations work WHILE TS Desktop is open
- (b) inserted items immediately available, usable, and FUNCTION when applied
- (c) modifications immediately picked up and applied to items already on
  charts, or already opened within TS Desktop as a tradingapp
- (d) deletions handled like TSDev: TS Desktop detects applied items, removes
  them, shows a "deleted" warning

The `dependents` workstream (M) is paused; it is beyond this acceptance target.

**Recalc (objectives c/d) is CLOSED** — see "Closed workstreams" below. The
active axis toward acceptance is **Python `TsDevClient` parity with the C++
CLI** (single-op Phase 1 APPROVED; DIRECT-mode batch Phase 2 APPROVED incl.
mixed-outcome/continue-on-failure E2E closure; serve-mode batch Phase 3 next,
to be split into sub-phases 3a/3b/3c).

## Current state (not derivable from code)

- TradeStation 9.5 installed: `C:\Program Files (x86)\TradeStation 9.5\Program\
  TSDev.exe` and `orlib20.dll` (v9.50.01.3344). TS Desktop process is
  `orchart.exe` (imports `_OrlibRegisterNotificationHandle@4`).
- `tsdev_cli.exe` built (32-bit Win32 Release) at `build\Release\tsdev_cli.exe`
  and `dist\tsdev_cli.exe`. NOTE: `cmake --build` exit code is non-zero due to a
  POST_BUILD packaging step missing `docs/BUNDLE_README.md`; the exe itself
  builds and runs fine. Repo-content issue, not toolchain.
- CLI works against the warehouse WITHOUT TS Desktop running (confirmed
  2026-08-30: `exists`/`insert`/`modify`/`delete` succeed with only
  `TradeStationAgentForms.exe` present). TS Desktop (`orchart.exe`) is only
  needed for recalc/chart-interaction verification (already closed).

**Probe items currently live:**
- Function `TsDeskFnProbe_20260829` — ProgId 84, `elmodule_1_84.ell`
  (currently v1, returns 1). STILL LIVE. (Reusable for TradingApp TEST 2.)
- Indicator `TsDeskFnProbeCaller_20260829` — ProgId 3008, `elmodule_2_3008.ell`.
  STILL LIVE; applied to a chart in TS Desktop (user-confirmed).
- The Strategy probes (`TsDeskStrategyProbe_20260829` ProgId 9042,
  `TsDeskStrategyFnProbe_20260829` ProgId 9043) are DELETED.
- The TradingApp probe `TsDeskTradingAppProbe_20260830` (ProgId 3030) is
  DELETED (cleaned up after TEST 1).
- The TradingApp fn-caller probe `TsDeskTradingAppFnProbe_20260830`
  (ProgId 3032) is DELETED (cleaned up after TEST 2).

## Closed workstreams (reference only — details in KB)

- **Recalc matrix: CLOSED.** All 8 entries (5 types × {direct-applied,
  function-caller}) confirmed. CLI has full TSDev.exe recalc parity. The
  mechanism is worker cmd 0x0D (FindProgram COM call) + Ordinal_57 (0x40A)
  cross-process notification. Full table and record: KB §22.12, §22.11b.
- **C++ CLI batch E2E: APPROVED.** `runBatch` (`src/tsdev_cli.cpp:2389-2472`)
  uses `flushPerItem=false` + single final `closeWarehouse()` (distinct
  failure surface: final-close failure → exit 6 retroactively). Verified by
  `tests/test_batch_e2e.ps1` (13 steps, PASS, exit 0): batch insert/modify/
  delete via `--batch-stdin` (3 types), ProgId preservation, upsert
  both-branches in one batch, manifest-FILE mode, mixed-outcome/continue-on-
  failure (exit 2). This is the parity reference for Python batch phases.

## TVS just completed (2026-08-30): Python DIRECT-mode batch parity E2E (Phase 2) — APPROVED

Verified `TsDevClient.batch_*` DIRECT mode (elevated Python → `tsdev_cli.exe`
subprocess; `_batch` pipes manifest to `batch-<op> --batch-stdin`) matches the
approved C++ batch behavior. No code changes needed.

- 7 batch/directory/orphan E2E tests PASS (exit 0, 445.42s; log
  `tmp/pytest_batch_phase2.log`): `test_batch_insert_20_then_delete`,
  `test_e2e_batch_upsert_mixed`, `test_e2e_batch_insert_dir_then_delete_dir`,
  `test_e2e_batch_upsert_dir_mixed`, `test_e2e_batch_modify_dir`,
  `test_e2e_batch_dir_t_override`, `test_no_orphans_after_suite`. All use the
  DIRECT `--batch-stdin` path (`_serve_active()` is false in an elevated
  shell); no serve tests ran.
- Parity covered: upsert both-branches in one batch, ProgId preservation,
  `--batch-stdin` mode (Python E2E). manifest-FILE mode + mixed-outcome/
  continue-on-failure confirmed via the C++ reference
  `tests/test_batch_e2e.ps1` (13/13 PASS; log `tmp/batch_e2e/run_log.txt`).
- Adversarial review APPROVED: independently re-verified the log, DIRECT-mode
  routing, C++ reference run, orphan sweep (no `PyAutoTest_*`/`BatchE2e_*`
  orphans), and the production gate. No code changes, no hacks, no scope drift.
- Mandatory production CLI E2E gate PASSED: single insert/modify/delete of
  `Phase2GateFn_20260830_164418_603` (progId 107), get-source confirmed modify
  `= 2;`, final exists=false, no popups, cleanup verified. Log
  `tmp/phase2_gate/report.txt`.
- `tests/run_unit_tests.ps1 -NoTs`: 17 passed, 0 failed, 19 skipped (require
  TS). No regressions.

Minor observations (non-blocking, for a future team to consider):
- Mixed-outcome/continue-on-failure is NOT exercised E2E through
  `TsDevClient.batch_*` in this slice — only via the C++ reference (Step 13)
  and mocked Python unit tests. The 7 in-scope tests are the defined scope; a
  future E2E test driving a mixed-outcome batch through the Python wrapper
  would close Python-side parity.
- pytest-run popup-check evidence is thin (no persisted popup-monitor log for
  the pytest run, unlike the C++ reference and gate). Future TVSs of this kind
  should capture per-phase popup-check output to a log.

## Previously completed (2026-08-30): Python single-op parity E2E (Phase 1) — APPROVED

Verified `TsDevClient` DIRECT mode (elevated Python → `tsdev_cli.exe`
subprocess) single-op parity with the C++ CLI. No code changes needed.

- 9 single-op E2E tests PASS (exit 0, 443.88s; log `tmp/pytest_single_op2.log`):
  `test_{strategy,function,indicator,tradingapp}_lifecycle` (insert/modify/
  delete for all 5 types), `test_showme_lifecycle_e2e` (full
  insert→exists→get-source→modify→get-source→upsert→delete→exists),
  `test_insert_duplicate_raises_already_exists`,
  `test_insert_invalid_source_raises_compile_error_and_autocleans`,
  `test_delete_nonexistent_raises_not_found`,
  `test_exists_case_insensitive_e2e`. 7 batch/directory/orphan tests
  correctly excluded (out of scope — later phases).
- Adversarial review APPROVED: independently re-verified the log, gate-item
  cleanup (`exists:false`), spot-checked test names all cleaned, no code
  changes, no hacks, scope respected.
- Coverage boundary (accepted as minimal slice, disclosed): exists/get-source/
  upsert isolated coverage is ShowMe-only for the other 4 types. Acceptable
  because `client.py` single-op methods are type-parameterized
  (`_single(op,type,name,source)`; no type-specific Python branching beyond
  `_validate_type`) — per-type repetition adds no Python-layer signal. The
  type-specific behavior lives in the C++ CLI (separately verified).
- Mandatory production CLI E2E gate passed: function
  `PyParityProbeFn_20260830_161411` insert(progId 102)→exists→modify→
  get-source(`= 42;`)→delete→exists(false); no TS crash/error dialogs;
  cleanup verified.

## TVS just completed (2026-08-30): Python DIRECT-mode batch mixed-outcome E2E — APPROVED

Closed the Python-side parity gap for mixed-outcome / continue-on-failure
(CLI exit 2) through `TsDevClient.batch_insert` DIRECT mode, previously only
covered by the C++ reference (`tests/test_batch_e2e.ps1` Step 13) and mocked
unit tests. No production code changes.

- Added ONE E2E test `test_e2e_batch_mixed_outcome_continue_on_failure` to
  `TestE2E` in `tests/test_tsdev_client.py` (after
  `test_e2e_batch_upsert_mixed`), plus `_MixA/_MixB/_MixC` to `_TEST_NAMES`
  for the autouse cleanup sweep. 3-item `batch_insert` (all `function`):
  A=pre-inserted duplicate → `success=False, exit_code=5,
  error.code="already_exists", error.existing_prog_id==progId_a`; B=new valid
  → `success=True, prog_id>0`; C=compile-error source
  (`"this is not valid easylanguage !!!\n"`, reused from
  `test_insert_invalid_source_raises_compile_error_and_autocleans`) →
  `success=False, exit_code=8, error.code="compile_error"`. Asserts the call
  returns normally (no raise = continue-on-failure), `len==3`, per-item order,
  and post-batch persistence (B exists w/ matching progId; A unchanged; C not
  created). Attribute names verified against `results.py`
  (`TsDevErrorInfo.existing_prog_id`/`code`/`compile_errors`).
- Verified from an elevated shell: new test PASSED (70.41s);
  `test_no_orphans_after_suite` PASSED (85.77s). Exit codes 5/8 confirmed
  against the real CLI (`tsdev_cli.cpp` exit-code scheme).
- Adversarial review APPROVED: test code, attribute names, DIRECT-mode
  routing (`_serve_active()` False when elevated → `--batch-stdin` path),
  no exception suppression, real (non-mocked) failure surfaces, no scope
  drift. One cleanup defect (stray `orphans_*.txt` verification artifacts in
  repo root) was fixed before closure.
- Mandatory production CLI E2E gate PASSED: function
  `GateMixFn_20260830_175926` insert(progId 120)→exists→modify→
  get-source(`= 2;`)→delete→exists(false); no TSCrashReport/WerFault/orchart
  error dialogs; temp sources cleaned.

Note (disclosed, non-blocking): the docstring says "Mirrors C++ reference
Step 13"; Step 13's item C is a missing-source pre-validation failure whereas
this test's item C is a compile-error (exit 8). The task spec required exit 8,
so this is a faithful closure of the Python-side continue-on-failure gap, not
a byte-for-byte mirror of Step 13's failure modes.

## Next TVS

**Phase 3: Python `TsDevClient` batch parity E2E (SERVE mode).** Verify
`_batch_serve` (`client.py:865-894`) and `_batch_dir_serve`
(`client.py:909-1007`) repeated single-item broker requests produce equivalent
end-state to the DIRECT-mode batch path (Phase 2, APPROVED). Serve mode is a
distinct code path (repeated single items over a long-lived named-pipe broker,
no `flushPerItem=false` batch semantics) — its verification is separate from
direct-mode batch. Serve-mode single-op upsert + non-strategy type coverage in
`tests/test_serve_e2e.py` is also still open.

This is larger than one minimal TVS and should be SPLIT into sub-phases by the
next team, e.g.:
- 3a: serve-mode single-item batch parity (`_batch_serve` for
  batch_insert/modify/delete/upsert) — the smallest end-to-end verifiable
  serve-mode batch slice.
- 3b: serve-mode directory batch parity (`_batch_dir_serve`).
- 3c: serve-mode single-op upsert + non-strategy type coverage in
  `test_serve_e2e.py`.

Do NOT plan beyond the next TVS; the next team will find their next step.

## Key references

- Function-modify / Strategy recalc signal (static + runtime findings, SSOT):
  `docs/KNOWLEDGE_BASE.md` §21 (static analysis), §22 (runtime confirmation),
  §22.8 (cmd 0x0D dispatch confirmation), §22.10 (Strategy recalc confirmation).
- Read-only function root cause (CONFIRMED): KB §20.
- CLI modify path: `src/tsdev_lib.cpp` `verifyModule` (post-success cmd 0x0D
  dispatch + NotifyLibChanged block) and `modifyEntry`;
  `traverseReverseDependents` (local re-verify, runs AFTER cmd 0x0D dispatch).
- orlib20 ordinal resolvers: `src/orlib_resolver.cpp` (CLI resolves
  28/57/66/67/69/70/85; does NOT resolve 10/17/25/56/68/71 — not needed for
  the cmd 0x0D dispatch which uses the generic dispatcher FUN_101cf5b0).
- Cross-process notification mechanism (SSOT, current):
  `docs/RE/RE_OrlibRegisterNotificationHandle.md` — shared `SHARE_WO` 32-slot
  HWND array, `_OrlibRegisterNotificationHandle@4`, `Ordinal_57` broadcast.
  (KB §19/§22 corrected for the 2026-08-30 runtime verification.) Dependents/callers: KB §18.
- Probe sources: `tmp/probe_modify_function_v1.el` (=`1`),
  `tmp/probe_modify_function_v2.el` (=`2`),
  `tmp/probe_modify_function_caller.el`,
  `tmp/probe_modify_showme_v1.el` (plots High), `tmp/probe_modify_showme_v2.el`
  (plots Low), `tmp/probe_modify_showme_fn.el` (ShowMe calling function),
  `tmp/probe_modify_strategy_v1.el`, `tmp/probe_modify_strategy_v2.el`,
  `tmp/probe_modify_strategy_fn.el` (Strategy calling function),
  `tmp/probe_modify_tradingapp_v1.el`, `tmp/probe_modify_tradingapp_v2.el`,
  `tmp/probe_modify_tradingapp_fn.el` (TradingApp calling function).
