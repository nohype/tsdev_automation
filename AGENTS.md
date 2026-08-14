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

List:

### DONE — TVS-1: Technical feasibility & approach investigation
- Reverse-engineered TSDev.exe, the MyWork\Bin library (`.ell` = compiled/encrypted, magic `RNAM`, paired with `.xml` symbol metadata), and the EasyLanguage toolchain DLLs.
- Identified two viable approaches; recommendation documented and adversarially APPROVED in `docs/FEASIBILITY.md` (evidence in `tmp/re_artifacts/`).
  - **Approach 1 (ELIS.exe COM `IELServices`)** — high-level but REJECTED as primary: no name→ProgId lookup, imports from an XML file (not raw EasyLanguage source), `void` error reporting on delete/verify, 32-bit only, `RunAs=Launching User`, possible TS Desktop dependency. Retained as fallback for delete.
  - **Approach 2 (orlib20.dll direct invocation)** — RECOMMENDED as primary: complete primitive set (init, name lookup, create entry, set source, compile/verify, error retrieval, save, delete+file removal). See `docs/FEASIBILITY.md` for the full primitive table, INSERT/MODIFY/DELETE pseudocode, and confidence levels.

### NEXT — TVS-2: De-risk the orlib20-direct approach (highest ROI, in order)
1. **Headless init smoke test (CRITICAL GATE):** Build a minimal 32-bit C++ probe that loads `orlib20.dll`, calls `_OrlibInit@8(workDirPath, contextId)` and `_OrlibExit@0()`, and reports success — WITHOUT the TradeStation Desktop running. If `ELRte::CreateRte` fails headless, the primary approach is blocked and we pivot (ELIS COM, or driving TSDev.exe). This gate must pass before any further implementation work.
2. **Resolve unknown orlib20 parameter orders:** Confirm exact arg positions/semantics for `_CreateEntryEX@24` (params 2-5, WHType position), `_SaveTextEX@16` (p1/p4), `_VerifyModule_New@20` (p3 type, p4/p5) by **live tracing via the frida MCP**: spawn or attach to TSDev.exe, hook the orlib20 functions with `Interceptor.attach` (capturing `args[0..N]` in `onEnter`), then trigger insert/delete/modify operations in the TSDev UI to observe the actual argument values passed by the authoritative caller. This is more reliable than static caller-site disassembly. Also confirm whether `_SaveTextEX` alone persists or a save step is mandatory, and validate the hidden-EDIT-control workaround for the GUI-bound `_SaveModule_New@20`.
3. **Implement the CLI with TDD (red-green), one function at a time:** DELETE first (simplest, lowest risk: `FindExistPos` → `_GetMasterRecByName` → `_SetSigDeleted` → `FILEDELETE` via `SigNumToFileName`), then INSERT (`_OrlibInit` → `StartCompilingProcess` → `_CreateEntryEX` → `_SaveTextEX` → `_VerifyModule_New` → `_GetCompilingErrors`/`_GetErrorLevel` → save → `EndCompilingProcess` → `_OrlibExit`; fail loudly on compile errors), then MODIFY (lookup existing → reuse ProgId → re-set source → re-verify → re-save; notify if not found). Each function must work reliably and consistently via the CLI per the acceptance criteria.

### Standing notes
- TradeStation install paths are READ-ONLY — never modify. Copy files into `tmp/` to work on them.
- All substantive results must be adversarially reviewed (APPROVE/DISAPPROVE loop) before being accepted.
- Frida MCP tools are available to subagents (confirmed). The MCP server is "frida" (Python frida MCP, Frida 17.17.0). Live RE works: 32-bit spawn/attach, interactive sessions, and the full Frida JS API via `execute_in_session` (symbol resolution, disassembly via `Instruction.parse`, function hooking via `Interceptor.attach`, memory reads, `NativeFunction` for direct calls). The tool surface is simpler than the former Luma MCP (no radare2/decompile/r2_cmd), but `execute_in_session` provides equivalent capability through Frida's JavaScript API.

