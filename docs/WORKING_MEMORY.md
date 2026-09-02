# Working Memory — TSDev Automation

Live, real-time state for the PM. Keep minimal; drop stale/superseded facts.
Do not restate what the code itself documents.

## Current objective

**TVS DONE (2026-09-01): Python wrapper verbose forwarding.**
`TsDevClient(..., verbose: int = 0)` clamped [0,2]; direct mode forwards
`["-v"]*n` via `_base_cmd` (single choke point); serve mode forwards to the
BROKER LAUNCH line only (`_build_serve_parameters`/`_ServeBroker`/
`_ensure_broker`); `_cli.py` gained `-v/--verbose` (count). 17 new tests;
`uv run pytest -m "not e2e"` 165 passed; `-NoTs` regression 2/2 suites 0
failed; adversarial review APPROVED; elevated E2E gate RUN-GATE: PASS
(direct insert/modify/delete verbose=2: Info+Debug stderr markers, stdout
JSON-pure, prog_id preserved, cleanup verified; serve smoke via `-v -v`
broker; no crash reports). UNCOMMITTED in working tree (3 files + tests).
Deferred by design (only if requested): serve-protocol per-request
verbosity + response-schema log field (both need C++ serve schema changes).

## Current state (not derivable from code)

- **Both former probe items are GONE from the warehouse** (confirmed live
  2026-09-01 via elevated `exists`: Function `TsDeskFnProbe_20260829` and
  Indicator `TsDeskFnProbeCaller_20260829` both `exists=False`). Do not
  rely on them; E2E suites now create their own fixtures.
- TradeStation 9.5 installed (`C:\Program Files (x86)\TradeStation 9.5`);
  CLI works against the warehouse WITHOUT TS Desktop running.
- Session shells are NOT elevated; every direct `tsdev_cli` run needs
  `Start-Process -Verb RunAs` (UAC ConsentPromptBehaviorAdmin=5 → each
  elevated launch costs ONE interactive prompt; batch elevated steps into
  one launcher script). UAC prompts were canceled twice on 2026-09-01
  before being approved on retry.
- PS 5.1 gotchas hit live: `native.exe 1>file` writes UTF-16LE (BOM FF FE)
  — decode with `ReadAllText` (BOM auto-detect), never explicit UTF-8;
  `Mandatory` string params reject empty strings unless
  `[AllowEmptyString()]`; debug traces embed addresses
  (`refreshCache: ENTRY 0x5E=<addr>`) → never byte-compare stderr across
  runs; `exists` on a MISSING item triggers second-pass `refreshCache`.

- Reusable elevated E2E gate assets: `tmp\e2e_verbose_gate\` (gitignored;
  `run_gate.ps1` = one batched launcher, ONE UAC prompt; `result.txt` is
  UTF-16LE — read via `Get-Content`, never as UTF-8).

## Next TVS

None committed. Deferred candidates (require C++ serve schema changes; do
only if explicitly requested): per-request `verbose` in serve requests;
response-schema field exposing broker/CLI log output.

## Key references

- Recalc / cross-process notification mechanism (SSOT):
  `docs/RE/RE_OrlibRegisterNotificationHandle.md`.
- Read-only function root cause: KB §11. Function-modify recalc + matrix: KB §13.
- TSDev UI save path vs CLI ordinal diff: KB §12. Dependents (0x3B empty, paused): KB §9.
