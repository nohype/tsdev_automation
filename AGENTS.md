# TSDev Automation

Automates management of the TradeStation Development Environment (TSDev) Library without using the TSDev UI (`TSDev.exe`). The rules below are mandatory.

## Mandatory rules

- **Communicate objectively:** do not flatter, affirm, or tell the user they are right; state facts and answers directly.
- **Never derive metadata from mutable item content.** EasyLanguage source, comments, and naming conventions are arbitrary user-controlled data. Never parse source text for names, types, or other metadata (including assumed `{ name }` comments). Stored names, ProgIds, and types must come from orlib20 warehouse records/APIs.
- **Emulate TSDev.exe:** replicate its APIs, enumeration sequences, cache refreshes, and observable behavior; do not invent workarounds where the application has a proper mechanism.
- **No hacks:** never suppress compiler errors/warnings or mask real problems. Fix the root cause or use a clean production solution. If no clean form exists, use an explicit/raw form with an explanatory comment (for example, `0xC0000374 // STATUS_HEAP_CORRUPTION` instead of disabling warning 4005 for `ntstatus.h` redefinitions).
- **Direct CLI elevation:** every direct `tsdev_cli` subcommand, including direct `serve`, requires an elevated administrator process. The `isElevated()` check returns false when the process is not elevated, and the CLI reports exit code 11; preserve this behavior.
- **Test and broker elevation are separate contracts:** TS-dependent test executables/harnesses have their own administrator prerequisite. The `-NoTs` path is non-TS: sigtool plus the non-TS C++ unit-test executables (current inventory in `tests/run_unit_tests.ps1`). An elevated parent can pass its token to child PowerShell, test-runner, and CLI processes; that inherited token is the child’s elevation state, not a different CLI contract. `TsDevClient(use_serve=True)` starts a separate elevated named-pipe `tsdev_cli serve` broker through `ShellExecuteExW`/`runas`. With `use_serve=False`, non-elevated Python raises `ElevationRequiredError` before direct launch.
- **Testing UAC elevatio**: To test whether auto-elevation via UAC works (e.g. launching Python unelevated, which launches `tsdev_cli.exe` with elevated privileges), *temporarily disable UAC prompts* before running the test from an unelevated shell. Avoid having the user to manually confirm UAC prompts during testing.
- **Reverse engineering:** use the working Ghidra MCP server (`ghidra-mcp`) for static analysis. Use Frida directly from the CLI: `frida-trace`, `frida -l script.js`, or `frida -p PID`. Frida 17.17.0 and 32-bit spawn/attach plus its full JS API are available. If necessary, use Frida from ad-hoc Python scripts.

## Single source of truth (SSOT)

Counts, listings, and details that could drift live in ONE authoritative location (the SSOT). Every other place references the SSOT instead of restating the count/list/detail, so that when a value changes only the SSOT needs editing.

| Category | Authoritative location |
|---|---|
| C++ unit test inventory (list + count) | `tests/run_unit_tests.ps1` (`$TestDefinitions` array); `CMakeLists.txt` keeps its `add_dependencies` list as a build requirement |
| Sub-suite case counts + sub-suites | `tests/run_regression.ps1` |
| Knowledge base with discovered, relevant facts about TSDev.exe (hard evidence and references) | `docs/KNOWLEDGE_BASE.md` |

## Operations (six)

- **Insert:** given EasyLanguage and a name, insert an item, compile, and verify; fail loudly on compilation failure.
- **Delete:** given a type and name, delete the item; notify if it does not exist.
- **Modify:** replace an existing item’s source and recompile/reverify; notify if absent and fail loudly on compilation failure.
- **Exists:** given type and name, return whether it exists, its verbatim stored name, and its ProgId when found. C++ uses `--case-insensitive`; Python uses `case_insensitive=True`. Human output is `exists=N`, `stored_name=...`, `progId=N`; `--json` selects JSON. Without the flag, search is case-sensitive. Never use source comments for the stored name.
- **Get source:** given type and name, emit raw EasyLanguage to stdout or use `--output <file_path>`; decrypt internal encrypted items automatically, as TSDev does.
- **Upsert:** given type, name, and a source file, modify if present or insert otherwise.

## Acceptance criteria

Production acceptance requires all six operations to work reliably and consistently through the C++ CLI (`tsdev_cli`) and Python wrapper (`TsDevClient`). The CLI must support batch operations from both a manifest path and stdin (without a temporary manifest), so Python can drive batches directly

This six-operation acceptance target is distinct from type coverage: five of eight warehouse mappings are supported, while ActivityBar, PaintBar, and ProbabilityMap remain unsupported (not needed).

## Stack and process

- C++ with the available Visual Studio Build Tools.
- Reverse engineer TSDev/orlib20 with Ghidra for static work and direct Frida CLI instrumentation for runtime work. Prefer the smallest, least-complex standalone implementation that reproduces TSDev behavior.

**Process:**
- Work iteratively: one minimal Thin Vertical Slice (TVS) at a time; determine the current state and best next slice, update relevant docs, and use TDD (red-green).
- TVSs are *minimal*: If a TVS can be reduced to a smaller scope while remaining an end-to-end verifyable TVS, split it.
- *Iterate*: Identify and execute the next TVS towards the oerarching objective(s). Then stop and hand-off. *Do not* plan subsequent work; the next team will find their next step.
- After every approved TVS, run a passing real production CLI E2E of `tsdev_cli insert/modify/delete` using the latest code from an elevated direct CLI. Cleanup before relying on approval or closure. This narrower gate supplements unit/regression tests and is distinct from six-operation production acceptance; it is mandatory before the TVS is done.

**MANDATORY:**
Always aim to *emulate the TSDev.exe UI without needing a GUI*:
- Trigger actions *at the highest level possible*, like TSDev.exe would
- Avoid digging deeper and fiddling at lower levels, if it is not *certain beyond any doubt* that there is no higher-level point at which to call or read
- The purpose of the two rules above is *simplicity* and *robustness*: Minimize reliance on binary internals. Few, simple calls7access points, with high reliability, which facilitate easy porting to future TradeStation/TSDev updates.

## PM and subagent rules

Skip the PM rules when operating as a subagent. When acting as PM:

- Delegate any work that can be delegated; do not perform work or read files/URLs that can be handed to a subagent. Perform management only and remain ultimately responsible for correctness and production quality.
- Use the `subagent?general` profile by default for every narrow task (research, MCP, analysis, implementation, tests, or docs). Use `subagent_explore` only for trivial read-only lookups; if insufficient, retry `subagent_general`.
- Maintain `docs/WORKING_MEMORY.md` as your *live, real-time* working memory. Drop information which has become irrelevant, been proven wrong or superseded. Only keep true facts about the current state. This replaces your reliance on internal context, which may be compacted with loss of information at any point.

## Work orchestration

- Decompose complex work into small atomic subtasks; a specialized subagent decomposes complex work, and each subagent handles exactly one narrow concern.
   - *Hard rule*: One subagent handles one *minimal*, single-focus, single-concern subtask
   - If a subtask can be split, split it and assign multiple subagents
- Every *substantive* research result, analysis, plan, document, or implementation requires independent adversarial review. The reviewer receives the original task and result, checks 100% completion, scope/drift, and hacks/shortcuts, then APPROVES or DISAPPROVES with findings. A disapproval requires implementer fixes and another review until approval.

## TradeStation component types

TradeStation has eight EasyLanguage types: **ActivityBar, Function, Indicator, PaintBar, ProbabilityMap, ShowMe, Strategy, TradingApp**. Strategies cannot use `Plot`; only Indicators, ShowMe, and PaintBar can plot (5 confirmed, 3 not required). The code SSOT is the `src/tsdev_lib.h` enum. The CLI currently supports the five confirmed types; ActivityBar, PaintBar, and ProbabilityMap are not required and remain unconfirmed (ignore them).

## Relevant paths

- Default TradeStation installation root: `C:\Program Files (x86)\TradeStation 9.5`
- Default TSDev executable: `C:\Program Files (x86)\TradeStation 9.5\Program\TSDev.exe`
- Default compiled/encrypted TSDev Library: `C:\Program Files (x86)\TradeStation 9.5\MyWork\Bin`
- Authoritative signatures: `signatures/*.json` → `tools/sigcodegen.py` → generated `signatures.gen.h`
- Regression entry point: `tests\run_regression.ps1`; unknown-link harness: `tests/test_unknown_link_regression.ps1`; auto-cleanup coverage: `tests/test_auto_cleanup.cpp`; baseline DLLs: `tests/fixtures/` (test runners can prompt for the baseline DLL); CLI install discovery also accepts `--ts-path`.
- CI: `.github/workflows/ci.yml` runs the Win32 build, `-NoTs` regression, and mocked pytest on pushes/PRs, and attaches the bundle zip to `v*` releases. CMake POST_BUILD assembles `dist/` and a timestamped zip in `artifacts/`; the diagnostic build assembles `dist_diag/` (parallel to `dist/`); `tools/create_bundle_zip.ps1` produces release bundles. Bundle lookup checks `<package_parent>/tsdev_cli.exe` before the source-tree walk-up. Runtime Python dependencies are stdlib-only.

## Documentation index

Read this file first. Read the following on demand:

## Current state and TODO

The PM keeps this list current and identifies the next step toward 100% acceptance; update it as project state changes.

- Keep it clean, minimal and relevant:
   - DO NOT keep history/records that are not critically relevant for future development (e.g. decision history, list of completed work)
   - DO NOT keep plan ore than one TVS ahead
   - ONLY keep a minimal "current state" section with relevant information *which cannot be gained from the code itself*
      - NEVER document what the code documents; no redundancies

## Verification baseline

- Regression command: `powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1`. Must support Windows PowerShell 5.1 and `pwsh` 7. The regression runner provides `-SkipServeE2e`, `-SkipShowMeE2e`, and `-NoTs` options. The current `-NoTs` path runs sigtool plus the non-TS C++ unit-test executables (current inventory in `tests/run_unit_tests.ps1`).

## Standing notes

- This machine uses PowerShell: do not use bash heredocs for `git commit -m` or other commands. Write a message to a temp file (for example, `tmp\commit_msg.txt`) and use `git commit -F tmp\commit_msg.txt`; use `;` rather than `&&`, and `&` for executable paths containing spaces.
- **Check for and HANDLE TSDev/TS error and crash dialogs after every action.** When triggering any action in TSDev.exe (insert/delete/modify/find-dependents/etc.) or running any project executable that touches TradeStation, always verify the result by checking for popup windows: error messages, warning dialogs, and crash reports (e.g. `TSCrashReport.exe`). A popup or crash report means the expected result was NOT achieved — a warning/error/crash was provoked instead. Do this for both the TSDev UI and our own executables (they can trigger TS crash reports and other errors too). Never assume an action succeeded without explicitly checking for these failure surfaces. When a popup IS found: (1) READ its message text, (2) dismiss it, (3) CORRECT COURSE accordingly — do not get stuck or retry blindly. Common examples: an "item already exists" popup on insert means the name is taken → use a unique name or delete the existing item first; a "not found" popup on delete/modify means the item is absent → skip or report it; a compile-error popup means the source is invalid → fix the source. Always record the popup message and the corrective action taken in the trace/log. Before inserting any item, proactively check whether the name already exists (via `tsdev_cli exists` or the UI) to avoid name-collision popups in the first place.

