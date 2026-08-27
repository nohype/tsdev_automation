# PM Working Memory — TSDev Automation

> When acting as PM, read this file first every turn and after context compaction.
> Condensed from the prior record; status and evidence retained through 2026-08-26. This is PM operational memory, not a replacement for the authoritative plans and RE records cited below.

## 1. Current truth and gates

### Completion and handoff gates
- Before marking any TVS complete, compare the implementation with that TVS's original plan: purpose of every point, omissions, improper deferrals, shortcuts/hacks, and purpose-changing divergences. Any finding requires another planning/execution round; only a review with no findings permits completion. M.3's first comparative review found five findings; the recorded second review found none.
- After every approved TVS, run the real production C++ `tsdev_cli` insert/modify/delete E2E required by `docs/E2E_TEST_SPEC.md` from an elevated direct CLI. Capture the exact post-TVS argv/procedure, result, exit code, item/ProgId, and cleanup. A unit/mock test, aggregate regression count, notification fixture, or observation is not a substitute. This narrower post-TVS gate is distinct from six-operation production acceptance; the audit status of retained claims is in §4.
- Every substantive result needs an independent adversarial review; disapproval requires fixes and another review. Keep one narrow TVS/subtask at a time and hand off only with truthful status.

### Elevation contract
- The elevation contract is defined in `AGENTS.md` (mandatory rules) and `docs/BUILD.md` (operational); see those for the direct-CLI/serve-broker/exit-11/use_serve rules. The `-NoTs` regression path is non-TS (sigtool plus non-TS C++ unit-test executables); see `tests/run_unit_tests.ps1` for the current non-TS inventory.
- **PM operational nuance (not in AGENTS.md):** In Windows PowerShell 5.1, child `Write-Host` is not reliable stdout evidence; capture exit codes or use a transcript.

## 2. Workstream status

- **A–K:** historical library/CLI/Python-wrapper completion summaries are retained, but they are not a current production-acceptance claim without the §4 audit evidence.
- **Acceptance/type scope:** Production acceptance covers six operations, distinct from type coverage: five of eight warehouse mappings are confirmed and ActivityBar, PaintBar, and ProbabilityMap remain unconfirmed. Implementation and happy-path support, including historical serve/batch results, are distinct from current production acceptance and post-TVS evidence.
- **L:** Workstream L is closed (final independent review completed). See `AGENTS.md §"Current state and TODO"` for authoritative workstream status.
- **M:** M.1/M.2/M.3 are approved historical slices; M.3b is planned; M.4 is implemented but manual/blocked (CLI-created object-code defect plus the three M.4 blocker escalations in §3); M.5 is pending; no approved post-TVS production-E2E claim exists for M.4. See `AGENTS.md §"Current state and TODO"` for authoritative workstream status.

## 3. M slice facts

- **M.1 (approved historical slice, 2026-08-24):** exact compiler lifecycle uses orlib20 ordinals 66 (cdecl alloc+ctor) and 67 (stdcall dtor+free), with category `+0x08`, subtype `+0x0C`, ProgId `+0x10`, HWND `+0x18`; `+0x00/+0x04` are zeroed before verify. See `docs/SIGNATURE_CATALOG.md` for ordinal details and for current/historical signature/catalog counts (tracked exports are not total PE exports). Residuals: `+0x28` is unasserted, no live compiler-object dump exists, and serve E2E was not separately run.
- **M.2 (approved historical slice, 2026-08-25):** Current `saveSource()` passes non-NULL flags and a non-NULL 64-byte password buffer to `_SaveModule_New@20`. The buffer is zero-initialized first, then its leading 10 bytes are populated with the internal default `"!p$Kn@aSus"`; only the unused tail remains zero. Insert flags are `(getCategory(type)&7)|0x60`; modify adds `0x10` (therefore `(getCategory(type)&7)|0x70`); CLI expiry support remains absent. Batch flag coverage was indirect. Modify-failure persistence is stage-specific: a compile failure before artifact writes, or a finalization failure with `FUN_10011230` returning false, can leave `.ell` unchanged; Fault A is a post-compile `.ell` write failure in which XML/warehouse source may change while stale `.ell` remains. The current post-verify persistence validator detects and reports Fault A; it only detects and fails, with no rollback/retry/restore (`docs/RE/RE_VERIFYMODULE_FINALIZATION_TRUNCATION.md`, `docs/RE/RE_L8_STALE_WRITE_FAULT_A.md`).
- **M.3 (approved historical slice, 2026-08-25):** successful insert/modify/upsert verification and successful delete for category != 4 send orlib20 ordinal 57 with `0x40A`, `wParam=0`; failed verify and Function delete send nothing. See `docs/RE/RE_M3_0x40A_NOTIFICATION.md` for the `lParam` packing formula. Identical-source no-op behavior and the delete-after-disk-cleanup nuance remain documented residuals.
- **M.4:** implementation exists, but the CLI-created object-code defect (zero element count/pointer) blocks its dependency test; M.4 is not acceptance. The linker-based existing-record enumeration work is retained as historical implementation evidence, not a current acceptance result.

### M.4 review escalations (separate, pre-existing blockers)

- M.4 blocker escalations are documented in full in `AGENTS.md §"M.4 blocker escalations"`; they remain escalated and unresolved. One-line orientation:
  - **Dependent revalidation flag:** `callVerify()` hardcodes the verify fourth argument to `1` for dependents despite the TSDev RE showing `primary=0`.
  - **Old master-record capture:** `saveSource()` never captures the old master record, so the metadata contract-change gate is effectively inactive (`contractChanged` defaults true).
  - **Cycle safety under the default change gate:** with `contractChanged=true`, `traverseReverseDependents()` does not establish safe termination for a reachable cycle.
- These three are pre-existing review findings, not newly introduced by this cleanup. M.4 remains manual/blocked; the CLI-created object-code defect remains a separate existing blocker.

## 4. Post-TVS production-E2E audit ledger

The mandatory post-TVS check is a real built C++ `tsdev_cli` against live TradeStation, covering the required insert/modify/delete procedure from an elevated direct CLI. Capture the exact argv/procedure, result, exit code, item/ProgId, and cleanup. This gate has no general Desktop-state requirement; any such state belongs to an individual test procedure. It is distinct from six-operation production acceptance.

The exact historical post-TVS command transcripts for the retained slices are unavailable in this memory. Historical test outputs and aggregate counts are not current acceptance evidence.

- **A–K:** per-TVS production transcripts are unavailable.
- **L:** Workstream L is closed; its retained exact post-TVS production transcript(s) are unavailable in this memory.
- **M.1:** exact post-M.1 production transcript is unavailable.
- **M.2:** exact post-M.2 production transcript is unavailable.
- **M.3:** exact post-M.3 production transcript is unavailable.
- **M.4:** manual/blocked; no approved post-TVS production-E2E claim exists. Its gate remains pending resolution of M.3b, the CLI-created object-code defect, and the three separate M.4 blocker escalations in §3.

### 2026-08-27 comprehensive QA pass (current evidence)

A full QA pass was run on 2026-08-27 against live TradeStation 9.5 from an elevated process (`p14s\user`, Elevated=True). Build: `cmake --build build --config Release --target test_regression` succeeded. Two test defects were found and fixed; adversarial review APPROVED both fixes. All gates below are current evidence from this session, not historical.

**C++ unit tests (`tests/run_unit_tests.ps1`):** 36/36 passed, 0 failed, 0 skipped. Includes `test_0x40a_notification` (179/179 assertions, 16/16 cases) after fixing Cases 15/16 (timing + ProgId filtering; see fix notes below).

**No-TS C++ unit tests (`tests/run_unit_tests.ps1 -NoTs`):** 17/36 passed, 0 failed, 19 skipped (TS-dependent). Baseline sanity check.

**Full regression (`tests/run_regression.ps1`):** All sub-suites passed. CLI E2E 34/34 (Part A 16/16, Part B 8/8, Part C 10/10). sigtool 12/12, exists 5/5, batch-stdin 8/8, upsert 9/9, get-source 8/8, exists case-insensitive 9/9, batch-upsert 5/5, directory-batch 9/9.

**L7 red fault injection (`tests/test_unknown_link_regression.ps1 -IncludeRedFaultInjection`):** 289/289 passed (L1: 40, L4: 195, L5: 14, L6: 4, L7: 36). L7.1/L7.2 GREEN detect-and-fail (Fault A), L7.3 GREEN detect-and-report (Fault B). Fixed em-dash encoding issue on line 4071 that prevented the harness from running.

**Post-TVS production E2E gate (five-type insert/modify/delete):** 20/20 PASS across all 5 supported types (Strategy, Indicator, Function, TradingApp, ShowMe). Run ID `20260827_083117`. Each type: insert (exit 0, ProgId captured), modify (exit 0, ProgId preserved), delete (exit 0), delete-negative (exit 5, "not found in the warehouse"). .ell files created on insert and removed on delete. No orphaned artifacts. No cleanup failures. ProgIds: strategy=8825, indicator=2838, function=1473, tradingapp=2842, showme=2843.

**Python non-E2E (`pytest -m "not e2e"`):** 148 passed, 16 deselected (0.47s).

**Python full suite (`pytest -q`):** 164 passed (885.73s / 14:45). Includes 16 E2E-marked tests.

**Serve mode verification:** Direct Python test from elevated process confirmed both paths work: `TsDevClient(use_serve=False)` insert/exists/delete (ProgId=8818) and `TsDevClient(use_serve=True)` insert/exists/delete (ProgId=8818). Serve broker starts correctly, creates named pipe, and processes framed JSON requests.

**Serve E2E (`tests/run_serve_e2e.ps1`):** 3 passed, 1 skipped, exit 0. The runner launches unelevated Python via Task Scheduler, which then elevates the broker via UAC (`ShellExecuteExW`/`runas`). Run with UAC auto-elevation (`ConsentPromptBehaviorAdmin=0` temporarily, restored to 5 after). Fixed a here-string bug: `$null` was not backtick-escaped in the inner script template, causing `if ( -eq $LASTEXITCODE)` (missing left operand). After the fix, all 3 serve E2E tests pass: `test_serve_full_lifecycle`, `test_serve_compile_failure_recovery`, `test_serve_batch_over_serve_l14b`. `test_serve_long_lived_stress_l15` is intentionally skipped.

**Fixes applied (adversarial review APPROVED):**
1. `tests/test_unknown_link_regression.ps1` line 4071: em-dash (U+2014) → ASCII hyphen in a Markdown header string literal. The em-dash caused a PowerShell parse error that prevented the L7 harness from running. No semantic change.
2. `tests/test_0x40a_notification.cpp` Cases 15/16: two test-harness defects fixed. (a) Receiver thread pump timeout increased from 5s to 120s — the 5s budget was too short for CLI child-process startup + EL compile latency (the CLI's notification send path was confirmed correct via `src/tsdev_lib.cpp:1162` and `1833-1848`). (b) Added ProgId-based filtering (`extractProgIdsFromJson` + `filterByProgIds`) to isolate the CLI child's notifications from concurrent orlib20 operations via the shared SHARE_WO HWND array (per `docs/RE/RE_M3_EVENTSVIAHANDLES_LOCALITY.md`). No production code changed.
3. `tests/run_serve_e2e.ps1` line 104: `$null` was not backtick-escaped in the here-string inner script template, causing the outer PowerShell to expand it to empty. The generated inner script received `if ( -eq $LASTEXITCODE)` (missing left operand), causing a parse error that made the runner report failure even when pytest passed. Fix: `` `$null `` (backtick-escaped). After the fix, serve E2E passes 3/3 (1 skipped).

## 5. Verification baselines: historical versus current

- Current and historical resolver/catalog counts (signature definitions, tracked exports, globals, patch sites; tracked exports are not total PE exports) are in `docs/SIGNATURE_CATALOG.md`.
- The current `-NoTs` path runs sigtool plus the non-TS C++ unit-test executables; see `tests/run_unit_tests.ps1` for the current non-TS inventory.
- Historical suite, case, Python, and aggregate result counts are unverified historical/transient baselines, not evidence of a currently green tree or current production acceptance.
- The last recorded green CI run is commit `8c50793`, Actions run `32449304024`. Git history places the M.1–M.3 implementation in later local commit `f88f082`; therefore CI predates those changes and does not validate them. The old “no commit was created” notes are stale; the M code is committed locally rather than uncommitted in the checked-out history. No post-M.1–M.3 CI result is recorded, so do not assert a current green state.
- The recorded regression-harness command is `powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1`; `-NoTs` is the non-TS path and `-ExtendedStress` is opt-in stress. L scripts support Windows PowerShell 5.1 and `pwsh` 7. Routine L7 runs write evidence to gitignored `tmp\`; only `-UpdateTrackedEvidence` updates the tracked L7 evidence file.

## 6. Manual crash recovery

- C++ `vehCrashHandler` is installed first and terminates the project process for access violation `0xC0000005`, stack overflow `0xC00000FD`, stack buffer overrun `0xC0000409`, and heap corruption `0xC0000374`, preventing the normal crash dialog. Python `TsDevCrashError` handles signed/unsigned NT-status exits, including serve-broker exit retrieval. `TSCrashReport.exe`, not WerFault, hosts TradeStation crash dialogs.
- Run the following only after a crash/hang. It is an inspection-only process-tree listing, not a kill list. `TSCrashReport`, WerFault, ORPlat, TSELAppHost, ordllhst, orchart, and chart-host processes are candidates to inspect; names alone are insufficient to select a process.

```powershell
Get-CimInstance Win32_Process -ErrorAction Stop |
  Select-Object ProcessId, ParentProcessId, Name, CommandLine |
  Sort-Object ProcessId |
  Format-Table -Wrap
```

- If the inspection query terminates or errors, discard its partial or absent output. The operator must manually confirm each exact PID, full command line, and parent/child relationship, and confirm that it belongs to the crashed CLI/test tree or is a known crash-report/chart-host descendant. Include a parent `powershell.exe`/`pwsh.exe` only when its full command line identifies the dedicated test runner. Unrelated live TradeStation and PowerShell processes are not implied targets.
- Immediately before termination, rerun the inspection successfully and revalidate every selected PID and full command line; abort if a PID is gone, reused, or changed. This revalidation prevents stale-PID termination.
- After that revalidation and manual confirmation, run one explicit command per confirmed PID; do not pipe the query into `taskkill` or use an automated selection/kill loop. Replace `<pid>` with the revalidated decimal PID in `taskkill /PID <pid> /T /F`; `/T` may terminate its descendants only as part of that confirmed crashed/test tree.

Do not rely on clicking “Don't Send” or Alt+F4. Do not use a blanket PowerShell kill, delete library artifacts, or perform other destructive cleanup. This is manual crash recovery for a confirmed crash or hang; it is not a Desktop-state requirement and is not a procedure for ordinary repository diagnostics. PowerShell is the shell: use `;`, not `&&`, and do not use bash heredocs.

### Standing boundaries

- Never derive metadata, names, types, ProgIds, dependencies, or correctness from mutable source/comments; use warehouse records/APIs. Do not call TSDev private functions or mask compiler errors/problems with hacks.
- **L7 red fault-injection scope:** Opt-in `-IncludeRedFaultInjection` intentionally targets the configured TradeStation installation/MyWork path (by default the real installation) and mutates only its exact L7 artifacts to exercise Fault A/B; it is not part of normal regression.
- Every orlib20 call needs a Ghidra-verified ABI, required thread-check patches, SEH protection, and the 64-MiB-stack worker pattern. Frida is direct CLI only (`frida-trace`, `frida -l`, `frida -p`); attach/detach carefully.

## 7. Single source of truth (SSOT)

The SSOT practice and the authoritative-location index are in `AGENTS.md §"Single source of truth (SSOT)"`. Counts, listings, and details that could drift live in one authoritative location; other docs and code comments reference the SSOT rather than restating values. When editing, change only the SSOT.

## 8. Authoritative pointers

- Current M order and M.3b/M.4/M.5 gates: `docs/workstream_tsdev_parity_plan.md`.
- L reference plan: `docs/workstream_unknown_link_fix_plan.md`.
- L technical evidence: `docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md`, `docs/RE/RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md`, `docs/RE/RE_L8_STALE_WRITE_FAULT_A.md`, `docs/RE/RE_VERIFYMODULE_FINALIZATION_TRUNCATION.md`.
- M evidence: `docs/RE/RE_M2_SAVEMODULE_NEW.md`, `docs/RE/RE_M3_0x40A_NOTIFICATION.md`, `docs/RE/RE_M3_EVENTSVIAHANDLES_LOCALITY.md`, `docs/RE/RE_M4_REVERSE_DEPENDENT_REVALIDATION.md`.
- L7 harness and durable-evidence option: `tests/test_unknown_link_regression.ps1` with `-IncludeRedFaultInjection` and optional `-UpdateTrackedEvidence`.
