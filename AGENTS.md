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

### DONE — TVS-1: Technical feasibility & approach investigation
- Reverse-engineered TSDev.exe, the MyWork\Bin library (`.ell` = compiled/encrypted, magic `RNAM`, paired with `.xml` symbol metadata), and the EasyLanguage toolchain DLLs.
- Identified two viable approaches; recommendation documented and adversarially APPROVED in `docs/FEASIBILITY.md` (evidence in `tmp/re_artifacts/`).
  - **Approach 1 (ELIS.exe COM `IELServices`)** — high-level but REJECTED as primary: no name→ProgId lookup, imports from an XML file (not raw EasyLanguage source), `void` error reporting on delete/verify, 32-bit only, `RunAs=Launching User`, possible TS Desktop dependency. Retained as fallback for delete.
  - **Approach 2 (orlib20.dll direct invocation)** — RECOMMENDED as primary: complete primitive set (init, name lookup, create entry, set source, compile/verify, error retrieval, save, delete+file removal). See `docs/FEASIBILITY.md` for the full primitive table, INSERT/MODIFY/DELETE pseudocode, and confidence levels.

### DONE — TVS-2.1: Headless orlib20 init smoke test (CRITICAL GATE — PASSED)
- **Args determined:** `_OrlibInit@8(NULL, 1)` → TRUE (workDirPath is a NULL sentinel; contextId=1). Confirmed via static disassembly + live Frida replication. Evidence: `tmp/re_artifacts/orlib_init_args.md`.
- **32-bit C++ build infra:** CMake + VS Build Tools 2026, Win32/x86, MSVC 19.51 → `build\Release\tsdev_probe.exe`. Build docs: `docs/BUILD.md`.
- **Headless smoke test:** Probe loads orlib20.dll, `_OrlibInit(NULL,1)` → 1, `_OrlibExit()` → 1, clean exit 0, no crashes (no TS processes/services running). Evidence: `tmp/re_artifacts/headless_init_smoke_test.md`.
- **Adversarial review:** APPROVED (independent rebuild + re-run, ABI audited, no scope creep). Review: `tmp/re_artifacts/tvs2_1_adversarial_review.md`.
- **GATE VERDICT: PASSED.** orlib20-direct approach is viable headless.

### DONE — TVS-2.2: Resolve unknown orlib20 parameter orders & persistence (PASSED)
- **Parameter orders CONFIRMED** for all core primitives via Frida + Ghidra + probe validation. Details: `tmp/re_artifacts/orlib_param_orders.md` (adversarially APPROVED).
  - `OrlibInit(NULL, 1)`, `FindExistPos(name, whType)`, `CreateEntryEX(whType, 0, name, 0, 0, &errCode)`, `SaveModule_New(whType, progId, NULL, hwnd, NULL)`, `VerifyModule_New(hwnd, whType, progId, 1, compilerObj)`, `GetCompilingErrors`, `StartCompilingProcess`/`EndCompilingProcess`, `CloseWarehouseForCurrentThreadAndPath(0)`.
  - Warehouse types CONFIRMED: 0=Strategy, 2=Indicator, 4=Function. UNRESOLVED: ActivityBar, PaintBar, ProbabilityMap, ShowMe, TradingApp.
- **CRITICAL PERSISTENCE BREAKTHROUGH:** `SymWriter::Save` in `ELDebugger.dll` calls `XmlDocument.Save` (.NET) which crashes standalone; this skips `.ell` writing. Fix: patch `FUN_10011230` in orlib20.dll (RVA 0x11230) with `MOV EAX,1; RET 0x0C` (`B8 01 00 00 00 C2 0C 00`). `.xml` not written — acceptable.
- **Standalone persistence CONFIRMED:** `elevated_test20.exe` with patch produces `elmodule_0_2572.ell` (5091 bytes, magic `RNAM`), no Frida. `VerifyModule_New = 1`.
- **GATE VERDICT: PASSED.** INSERT persistence demonstrated standalone. Proceed to TVS-2.3.

### DONE — TVS-2.3: INSERT / DELETE / MODIFY — all APPROVED (library level)
- **INSERT — DONE & APPROVED** (TVS-2.3.2-4): Library-level `createEntry` + `saveSource` + `verifyModule` + `.ell` persistence via FUN_10011230 patch. Production flow wraps the confirmed 20-step pipeline (load DLLs → patch → OrlibInit → FindExistPos/CreateEntryEX → hidden EDIT control → SaveModule_New → CELCompiler → StartCompilingProcess → VerifyModule_New → GetCompilingErrors → EndCompilingProcess → CloseWarehouse → OrlibExit → verify `.ell`). Fails loudly on compile errors.
- **DELETE — DONE & APPROVED** (TVS-2.3.6): Library-level orchestrator `FUN_101a8360` (RVA `0x1A8360`, `__fastcall` ECX=whType EDX=progId, SEH-protected) + thread-check INT3 patches NOP'd (RVA `0x1D0993`, `0x1CF5D4`, `0x1D1FC8`). Flow: `FindExistPos` → `FUN_101a8360` (DeleteFromTree → closeWarehouse → commit) → delete `.ell`/`.xml` → `OrlibExit`. Verified by 2 independent runs (exit 0, `MWRKArea.wrk` shrinks 4096 bytes, fresh `FindExistPos`=-1, `.ell` removed, no WerFault). Prior failed approaches RETIRED. See `tmp/re_artifacts/DELETE_WORKING_MEMORY.md` and `docs/FEASIBILITY.md` §5.7.
- **MODIFY — DONE & APPROVED** (TVS-2.3.9-11): Library-level `findExistPos` → `saveSource` → `verifyModule` composition; no new orlib20 calls/RVAs. `CreateEntryEX` skipped — existing ProgId reused (preserved). Verified by 2 independent runs (ProgId preserved 2609→2609, 2610→2610; `.ell` timestamp changed; negative + compile-error tests return `success=false`; cleanup passes; no crashes). See `docs/FEASIBILITY.md` §5.8.
- **All three core functions work standalone** (no TSDev.exe, no WHServer killing). DLL injection fallback no longer needed. The primary acceptance criteria is satisfied at the LIBRARY level.

### DONE — CLI: all three subcommands functional (APPROVED)
- The CLI executable (`tsdev_cli`) now exposes `insert`, `delete`, and `modify` subcommands. The `modify` subcommand was added (calls `modifyEntry`, handles success/not-found/compile-error with exit codes 0/5/8 matching insert/delete conventions, runs on a 64MB-stack worker thread, preserves ProgId). Insert and delete were already up to date and unchanged.
- Independently verified: insert→modify→delete cycle, error cases (nonexistent, invalid source), arg parsing, and all three test suites (test_insert, test_delete_diag, test_modify) — all exit 0. Adversarial review: APPROVED (no scope creep, library untouched).
- **The primary acceptance criteria ("the three functions must work reliably and consistently via a CLI") is now fully satisfied** — not just at the library level, but via the CLI itself.

### DONE — TVS-A.2: GetCompilingErrors ABI + error array population path (APPROVED, then CORRECTED)
- **Original ABI conclusion was WRONG:** TVS-A.2 concluded `_GetCompilingErrors@4` returns `std::string` by value via hidden return pointer. This was corrected during TVS-A.4: the function actually takes a `std::vector<ErrorOutputRecord>*` (3 pointers, 12 bytes). Evidence: `FUN_1015fd20` calls `std::_Xlength_error("vector<T> too long")`; `FUN_10160570` initializes each 177-byte (0xB1) element with 7 std::strings + 2 ints + 1 flag. The singular `GetCompilingError` (not plural) DOES use std::string hidden return ptr — TVS-A.2 confused the two. See correction notice in `tmp/re_artifacts/A2_geterrors_static_abi.md`.
- **Error-record layout, population path, and VERIFY.ERR semantics remain CORRECT** (only the ABI conclusion was wrong). See `tmp/re_artifacts/A2_geterrors_static_abi.md` for details.
- **Error-record layout (stride 0x129 = 297 bytes):** 0x00 short=level (0x7775=error), 0x02 int=moduleID, 0x12 short=string-table resource ID, 0x29 char[]=error text, 0x128 byte=flag. Line number computed by `CELBasicCompiler::GetLineNumber` (not stored).
- **Population:** `FUN_10156850` (RVA 0x156850) appends records — 28 callers across compile pipeline. `StartCompilingProcess` (RVA 0x165DE0) resets.

### DONE — TVS-A.4: Real compiler error text via direct global array access (APPROVED)
- **Root cause fixed:** The empty/garbage error buffer was caused by calling `_GetCompilingErrors@4` with the wrong ABI (char[] then std::string*, but the function expects std::vector*). Since matching std::vector across DLL boundaries is impractical (different CRT allocators), the fix **bypasses GetCompilingErrors entirely** and reads the global error array directly from orlib20.dll's data section.
- **Implementation:** `readErrorsFromArray(HMODULE orlibBase, const std::string* source)` reads `DAT_1023cea0` (array pointer, RVA 0x23cea0) and `DAT_1023cea4` (count, RVA 0x23cea4), iterates 297-byte records, extracts null-terminated text at offset 0x29, falls back to `LoadStringA` with resource ID at offset 0x12 and HINSTANCE at RVA 0x23aab8 if text is empty. Count bounded to 1000 for safety.
- **GetCompilingErrors fully removed:** typedef, member variable, export resolution, and call site all removed from `tsdev_lib.h` and `tsdev_lib.cpp`. `callVerifyAndErrors` replaced with `callVerify` (SEH-protected VerifyModule_New only). Error array read between VerifyModule_New and EndCompilingProcess.
- **TDD red-green:** RED confirmed (exit 117, garbage "11" with no alpha chars). GREEN confirmed (exit 0, real error text: `"unknown identifier\nSemicolon (;) expected here."`). Regression: test_insert, test_delete_diag, test_delete all exit 0.
- **Adversarial review: APPROVED.** Minor observations: SEH protection on array reads deferred to future robustness TVS; unused `#include <vector>` is pre-existing.
- **TVS-A.3 answered implicitly:** The error array IS populated headlessly (real error text appeared after the fix).

### DONE — TVS-A.6: Source location (line/column) in error messages (APPROVED)
- **Feature:** Compiler error messages now include `"Line X, Col Y: "` prefix pointing the user to the exact source location. Example: `"Line 2, Col 1: unknown identifier\nLine 2, Col 27: Semicolon (;) expected here."`.
- **How it works:** Error record offset 0x0A (int) contains a character offset into the source code. `readErrorsFromArray` counts newlines in the source up to that offset to compute line number; column = offset - start of current line + 1. No orlib20 function calls needed — pure caller-side computation.
- **Implementation:** `m_lastSource` member added to TsDevLib, stored in `saveSource()`. `readErrorsFromArray` takes optional `const std::string* source` parameter. Line/col computed only when source is available and charOffset > 0. Low complexity: ~15 lines of newline-counting code, no new RVAs/exports, no orlib20 calls.
- **TDD red-green:** RED (exit 118, no "Line" in message). GREEN (exit 0, "Line 2, Col 1: unknown identifier"). Regression: all 3 tests pass.
- **Adversarial review: APPROVED.** Minor observations: charOffset=0 produces no location prefix (reasonable for unknown positions); m_lastSource is a full copy (simplest correct approach).
- **Workstream A is now COMPLETE.** All 6 TVSs resolved: A.2 (corrected), A.4 (error text), A.6 (source location). A.1/A.3/A.5 no longer needed.

### NEXT WORKSTREAMS (in priority order)
Hand-off documents exist in `docs/` for future workstreams. Each lists its own TVSs and is ready for a new team to pick up.
1. **(a) Compilation errors** — `docs/HANDOFF_A_COMPILATION_ERRORS.md`: **COMPLETE.** Real compiler error text with source location now works. TVS-A.2 (corrected), TVS-A.4, TVS-A.6 all done and approved.
2. **(b) Robustness & architecture** — `docs/HANDOFF_B_ROBUSTNESS.md`: Robustness + architecture for future TS/TSDev updates (12+ hardcoded RVAs; signature-based resolver + version detection + regression suite). 7 TVSs. NOT started.
3. **(c) TradingApp component type** — `docs/HANDOFF_C_TRADINGAPP.md`: TradingApp component type support (whType unknown; all primitives are type-agnostic once the code is found). 7 TVSs. NOT started.
4. **(d) Python DLL (64-bit)** — `docs/HANDOFF_D_PYTHON_DLL.md`: DLL library for 64-bit Python via msl-loadlib (32-bit DLL + Server32/Client64 bridge). 7 TVSs. NOT started.

### REMAINING FOLLOW-UPS (smaller items — note overlap with workstreams above)
- **FindExistPos export name:** Confirm/normalize the decorated export name used by the CLI (currently resolved by RVA; the decorated `_FindExistPos@8` form should be validated against the export table for robustness). *Also relevant to workstream (b).*
- **Compiler error text — DONE (TVS-A.4):** Real compiler error text now available via direct global array access. The CLI already prints `res.message` to stderr on compile failures (exit code 8), so real error text should already flow to the user. Verify in a follow-up that the CLI output is correct (TVS-A.6).
- **Unresolved warehouse-type mappings:** Warehouse-type codes for 5 component types (ActivityBar, PaintBar, ProbabilityMap, ShowMe, TradingApp) are NOT confirmed. Code today must restrict itself to WHType 0/2/4 (Strategy, Indicator, Function). See `docs/FEASIBILITY.md` §5.2 and §5.5. *TradingApp is covered by workstream (c); the other 4 types are not yet scoped in a hand-off.*

### Standing notes
- TradeStation install paths are READ-ONLY — never modify. Copy files into `tmp/` to work on them.
- All substantive results must be adversarially reviewed (APPROVE/DISAPPROVE loop) before being accepted.
- **Frida**: Use Frida DIRECTLY via the `frida` CLI (`frida-trace`, `frida -l script.js`, `frida -p PID`), NEVER via MCP tools. The Frida MCP server was removed because it never worked reliably. Frida 17.17.0 is installed. Live RE works: 32-bit spawn/attach, interactive sessions, and the full Frida JS API via `execute_in_session` (symbol resolution, disassembly via `Instruction.parse`, function hooking via `Interceptor.attach`, memory reads, `NativeFunction` for direct calls).
- **Ghidra**: Ghidra MCP tools ARE available and work reliably. Use them for static analysis (decompilation, disassembly, symbol resolution, cross-references). The Ghidra MCP server is "ghidra".

