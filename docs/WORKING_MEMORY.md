# Working Memory — TSDev Automation

Live, real-time state for the PM. Keep minimal; drop stale/superseded facts.
Do not restate what the code itself documents.

## Current state (not derivable from code)

- **Quiet-output TVS done — committed + released as v0.1.3.** New batch
  console contract: human mode = per-item `BATCH SUCCESS/FAILED` completion
  line only (no `BATCH START`; FAILED followed by the item's `humanStderr`
  detail block) + `BATCH COMPLETE`; stdout JSON array ONLY under `--json`;
  `--json` keeps full NDJSON incl. `batch_item_started` (client's
  `_run_batch_controlled` requires the pairing). `--pbar`: bar-only while
  items succeed, `BATCH COMPLETE` only when not clean, single mutation-op
  `SUCCESS:` stdout suppressed (query ops keep answers), lib log lines
  routed above the bar via new `tsdev_log_set_sink` hook. Elevated gate
  36/37 green (1 fail = gate-script cleanup double-delete, items verified
  gone). Adversarially reviewed (2 rounds: code + tests/docs).
- **`--pbar` shipped earlier:** global position-free boolean,
  stderr-only in-place `\r` bar (WriteConsoleW on console / UTF-8 on pipe),
  0->N batch + 0->1 single ops, `— N failed` suffix, `--json`/`serve` =
  usage error. E2E suite 16 (`tests/test_pbar.ps1`, `-SkipPbar`, now
  PB1–PB10) wired into run_regression.ps1.
- **Console-path verification recipe** (for future UI work): PowerShell
  `Add-Type` C# ConPTY harness — CreatePipe pair + CreatePseudoConsole +
  STARTUPINFOEX with `PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE = 0x00020016`
  (attribute NUMBER=22 | THREAD flag 0x00020000; the INPUT flag is
  0x00040000 and does NOT work here); `CreateProcessW` needs
  `CharSet.Unicode` + `lpApplicationName` set; ConPTY holds the output
  pipe open — WaitForSingleObject child exit THEN ClosePseudoConsole,
  then drain. Script was throwaway `tmp\conpty_run.ps1` (deleted).
- The requested batch-progress observability and
  cooperative cancellation work (C++ direct CLI, serve broker, Python
  client) is implemented and verified: the final full regression is green
  and there is no pending acceptance blocker.
- **Both former probe items are GONE from the warehouse** (Function
  `TsDeskFnProbe_20260829`, Indicator `TsDeskFnProbeCaller_20260829`).
  Do not rely on or reuse them; E2E suites create their own disposable
  fixtures.
- TradeStation 9.5 installed (`C:\Program Files (x86)\TradeStation 9.5`);
  CLI works against the warehouse WITHOUT TS Desktop running.
- **Current shell IS elevated** — verified this session via
  `net session`; direct `tsdev_cli` runs work without
  `Start-Process -Verb RunAs`. From a non-elevated shell each elevated
  launch still costs ONE interactive UAC prompt
  (ConsentPromptBehaviorAdmin=5); batch elevated steps into one launcher
  script.
- PS 5.1 gotchas hit live: `native.exe 1>file` writes UTF-16LE (BOM FF FE)
  — decode with `ReadAllText` (BOM auto-detect), never explicit UTF-8;
  `Mandatory` string params reject empty strings unless
  `[AllowEmptyString()]`; debug traces embed addresses
  (`refreshCache: ENTRY 0x5E=<addr>`) → never byte-compare stderr across
  runs; `exists` on a MISSING item triggers second-pass `refreshCache`.
- **Ambient PSModulePath is misordered on this machine**: pwsh-7 module
  dirs precede `System32\WindowsPowerShell\v1.0\Modules`, so unguarded
  PS5.1 child launches can resolve Core-only pwsh-7 modules and lose
  cmdlets like `Get-FileHash`.
- Launching the requireAdministrator build of `tsdev_cli.exe` from a
  non-elevated, non-interactive launcher pops an invisible
  secure-desktop UAC prompt and hangs the launcher; do not spawn it
  that way from automation. Non-elevated CLI behavior is exercised via
  the supported `TSDEV_REQUIRE_ADMIN=OFF` asInvoker build (exit 11),
  not a runtime shim.

## Next TVS

None committed. Deferred candidates (require C++ serve schema changes; do
only if explicitly requested): per-request `verbose` in serve requests;
response-schema field exposing broker/CLI log output.

## Key references

- Recalc / cross-process notification mechanism (SSOT):
  `docs/RE/RE_OrlibRegisterNotificationHandle.md`.
- Read-only function root cause: KB §11. Function-modify recalc + matrix: KB §13.
- TSDev UI save path vs CLI ordinal diff: KB §12. Dependents (0x3B empty, paused): KB §9.
