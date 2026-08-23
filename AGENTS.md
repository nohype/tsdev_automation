# TSDev Automation

This project enables *automated management* of the *TradeStation Development Environment (TSDev) Library* without using the TSDev UI (TSDev.exe).

**IMPORTANT**:
The rules and guidelines below are *MANDATORY* unless *explicitly* overridden by user instructions.

**CRITICAL RULE - Effective communication:**
NEVER use flattery or affirmation to please the user. Be sober, factual, objective.
For example:
Do not tell the user it's a good question; instead, just answer it.
Do not tell the user he is right; instead, only state the objective statement you want to make.

**CRITICAL RULE — Never rely on mutable item internals for correctness:**
Item source text, comments, naming conventions inside source code, etc. are ARBITRARY user-controlled content. We can NEVER assume specific comments are present (e.g. `{ name }` blocks), that they match the stored name, or that they exist at all. Any implementation that parses source text to extract metadata (names, types, etc.) is INVALID. Metadata (stored names, ProgIds, types) must come from orlib20 warehouse records or APIs, never from item source content.

**CRITICAL RULE — Emulate TSDev.exe:**
We are replacing TSDev.exe for automated TS library management. Our code should get as close as possible to imitating what the TSDev app does. If TSDev refreshes its cache after insert/modify/delete, we should do the same. If TSDev uses a specific API or sequence to enumerate items, we should use the same. Do not invent workarounds when TSDev has a proper mechanism — reverse engineer and replicate TSDev's behavior.

**CRITICAL RULE — No hacks (hard rule 5, see `docs/PM_WORKING_MEMORY.md`):**
Never suppress compiler errors/warnings or use workarounds that mask real issues. If code produces warnings or errors, fix the root cause or find a clean, production-worthy alternative. If no clean approach exists, use the most explicit/raw form (e.g. a numeric constant with a comment naming the intended symbol) rather than suppressing the warning. Example: `ntstatus.h` redefines some `STATUS_SXS_*` macros from `winnt.h` — instead of `#pragma warning(disable: 4005)`, use the raw hex constant `0xC0000374` with a comment `// STATUS_HEAP_CORRUPTION`.

## Functions
6 interactions with the TSDev Libray:

- Inserting new Strategies and Functions:
   - Provided EasyLanguage source code and the strategy/function name, insert a new strategy/function into the TSDev Library and compile/verify it (fail loudly if compilation fails)
- Deleting existing Strategies and Functions:
   - Provided the name of an existing strategy/function, delete it from the TSDev Library (notify if it doesn't exist)
- Modifying existing Strategies and Functions:
   - Provided the name of an existing strategy/function and new EasyLanguage source code, replace the existing strategy's/function's EasyLanguage code and recompile/re-verify (notify if it doesn't exist; fail loudly if compilation of new EasyLanguage code fails)
- Querying whether an item exists:
   - Provided a component type and an item name, query whether an item with that name exists in the TSDev Library. Supports an optional case-insensitive search flag (`--case-insensitive` on the C++ CLI; `case_insensitive=True` on the Python wrapper). Returns the verbatim (correctly-cased) stored item name, whether it exists, and the ProgId if found. Reuses the existing `--json` flag to toggle between human-friendly and JSON output. Output format: `exists=N`, `stored_name=...`, `progId=N`. Default (no flag): case-sensitive (post-compares stored name with query name). With flag: case-insensitive (uses two-phase `enumerateRecords` — Phase 1: `_LoadMasterRecsEX@16` cached search with `_stricmp`; Phase 2: `FUN_101d05e0` BST search + `FUN_101c36d0` verbatim name getter — retrieves the stored name directly from the warehouse node structure, NOT from source comments).
- Retrieving the EL source code of an item:
   - Provided the name of an existing item (and its component type), retrieve its EasyLanguage source code. By default, prints the raw EL source to stdout. With an additional `--output <file_path>` flag, writes the EL source directly to the specified file. Internally encrypted items are decrypted automatically to plain text, matching TSDev.
- Upserting (insert-or-update) an item:
   - Provided a name, a component type, and an EasyLanguage source file: if the item already exists, perform a modify operation; otherwise perform an insert operation.

## Acceptance criteria
All functions (see `## Functions` above) *must* work reliably and consistently via both:
- the C++ CLI (`tsdev_cli`), and
- the Python wrapper (`TsDevClient`).

The C++ CLI must additionally support batch operations both via a manifest file path AND via stdin pipe (no manifest file), so the Python wrapper can drive batches without writing temporary manifest files.

## Tech stack and tooling
- This project is written in C++ using the Visual Studio Build Tools (already available on this machine).

## Technical process
- Use the *Ghidra MCP tools* and *Frida directly (command line)* to reverse engineer, inspect and analyze the TSDev.exe application and its modules:
   - Ghidra for static
   - Frida for dynamic/runtime anaylysis (e.g. instrumentation, tracing, etc.)
- Determine how this project (a custom, standalone CLI application) can most efficiently, with the least amount of custom implementation and the least complexity, implement the functions (see `## Functions` above)

## PM (lead agent) role:
(Skip if you are a subagent)

If you are *not a subagent:* You are the Product Manager (PM).
- You *do not* engange in any work if you can delegate it to a subagent
- You *do not* read any files/urls if you can pass the file path/url to a subagent directly
- You only perform management tasks in your role as PM
- You are leading the team, so you are ultimately responsible for the *correctness* and *production quality* of all results

### Subagent profile selection (HARD RULE)
- Use the **`universal`** profile by default for every narrow, clearly defined subtask, including research, analysis, MCP work, implementation, tests, and documentation. It has full tool access.
- Fall back to **`subagent_general`** only when a `universal` subagent fails or cannot use a required capability.
- Use **`subagent_explore`** only for trivial read-only lookups (for example, reading one file and quoting a section). If it fails or is insufficient, retry with `universal` first, then `subagent_general` only if `universal` fails.

### Working memory (HARD RULE)
- The PM maintains a markdown working-memory document at `docs/PM_WORKING_MEMORY.md`.
- Keep it updated with: reminders to stay in the PM role, hard rules, current TVS in flight, key findings, decisions, and anything worth remembering across context compactions.
- Update it as new information emerges or old information is deprecated/superceded/updated.
- Regularly re-read it to refresh memory and remind yourself of priorities and hard rules (context is limited and will be compacted, causing loss of information and relationships between pieces of information — outsource working memory to stay effective).

## Work orchestration
- Every complex or multi-step task is decompose into *small, atomic subtasks*
   - For complex tasks, a specialized subagent does the decomposition
   - Each subtask is executed by a singularly focused, appropriately specialized subagent
   - Hard rule: One subagent -> one narrowly scoped subtask. No multi-stage, multi-concern or complex subtasks for any individual subagent.
- *Every* substantive result (research or analysis finding, plan, document, implementation) *must* be adversarially reviewed by an independent subagent. The adversarial reviewer can APPROVE or DISAPPROVE with a list of findings. On DISAPPROVE, a loop is entered: An implementer subagent fixes the findings, then another adversarial reviewer reviews again. This continues until the adversarial reviewer APPROVES.
   - The reviewer receives the original subtask instructions and the result. It looks for *complete execution* of the original instructions (goal 100% met), *no scope creep*, *no drift*, *no undue shortcuts/hacks which defy the original purpose*

- We work *iteratively*:
   - One *minimal Thin Vertical Slice* (TVS) at a time
   - Each new team determines the project state and determines the best next TVS towards (highest ROI) towards our acceptance criteria, then the team executes and updates all relevant docs
   - We follow TDD (red-green method)
- **End-to-end CLI verification (MANDATORY after every TVS):** After any TVS implementation is successfully completed and approved, run an end-to-end test with the real production CLI (`tsdev_cli insert/delete/modify`) using the latest state of the codebase at that point in time. This ensures the most important artifact — the production CLI that satisfies the primary acceptance criteria — remains operational after every change. This is in addition to (not a replacement for) the unit/regression test suite. The e2e CLI test must PASS before the TVS is considered fully done. For the comprehensive E2E test procedure covering all 5 supported component types (Strategy, Indicator, Function, TradingApp, ShowMe) with type-specific semantic error cases, see `docs/E2E_TEST_SPEC.md`. Agents or subagents performing E2E CLI verification should read that document for the full test specification.

## TradeStation component types
TradeStation supports 8 EasyLanguage component types: **ActivityBar, Function, Indicator, PaintBar, ProbabilityMap, ShowMe, Strategy, TradingApp**. Each has different purposes and constraints (e.g., Strategies cannot use Plot statements; only Indicators, ShowMe, and PaintBar can plot). See `docs/TS_COMPONENT_TYPES.md` for the full mapping to orlib20 warehouse types. The CLI currently supports 5 confirmed types: **Strategy** (whType 0), **Indicator** (whType 2), **Function** (whType 4), **TradingApp** (whType 5122), and **ShowMe** (whType 1282). The remaining 3 (ActivityBar, PaintBar, ProbabilityMap) have unconfirmed warehouse types.

## Relevant directories and paths
These TradeStation (TS) installation paths are *READ-ONLY* and *MUST NEVER BE MODIFIED*! We cannot break the existing installation.
Any files from the TS installation directory which must be inspected, modified, etc. can be copied into a temporary subdirectory of this project to work on them.

- The TradeStation Desktop installation includes the TradeStation Development Environment (TSDev): `C:\Program Files (x86)\TradeStation 9.5`
- The TradeStation Development Environment application (TSDev.exe): `C:\Program Files (x86)\TradeStation 9.5\Program\TSDev.exe`
- The TSDev Library (where strategies, functions, etc. are stored *compiled and encrypted*): `C:\Program Files (x86)\TradeStation 9.5\MyWork\Bin`

## Documentation Index

Read `AGENTS.md` (this file) first — it contains always-on rules and project overview. Read other documents on-demand based on the task:

**Operational reference (read when doing the relevant task):**
- `docs/BUILD.md` — Build & test instructions. Read when building, running tests, or updating the baseline DLL.
- `docs/E2E_TEST_SPEC.md` — End-to-end CLI test specification (all 5 supported component types, all subcommands). Read when running or extending E2E tests.
- `docs/SIGNATURE_CATALOG.md` — Authoritative human-readable catalog of all 24 orlib20.dll signatures, 10 exports, 3 data globals, 11 patch sites. Read when working with signatures, patches, or the OrlibResolver.
- `docs/VERSION_DETECTION.md` — orlib20.dll version detection strategy (3-tier multi-factor method). Read when working on version detection or the version manifest.
- `docs/TS_COMPONENT_TYPES.md` — TradeStation component types and orlib20 warehouse type mappings. Read when working with component types or adding new types.
- `docs/FEASIBILITY.md` — Reverse-engineering feasibility study (orlib20 primitives, parameter orders, INSERT/MODIFY/DELETE pseudocode). Read when understanding the RE foundation.
- `docs/workstream_serve_plan.md` — Plan for Workstream K (Serve mode; K.1–K.6b COMPLETE — Workstream K fully complete). Read when working on serve mode.
- `docs/workstream_unknown_link_fix_plan.md` — Plan for Workstream L (Unknown link runtime defect fix; NOT YET CLOSED — recovery L.6–L.15 implemented on branch `workstream-L-recovery`; L.1–L.3 remain COMPLETE; L.4/L.5 completion was FALSIFIED and is superseded by L.6–L.15; L.8 BLOCKED, L.9 detect-and-fail stale-ELL modify/upsert, L.12 detect-and-report orphan-ELL delete). Read when working on the unknown link defect.
- `docs/workstream_tsdev_parity_plan.md` — Plan for Workstream M (TSDev parity; BLOCKED until an explicit close/unblocking decision — user-supervised Desktop restart check SATISFIED 2026-08-24, but L.8 remains BLOCKED, so Workstream L is not yet CLOSED; PLANNED, not implemented). Read when working on TSDev parity work.

**Python integration:**
- `docs/INTEGRATION_APPROACH_EVALUATION.md` — Decision document for 64-bit Python ↔ 32-bit orlib20.dll integration (Option A+ chosen: batch CLI + JSON I/O). Read when working on the Python wrapper.

**Reverse-engineering reference (read when doing RE work on orlib20.dll):**
- `docs/RE/` — Durable RE findings for TVS-F.4 (get-source) and TVS-F.5 (case-insensitive exists). Contains ABI documentation, patch-site analysis, BST node structure, and warehouse COM interface analysis. Read when investigating orlib20 internals.
- `docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md` — Durable root-cause analysis for the unknown link runtime defect (Workstream L foundation). Read when investigating the unknown link failure.
- `docs/RE/RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md` — Durable Ghidra static evidence appendix for the unknown link defect. Read when investigating orlib20 link-stage internals.
- `docs/RE/RE_L8_STALE_WRITE_FAULT_A.md` — Durable RE evidence record for TVS-L.8 (post-compile stale-write Fault A: read-only ELL false success). L.8 decision-gate is BLOCKED (no TSDev.exe save-failure observation; L.9 cannot invent recovery). Read when investigating Fault A / stale-ELL modify recovery.
- `docs/RE/archive/` — Historical RE process artifacts (superseded approaches, adversarial reviews of rejected approaches). Historical context only.

**Historical reference (read for context, not operational guidance):**
- `docs/PROJECT_HISTORY.md` — Detailed per-TVS completion log. Read for historical context on completed work.
- `docs/PM_WORKING_MEMORY.md` — PM's operational memory (crash-safety procedures, Frida tips, current state). The PM reads this at the start of every turn.
- `docs/archive/` — Completed workstream plans, handoffs, and design documents. Historical reference only; counts and details within may be stale.

## TODO
This is a running, continually updated TODO list to bring this project to complection.
The PM updates this list according to the current project state: Analyze the current project state and map out the next 1-3 steps with the highest ROI to bring us towards satisfying our acceptance criteria (see `## Acceptance Criteria` above) 100%.
Update this list to reflect the current state and the next best steps as the project progresses and information changes.

### Completed Work Summary

> **Workstream L is NOT YET CLOSED — see docs/PM_WORKING_MEMORY.md. Recovery L.6–L.15 is implemented on branch `workstream-L-recovery`; L.4/L.5 completion was FALSIFIED and is superseded by L.6–L.15.**

All workstreams A, B, C, D, E, F, G, H, I, J are COMPLETE. Workstream K (Serve mode) K.1–K.6b COMPLETE. Crash handling (VEH + TsDevCrashError) COMPLETE. ShowMe (TVS-C.8) COMPLETE. Workstream L (Unknown link defect fix) is NOT YET CLOSED — TVS-L.1 (RED harness) COMPLETE, TVS-L.2 (STA COM lifecycle guard) COMPLETE, TVS-L.3 (Remove persistence bypass) COMPLETE; the TVS-L.4 (Persistence validation + cleanup/rollback hardening) and TVS-L.5 (Full verification matrix + Desktop runtime gate) completion claim was FALSIFIED by persistence and deletion fault injection and is superseded by the L.6–L.15 recovery, which is implemented on branch `workstream-L-recovery` (L.8 BLOCKED — no invented TSDev recovery; L.9 detect-and-fail stale-ELL modify/upsert; L.12 detect-and-report orphan-ELL delete; existence checks are necessary but not sufficient; the `FUN_10011230` persistence bypass is removed). See `docs/PM_WORKING_MEMORY.md`. Detailed per-TVS logs are archived in `docs/PROJECT_HISTORY.md`.

**Next planned work:** Workstream M (TSDev parity) — BLOCKED until an explicit close/unblocking decision (the user-supervised Desktop restart check is now SATISFIED 2026-08-24, but L.8 remains BLOCKED, so Workstream L is not yet CLOSED); PLANNED, not implemented; plan at `docs/workstream_tsdev_parity_plan.md`. Workstream L is not yet CLOSED — the L.4/L.5 completion claim was falsified and is superseded by the L.6–L.15 recovery (see `docs/PM_WORKING_MEMORY.md`). See `### Planned Workstreams (not implemented)` below for details.

> **Workstream L is NOT YET CLOSED — see docs/PM_WORKING_MEMORY.md. Recovery L.6–L.15 is implemented on branch `workstream-L-recovery`; L.4/L.5 completion was FALSIFIED and is superseded by L.6–L.15.**

| TVS | Result | Status |
|-----|--------|--------|
| TVS-1 | Technical feasibility & approach investigation | COMPLETE |
| TVS-2.1 | Headless orlib20 init smoke test | COMPLETE |
| TVS-2.2 | Resolve orlib20 parameter orders & persistence | COMPLETE |
| TVS-2.3 | INSERT/DELETE/MODIFY (library level) | COMPLETE |
| CLI | All three subcommands (insert/delete/modify) functional | COMPLETE |
| TVS-A.2 | GetCompilingErrors ABI + error array population path | COMPLETE |
| TVS-A.4 | Real compiler error text via direct global array access | COMPLETE |
| TVS-A.6 | Source location (line/column) in error messages | COMPLETE |
| TVS-B.1 | Catalog all hardcoded offsets, patches, signatures | COMPLETE |
| TVS-B.1.5 | Validate original bytes before persistence patch | COMPLETE (persistence patch retired in TVS-L.3; the original-bytes validation test was repurposed to assert the patch is no longer applied) |
| TVS-B.2 | Version detection strategy & variability assessment | COMPLETE |
| TVS-B.3 | OrlibResolver abstraction layer design | COMPLETE |
| TVS-B.4 | OrlibResolver .cpp implementation | COMPLETE |
| TVS-B.4.5 | resolveErrorHinstRva instruction-offset extraction | COMPLETE |
| TVS-B.5 | OrlibResolver integration with TsDevLib | COMPLETE |
| TVS-B.6 | Automated regression test suite (initial 60 tests, now 136 across 13 default sub-suites; 14 registered — Suite 14 stress opt-in via `-ExtendedStress`) | COMPLETE |
| TVS-B.6.5 | Baseline DLL committed to tests/fixtures/ | COMPLETE |
| TVS-B.6.6 | Interactive baseline DLL prompting in test runners | COMPLETE |
| TVS-B.7 | TS install path discovery + --ts-path CLI override | COMPLETE |
| TVS-C.1–C.7 | TradingApp component type (whType 5122) | COMPLETE |
| TVS-E.1 | Signature JSON migration + codegen + generated header | COMPLETE |
| TVS-E.2 | sigtool CLI | COMPLETE |
| TVS-E.3 | Deduplicate test_orlib_resolver.cpp RVAs | COMPLETE |
| TVS-E.4 | Evolve test_no_hardcoded_rvas.cpp (derived forbidden constants) | COMPLETE |
| TVS-E.5 | Archive experimental probes | COMPLETE |
| TVS-E.6 | Move tests to tests/ | COMPLETE |
| TVS-E.7 | Clean stale RVA comments + rewrite BUILD.md | COMPLETE |
| TVS-D.0 | Python integration approach evaluation (A+ chosen: batch CLI + JSON, no daemon) | COMPLETE |
| TVS-D.1 | Stress-test TsDevLib reuse across 600 ops in one process | COMPLETE |
| TVS-D.2 | --json structured output for single-item CLI subcommands | COMPLETE |
| TVS-D.3 | Batch mode (batch-insert/modify/delete, line-based manifest, JSON results) | COMPLETE |
| TVS-D.4 | Python TsDevClient wrapper (single-item + batch, typed exceptions) | COMPLETE |
| TVS-D.5 | Comprehensive Python E2E (all 4 types) + packaging + README | COMPLETE |
| TVS-F.1 | exists subcommand (exact-match, C++ CLI) | COMPLETE |
| TVS-F.2 | Inline batch via stdin (`--batch-stdin` flag, C++ CLI) | COMPLETE |
| TVS-F.3 | upsert subcommand (C++ CLI) | COMPLETE |
| TVS-F.4 | get-source subcommand (C++ CLI, RE-based) | COMPLETE |
| TVS-F.6 | Python wrapper for all F ops (exists/get-source/upsert + inline batch stdin) | COMPLETE |
| TVS-F.5 | Case-insensitive exists with stored name + ProgId return (C++ CLI + Python) | COMPLETE |
| TVS-G.1–G.5 | Distributable bundle (dist/ + artifacts/ zip) | COMPLETE |
| TVS-I.1 | batch-upsert (C++ CLI — manifest file + stdin modes) | COMPLETE |
| TVS-I.2 | Directory batch mode (C++ CLI — all 4 ops: batch-insert-dir/batch-modify-dir/batch-delete-dir/batch-upsert-dir) | COMPLETE |
| TVS-I.3 | Python wrapper — batch_upsert + directory batch + _cli.py subcommands | COMPLETE |
| TVS-C.8 | ShowMe component type (whType 1282) | COMPLETE |
| TVS-K.1 | Elevation guard for all subcommands (exit 11, isElevated()) | COMPLETE |
| TVS-K.2 | Extract cliResultToJson() reusable serialization helper | COMPLETE |
| TVS-K.3 | Serve request dispatch (ServeRequest, parseServeRequest, dispatchServeRequest) | COMPLETE |
| TVS-K.4 | Named pipe server (framing, createServePipe, runServe, cmdServe, serve subcommand) | COMPLETE |
| TVS-K.5 | Python serve-broker (_pipe.py, _ServeBroker, elevation detection, routing) | COMPLETE |
| TVS-K.6a | E2E integration + backwards-compat verification | COMPLETE |
| TVS-K.6b | Documentation + use_serve default flip | COMPLETE |
| Crash Handling | C++ VEH crash handler + Python TsDevCrashError | COMPLETE |
| CI | GitHub Actions build+release workflow (`.github/workflows/ci.yml`: Win32 build + `-NoTs` regression + mocked pytest on every push/PR; bundle zip attached to GitHub Releases on `v*` tags) | COMPLETE |
| TVS-L.1 | RED regression/acceptance harness for unknown-link defect (`tests/test_unknown_link_regression.ps1`, registered in Suite 13 after L.3 made it green; was opt-in/not registered while red pre-L.3) | COMPLETE |
| TVS-L.2 | STA COM lifecycle guard (ComApartmentGuard, RAII, injectable seam, Ole32 link) | COMPLETE |
| TVS-L.3 | Remove persistence bypass + run real finalization path (24 sigs, 11 patches) | COMPLETE |
| TVS-L.4 | Persistence validation + cleanup/rollback hardening | FALSIFIED — completion claim jointly falsified by both persistence and deletion fault injection; superseded by L.6–L.15 recovery (implemented on branch `workstream-L-recovery`). Historical DONE text retained as dated/superseded. |
| TVS-L.5 | Full verification matrix + Desktop runtime gate | FALSIFIED — completion claim jointly falsified by both persistence and deletion fault injection; superseded by L.6–L.15 recovery (implemented on branch `workstream-L-recovery`). Historical DONE text retained as dated/superseded. No new Desktop runtime re-gate claimed without an L.17 result. |

**Key architectural facts:**
- Signature data: `signatures/*.json` (authoritative) → `tools/sigcodegen.py` → `signatures.gen.h` (build-time codegen) — 24 signatures total, 11 patch sites
- OrlibResolver: signature-based RVA resolution, no hardcoded RVAs in production code
- CLI supports 5 component types: Strategy (0), Indicator (2), Function (4), TradingApp (5122), ShowMe (1282)
- Regression suite: `powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1` (14 registered C++ regression suites, 13 default — Suite 14 stress opt-in via `-ExtendedStress` / 136 test cases across case-counting suites: sigtool (12) + unit (27) + E2E (34) + exists (5) + batch-stdin (8) + upsert (9) + get-source (7) + exists-ci (9) + batch-upsert (5) + directory-batch (9) + serve E2E (2) + ShowMe E2E (9); plus Suite 13 unknown-link (249 assertions) and Suite 14 stress (600 cycles)). Suite 14 stress (600 cycles) is skipped by default and opt-in via `-ExtendedStress`. Per-suite skip flags include `-SkipServeE2e` and `-SkipShowMeE2e`; under `-NoTs` only the sigtool suite + 10 non-TS unit tests run. C++ unit tests: 27 (including test_elevation_guard, test_json_serialize, test_serve_dispatch, test_serve_framing, test_crash_handler, test_showme_type, test_com_apartment_guard). Python: 154 tests collected (139 selected by `-m "not e2e"`; 15 e2e deselected without live TradeStation). Non-TS C++ tests: 10 (all pass without TradeStation). All passing.
- Fresh clone builds without `tmp/` dependencies (Workstream E fixed this)
- Case-insensitive `exists` uses two-phase `enumerateRecords`: Phase 1 = `_LoadMasterRecsEX@16` (cached, `_stricmp` search, two-pass without/with refreshCache); Phase 2 = `FUN_101d05e0` (BST search, returns node pointer) + `FUN_101c36d0` (verbatim name getter, returns char* from warehouse node at offset +0x1D). The stored name comes from the WAREHOUSE, not from source text. Key decision: two-phase instead of cmd 0x0F + cmd 0x4E cache refresh.

### Project Status

Workstreams A, B, C, D, E, F, G, H, I, J are COMPLETE. Workstream K (Serve mode) K.1–K.6b COMPLETE. Crash handling (VEH + TsDevCrashError) COMPLETE. ShowMe (TVS-C.8) COMPLETE. All 6 functions (INSERT, DELETE, MODIFY, exists, get-source, upsert) operate via both the C++ CLI (`tsdev_cli`) and the Python wrapper (`TsDevClient`); inline batch via stdin, case-insensitive exists with stored name + ProgId return, and directory batch operations are available via both; a distributable bundle (`dist/` + timestamped zip in `artifacts/`) is assembled automatically by CMake POST_BUILD on every build; the CLI supports 5 component types (Strategy, Indicator, Function, TradingApp, ShowMe); serve mode infrastructure is complete, E2E-verified, and enabled by default (`use_serve=True`, flipped in K.6b).

**Acceptance criteria are NOT currently satisfied at production runtime.** Workstream L is NOT YET CLOSED: the TVS-L.4/L.5 completion claim was FALSIFIED by independent persistence and deletion fault injection and is superseded by the L.6–L.15 recovery, which is implemented on branch `workstream-L-recovery` (see `docs/PM_WORKING_MEMORY.md`). The causal bypass (the `FUN_10011230` persistence patch, which skipped compiler/link-finalization behavior (`FUN_1008E640`), offset adjustment, and symbol/XML generation and falsely returned success) was removed in TVS-L.3 and the real finalization path runs (TVS-L.1–L.3 remain COMPLETE; the `FUN_10011230` persistence bypass is removed). However, the L.4 "no orphan artifacts on failed insert / intact old artifacts on failed modify" claim and the L.5 "full verification matrix + Desktop runtime gate across all 5 types" claim were both falsified: a stale-ELL modify (Fault A) and an orphan-ELL delete (Fault B) each returned exit 0 / success=true despite leaving the library inconsistent. The L.6–L.15 recovery is implemented on branch `workstream-L-recovery`: L.8 is BLOCKED (no invented TSDev recovery); L.9 detect-and-fail stale-ELL modify/upsert; L.12 detect-and-report orphan-ELL delete (non-success + residual path/error); existence checks are necessary but not sufficient. The historical TVS-L.5 Desktop runtime gate observed no "Unknown link error occurred" for any CLI-inserted item before or after Desktop restart, but that gate did not exercise the persistence/delete fault paths whose failures falsified L.4/L.5; no new Desktop runtime re-gate is claimed without an L.17 result.

**TVS-L.17 automated verification — APPROVED 2026-08-24 (documentation-only record; NOT a Workstream-L-closed / COMPLETE claim).** All listed automated gates PASS: Build PASS; 24/11 signatures PASS (24 signatures, 11 patch sites); 14 NoTs units PASS; 34/34 CLI E2E PASS; 253/253 unknown-link PASS; 288/288 with L7 RED PASS; 139 Python mocked PASS; 600/600 Suite 14 PASS. Two findings (open, non-blocking for the automated record): (1) L-suite scripts parse/run under pwsh 7, not Windows PowerShell 5.1 — `docs/BUILD.md` and `tests/run_regression.ps1` still invoke Windows PowerShell 5.1; invoke under pwsh 7 until the host invocation is updated. (2) The L7 harness regenerates `docs/RE/RE_L7_RED_FAULT_INJECTION_EVIDENCE.md` on each run (overwrites the durable evidence file). **User-supervised Desktop restart check — SATISFIED 2026-08-24** (user-supervised, NOT agent computer-use; the agent did not drive the Desktop UI): after a Desktop restart, all five L.17 component types on `+TP:GC_1M_010115_081426_CLEAN*` still work as expected — Strategy `TS_AUTO_L17_RT_STRAT_20260824`, Function via caller `TS_AUTO_L17_RT_CALLER_20260824` (calls `TS_AUTO_L17_RT_FUNC_20260824`), Indicator `TS_AUTO_L17_RT_IND_20260824`, ShowMe `TS_AUTO_L17_RT_SHOWME_20260824`, and TradingApp `TS_AUTO_L17_RT_TA_20260824` (no-crash/no-error only — no UI; matches H.10 expected black/minimal window behavior). Strategies use one historical closed Buy/Sell on CurrentBar 1/3 (not live `x=1` orders). First-pass also PASS; the first-pass user-observed PASS plus this post-restart PASS together satisfy the user-supervised Desktop runtime gate for the L.17 item set. No TSDev re-verify was performed (no TSDev.exe, no F5, no Build > Verify); no `tsdev_cli` insert/modify/delete was run while Desktop was open for this restart check. **Workstream L is still NOT CLOSED** — the Desktop restart check being SATISFIED does NOT prove Fault A/B persist/delete recovery; **L.8 remains BLOCKED** (no invented TSDev persist recovery — no `TSDev.exe`-equivalent save-failure observation was established; L.9 cannot invent a recovery sequence not present in the binary). The Desktop gate and persist/delete recovery are separate concerns; passing the Desktop gate does NOT claim persist-recovery completeness. **Workstream M remains BLOCKED** until an explicit close/unblocking decision; do not silently mark M unblocked. The 2026-08-24 concurrent-write incident (a `tsdev_cli` modify run while Desktop was open, which crashed `ORPlat.exe`) remains recorded; the durable operational rule is: **do NOT run `tsdev_cli` insert/modify/delete while TradeStation Desktop is open.** See `docs/PM_WORKING_MEMORY.md`, `docs/workstream_unknown_link_fix_plan.md`, and `docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md`.

### Planned Workstreams (not implemented)

| Workstream | Scope | Status |
|------------|-------|--------|
| L (Unknown link defect fix) | Remove `FUN_10011230` persistence patch; initialize STA COM on every operation worker thread before orlib init; require full real link stage and ELL persistence; add artifact/runtime regression checks. Highest priority — blocks production runtime acceptance. | NOT YET CLOSED — L.1–L.3 COMPLETE; L.4/L.5 completion claim FALSIFIED and superseded by L.6–L.15 recovery (implemented on branch `workstream-L-recovery`; L.8 BLOCKED, L.9 detect-and-fail stale-ELL modify/upsert, L.12 detect-and-report orphan-ELL delete). Plan: `docs/workstream_unknown_link_fix_plan.md` |
| M (TSDev parity) | Correct compiler object setup/use ordinal 66/67; explicit SaveModule flags/CELProperties metadata; post-success event 0x40A; reverse-dependent revalidation. Scoped after L; not the immediate root cause. | BLOCKED until an explicit close/unblocking decision (user-supervised Desktop restart check SATISFIED 2026-08-24, but L.8 remains BLOCKED, so Workstream L is not yet CLOSED); PLANNED, not implemented. Plan: `docs/workstream_tsdev_parity_plan.md` |

### Completed Workstream: G — Distributable Bundle

| TVS | Scope | Status |
|-----|-------|--------|
| TVS-G.1 | CMake POST_BUILD bundle assembly + .gitignore (dist/ + artifacts/) | COMPLETE |
| TVS-G.2 | Python wrapper bundle-layout auto-discovery + unit test | COMPLETE |
| TVS-G.3 | Bundle README template + root README/BUILD.md docs + CMake README copy | COMPLETE |
| TVS-G.5 | Automatic timestamped zip creation in artifacts/ (PowerShell POST_BUILD) | COMPLETE |
| TVS-G.4 | (optional) Remove stale capstone runtime dep from pyproject.toml | COMPLETE |

### Candidate Future Work
1. **3 unconfirmed warehouse-type mappings** (ActivityBar, PaintBar, ProbabilityMap) — feature gap, requires Ghidra/Frida RE work per type. See `docs/TS_COMPONENT_TYPES.md` and `docs/FEASIBILITY.md` §5.2/§5.5. Currently no user need. Defer. (ShowMe was previously in this list but is now CONFIRMED — whType=1282, TVS-C.8.)
2. **SEH protection on error array reads** — `readErrorsFromArray` reads raw memory without SEH guards. Low likelihood (array populated by compiler in normal flow). Defer.

### Workstream I — Directory Batch Operations (COMPLETE)

Plan: `docs/archive/WORKSTREAM_I_PLAN.md` (3 TVSs: I.1 COMPLETE, I.2 COMPLETE, I.3 COMPLETE — all 3 TVSs done)

**User requirements:**
- Batch insert/modify/upsert/delete on directories via C++ CLI (`batch-<op>-dir <type> <dir_path>`) and Python wrapper
- File names (without extension) = item names; file content = EL source
- `-t <ext>` flag for extension filtering (default `.el`+`.txt`; first `-t` overrides defaults; repeatable)
- DRY: share logic with manifest codepath (shared `runBatchAndEmit()` helper)
- `batch-upsert` must be added to manifest file, `--batch-stdin`, AND directory modes

| TVS | Scope | Status |
|-----|-------|--------|
| TVS-I.1 | batch-upsert (C++ CLI — manifest + stdin) | COMPLETE |
| TVS-I.2 | Directory batch mode (C++ CLI — all 4 ops) | COMPLETE |
| TVS-I.3 | Python wrapper — batch_upsert + directory batch + _cli.py subcommands | COMPLETE |

### REMAINING FOLLOW-UPS
- **3 unresolved warehouse-type mappings:** ActivityBar, PaintBar, ProbabilityMap whType codes are NOT confirmed. Code restricts to 5 confirmed types: Strategy (0), Indicator (2), Function (4), TradingApp (5122), ShowMe (1282). See `docs/TS_COMPONENT_TYPES.md` and `docs/FEASIBILITY.md` §5.2/§5.5. These require Ghidra/Frida RE work per type and are not yet scoped in a handoff. Currently no user need. Defer.
- **Partial warehouse entry on compile failure (TD-001):** When `insert` fails compilation, `createEntry` has already allocated a ProgId and created a warehouse entry. The CLI now auto-cleans up this partial entry (fixed in this cleanup pass). See `tests/test_auto_cleanup.cpp`.

### Resolved follow-ups (closed)
- ~~FindExistPos export name~~ — CONFIRMED (undecorated only, ordinal 453). Production code correct.
- ~~Compiler error text CLI output~~ — DONE (TVS-A.4 + TVS-A.6). Verified working via live testing.
- ~~TVS-A.6 "Line X, Col Y:" prefix regression~~ — NOT REPRODUCIBLE. Live testing confirms the prefix works correctly for all component types. The flag was a stale misobservation.
- ~~Stale +0x3E comment in tsdev_lib.cpp~~ — Already correct (+0x43). The B.3 flag was itself stale.
- ~~Stale SIGNATURE_CATALOG.md §4.9 note~~ — RESOLVED (TVS-B.1.5). Doc updated.
- ~~Partial-construction patch restoration gap (TD-002)~~ — FIXED. `m_patchesApplied` now set after each individual patch; destructor guards restoration by cached RVA.

## Workstream G progress
- **TVS-G.1: CMake POST_BUILD bundle assembly + .gitignore** — COMPLETE. CMake POST_BUILD copies `tsdev_cli.exe` + all wrapper `.py` files via `file(GLOB src/tsdev_automation/*.py CONFIGURE_DEPENDS)` (currently 7: `__init__.py`, `client.py`, `exceptions.py`, `results.py`, `__main__.py`, `_cli.py`, `_pipe.py`; no `py.typed` exists in the package) to `dist/` on every build via `copy_if_different`. `dist/.gitignore` removed; root `.gitignore` updated with `/dist/` and `/artifacts/` entries. Adversarial review APPROVED.
- **TVS-G.2: Python wrapper bundle-layout auto-discovery** — COMPLETE. New step 3 in `_resolve_cli_path()` (`src/tsdev_automation/client.py`): checks `<package_parent>/tsdev_cli.exe` before the CMakeLists.txt walk-up, so the wrapper auto-discovers the CLI when imported from the `dist/` bundle layout. 2 unit tests added (62 Python tests total). Adversarial review APPROVED.
- **TVS-G.3: Bundle README** — COMPLETE. `docs/BUNDLE_README.md` → `dist/README.md` via CMake POST_BUILD `copy_if_different`. Root `README.md` "Distribution / Bundle" section added. `docs/BUILD.md` "### 5. Bundle output" subsection added. Adversarial review APPROVED.
- **TVS-G.5: Automatic timestamped zip in artifacts/** — COMPLETE. `tools/create_bundle_zip.ps1` with `$ErrorActionPreference='Stop'` (fail-loudly). CMake POST_BUILD invokes it. Zip pattern: `tsdev_automation_<yymmdd><HHMM>.zip`. Selective contents: exe + `tsdev_automation/` + `README.md` only. Adversarial review APPROVED.
- **TVS-G.4 (optional): Remove stale capstone runtime dep** — COMPLETE. Removed `capstone>=5.0.9` from `pyproject.toml` `dependencies` (now `dependencies = []`). Fixed false capstone claim in `README.md` (now correctly states "stdlib-only"). Wrapper and `tools/sigcodegen.py` are stdlib-only (verified: zero capstone imports in production code). Adversarial review APPROVED.
- **Full E2E:** 7-step E2E passed (8/8 C++ regression suites + 62 Python tests). All adversarial reviews APPROVED.

### Standing notes
- TradeStation install paths are READ-ONLY — never modify. Copy files into `tmp/` to work on them.
- All substantive results must be adversarially reviewed (APPROVE/DISAPPROVE loop) before being accepted.
- **Git commits on Windows (PowerShell) — NO HEREDOC:** This machine's shell is PowerShell. PowerShell does NOT support bash-style heredocs (`$(cat <<'EOF' ... EOF)`) — attempting this fails with parser errors (`'<' operator is reserved`, `Missing file specification after redirection operator`). **NEVER use heredoc syntax for `git commit -m` or any other command.** Instead, write the commit message to a temp file (e.g. `tmp\commit_msg.txt`) and use `git commit -F tmp\commit_msg.txt`. This is the ONLY reliable way to pass multi-line commit messages on this machine. Also remember: PowerShell uses `;` (not `&&`) to chain commands, and `&` is needed to invoke executables whose path contains spaces.
- **Frida**: Use Frida DIRECTLY via the `frida` CLI (`frida-trace`, `frida -l script.js`, `frida -p PID`), NEVER via MCP tools. The Frida MCP server was removed because it never worked reliably. Frida 17.17.0 is installed. Live RE works: 32-bit spawn/attach, interactive sessions, and the full Frida JS API via `execute_in_session` (symbol resolution, disassembly via `Instruction.parse`, function hooking via `Interceptor.attach`, memory reads, `NativeFunction` for direct calls).
- **Ghidra**: Ghidra MCP tools ARE available and work reliably. Use them for static analysis (decompilation, disassembly, symbol resolution, cross-references). The Ghidra MCP server is "ghidra".
