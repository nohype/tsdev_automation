# TSDev Automation — Bundle Quick-Start

This bundle lets you manage the TradeStation Development Environment (TSDev)
Library without the TSDev UI. It contains:

- **`tsdev_cli.exe`** — a standalone 32-bit C++ CLI that loads `orlib20.dll`
  from your TradeStation install and drives the TSDev Library directly.
- **`tsdev_automation/`** — a stdlib-only Python wrapper package that drives
  `tsdev_cli.exe` as a subprocess from 64-bit Python.

## Known Critical Issue — Historical Workstream L record

> **Current status:** Workstream L is closed; current Workstream M status is
> tracked in the project repository's `AGENTS.md` §"Current state and TODO".
>
> The L.1–L.17 claims, results, and fault records below are retained for provenance.

**Historical L evidence records that strategies inserted/modified/upserted by
`tsdev_cli.exe` / the Python wrapper could appear VERIFIED in TSDev but fail at
runtime in TradeStation Desktop with "Unknown link error occurred."** A full
Desktop restart did not repair a broken CLI Strategy in that historical
observation. Other supported component types used the common
compile/finalization path and were included in the historical Workstream L
validation, but an equivalent runtime failure was not claimed without
type-specific evidence.

The historical workaround was to open the item in TSDev, make a harmless edit,
and re-Verify — this re-ran the original finalization path. **This was a
workaround only and was not an acceptable fix for an automated tool.**

The retained L record describes a confirmed root-cause defect in the CLI's
orlib20 finalization path and the historical L remediation/verification work.
It does not change the current status above or imply that its historical test
counts are current acceptance evidence. For current Workstream M status, see
`AGENTS.md` §"Current state and TODO".

For details, see the project repository's
`docs/workstream_unknown_link_fix_plan.md` (Workstream L plan) and
`docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md` (durable root-cause record).

## Prerequisites

- **TradeStation 9.5 installed.** The CLI loads `orlib20.dll` (and support
  DLLs) from the TradeStation install directory at runtime. Nothing is bundled.
- **Python 3.14+ 64-bit** (only if you use the Python wrapper). The wrapper is
  stdlib-only — no third-party packages required.
- **Administrator privileges for direct CLI use.** Every direct
  `tsdev_cli.exe` subcommand, including `serve`, requires an already elevated
  administrator process; if the CLI reaches its elevation guard without an
  elevated token, it returns exit code **11** (`not_elevated`). The
  `sigtool.exe` file-analysis CLI does not access the live TSDev Library and
  does not have this requirement. The Python wrapper's `TsDevClient(use_serve=True)`
  (the default) starts a separate elevated broker through normal Windows UAC
  consent, so the Python process itself need not be elevated; with
  `use_serve=False`, non-elevated Python raises `ElevationRequiredError` before
  launch. For the full elevation contract (direct-CLI / exit 11 / serve broker),
  see the project repository's `docs/BUILD.md` (operational) and `AGENTS.md`
  (rule).

## CLI usage

```
tsdev_cli [--json] [--ts-path <dir>] <subcommand> ...
```

Examples:

```
tsdev_cli insert       strategy MyStrat my_strategy.el
tsdev_cli modify       strategy MyStrat my_strategy_v2.el
tsdev_cli delete       strategy MyStrat
tsdev_cli exists       strategy MyStrat
tsdev_cli get-source   strategy MyStrat --output MyStrat.el
tsdev_cli upsert       strategy MyStrat my_strategy.el
```

- `--json` — emit structured JSON output (useful for scripting / the wrapper).
- `--ts-path <dir>` — override the TradeStation install directory if it is not
  at the default location (`C:\Program Files (x86)\TradeStation 9.5`) or not
  discoverable via the registry.

`<type>` is one of `strategy`, `function`, `indicator`, `tradingapp`, `showme`
(case-insensitive). Run `tsdev_cli` with no arguments to see the full usage
reference (note: the CLI has no `--help` flag; it prints usage on any
unrecognized invocation).

## Python wrapper usage

Add this bundle directory to `sys.path` (or set `PYTHONPATH` to it), then
import the wrapper:

```python
import sys
sys.path.insert(0, ".")  # or the absolute path to this bundle directory

from tsdev_automation import TsDevClient

client = TsDevClient()  # auto-discovers the sibling tsdev_cli.exe
result = client.insert("strategy", "MyStrat",
                       "Inputs: Length(10); "
                       "If Close > Average(Close, Length) Then Buy next bar at Market;")
print(f"ProgId={result.prog_id}  ell={result.ell_path}")
```

The wrapper **auto-discovers the sibling `tsdev_cli.exe`** in this bundle
directory — no `cli_path` argument is needed. You can still override it
explicitly via `cli_path=` or the `TSDEV_CLI_PATH` environment variable.

Alternatively, set `PYTHONPATH` and run from anywhere:

```powershell
$env:PYTHONPATH = "C:\path\to\this\bundle"
python -c "from tsdev_automation import TsDevClient; c = TsDevClient(); print(c.cli_path)"
```

## Full documentation

For advanced usage, build instructions, and the full API reference, see the
project repository's `README.md` and `docs/BUILD.md`.
