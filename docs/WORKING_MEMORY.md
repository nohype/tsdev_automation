# Working Memory — TSDev Automation

Live, real-time state for the PM. Keep minimal; drop stale/superseded facts.
Do not restate what the code itself documents.

## Current objective

**TS Desktop interaction acceptance.** The six operations (Insert/Delete/Modify/
Exists/Get-source/Upsert) are implemented in C++ CLI + Python `TsDevClient` with
batch (manifest + stdin) support. The user has clarified that production
acceptance REQUIRES proper interaction with a running TS Desktop, matching
TSDev.exe:
- (a) all operations work WHILE TS Desktop is open
- (b) inserted items immediately available, usable, and FUNCTION when applied
- (c) modifications immediately picked up and applied to items already on
  charts, or already opened within TS Desktop as a tradingapp
- (d) deletions handled like TSDev: TS Desktop detects applied items, removes
  them, shows a "deleted" warning

The `dependents` workstream (M) is paused; it is beyond this acceptance target.

## Current state (not derivable from code)

- TradeStation 9.5 installed: `C:\Program Files (x86)\TradeStation 9.5\Program\
  TSDev.exe` and `orlib20.dll` (v9.50.01.3344). TS Desktop process is
  `orchart.exe` (imports `_OrlibRegisterNotificationHandle@4`).
- `tsdev_cli.exe` built (32-bit Win32 Release) at `build\Release\tsdev_cli.exe`
  and `dist\tsdev_cli.exe`. NOTE: `cmake --build` exit code is non-zero due to a
  POST_BUILD packaging step missing `docs/BUNDLE_README.md`; the exe itself
  builds and runs fine. Repo-content issue, not toolchain.
- **Function-modify recalc gap is CLOSED (2026-08-30).** The CLI's
  `verifyModule` post-success path now dispatches orlib20 worker cmd 0x0D
  (FindProgram) for categories 0/1/4, replicating TSDev's `FUN_004d75b0`. This
  sets `warehouse+0x12=category`, `warehouse+0x16=progId`, then dispatches via
  `callCmdDispatcher(m_pCmdDispatcher, warehouse, 0x0D)`. Combined with the
  existing `Ordinal_57` (0x40A cross-process notification), the applied caller
  indicator auto-recalculates immediately after a CLI `modify function`,
  matching TSDev.exe behavior. User-confirmed: chart updated from 2→1
  immediately after CLI modify, no manual refresh needed.
- The cross-process recalc mechanism is: worker cmd 0x0D (FindProgram COM call
  to WHServer.exe) fires `_IWHouseEvents` COM events → orchart receives and
  recalculates. 0x40A delivery alone was insufficient (confirmed 2026-08-30);
  cmd 0x0D is the necessary additional trigger. See KB §22.8.

**Probe items currently live (category 1):**
- Function `TsDeskFnProbe_20260829` — ProgId 84, `elmodule_1_84.ell`
  (currently v1, returns 1)
- Indicator `TsDeskFnProbeCaller_20260829` — ProgId 3008, `elmodule_2_3008.ell`
- Caller indicator is applied to a chart in TS Desktop (user-confirmed).

## TVS just completed (2026-08-30): Worker cmd 0x0D dispatch in verifyModule post-success path

Added worker cmd 0x0D (FindProgram) dispatch to `TsDevLib::verifyModule()` in
`src/tsdev_lib.cpp`, inside the existing category-gated (0/1/4) post-success
block, after `linkModule` and before `traverseReverseDependents`. The dispatch
replicates TSDev's `FUN_004d75b0`: checks `warehouse+0xAE != 0`, sets
`warehouse+0x12 = category` and `warehouse+0x16 = progId`, then calls
`callCmdDispatcher(m_pCmdDispatcher, warehouse, 0x0D)`. Non-fatal on all
failure modes (NULL warehouse, NULL +0xAE, SEH). Purely additive — no existing
code modified.

- Build: success (exe rebuilt, POST_BUILD packaging non-zero exit is known
  repo-content issue).
- Unit tests: 17/17 non-TS pass, 2/2 suites pass.
- Adversarial review: APPROVED (TSDev parity verified, no regressions, no
  hacks, correct ordering before EndCompilingProcess).
- E2E gate: CLI `modify function TsDeskFnProbe_20260829` v2→v1 from elevated
  direct CLI, exit 0, cmd 0x0D dispatch confirmed in debug log
  (`warehouse+0x5E` = valid IProgram). No crashes (no TSCrashReport, 16 orchart
  processes stable).
- **User-confirmed: applied caller indicator auto-recalculated immediately
  (2→1) without manual refresh.** Acceptance criterion (c) for function-modify
  is met.

## Next step (one TVS ahead)

**RE TVS: confirm whether TSDev.exe triggers worker cmd 0x0D for categories
other than 0/1/4 (i.e. Indicator cat 2, ShowMe cat 3, TradingApp cat 6).**
Static analysis (KB §21.2/§21.7) indicates the category gate in
`FUN_004D91E0` is `uVar2 ∈ {0, 1, 4}` (Strategy, Function subtype, Function),
which would SKIP the post-verify cmd 0x0D block for Indicator/ShowMe/TradingApp.
This needs runtime confirmation via Frida: trace a TSDev.exe UI modify of an
Indicator (and/or ShowMe/TradingApp) applied to a chart, hooking the worker
cmd 0x0D dispatch path (`FUN_004d75b0` / `FUN_101cf5b0` / `callCmdDispatcher`
equivalent) and Ordinal_57, to determine:
1. Does TSDev dispatch cmd 0x0D for cat 2/3/6? (Static says NO — confirm at
   runtime.)
2. If NOT, does the existing Ordinal_57 (0x40A) notification alone trigger
   immediate chart recalc for these types? (The modified item IS the applied
   item for non-function types, so 0x40A may be sufficient — KB §21.7.)
3. If 0x40A alone is insufficient for any non-function type, what additional
   call(s) does TSDev make that the CLI is missing?

Emulate TSDev as closely as possible: if TSDev skips cmd 0x0D for cat 2/3/6,
the CLI should too (it already does — the category gate matches). If TSDev
makes other calls for these types, replicate them. The goal is exact parity,
not just "it works."

Reuse the existing Frida trace infrastructure (`tmp/frida_recalc_trace.js`
and related scripts) adapted for the non-function-type probe.

**After the runtime confirmation, run a full E2E test on each category to
verify our project's actual behavior** (this is what matters for users):
For each supported type (Function cat 1, Indicator cat 2, ShowMe cat 3,
Strategy cat 0, TradingApp cat 6), test both scenarios via the CLI:
1. **Modify an item that is applied directly to a chart** (the modified item
   IS the applied item — e.g. modify an Indicator that is on a chart). Verify
   the chart recalculates immediately.
2. **Modify an item that is a dependency of an applied item** (the modified
   item is called by something on a chart — e.g. modify a Function that an
   Indicator on a chart calls; or modify a Function that a Strategy on a
   chart calls). Verify the applied caller recalculates immediately.

The function-modify→indicator-caller case is already confirmed (this TVS).
The remaining cases need probe items created per type and applied to charts.
Compare CLI behavior against TSDev.exe UI behavior for the same modifications.
If any CLI modify does NOT trigger immediate recalc where TSDev does, that is
a gap to fix in a follow-up implementation TVS.

## Key references

- Function-modify recalc signal (static + runtime findings, SSOT):
  `docs/KNOWLEDGE_BASE.md` §21 (static analysis), §22 (runtime confirmation),
  §22.8 (cmd 0x0D dispatch confirmation).
- Read-only function root cause (CONFIRMED): KB §20.
- CLI modify path: `src/tsdev_lib.cpp` `verifyModule` (post-success cmd 0x0D
  dispatch + NotifyLibChanged block) and `modifyEntry`;
  `traverseReverseDependents` (local re-verify, runs AFTER cmd 0x0D dispatch).
- orlib20 ordinal resolvers: `src/orlib_resolver.cpp` (CLI resolves
  28/57/66/67/69/70/85; does NOT resolve 10/17/25/56/68/71 — not needed for
  the cmd 0x0D dispatch which uses the generic dispatcher FUN_101cf5b0).
- Cross-process notification mechanism (SSOT, current):
  `docs/RE/RE_OrlibRegisterNotificationHandle.md` — shared `SHARE_WO` 32-slot
  HWND array, `_OrlibRegisterNotificationHandle@4`, `Ordinal_57` broadcast.
  (KB §19/§22 corrected for the 2026-08-30 runtime verification.) Dependents/callers: KB §18.
- Probe sources: `tmp/probe_modify_function_v1.el` (=`1`),
  `tmp/probe_modify_function_v2.el` (=`2`),
  `tmp/probe_modify_function_caller.el`.
