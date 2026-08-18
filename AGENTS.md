# TSDev Automation

This project enables *automated management* of the *TradeStation Development Environment (TSDev) Library* without using the TSDev UI (TSDev.exe).

**IMPORTANT**:
The rules and guidelines below are *MANDATORY* unless *explicitly* overridden by user instructions.

## Functions
3 main interactions with the TSDev Libray:

- Inserting new Strategies and Functions:
   - Provided EasyLanguage source code and the strategy/function name, insert a new strategy/function into the TSDev Library and compile/verify it (fail loudly if compilation fails)
- Deleting existing Strategies and Functions:
   - Provided the name of an existing strategy/function, delete it from the TSDev Library (notify if it doesn't exist)
- Modifying existing Strategies and Functions:
   - Provided the name of an existing strategy/function and new EasyLanguage source code, replace the existing strategy's/function's EasyLanguage code and recompile/re-verify (notify if it doesn't exist; fail loudly if compilation of new EasyLanguage code fails)

## Acceptance criteria
The three functions (see `## Functions` above) *must* work reliably and consistently via a CLI.

## Tech stack and tooling
- This project is written in C++ using the Visual Studio Build Tools (already available on this machine).

## Technical process
- Use the *luma* MCP tools to reverse engineer, inspect and analyze the TSDev.exe application and its modules
- Determine how this project (a custom, standalone CLI application) can most efficiently, with the least amount of custom implementation and the least complexity, implement the three functions (see `## Functions` above)

## PM (lead agent) role:
(Skip if you are a subagent)

If you are *not a subagent:* You are the Product Manager (PM).
- You *do not* engange in any work if you can delegate it to a subagent
- You *do not* read any files/urls if you can pass the file path/url to a subagent directly
- You only perform management tasks in your role as PM
- You are leading the team, so you are ultimately responsible for the *correctness* and *production quality* of all results

### Subagent profile selection (HARD RULE)
- Use the **`subagent_explore`** profile ONLY for trivial lookups (e.g. "read file X and quote section Y verbatim"). It has very limited capabilities and tool access (read-only: grep, glob, read, web_search).
- For **research, analysis, any task that needs MCP tools (e.g. Ghidra), or any non-trivial work**, use the **`subagent_general`** profile. This is a HARD RULE.
- When an `explore` subagent fails (e.g. due to lacking tool access) or produces unsatisfactory output/results, retry the same task with the `general` subagent profile.

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
- **End-to-end CLI verification (MANDATORY after every TVS):** After any TVS implementation is successfully completed and approved, run an end-to-end test with the real production CLI (`tsdev_cli insert/delete/modify`) using the latest state of the codebase at that point in time. This ensures the most important artifact — the production CLI that satisfies the primary acceptance criteria — remains operational after every change. This is in addition to (not a replacement for) the unit/regression test suite. The e2e CLI test must PASS before the TVS is considered fully done. For the comprehensive E2E test procedure covering all 4 component types (Strategy, Indicator, Function, TradingApp) with type-specific semantic error cases, see `docs/E2E_TEST_SPEC.md`. Agents or subagents performing E2E CLI verification should read that document for the full test specification.

## TradeStation component types
TradeStation supports 8 EasyLanguage component types: **ActivityBar, Function, Indicator, PaintBar, ProbabilityMap, ShowMe, Strategy, TradingApp**. Each has different purposes and constraints (e.g., Strategies cannot use Plot statements; only Indicators can plot). See `docs/TS_COMPONENT_TYPES.md` for the full mapping to orlib20 warehouse types.

## Relevant directories and paths
These TradeStation (TS) installation paths are *READ-ONLY* and *MUST NEVER BE MODIFIED*! We cannot break the existing installation.
Any files from the TS installation directory which must be inspected, modified, etc. can be copied into a temporary subdirectory of this project to work on them.

- The TradeStation Desktop installation includes the TradeStation Development Environment (TSDev): `C:\Program Files (x86)\TradeStation 9.5`
- The TradeStation Development Environment application (TSDev.exe): `C:\Program Files (x86)\TradeStation 9.5\Program\TSDev.exe`
- The TSDev Library (where strategies, functions, etc. are stored *compiled and encrypted*): `C:\Program Files (x86)\TradeStation 9.5\MyWork\Bin`


## TODO
This is a running, continually updated TODO list to bring this project to complection.
The PM updates this list according to the current project state: Analyze the current project state and map out the next 1-3 steps with the highest ROI to bring us towards satisfying our acceptance criteria (see `## Acceptance Criteria` above) 100%.
Update this list to reflect the current state and the next best steps as the project progresses and information changes.

### Completed Work Summary

All workstreams A, B, C, E are COMPLETE. Detailed per-TVS logs are archived in `docs/PROJECT_HISTORY.md`.

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
| TVS-B.1.5 | Validate original bytes before persistence patch | COMPLETE |
| TVS-B.2 | Version detection strategy & variability assessment | COMPLETE |
| TVS-B.3 | OrlibResolver abstraction layer design | COMPLETE |
| TVS-B.4 | OrlibResolver .cpp implementation | COMPLETE |
| TVS-B.4.5 | resolveErrorHinstRva instruction-offset extraction | COMPLETE |
| TVS-B.5 | OrlibResolver integration with TsDevLib | COMPLETE |
| TVS-B.6 | Automated regression test suite (60 tests) | COMPLETE |
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

**Key architectural facts:**
- Signature data: `signatures/*.json` (authoritative) → `tools/sigcodegen.py` → `signatures.gen.h` (build-time codegen)
- OrlibResolver: signature-based RVA resolution, no hardcoded RVAs in production code
- CLI supports 4 component types: Strategy (0), Indicator (2), Function (4), TradingApp (5122)
- Regression suite: `powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1` (60 tests)
- Fresh clone builds without `tmp/` dependencies (Workstream E fixed this)

### NEXT WORKSTREAMS (in priority order)
1. **(D) Python Integration** — COMPLETE. See `docs/INTEGRATION_APPROACH_EVALUATION.md` (approach decision) and the `tsdev_automation` Python package (`src/tsdev_automation/`). Approach: Option A+ (batch CLI + JSON I/O, no daemon). 64-bit Python drives the 32-bit `tsdev_cli.exe` as a subprocess (in-process 32-bit DLL loading is impossible from 64-bit Python — WOW64 constraint, verified). `TsDevClient` API: `insert`/`modify`/`delete` (single-item via `--json`, returns `TsDevResult`, raises typed exceptions) + `batch_insert`/`batch_modify`/`batch_delete` (line-based manifest, JSON results, init amortized — 600 items in ~140s vs ~386s unbatched). 29 Python tests (20 unit + 9 E2E) + 60 C++ regression tests all passing. Wheel builds via `uv build`.

Workstreams A (compilation errors), B (robustness), C (TradingApp), D (Python integration), and E (architecture redesign) are ALL COMPLETE. Their handoff documents have been archived to `docs/archive/`.

### REMAINING FOLLOW-UPS
- **4 unresolved warehouse-type mappings:** ActivityBar, PaintBar, ProbabilityMap, ShowMe whType codes are NOT confirmed. Code restricts to 4 confirmed types: Strategy (0), Indicator (2), Function (4), TradingApp (5122). See `docs/TS_COMPONENT_TYPES.md` and `docs/FEASIBILITY.md` §5.2/§5.5. These require Ghidra/Frida RE work per type and are not yet scoped in a handoff.
- **Partial warehouse entry on compile failure (TD-001):** When `insert` fails compilation, `createEntry` has already allocated a ProgId and created a warehouse entry. The CLI now auto-cleans up this partial entry (fixed in this cleanup pass). See `tests/test_auto_cleanup.cpp`.

### Resolved follow-ups (closed)
- ~~FindExistPos export name~~ — CONFIRMED (undecorated only, ordinal 453). Production code correct.
- ~~Compiler error text CLI output~~ — DONE (TVS-A.4 + TVS-A.6). Verified working via live testing.
- ~~TVS-A.6 "Line X, Col Y:" prefix regression~~ — NOT REPRODUCIBLE. Live testing confirms the prefix works correctly for all component types. The flag was a stale misobservation.
- ~~Stale +0x3E comment in tsdev_lib.cpp~~ — Already correct (+0x43). The B.3 flag was itself stale.
- ~~Stale SIGNATURE_CATALOG.md §4.9 note~~ — RESOLVED (TVS-B.1.5). Doc updated.
- ~~Partial-construction patch restoration gap (TD-002)~~ — FIXED. `m_patchesApplied` now set after each individual patch; destructor guards restoration by cached RVA.

### Standing notes
- TradeStation install paths are READ-ONLY — never modify. Copy files into `tmp/` to work on them.
- All substantive results must be adversarially reviewed (APPROVE/DISAPPROVE loop) before being accepted.
- **Git commits on Windows (PowerShell) — NO HEREDOC:** This machine's shell is PowerShell. PowerShell does NOT support bash-style heredocs (`$(cat <<'EOF' ... EOF)`) — attempting this fails with parser errors (`'<' operator is reserved`, `Missing file specification after redirection operator`). **NEVER use heredoc syntax for `git commit -m` or any other command.** Instead, write the commit message to a temp file (e.g. `tmp\commit_msg.txt`) and use `git commit -F tmp\commit_msg.txt`. This is the ONLY reliable way to pass multi-line commit messages on this machine. Also remember: PowerShell uses `;` (not `&&`) to chain commands, and `&` is needed to invoke executables whose path contains spaces.
- **Frida**: Use Frida DIRECTLY via the `frida` CLI (`frida-trace`, `frida -l script.js`, `frida -p PID`), NEVER via MCP tools. The Frida MCP server was removed because it never worked reliably. Frida 17.17.0 is installed. Live RE works: 32-bit spawn/attach, interactive sessions, and the full Frida JS API via `execute_in_session` (symbol resolution, disassembly via `Instruction.parse`, function hooking via `Interceptor.attach`, memory reads, `NativeFunction` for direct calls).
- **Ghidra**: Ghidra MCP tools ARE available and work reliably. Use them for static analysis (decompilation, disassembly, symbol resolution, cross-references). The Ghidra MCP server is "ghidra".
