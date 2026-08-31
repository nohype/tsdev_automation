# Working Memory — TSDev Automation

Live, real-time state for the PM. Keep minimal; drop stale/superseded facts.
Do not restate what the code itself documents.

## Current objective

**TS Desktop interaction acceptance.** The six operations (Insert/Delete/Modify/
Exists/Get-source/Upsert) are implemented in C++ CLI + Python `TsDevClient` with
batch (manifest + stdin) support. Production acceptance REQUIRES proper
interaction with a running TS Desktop, matching TSDev.exe:

- (a) all operations work WHILE TS Desktop is open
- (b) inserted items immediately available, usable, and FUNCTION when applied
- (c) modifications immediately picked up and applied to items already on
  charts, or already opened within TS Desktop as a tradingapp
- (d) deletions handled like TSDev: TS Desktop detects applied items, removes
  them, shows a "deleted" warning

The `dependents` workstream (M) is paused; it is beyond this acceptance target.
The Python `TsDevClient` parity axis (Phase 3) is COMPLETE. The next team
determines the next axis toward objectives a–d.

## Current state (not derivable from code)

- TradeStation 9.5 installed: `C:\Program Files (x86)\TradeStation 9.5\Program\
  TSDev.exe` and `orlib20.dll` (v9.50.01.3344). TS Desktop process is
  `orchart.exe` (imports `_OrlibRegisterNotificationHandle@4`).
- `tsdev_cli.exe` built (32-bit Win32 Release) at `build\Release\tsdev_cli.exe`
  and `dist\tsdev_cli.exe`. NOTE: `cmake --build` exit code is non-zero due to a
  POST_BUILD packaging step missing `docs/BUNDLE_README.md`; the exe itself
  builds and runs fine. Repo-content issue, not toolchain.
- CLI works against the warehouse WITHOUT TS Desktop running (confirmed
  2026-08-30: `exists`/`insert`/`modify`/`delete` succeed with only
  `TradeStationAgentForms.exe` present). TS Desktop (`orchart.exe`) is only
  needed for recalc/chart-interaction verification.

**Probe items currently live:**

- Function `TsDeskFnProbe_20260829` — ProgId 84, `elmodule_1_84.ell`
  (currently v1, returns 1). Reusable for TradingApp TEST 2.
- Indicator `TsDeskFnProbeCaller_20260829` — ProgId 3008,
  `elmodule_2_3008.ell`. Applied to a chart in TS Desktop (user-confirmed).

## Next TVS

The Python `TsDevClient` parity axis (Phase 3) is COMPLETE. The next team
determines the next axis toward full TS Desktop interaction acceptance
(objectives a–d above).

Do NOT plan beyond the next TVS; the next team will find their next step.

## Key references

- Recalc / cross-process notification mechanism (SSOT, current):
  `docs/RE/RE_OrlibRegisterNotificationHandle.md` — shared `SHARE_WO` 32-slot
  HWND array, `_OrlibRegisterNotificationHandle@4`, `Ordinal_57` broadcast.
- Read-only function root cause (CONFIRMED): KB §11.
- Function-modify recalc mechanism (confirmed) + recalc matrix: KB §13.
- TSDev UI save path vs CLI ordinal diff: KB §12.
- Dependents/callers (0x3B empty result, paused workstream): KB §9.
