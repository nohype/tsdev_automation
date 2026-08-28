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
| Workstream status (L closed; M.1–M.3b.2c complete; M.4a complete; M.4b.1 RE-OPENED; M.4b.2 pending; M.5 pending) | `AGENTS.md` §"Current state and TODO"; detail: `docs/workstream_tsdev_parity_plan.md`, `docs/M4B1_HANDOFF.md` |
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
- `docs/workstream_tsdev_parity_plan.md` — Workstream M TSDev parity (M.1–M.3b.2c complete; M.4a complete; M.4b.1 RE-OPENED; M.4b.2 pending; M.5 pending). Current M.4b.1 detail in `docs/M4B1_HANDOFF.md`.

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

- Workstreams A–J, K (serve mode K.1–K.6b), crash handling (C++ VEH + Python `TsDevCrashError`), ShowMe (`TVS-C.8`), and CI.
- All six operations implemented (happy-path, not production acceptance) through CLI and Python: insert, delete, modify, exists, get-source, upsert. Stdin/directory batches, case-insensitive exists, serve mode (`use_serve=True` default), distribution packaging.
- Directory commands: `batch-<op>-dir <type> <dir_path>`; file stem = item name, content = source; `-t <ext>` (default `.el`/`.txt`, repeatable, first replaces defaults). Shared via `runBatchAndEmit()`.
- Signature resolution is data-driven (no hardcoded RVAs); fresh clones build without `tmp/`.

### Workstream L — closed

- Completed and independently reviewed.

### Next planned work

- **M (TSDev parity):** M.1–M.3b.2c complete (M.3b "defect" was a test artifact — declaration-free source legitimately has elemCount=0; see `docs/workstream_tsdev_parity_plan.md`). M.4a complete (warehouse provides reverse dependents via cmd 0x3B / IProgram::Callers). M.4b.1 RE-OPENED — 13 missing warehouse commands identified via Frida trace of TSDev.exe; next: Ghidra-analyze them; see `docs/M4B1_HANDOFF.md` §10. M.4b.2 (wire into revalidation + fix blockers + remove linker code) pending. M.5 pending.

### M.4 blocker escalations (pre-existing review findings)

- **Dependent revalidation flag:** `callVerify()` hardcodes the 4th arg to `1` (`src/tsdev_lib.cpp:178`); `traverseReverseDependents()` calls it for dependents (`src/tsdev_lib.cpp:2629`) despite comment saying `flag=0`. TSDev RE shows `primary=0` for reverse dependents (`docs/RE/RE_M4_REVERSE_DEPENDENT_REVALIDATION.md:103-105`).
- **Old master-record capture:** `m_hasOldMasterRec`/`m_oldMasterRec` init to false/zero (`src/tsdev_lib.h:693-694`); `saveSource()` never captures the old record (`src/tsdev_lib.cpp:1543`); `verifyModule()` only conditionally compares then resets (`src/tsdev_lib.cpp:1978-1991`). RE contract requires old-vs-new comparison (`docs/RE/RE_M4_REVERSE_DEPENDENT_REVALIDATION.md:16-17, 178-182`). `contractChanged` defaults true; metadata gate inactive.
- **Cycle safety:** With `contractChanged=true` (`src/tsdev_lib.cpp:1979`), `traverseReverseDependents()` skips visited nodes only when `!contractChanged` (`src/tsdev_lib.cpp:2607`); recursion can revisit nodes (`src/tsdev_lib.cpp:2666`). Existing cycle test only pins hook signatures, defers live cycle (`tests/test_reverse_dependent_revalidation.cpp:741-851`). RE records no visited-set/depth for TSDev (`docs/RE/RE_M4_REVERSE_DEPENDENT_REVALIDATION.md:439-442, 481-483`).
- These three blockers are pre-existing, addressed in M.4b.2 (M.4b.1 does not touch them). M.3b "defect" was a test artifact, no longer a blocker. M.4 remains manual/blocked pending M.4b.1; see `docs/M4B1_HANDOFF.md` §10.

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
