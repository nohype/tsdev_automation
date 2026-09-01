# Working Memory — TSDev Automation

Live, real-time state for the PM. Keep minimal; drop stale/superseded facts.
Do not restate what the code itself documents.

## Current objective

**TVS DONE (2026-09-01): CLI log levels + repeatable `-v/--verbose`.**
4 levels (Error < Warning < Info < Debug), default Warning, `-v`→Info,
`-vv`→Debug (clamped), position-free global flag, runtime-gated in tsdev_lib
(`TSDEV_DEBUG` compile define removed). Results/JSON never gated; stdout
byte-pure. Verified: build + `test_log_levels` + `-NoTs` regression ALL
PASSED; adversarial review APPROVED (initial + delta); elevated E2E gate
40/40 (insert prog_id 55 / modify preserved / delete / cleanup + all level
semantics, no crash reports); Suite 15 live 7/7. Uncommitted in working
tree at hand-off (commit may have followed).

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

## Next TVS (candidate, not committed)

Python wrapper `-v/--verbose` forwarding (`_base_cmd` +
`_build_serve_parameters` in src/tsdev_automation/client.py) so TsDevClient
can request verbosity; broker stderr is invisible (ShellExecuteExW, SW_HIDE)
— broker verbosity only observable via manual launch or a future
response-schema field.

## Key references

- Recalc / cross-process notification mechanism (SSOT):
  `docs/RE/RE_OrlibRegisterNotificationHandle.md`.
- Read-only function root cause: KB §11. Function-modify recalc + matrix: KB §13.
- TSDev UI save path vs CLI ordinal diff: KB §12. Dependents (0x3B empty, paused): KB §9.
