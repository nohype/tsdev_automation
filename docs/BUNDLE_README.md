# TSDev Automation — Bundle Quick-Start

This bundle lets you manage the TradeStation Development Environment (TSDev)
Library without the TSDev UI. It contains:

- **`tsdev_cli.exe`** — a standalone 32-bit C++ CLI that loads `orlib20.dll`
  from your TradeStation install and drives the TSDev Library directly.
- **`tsdev_automation/`** — a stdlib-only Python wrapper package that drives
  `tsdev_cli.exe` as a subprocess from 64-bit Python.

## Prerequisites

- **TradeStation 9.5 installed.** The CLI loads `orlib20.dll` (and support
  DLLs) from the TradeStation install directory at runtime. Nothing is bundled.
- **Python 3.14+ 64-bit** (only if you use the Python wrapper). The wrapper is
  stdlib-only — no third-party packages required.
- **Administrator privileges.** `tsdev_cli.exe` requires an elevated token
  (TradeStation's TSDev Library is only accessible to elevated processes). Run
  your terminal or Python session as Administrator — **one UAC prompt per
  session**, then all CLI invocations are silent (subprocesses inherit the
  elevated token).

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
