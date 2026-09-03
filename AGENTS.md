# TSDev Automation

Automates management of the TradeStation Development Environment (TSDev) Library by **driving TSDev.exe itself**: triggering the same code paths its UI controls trigger, without GUI automation and without requiring a visible window. This branch is an overhaul — the previous approach (directly interfacing with the TS library without TSDev.exe) is discarded.

The rules below are mandatory.

## Branch direction (fixed — not negotiable)

- Interact with TSDev.exe; never manipulate TS library files directly.
- The TSDev window may be hidden or minimized, but actions are triggered as if the corresponding UI controls were operated:
  - **Insert:** put the EasyLanguage code directly into the text editor element (set the text content directly; avoid the clipboard), then trigger the same verify (compile + save) code path the UI would.
  - **Verify:** trigger the compile + save code path.
  - **List / Exists:** trigger the same code path the UI uses to enumerate items (all, or per component type); this also provides existence checking.
  - **Modify:** open the existing item, set the new code in the editor, re-verify to save.
  - **Delete:** trigger the same deletion code path the UI would.
- No UI automation (no synthetic clicks/keys on controls); trigger code paths at the highest level possible.
- Motivation: safety (no data corruption, no interference with running TS processes, no crashes) and easy porting to future TSDev versions.
- **Purpose of the alternative approach — every design decision is measured against these:**
  - **a) Minimize reverse-engineering work.**
  - **b) Minimize reliance on binary internals** (brittle across TSDev/TradeStation versions).
  - **c) Minimize implementation complexity.**
  - **d) Be minimally invasive: interaction is constrained to the TSDev.exe process directly — no other binaries or processes shall be touched.** No injection into, instrumentation of, or manipulation of TS Desktop, orlib/other DLLs, or any process or binary other than TSDev.exe.

## Users

Traders, not developers. The tool is part of their automated trading-system development pipeline. Primary callers: Power Automate Desktop (PAD) flows and Python; some users script in other languages via the CLI. The CLI (for PAD etc.) and the Python package (proper, robust, ready to use) both have first-class support.

## Requirements (user interview, 2026-09-03)

- **R1 — Users/callers:** Traders; PAD flows and Python are the primary callers, other scripting languages via the CLI. CLI and Python package both first-class.
- **R2 — Operations:** Seven: Insert, Delete, Modify, Exists, Get source, Upsert, List (all items or per component type). Verify is part of Insert/Modify, not a standalone operation.
- **R3 — CLI contract:** Human-readable output by default; `--json` on every operation for stable machine-readable output; documented, stable exit codes for success/failure branching.
- **R4 — Batch + progress:** All seven operations batchable from a manifest file AND via stdin (no temporary manifest). Progress reporting for long-running batches, including a callback/notification mechanism in the Python package, so users can tell the tool is working rather than frozen.
- **R5 — TSDev lifecycle:** Tool-managed with reuse: commands work whether or not TSDev is running; the tool starts a hidden instance, reuses it across commands, and cleans it up. An **attach-only** flag (CLI and Python) disables auto-start: the tool then errors and terminates if TSDev is not already running.
- **R6 — Coexistence:** Full coexistence: the tool must work safely while TradeStation Desktop is open (even with strategies running) and/or the user's own TSDev window is open — no disturbance, no corruption, ever.
- **R7 — Visibility:** While running an operation, never change TSDev window visibility (the user may have brought it to foreground to watch, or minimized it intentionally). When attaching or launching, hide by default. A `nohide` flag (CLI and Python) disables hide-on-launch/attach.
- **R8 — Elevation:** Required. TS 9.5 processes (including TS Desktop and TSDev) can only run as elevated processes; the tool cannot access them unelevated.
- **R9 — Priorities:** Correctness is primary, speed secondary (once correctness is guaranteed, optimize for speed as much as possible by any means available); polished/optimized UX is tertiary.
- **R10 — Single-op failure contract:** Every failure (compile error, TSDev error dialog, crash, hang/timeout) produces a clear, machine-usable error with a distinct exit code; the library ends in a consistent, documented state — either fully applied, or not applied with any partial artifact removed or clearly reported. Timeouts are enforced and configurable; a timeout value ≤ 0 disables timeouts entirely.
- **R11 — Batch error policy:** Abort on first failure is the default; an optional flag (CLI and Python) continues past per-item failures. Per-item results plus a summary are always reported; the exit code reflects the overall outcome.
- **R12 — Compatibility:** Clean break. No shims for the old CLI syntax, manifest format, or Python API; existing callers migrate once.
- **R13 — Type coverage:** Five types required now (Function, Indicator, ShowMe, Strategy, TradingApp); the design must not preclude adding ActivityBar, PaintBar, and ProbabilityMap later.
- **R14 — Distribution:** Zip bundle: downloadable bundle containing the CLI executable plus the Python package as a wheel (`pip install` from the extracted folder); no public infrastructure.
- **R15 — Concurrent invocations:** Unsupported: while an operation is running, other invocations fail immediately with a distinct "busy" exit code.

## Operations (seven)

- **Insert:** given EasyLanguage and a name, insert the item, compile, and verify; fail loudly on compilation failure; fail cleanly if the name already exists (proactively check first; never duplicate or corrupt).
- **Delete:** given a type and name, delete the item; notify if it does not exist.
- **Modify:** replace an existing item's source and recompile/reverify; notify if absent; fail loudly on compilation failure.
- **Exists:** given type and name, return whether it exists, its verbatim stored name, and its ProgId when found. Search is case-sensitive by default with a case-insensitive option. Never use source comments for the stored name.
- **Get source:** given type and name, emit raw EasyLanguage to stdout or `--output <file_path>`; decrypt internal encrypted items automatically, as TSDev does.
- **Upsert:** given type, name, and a source file, modify if present or insert otherwise.
- **List:** all items or per component type; verbatim stored names (plus ProgId when available).

## Acceptance criteria

Production acceptance requires all seven operations (R2) to work reliably and consistently through the CLI and the Python package, including batch operations from both a manifest path and stdin (R4). Type coverage per R13.

## Mandatory rules

- **Communicate objectively:** do not flatter, affirm, or tell the user they are right; state facts and answers directly.
- **Never derive metadata from mutable item content.** EasyLanguage source, comments, and naming conventions are arbitrary user-controlled data. Never parse source text for names, types, or other metadata (including assumed `{ name }` comments). Stored names, ProgIds, and types must come from TSDev's own records (its enumeration/APIs).
- **Emulate TSDev.exe:** trigger actions at the highest level possible, exactly as TSDev.exe would when its controls are operated; avoid digging into lower levels unless it is certain beyond any doubt that no higher-level access point exists. Do not invent workarounds where the application has a proper mechanism. The purpose is simplicity and robustness: few, simple access points with high reliability, which facilitate easy porting to future TradeStation/TSDev updates. This rule directly serves purpose items (a)–(c); item (d) is absolute: never touch any process or binary other than TSDev.exe.
- **No UI automation, no clipboard:** never synthesize clicks/keys on UI controls; set editor text content directly; avoid the clipboard.
- **No hacks:** never suppress compiler errors/warnings or mask real problems. Fix the root cause or use a clean production solution. If no clean form exists, use an explicit/raw form with an explanatory comment.
- **Elevation:** every tool invocation (CLI or Python driving it) requires an elevated administrator process (R8). **Testing UAC:** to test auto-elevation, temporarily disable UAC prompts before running the test from an unelevated shell; never make the user manually confirm UAC prompts during testing.
- **Reverse engineering (minimized — purpose (a)):** RE is a means to find the highest-level access points in TSDev.exe, not an end; keep it to the minimum needed and prefer observable behavior over internals (purpose (b)). Scope: **TSDev.exe only** (purpose (d)) — never attach to, inject into, or analyze other processes or binaries. Use the working Ghidra MCP server (`ghidra-mcp`) for static analysis. Use Frida directly from the CLI: `frida-trace`, `frida -l script.js`, or `frida -p PID`. Frida 17.17.0 and 32-bit spawn/attach plus its full JS API are available. If necessary, use Frida from ad-hoc Python scripts.
- **Check for and HANDLE TSDev/TS error and crash dialogs after every action.** After any action that touches TSDev.exe or TradeStation (and after running any project executable that does), always verify the result by checking for popup windows: error messages, warning dialogs, and crash reports (e.g. `TSCrashReport.exe`). A popup or crash report means the expected result was NOT achieved. When a popup IS found: (1) READ its message text, (2) dismiss it, (3) CORRECT COURSE — do not get stuck or retry blindly. Record the popup message and the corrective action in the trace/log. Before inserting any item, proactively check whether the name already exists (via `exists` or `list`) to avoid name-collision popups in the first place.

## Stack and process

- C++ with the available Visual Studio Build Tools for the CLI; Python (stdlib-first mindset) for the wrapper package. Prefer the smallest, least-complex implementation that reproduces TSDev behavior.
- Work iteratively: one minimal Thin Vertical Slice (TVS) at a time; determine the current state and best next slice, update relevant docs, and use TDD (red-green).
- TVSs are *minimal*: if a TVS can be reduced to a smaller scope while remaining an end-to-end verifiable TVS, split it.
- *Iterate*: identify and execute the next TVS towards the overarching objective(s). Then stop and hand off. *Do not* plan subsequent work; the next team will find their next step.
- After every approved TVS, run a passing real production CLI E2E of insert/modify/delete using the latest code from an elevated direct CLI, against a real TSDev instance. Check for error/crash dialogs; clean up created items. This gate supplements unit/regression tests and is mandatory before the TVS is done.

## PM and subagent rules

Skip the PM rules when operating as a subagent. When acting as PM:

- Delegate any work that can be delegated; do not perform work or read files/URLs that can be handed to a subagent. Perform management only and remain ultimately responsible for correctness and production quality.
- Use the `subagent_general` profile by default for every narrow task (research, MCP, analysis, implementation, tests, or docs). Use `subagent_explore` only for trivial read-only lookups; if insufficient, retry `subagent_general`.
- Maintain `docs/WORKING_MEMORY.md` as your *live, real-time* working memory. Drop information which has become irrelevant, been proven wrong or superseded. Only keep true facts about the current state. This replaces your reliance on internal context, which may be compacted with loss of information at any point.

## Work orchestration

- Decompose complex work into small atomic subtasks; a specialized subagent decomposes complex work, and each subagent handles exactly one narrow concern.
  - *Hard rule*: one subagent handles one *minimal*, single-focus, single-concern subtask.
  - If a subtask can be split, split it and assign multiple subagents.
- Every *substantive* research result, analysis, plan, document, or implementation requires independent adversarial review. The reviewer receives the original task and result, checks 100% completion, scope/drift, and hacks/shortcuts, then APPROVES or DISAPPROVES with findings. A disapproval requires implementer fixes and another review until approval.

## TradeStation component types

TradeStation has eight EasyLanguage types: **ActivityBar, Function, Indicator, PaintBar, ProbabilityMap, ShowMe, Strategy, TradingApp**. Strategies cannot use `Plot`; only Indicators, ShowMe, and PaintBar can plot. Required now: Function, Indicator, ShowMe, Strategy, TradingApp (R13); ActivityBar, PaintBar, and ProbabilityMap stay out of scope but must not be precluded by the design.

## Single source of truth (SSOT)

Counts, listings, and details that could drift live in ONE authoritative location (the SSOT). Every other place references the SSOT instead of restating the count/list/detail, so that when a value changes only the SSOT needs editing.

| Category | Authoritative location |
|---|---|
| Knowledge base with discovered, relevant facts about TSDev.exe (hard evidence and references) | `docs/KNOWLEDGE_BASE.md` |
| Test inventory (list + count) | to be re-established with the new test infrastructure; keep the SSOT pattern (one authoritative list, referenced everywhere else) |

## Relevant paths

- Default TradeStation installation root: `C:\Program Files (x86)\TradeStation 9.5`
- Default TSDev executable: `C:\Program Files (x86)\TradeStation 9.5\Program\TSDev.exe`
- Default compiled/encrypted TSDev Library: `C:\Program Files (x86)\TradeStation 9.5\MyWork\Bin`
- The tool must locate the TradeStation/TSDev installation automatically and provide a manual override flag (e.g. `--ts-path`).
- PM working memory: `docs/WORKING_MEMORY.md`; knowledge base: `docs/KNOWLEDGE_BASE.md`.

## CI and testing baseline

- CI must run without TradeStation installed: build plus non-TS unit tests plus mocked Python tests on pushes/PRs; release bundles attached to version tags (R14).
- Test infrastructure must include both non-TS paths (unit tests that never touch TradeStation) and TS-dependent E2E paths (elevated, real TSDev). The concrete runners and their entry points are re-established on this branch; keep PowerShell 5.1 and `pwsh` 7 compatibility for any test scripts.

## Current state and TODO

The PM keeps this list current and identifies the next step toward 100% acceptance; update it as project state changes.

- Requirements interview complete (2026-09-03); this file is the requirements SSOT for the new branch.
- Next: implementing teams shape and slice the work (TVSs) against these requirements.
- Keep it clean, minimal and relevant: no history/records that are not critically relevant; do not plan more than one TVS ahead; never document what the code documents.

## Standing notes

- This machine uses PowerShell: do not use bash heredocs for `git commit -m` or other commands. Write a message to a temp file (for example, `tmp\commit_msg.txt`) and use `git commit -F tmp\commit_msg.txt`; use `;` rather than `&&`, and `&` for executable paths containing spaces.
