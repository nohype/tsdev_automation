# Working Memory — TSDev Automation

Live, real-time state for the PM. Keep minimal; drop stale/superseded facts.
Do not restate what the code itself documents.

## Current objective

**First release.** All five hard requirements are MET and user-verified
(2026-08-31):

- Six operations (Insert/Delete/Modify/Exists/Get-source/Upsert) via C++ CLI
  and Python `TsDevClient` with batch (manifest + stdin) support.
- Five component types (Function/Indicator/ShowMe/Strategy/TradingApp).
- TS Desktop interaction (objectives a-d): user-verified 2026-08-31 —
  insertion, modification (both item-code and function-only), and deletion
  of all 5 types tested while items open/applied in TS Desktop.
- Python wrapper parity with CLI.
- **Bundle packaging green (2026-08-31):** `docs/BUNDLE_README.md` created
  (user-facing quick-start, 89 lines, adversarially reviewed/approved) and
  `tools/create_bundle_zip.ps1` fixed to auto-create `artifacts/` before
  `Compress-Archive`. `cmake --build build --config Release` now exits 0 and
  produces `dist/` (tsdev_cli.exe + tsdev_automation/ + README.md) and a
  timestamped `artifacts/tsdev_automation_*.zip`. CI build is unblocked.

No release blockers remain. The `dependents` workstream (M) remains paused;
it is beyond the release scope.

## Current state (not derivable from code)

- TradeStation 9.5 installed: `C:\Program Files (x86)\TradeStation 9.5\Program\
  TSDev.exe` and `orlib20.dll` (v9.50.01.3344). TS Desktop process is
  `orchart.exe` (imports `_OrlibRegisterNotificationHandle@4`).
- `tsdev_cli.exe` built (32-bit Win32 Release) at `build\Release\tsdev_cli.exe`
  and `dist\tsdev_cli.exe`. `cmake --build build --config Release` exits 0 and
  assembles `dist/` + a timestamped `artifacts/tsdev_automation_*.zip`.
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

None planned. The release blocker (bundle packaging) is resolved and the
build is green. The next team should identify the next step toward 100%
acceptance or release cut. Candidate areas (not committed): the paused
`dependents` workstream (M), or final release-cut verification (CI run on
push, bundle smoke test). Do NOT plan beyond one TVS.

## Key references

- Recalc / cross-process notification mechanism (SSOT, current):
  `docs/RE/RE_OrlibRegisterNotificationHandle.md` — shared `SHARE_WO` 32-slot
  HWND array, `_OrlibRegisterNotificationHandle@4`, `Ordinal_57` broadcast.
- Read-only function root cause (CONFIRMED): KB §11.
- Function-modify recalc mechanism (confirmed) + recalc matrix: KB §13.
- TSDev UI save path vs CLI ordinal diff: KB §12.
- Dependents/callers (0x3B empty result, paused workstream): KB §9.
