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
11. **End-to-end CLI verification after every TVS:** After any TVS implementation is successfully completed and approved, run an end-to-end test with the real production CLI (`tsdev_cli insert/delete/modify`) using the latest codebase state. Ensures the primary acceptance criteria artifact remains operational. Must PASS before the TVS is considered fully done. This is MANDATORY and in addition to unit/regression tests.

## Project state (as of 2026-08-18)
- **ALL WORKSTREAMS COMPLETE:** A (compilation errors), B (robustness), C (TradingApp), E (architecture redesign). Primary acceptance criteria SATISFIED at library + CLI level.
- **CURRENT WORKSTREAM: NONE IN FLIGHT.** Next: Workstream D (Python Integration) — NOT started.
- **Tech debt cleanup pass (2026-08-18):** Documentation consolidated (AGENTS.md/PM_WORKING_MEMORY.md condensed, historical detail archived to `docs/PROJECT_HISTORY.md`, 4 completed handoffs archived to `docs/archive/`). Code fixes: TD-001 (auto-cleanup partial entry on compile failure), TD-002 (per-patch restoration gap). Stale comments cleaned. Stray files removed.
- **CLI supports 4 component types:** Strategy (0), Indicator (2), Function (4), TradingApp (5122, category 2, sub-type 0x14). 4 unconfirmed: ActivityBar, PaintBar, ProbabilityMap, ShowMe.
- **Regression suite:** `powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1` (sigtool 12 + unit 14 + E2E 34 = 60 tests).
- **Architecture:** `signatures/*.json` (authoritative) → `tools/sigcodegen.py` → `signatures.gen.h` (build-time codegen) → `src/orlib_resolver.cpp`. No runtime JSON dep in production CLI. `src/sigtool.cpp` = standalone signature verification tool.
- **Detailed TVS history:** See `docs/PROJECT_HISTORY.md`.

## NEXT WORKSTREAM
- **(D) Python Integration** — `docs/HANDOFF_D_PYTHON_INTEGRATION.md`. Simple Python wrapper around `tsdev_cli` (msl-loadlib rejected). NOT started. TVS decomposition deferred.

## Completed Work Summary

All workstreams A/B/C/E complete. Detailed per-TVS logs archived in `docs/PROJECT_HISTORY.md`.

| Workstream | TVSs | Result |
|------------|------|--------|
| A (Compilation errors) | A.2, A.4, A.6 | Real compiler error text with source location (Line/Col) |
| B (Robustness) | B.1–B.7, B.6.5, B.6.6 | OrlibResolver (signature-based RVA resolution), version detection, regression suite (60 tests), path discovery |
| C (TradingApp) | C.1–C.7 | TradingApp whType 5122, INSERT/DELETE/MODIFY via CLI |
| E (Architecture) | E.1–E.7 | JSON signature source of truth + build-time codegen, sigtool CLI, probe archival, test relocation, RVA dedup |

## OBSERVATIONS / potential future TVS items (not blocking)
- **SEH protection on error array reads:** `readErrorsFromArray` reads raw memory from orlib20.dll's data section without SEH guards. Low likelihood (array populated by compiler in normal flow). Defer.
- **4 unconfirmed warehouse types:** ActivityBar, PaintBar, ProbabilityMap, ShowMe. Require Ghidra/Frida RE work per type. Feature gap, not debt.
- **Multi-version signature selection untested:** Only one orlib20.dll version exists. Design is conservative (unknown → warning). Inherent limitation.

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
- **Git commits on PowerShell — NO HEREDOC:** PowerShell does NOT support `$(cat <<'EOF' ... EOF)`. Write commit message to `tmp\commit_msg.txt` then `git commit -F tmp\commit_msg.txt`. Also: PowerShell uses `;` not `&&`; use `&` for executables with spaces in path. This is now a Standing Note in AGENTS.md so all agents are aware.
