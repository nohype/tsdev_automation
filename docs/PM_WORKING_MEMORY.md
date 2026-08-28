# PM Working Memory — TSDev Automation

> Read this file first every turn and after context compaction. Short, current, points to other docs for historical detail.

## 1. Current truth and gates

### Completion and handoff gates
- Before marking any TVS complete, compare implementation against that TVS's original plan: purpose of every point, omissions, improper deferrals, shortcuts/hacks, purpose-changing divergences. Any finding → another round; zero findings → complete.
- After every approved TVS, run real production C++ `tsdev_cli` insert/modify/delete E2E (`docs/E2E_TEST_SPEC.md`) from elevated direct CLI. Capture argv/procedure, result, exit code, item/ProgId, cleanup. Unit/mock/aggregate counts are NOT substitutes. Distinct from six-operation production acceptance; audit status in §4.
- Every substantive result needs independent adversarial review; disapproval → fixes + another review. One narrow TVS/subtask at a time; hand off only with truthful status.

### Elevation contract
- Defined in `AGENTS.md` (rules) + `docs/BUILD.md` (operational): direct-CLI/serve-broker/exit-11/use_serve. `-NoTs` path = sigtool + non-TS C++ unit tests (`tests/run_unit_tests.ps1`).
- **PM nuance:** Windows PowerShell 5.1 child `Write-Host` is not reliable stdout; capture exit codes or use transcript.
- **UAC auto-elevation for serve E2E:** `tests/run_serve_e2e.ps1` triggers a UAC consent prompt. Disable before run: `reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System" /v ConsentPromptBehaviorAdmin /t REG_DWORD /d 0 /f`; restore to `5` after.

## 2. Workstream status

- **A–K:** complete (historical); not a current production-acceptance claim without §4 audit evidence.
- **L:** closed (final review done). See `AGENTS.md §"Current state and TODO"`.
- **Acceptance/type scope:** six operations for production acceptance; five of eight warehouse mappings confirmed (ActivityBar, PaintBar, ProbabilityMap unconfirmed).
- **M.4b.1 (ACTIVE WORK):** RE-OPENED — 13 missing warehouse commands identified via Frida trace; next: Ghidra-analyze; see `docs/M4B1_HANDOFF.md`.

## 3. M slice facts

- **M.1** (approved 2026-08-24): compiler lifecycle via orlib20 ordinals 66/67; offsets category `+0x08`, subtype `+0x0C`, ProgId `+0x10`, HWND `+0x18`. See `docs/SIGNATURE_CATALOG.md`.
- **M.2** (approved 2026-08-25): `saveSource()` flags/password buffer details; Fault A post-compile `.ell` write failure detected/reported (no rollback). See `docs/RE/RE_M2_SAVEMODULE_NEW.md`, `docs/RE/RE_L8_STALE_WRITE_FAULT_A.md`.
- **M.3** (approved 2026-08-25): insert/modify/upsert verify + delete (cat≠4) send orlib20 ordinal 57 with `0x40A`, `wParam=0`; failed verify/delete send nothing. See `docs/RE/RE_M3_0x40A_NOTIFICATION.md`.
- **M.3b.1** (approved): diagnosis-only — root cause in `docs/RE/RE_M3B_OBJECT_CODE_VERSION_DEFECT.md`; INFOREC counts zeroed by `InitTokenListNew`, populators return NULL when counts stay 0.
- **M.3b.2a** (approved): Phase 1 diagnosis — RED test confirmed INFOREC counts are 0; no higher-level TSDev mechanism missing from CLI.
- **M.3b.2b** (approved): Phase 2 dynamic — M.3b "defect" is a TEST ARTIFACT (declaration-free source `X = 1;` legitimately has elemCount=0); CLI-compiled items WITH declarations produce non-zero elemCount.
- **M.3b.2c** (approved): Phase 3 — RED test fixed + parameterized for all 5 types; §13.6 sample-size gap closed; M.4 note §8.8 corrected. New finding: linker takes expanded whType, not category — production `enumerateReverseDependents()` fails for TradingApp/ShowMe.
- **M.4** (re-scoped 2026-08-27): linker-based `enumerateReverseDependents` is wrong abstraction level (parses orlib20 internal buffers TSDev.exe never touches). Split into M.4a (RE) + M.4b (implementation).
  - **M.4a** (COMPLETE, approved iter 2): warehouse supplies reverse dependents directly via cmd `0x3B` (`IProgram::Callers`), not by enumerating+filtering. Chain: cmd `0x0D` (root record) → cmd `0x3B` (callers) → SafeArray at `warehouse+0x26` → BST. SafeArray entry stride `8 + nameLength + 0x16`. Contract-change detection (`FUN_004d9040`) is warehouse-metadata-derived (no source-text). See `docs/RE/RE_M4_REVERSE_DEPENDENT_REVALIDATION.md` §9.
  - **M.4b.1** (RE-OPENED — active work): enumeration primitive — cmd `0x0D` → cmd `0x3B` → SafeArray protocol implemented and confirmed dynamically, but COM server returns zero callers for ALL items (dependency DB empty). WHServer.exe is OUT-OF-PROCESS (LocalServer32); missing a step TSDev performs to populate the dependency DB. **13 missing warehouse commands identified via Frida trace; next: Ghidra analysis.** See `docs/M4B1_HANDOFF.md`. Does NOT touch blockers/linker-removal.
  - **M.4b.2** (pending): wire into `traverseReverseDependents`, implement contract-change detection + cycle safety, fix three blocker escalations, remove linker-based code.
- **M.5** (pending).

### M.4 blocker escalations (pre-existing, documented in `AGENTS.md §"M.4 blocker escalations"`)
- **Dependent revalidation flag:** `callVerify()` hardcodes verify fourth arg to `1` for dependents (TSDev RE shows `primary=0`).
- **Old master-record capture:** `saveSource()` never captures old master record → metadata contract-change gate inactive (`contractChanged` defaults true).
- **Cycle safety:** with `contractChanged=true`, `traverseReverseDependents()` has no safe termination for reachable cycles.
- These three are addressed in M.4b.2, NOT M.4b.1. M.3b "CLI-created object-code defect" was a test artifact (no longer a blocker). M.4 remains manual/blocked pending M.4b.1.

## 4. Post-TVS production-E2E audit ledger

Mandatory post-TVS gate: real built C++ `tsdev_cli` against live TradeStation, insert/modify/delete from elevated direct CLI. Capture argv/procedure, result, exit code, item/ProgId, cleanup. Distinct from six-operation acceptance. Diagnosis/research-only TVSs with no `src/` changes: gate N/A.

- **A–K:** transcripts unavailable.
- **L:** closed; transcripts unavailable.
- **M.1 / M.2 / M.3:** transcripts unavailable.
- **M.3b.1:** N/A — no src changes (diagnosis-only).
- **M.3b.2a:** N/A — no src changes (diagnosis-only).
- **M.3b.2b:** N/A — no src changes (diagnosis-only).
- **M.3b.2c:** N/A — no src changes (test-fix + doc-correction; full regression passed 12/13 suites, serve E2E re-run with UAC auto-elevation).
- **M.4:** blocked; no post-TVS claim.
- **M.4a:** N/A — no src changes (research-only).

### 2026-08-27 comprehensive QA pass (current evidence)
Full QA from elevated process against live TradeStation 9.5. All gates green: C++ unit 36/36, No-TS 17/36 (19 skipped), regression all sub-suites (CLI E2E 34/34, sigtool 12/12, exists 5/5, batch-stdin 8/8, upsert 9/9, get-source 8/8, exists-ci 9/9, batch-upsert 5/5, directory-batch 9/9), L7 red fault injection 289/289, post-TVS five-type E2E 20/20, Python non-E2E 148/0, Python full 164/0, serve E2E 3/1-skipped. Three test-harness fixes applied (adversarial review APPROVED): em-dash→ASCII in L7 harness, 0x40A test timeout+ProgId filtering, serve E2E here-string `$null` escape.

## 5. Verification baselines
- Current/historical resolver/catalog counts: `docs/SIGNATURE_CATALOG.md`.
- `-NoTs` path: sigtool + non-TS C++ unit tests (`tests/run_unit_tests.ps1`).
- Historical counts are unverified baselines, not current-green evidence. Last recorded green CI: commit `8c50793` (predates M.1–M.3). Do not assert current green state.
- Regression: `powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1`; `-NoTs` = non-TS; `-ExtendedStress` = opt-in.

## 6. Manual crash recovery
- `vehCrashHandler` terminates on access violation/stack overflow/heap corruption; Python `TsDevCrashError` handles NT-status exits. `TSCrashReport.exe` hosts crash dialogs (not WerFault).
- After crash/hang: inspection-only process-tree listing (not a kill list). Manually confirm each PID, full command line, parent/child. Rerun inspection + revalidate PIDs immediately before termination. One explicit `taskkill /PID <pid> /T /F` per confirmed PID. No blanket kills, no artifact deletion. PowerShell shell: use `;`, not `&&`.
- **Standing boundaries:** never derive metadata from source/comments; use warehouse records/APIs. No TSDev private functions, no error masking. Every orlib20 call needs Ghidra-verified ABI, thread-check patches, SEH protection, 64-MiB-stack worker. Frida direct CLI only.

## 7. SSOT
SSOT practice + authoritative-location index in `AGENTS.md §"Single source of truth (SSOT)"`. Change only the SSOT; other docs reference it.

## 8. Authoritative pointers
- M order/gates: `docs/workstream_tsdev_parity_plan.md`. M.4b.1 handoff: `docs/M4B1_HANDOFF.md`.
- L plan: `docs/workstream_unknown_link_fix_plan.md`. L evidence: `docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md`, `RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md`, `RE_L8_STALE_WRITE_FAULT_A.md`, `RE_VERIFYMODULE_FINALIZATION_TRUNCATION.md`.
- M evidence: `docs/RE/RE_M2_SAVEMODULE_NEW.md`, `RE_M3_0x40A_NOTIFICATION.md`, `RE_M3_EVENTSVIAHANDLES_LOCALITY.md`, `RE_M4_REVERSE_DEPENDENT_REVALIDATION.md` (§9 = M.4a), `RE_M3B_OBJECT_CODE_VERSION_DEFECT.md` (M.3b.1 + M.3b.2a).
- L7 harness: `tests/test_unknown_link_regression.ps1` (`-IncludeRedFaultInjection`, optional `-UpdateTrackedEvidence`).
