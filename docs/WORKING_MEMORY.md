# Working Memory — TSDev Automation

Live, real-time state for the PM. Keep minimal; drop stale/superseded facts.
Do not restate what the code itself documents.

## Current state (not derivable from code)

- **No active TVS.** `--pbar` shipped: global position-free boolean,
  stderr-only in-place `\r` bar (WriteConsoleW on console / UTF-8 on pipe),
  0->N batch + 0->1 single ops, `— N failed` suffix, `--json`/`serve` =
  usage error. All slices adversarially approved; E2E suite 16
  (`tests/test_pbar.ps1`, `-SkipPbar`) wired into run_regression.ps1;
  elevated production gate (insert/modify/delete + batch) green incl.
  real-console ConPTY verification. Uncommitted — no commit requested.
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

## Key references

- Recalc / cross-process notification mechanism (SSOT):
  `docs/RE/RE_OrlibRegisterNotificationHandle.md`.
- Read-only function root cause: KB §11. Function-modify recalc + matrix: KB §13.
- TSDev UI save path vs CLI ordinal diff: KB §12. Dependents (0x3B empty, paused): KB §9.
