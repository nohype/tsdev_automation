# PM Working Memory — TSDev Automation

> **READ THIS FILE FIRST at the start of every turn / after any context compaction.**
> This is the PM's outsourced memory. Keep it updated; re-read it regularly.

## HARD RULES (never violate)
1. **Stay in the PM role.** Do not do work that can be delegated to a subagent. Do not read files/URLs that can be passed to a subagent. Only perform management tasks.
2. **Subagent profile selection:**
   - `subagent_explore` = trivial lookups ONLY (read-only: grep, glob, read, web_search). Very limited.
   - `subagent_general` = research, analysis, MCP-tool tasks (Ghidra), non-trivial work. **Use this for anything beyond a trivial verbatim quote.**
   - If an `explore` subagent fails or produces unsatisfactory output, retry with `general`.
3. **Working memory:** Keep THIS file updated. Re-read it every turn.
4. **Adversarial review:** Every substantive result must be independently reviewed (APPROVE/DISAPPROVE loop) before acceptance.
5. **One subagent → one narrowly scoped subtask.** No multi-stage/complex subtasks per subagent.
6. **TDD** (red-green) for implementation TVSs.
7. **TradeStation install paths are READ-ONLY.** Copy files into `tmp/` to work on them.
8. **Frida:** use the `frida` CLI directly, NEVER via MCP tools (Frida MCP server removed).
9. **Ghidra:** Ghidra MCP server = `"ghidra"`. Works reliably. Use for static analysis.
10. **Acceptance criteria:** The three functions (INSERT/DELETE/MODIFY) must work reliably and consistently via a CLI. Already satisfied at library + CLI level. Remaining work = quality/robustness/feature gaps.

## Project state (as of 2026-08-17)
- **DONE:** TVS-1 (feasibility), TVS-2.1 (headless init), TVS-2.2 (param orders + persistence), TVS-2.3 (INSERT/DELETE/MODIFY library-level, all APPROVED), CLI (insert/delete/modify subcommands, APPROVED).
- **Primary acceptance criteria SATISFIED** at library + CLI level.
- **NEXT WORKSTREAMS (priority order, NOT started):**
  1. (A) Compilation errors — `docs/HANDOFF_A_COMPILATION_ERRORS.md` (6 TVSs). Get real compiler error text.
  2. (B) Robustness & architecture — `docs/HANDOFF_B_ROBUSTNESS.md` (7 TVSs). Signature-based RVA resolution.
  3. (C) TradingApp component type — `docs/HANDOFF_C_TRADINGAPP.md` (7 TVSs).
  4. (D) Python DLL (64-bit) — `docs/HANDOFF_D_PYTHON_DLL.md` (7 TVSs).

## CURRENT TVS: WORKSTREAM A COMPLETE (TVS-A.4 + TVS-A.6 DONE)
- **TVS-A.4 DONE & APPROVED:** Real compiler error text via direct global array access. Bypassed GetCompilingErrors (std::vector ABI impractical). readErrorsFromArray reads DAT_1023cea0/DAT_1023cea4 directly.
- **TVS-A.6 DONE & APPROVED:** Source location (line/column) in error messages. Error record offset 0x0A = char offset into source. readErrorsFromArray counts newlines to compute line/col. Example: "Line 2, Col 1: unknown identifier".
- **TVS-A.2 CORRECTED:** Original ABI conclusion (std::string return-by-value) was WRONG. Actual ABI is std::vector<ErrorOutputRecord>*. Findings doc updated.
- **Workstream A is COMPLETE.** All TVSs resolved. Next: Workstream B (robustness), C (TradingApp), or D (Python DLL).

## Key RVAs for direct error array access (in production code now)
- orlib20.dll base: m_orlib (HMODULE)
- DAT_1023cea0 (error array pointer): RVA 0x23cea0
- DAT_1023cea4 (error count): RVA 0x23cea4
- DAT_1023aab8 (HINSTANCE for string table): RVA 0x23aab8
- Error record stride: 0x129 = 297 bytes
- Error text offset: 0x29 (null-terminated char string)
- Char offset (for line/col): 0x0A (int, character offset into source)
- Resource ID offset: 0x12 (short, for LoadStringA fallback)

## Ghidra MCP notes (for future PM reference)
- Ghidra MCP server = `"ghidra"`. Project `tsdev.exe`, current program `orlib20.dll` (base 0x10000000).
- Subagent profiles do NOT have MCP tool access. PM must perform Ghidra MCP calls directly, save evidence to files, then delegate analysis/writeup to `subagent_general`.
- Key tool param quirks: `disassemble_function` needs `address` (not `function`); `get_function_callers` needs `function` (not `function_name`); `get_function_xrefs` needs `name`; `search_strings` needs `search_term`; `search_byte_patterns` does NOT filter by address range (returns program-wide matches); `search_instructions` `operand_pattern` (not `operand_substring`) but also doesn't filter by range.
- `decompile_function` accepts `functions` (comma-separated names/addresses). Works well.

## Decisions log
- 2026-08-17: Chose Workstream A as next highest-ROI (completes "fail loudly" criterion).
- 2026-08-17: Chose TVS-A.2 (static/Ghidra) over TVS-A.1 (Frida/GUI) as entry point — A.1 needs manual GUI, A.2 is autonomous.
- 2026-08-17: PM performs Ghidra MCP calls directly (subagents lack MCP access); delegates analysis/writeup to `subagent_general`.
- 2026-08-17: TVS-A.2 APPROVED. Added FUN_10156850 call sites (28 xrefs) after adversarial review flagged gap.
- 2026-08-17: Next TVS = A.4 (ABI fix, caller-side only). A.1/A.3 can be skipped/bypassed by A.4.

## CRITICAL REMINDER — Atomic subtask decomposition (VIOLATED ONCE, NEVER AGAIN)
- **One subagent → one narrowly scoped subtask.** NO multi-stage, multi-concern, or complex tasks for any individual subagent.
- TDD red-green is NOT one task. It is: (1) write failing test [edit], (2) build+run to confirm RED [verify], (3) implement fix [edit], (4) build+run to confirm GREEN [verify], (5) run regression [verify]. Each is a SEPARATE subagent.
- Build+run of ONE test = one task (acceptable granularity). Running 3 regression tests = one task. Editing one file = one task. Editing 2 files for one coherent logical change = one task (acceptable). But NEVER combine edit + build + run + regression in one subagent.
- If a subagent task description has "Step 1... Step 2... Step 3..." it is TOO BIG. Split it.

## Reminders to self
- Re-read this file every turn / after context compaction.
- Do NOT accept sub-par quality from subagents. Production-grade only.
- Adversarial review is mandatory for every substantive result.
- Update AGENTS.md TODO section after each TVS completes.
- Use `subagent_general` for non-trivial work (NOT `subagent_explore`).
- **ATOMIC SUBTASKS ONLY.** One subagent = one narrowly focused task. No multi-stage/complex tasks.
