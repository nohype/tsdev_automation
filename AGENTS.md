# TSDev Automation

Automates management of the TradeStation Development Environment (TSDev) Library without using the TSDev UI (`TSDev.exe`). The rules below are mandatory.

## Mandatory rules

- **Communicate objectively:** do not flatter, affirm, or tell the user they are right; state facts and answers directly.
- **Never derive metadata from mutable item content.** EasyLanguage source, comments, and naming conventions are arbitrary user-controlled data. Never parse source text for names, types, or other metadata (including assumed `{ name }` comments). Stored names, ProgIds, and types must come from orlib20 warehouse records/APIs.
- **Emulate TSDev.exe:** replicate its APIs, enumeration sequences, cache refreshes, and observable behavior; do not invent workarounds where the application has a proper mechanism.
- **No hacks (hard rule 5; see `docs/PM_WORKING_MEMORY.md`):** never suppress compiler errors/warnings or mask real problems. Fix the root cause or use a clean production solution. If no clean form exists, use an explicit/raw form with an explanatory comment (for example, `0xC0000374 // STATUS_HEAP_CORRUPTION` instead of disabling warning 4005 for `ntstatus.h` redefinitions).
- **L7 red fault-injection scope:** opt-in `-IncludeRedFaultInjection` intentionally targets the configured TradeStation installation/MyWork path (by default the real installation) and mutates only its exact L7 artifacts to exercise Fault A/B; it is not part of normal regression.
- **Direct CLI elevation:** every direct `tsdev_cli` subcommand, including direct `serve`, requires an elevated administrator process. The `isElevated()` check returns false when the process is not elevated, and the CLI reports exit code 11; preserve this behavior.
- **Test and broker elevation are separate contracts:** TS-dependent test executables/harnesses have their own administrator prerequisite. The `-NoTs` path is non-TS: sigtool plus the non-TS C++ unit-test executables (current inventory in `tests/run_unit_tests.ps1`). An elevated parent can pass its token to child PowerShell, test-runner, and CLI processes; that inherited token is the child’s elevation state, not a different CLI contract. `TsDevClient(use_serve=True)` starts a separate elevated named-pipe `tsdev_cli serve` broker through `ShellExecuteExW`/`runas`. With `use_serve=False`, non-elevated Python raises `ElevationRequiredError` before direct launch.
- **Reverse engineering:** use the working Ghidra MCP server (`ghidra`) for static analysis. Use Frida directly from the CLI, never Frida MCP (removed because it was unreliable): `frida-trace`, `frida -l script.js`, or `frida -p PID`. Frida 17.17.0 and 32-bit spawn/attach plus its full JS API are available.

## Single source of truth (SSOT)

Counts, listings, and details that could drift live in ONE authoritative location (the SSOT). Every other place references the SSOT instead of restating the count/list/detail, so that when a value changes only the SSOT needs editing.

| Category | Authoritative location |
|---|---|
| C++ unit test inventory (list + count) | `tests/run_unit_tests.ps1` (`$TestDefinitions` array); `CMakeLists.txt` keeps its `add_dependencies` list as a build requirement |
| Signature/catalog counts (current + historical) | `docs/SIGNATURE_CATALOG.md` (machine-enforced by `tools/sigcodegen.py` + `signatures/*.json`) |
| Python test counts (baseline) | `docs/BUILD.md` |
| Warehouse type mappings (5 confirmed + 3 unconfirmed) | `docs/TS_COMPONENT_TYPES.md` (code SSOT: `src/tsdev_lib.h` enum) |
| M.4 blocker escalations (3 blockers + src line citations) | `AGENTS.md` §"M.4 blocker escalations" (evidence: `docs/RE/RE_M4_REVERSE_DEPENDENT_REVALIDATION.md`) |
| Elevation contract (direct-CLI / exit 11 / serve broker) | `AGENTS.md` (rule) + `docs/BUILD.md` (operational); code: `src/tsdev_cli.cpp`, `src/tsdev_automation/exceptions.py` |
| E2E CLI test cases (34 + 16/8/10 parts) | `docs/E2E_TEST_SPEC.md` |
| Sub-suite case counts + "14 sub-suites" | `tests/run_regression.ps1` |
| Workstream status (L closed; M.1-3 approved; M.3b planned; M.4 blocked; M.5 pending) | `AGENTS.md` §"Current state and TODO" |
| Ordinals 66/67/57 | `docs/SIGNATURE_CATALOG.md` |
| 0x40A packing | `docs/RE/RE_M3_0x40A_NOTIFICATION.md` |

## Operations (six)

- **Insert:** given EasyLanguage and a name, insert an item, compile, and verify; fail loudly on compilation failure.
- **Delete:** given a type and name, delete the item; notify if it does not exist.
- **Modify:** replace an existing item’s source and recompile/reverify; notify if absent and fail loudly on compilation failure.
- **Exists:** given type and name, return whether it exists, its verbatim stored name, and its ProgId when found. C++ uses `--case-insensitive`; Python uses `case_insensitive=True`. Human output is `exists=N`, `stored_name=...`, `progId=N`; `--json` selects JSON. Without the flag, search is case-sensitive (post-compare the stored name). With it, `enumerateRecords` performs a two-phase search: Phase 1 uses `_LoadMasterRecsEX@16` with cached `_stricmp` matching in passes without/with `refreshCache`; Phase 2 uses `FUN_101d05e0` (BST node search) and `FUN_101c36d0` (verbatim warehouse-name getter at node offset `+0x1D`). Never use source comments for the stored name.
- **Get source:** given type and name, emit raw EasyLanguage to stdout or use `--output <file_path>`; decrypt internal encrypted items automatically, as TSDev does.
- **Upsert:** given type, name, and a source file, modify if present or insert otherwise.

## Acceptance criteria

Production acceptance requires all six operations to work reliably and consistently through the C++ CLI (`tsdev_cli`) and Python wrapper (`TsDevClient`); implementation and happy-path support are not production acceptance. This six-operation acceptance target is distinct from type coverage: five of eight warehouse mappings are confirmed, while ActivityBar, PaintBar, and ProbabilityMap remain unconfirmed. Historical serve/batch results are not current production acceptance or post-TVS evidence. The CLI must support batch operations from both a manifest path and stdin (without a temporary manifest), so Python can drive batches directly.

## Stack and process

- C++ with the available Visual Studio Build Tools.
- Reverse engineer TSDev/orlib20 with Ghidra for static work and direct Frida CLI instrumentation for runtime work. Prefer the smallest, least-complex standalone implementation that reproduces TSDev behavior.

## PM and subagent rules

Skip the PM rules when operating as a subagent. When acting as PM:

- Delegate any work that can be delegated; do not perform work or read files/URLs that can be handed to a subagent. Perform management only and remain ultimately responsible for correctness and production quality.
- Use the `universal` profile by default for every narrow task (research, MCP, analysis, implementation, tests, or docs). Use `subagent_general` only if `universal` fails or lacks a required capability. Use `subagent_explore` only for trivial read-only lookups; if insufficient, retry `universal`, then `subagent_general` only if `universal` fails.
- Maintain `docs/PM_WORKING_MEMORY.md` with PM-role reminders, hard rules, current TVS, findings, decisions, and cross-turn facts; update deprecated/superseded information. When acting as PM, read this file first every turn and after context compaction.

## Work orchestration

- Decompose complex work into small atomic subtasks; a specialized subagent decomposes complex work, and each subagent handles exactly one narrow concern.
- Every substantive research result, analysis, plan, document, or implementation requires independent adversarial review. The reviewer receives the original task and result, checks 100% completion, scope/drift, and hacks/shortcuts, then APPROVES or DISAPPROVES with findings. A disapproval requires implementer fixes and another review until approval.
- Work iteratively: one minimal Thin Vertical Slice (TVS) at a time; determine the current state and highest-ROI next slice, update relevant docs, and use TDD (red-green).
- After every approved TVS, run a passing real production CLI E2E of `tsdev_cli insert/modify/delete` using the latest code from an elevated direct CLI. Capture the exact post-TVS argv/procedure, result, exit code, item/ProgId, and cleanup before relying on approval or closure. This narrower gate supplements unit/regression tests and is distinct from six-operation production acceptance; it is mandatory before the TVS is done. The full five-type procedure and semantic error cases are in `docs/E2E_TEST_SPEC.md`.

## TradeStation component types

TradeStation has eight EasyLanguage types: **ActivityBar, Function, Indicator, PaintBar, ProbabilityMap, ShowMe, Strategy, TradingApp**. Strategies cannot use `Plot`; only Indicators, ShowMe, and PaintBar can plot. The authoritative type-to-`whType` mapping table (5 confirmed, 3 unconfirmed) is in `docs/TS_COMPONENT_TYPES.md`; the code SSOT is the `src/tsdev_lib.h` enum. The CLI currently supports the five confirmed types; ActivityBar, PaintBar, and ProbabilityMap remain unconfirmed.

## Relevant paths

- Default TradeStation installation root: `C:\Program Files (x86)\TradeStation 9.5`
- Default TSDev executable: `C:\Program Files (x86)\TradeStation 9.5\Program\TSDev.exe`
- Default compiled/encrypted TSDev Library: `C:\Program Files (x86)\TradeStation 9.5\MyWork\Bin`
- Authoritative signatures: `signatures/*.json` → `tools/sigcodegen.py` → generated `signatures.gen.h`; temporary working copies: `tmp/`.
- Regression entry point: `tests\run_regression.ps1`; unknown-link harness: `tests/test_unknown_link_regression.ps1`; auto-cleanup coverage: `tests/test_auto_cleanup.cpp`; baseline DLLs: `tests/fixtures/` (test runners can prompt for the baseline DLL); CLI install discovery also accepts `--ts-path`.
- CI: `.github/workflows/ci.yml` runs the Win32 build, `-NoTs` regression, and mocked pytest on pushes/PRs, and attaches the bundle zip to `v*` releases. CMake POST_BUILD assembles `dist/` and a timestamped zip in `artifacts/`; `tools/create_bundle_zip.ps1` produces release bundles. Bundle lookup checks `<package_parent>/tsdev_cli.exe` before the source-tree walk-up. Runtime Python dependencies are stdlib-only.

## Documentation index

Read this file first. Read the following on demand:

**Operational**
- `docs/BUILD.md` — build, tests, and baseline-DLL updates.
- `docs/E2E_TEST_SPEC.md` — CLI E2E procedure for Strategy, Indicator, Function, TradingApp, and ShowMe, including semantic failures.
- `docs/SIGNATURE_CATALOG.md` — authoritative current and historical resolver/catalog counts (signature definitions, tracked exports, globals, patch sites); use for signatures, patches, and `OrlibResolver`.
- `docs/VERSION_DETECTION.md` — three-tier orlib20 version detection and version manifest.
- `docs/TS_COMPONENT_TYPES.md` — component purposes and warehouse mappings.
- `docs/FEASIBILITY.md` — orlib20 primitives, parameter order, and INSERT/MODIFY/DELETE pseudocode.
- `docs/workstream_serve_plan.md` — Workstream K serve mode (K.1–K.6b complete).
- `docs/workstream_unknown_link_fix_plan.md` — Workstream L reference plan.
- `docs/workstream_tsdev_parity_plan.md` — Workstream M TSDev parity; M.1–M.3 are approved historical slices, M.3b is planned, M.4 is implemented but manual/blocked, and M.5 is pending.

**Python integration**
- `docs/INTEGRATION_APPROACH_EVALUATION.md` — chosen Option A+ (batch CLI + JSON) for 64-bit Python ↔ 32-bit orlib20 integration; current wrapper also has completed serve-mode support.

**Reverse engineering**
- `docs/RE/` — durable TVS-F.4 get-source and TVS-F.5 case-insensitive-exists findings (ABI, patch sites, BST node, warehouse COM).
- `docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md` — unknown-link root cause.
- `docs/RE/RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md` — Ghidra evidence for link-stage internals.
- `docs/RE/RE_L8_STALE_WRITE_FAULT_A.md` — Workstream L.8 evidence.
- `docs/RE/archive/` — historical RE artifacts and rejected approaches; not operational guidance.

**Historical**
- `docs/PROJECT_HISTORY.md` — detailed per-TVS completion history.
- `docs/PM_WORKING_MEMORY.md` — PM operational memory, manual crash-recovery procedures, Frida tips, and current state. When acting as PM, read this file first every turn and after context compaction.
- `docs/archive/` — completed plans, handoffs, and designs; historical only and potentially stale.

## Current state and TODO

The PM keeps this list current and identifies the next 1–3 highest-ROI steps toward 100% acceptance; update it as project state changes.

### Completed

- Workstreams A–J are complete. Workstream K (serve mode) K.1–K.6b, crash handling (C++ VEH plus Python `TsDevCrashError`), ShowMe (`TVS-C.8`), and CI are complete.
- Implementation and happy-path support for INSERT, DELETE, MODIFY, `exists`, `get-source`, and `upsert` is present through both CLI and Python; this implementation status is distinct from production acceptance. Stdin batches, case-insensitive exists with warehouse stored-name/ProgId return, directory batches, serve mode (`_pipe.py` broker), and distribution packaging are implemented. Serve mode has historical E2E verification and is enabled by default (`use_serve=True`); Python and `_cli.py` expose the batch/directory operations.
- Directory commands are `batch-<op>-dir <type> <dir_path>` for insert/modify/delete/upsert. File stem is the item name and file content is source; `-t <ext>` defaults to `.el` and `.txt`, the first `-t` replaces defaults, and the flag is repeatable. Batch logic is shared via `runBatchAndEmit()`; `batch-upsert` exists in manifest, `--batch-stdin`, and directory modes.
- Signature resolution is data-driven: production has no hardcoded RVAs. Fresh clones build without `tmp/` dependencies. The case-insensitive exists algorithm is the two-phase warehouse search described above.

### Workstream L — closed

- Workstream L is closed because its team completed it and a final independent review was completed.

### Next planned work

- **M (TSDev parity):** M.1/M.2/M.3 are approved historical slices. M.3b is planned. M.4 implementation exists but remains manual/blocked by the existing CLI-created object-code defect and the three separate M.4 blocker escalations below. M.5 is pending. There is no approved post-TVS production-E2E claim for M.4. Scope: compiler-object setup/use ordinals 66/67, explicit SaveModule flags/`CELProperties` metadata, post-success event `0x40A`, and reverse-dependent revalidation. Plan: `docs/workstream_tsdev_parity_plan.md`.

### M.4 blocker escalations (pre-existing review findings)

- **Dependent revalidation flag:** `callVerify()` hardcodes `_VerifyModule_New@20`'s fourth argument to `1`: the exact `verifyFn(hwnd, whType, progId, 1, compilerMem)` call is at `src/tsdev_lib.cpp:178` (within `callVerify()`, `src/tsdev_lib.cpp:173-183`). `traverseReverseDependents()` calls that helper for the dependent at `src/tsdev_lib.cpp:2132-2136` (function `src/tsdev_lib.cpp:2078-2175`), despite its local comment saying `flag=0`. The TSDev RE shows `primary=0` for a reverse dependent and passes that value as the fourth argument (`docs/RE/RE_M4_REVERSE_DEPENDENT_REVALIDATION.md:155-190`). This is a separate M.4 blocker/escalation.
- **Old master-record capture:** The checked-in `m_hasOldMasterRec` and `m_oldMasterRec` members are initialized to `false`/zero (`src/tsdev_lib.h:630-631`); `TsDevLib::saveSource()` contains no assignment that captures the old record (`src/tsdev_lib.cpp:1413-1448`), while `TsDevLib::verifyModule()` only conditionally compares the stored record and then resets `m_hasOldMasterRec` (`src/tsdev_lib.cpp:1788-1801`). The TSDev-derived RE contract requires comparing the root's old record captured before `saveSource` with the new record after verification (`docs/RE/RE_M4_REVERSE_DEPENDENT_REVALIDATION.md:1127-1138`). Consequently, the metadata contract-change gate is effectively inactive and `contractChanged` defaults true. This is a separate M.4 blocker/escalation.
- **Cycle safety under the default change gate:** Because the missing old-master-record capture leaves `contractChanged=true` (`TsDevLib::verifyModule()`, `src/tsdev_lib.cpp:1789-1801`), `TsDevLib::traverseReverseDependents()` checks `visited` but skips an already-visited node only when `!contractChanged`, then marks nodes before verification (`src/tsdev_lib.cpp:2108-2127`). Its dependent recursion continues through the same function (`src/tsdev_lib.cpp:2170-2172`); an already-visited node can therefore be reverified and recursion can continue. The current logic does not establish safe termination for a reachable cycle; this is not a claim that an infinite loop definitely occurs. The existing cycle case only pins hook signatures (including `traversalDepth`) and explicitly defers a live warehouse cycle (`tests/test_reverse_dependent_revalidation.cpp:741-766`); its direct synthetic hook calls do not execute runtime cycle traversal (`tests/test_reverse_dependent_revalidation.cpp:770-851`). The RE likewise records no explicit visited-set/depth or max depth for TSDev and says static analysis cannot fully rule out unbounded recursion (`docs/RE/RE_M4_REVERSE_DEPENDENT_REVALIDATION.md:672-747`, `docs/RE/RE_M4_REVERSE_DEPENDENT_REVALIDATION.md:749-769`). This is a separate M.4 blocker/escalation.
- These three M.4 blockers/escalations are pre-existing review findings, not newly introduced by this cleanup. M.4 remains manual/blocked; the CLI-created object-code defect remains a separate existing blocker.

### Deferred or resolved follow-ups

- Defer ActivityBar/PaintBar/ProbabilityMap mappings: no current user need; requires per-type Ghidra/Frida work. See `docs/TS_COMPONENT_TYPES.md` and `docs/FEASIBILITY.md` §5.2/§5.5.
- Defer SEH protection for raw-memory `readErrorsFromArray` reads; compiler-populated arrays make the normal-flow risk low.
- TD-001 is fixed: failed-compile insert cleanup now removes the `createEntry`-allocated ProgId/warehouse entry; coverage is `tests/test_auto_cleanup.cpp`.
- Resolved facts not to reopen: `FindExistPos` is the undecorated export at ordinal 453; compiler error text and line/column output work live; the TVS-A.6 prefix issue was not reproducible; `tsdev_lib.cpp` uses the correct `+0x43` offset; the SIGNATURE_CATALOG §4.9 note is resolved; TD-002 patch restoration is fixed by setting `m_patchesApplied` per patch and restoring only cached RVAs.

## Verification baseline

- Regression command: `powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1`. L scripts support Windows PowerShell 5.1 and `pwsh` 7. Routine L7 runs write evidence to gitignored `tmp\`; only `-UpdateTrackedEvidence` updates the tracked L7 evidence file. The regression runner provides `-SkipServeE2e`, `-SkipShowMeE2e`, and `-NoTs` options. The current `-NoTs` path runs sigtool plus the non-TS C++ unit-test executables (current inventory in `tests/run_unit_tests.ps1`); older unit-count descriptions are historical/stale, not current.
- Exact historical post-TVS command transcripts are unavailable. Historical suite, case, Python, and aggregate result counts are unverified historical/transient baselines, retained only for provenance and never as current acceptance evidence. Exact post-TVS argv/procedure, exit code, item/ProgId, and cleanup must be recaptured before relying on approval or closure; M.4 has no post-TVS production-E2E claim while manual/blocked. Six-operation production acceptance remains distinct from the narrower post-TVS insert/modify/delete gate.

## Standing notes

- This machine uses PowerShell: do not use bash heredocs for `git commit -m` or other commands. Write a message to a temp file (for example, `tmp\commit_msg.txt`) and use `git commit -F tmp\commit_msg.txt`; use `;` rather than `&&`, and `&` for executable paths containing spaces.
