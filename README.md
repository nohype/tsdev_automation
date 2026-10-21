# TSDev Automation

Manage your TradeStation EasyLanguage library — insert, update, delete, check, and retrieve items — from the command line or a script, **without opening the TSDev window**.

## Before you start

You need three things:

1. **TradeStation 9.5 installed** on this machine (TradeStation 10 is *not* supported). The tool finds the standard installation at `C:\Program Files (x86)\TradeStation 9.5` by itself. If yours lives elsewhere, add `--ts-path "<install folder>"` to every command. TradeStation itself does **not** need to be running.
2. **The tool.** Download the `tsdev_automation_*.zip` release and unzip it anywhere. Inside you find `tsdev_cli.exe` (the program) and a `tsdev_automation` folder (a Python package — only needed if you script in Python).
3. **An administrator command prompt.** Every `tsdev_cli.exe` command must run from an elevated terminal. To open one: press **Start**, type `cmd` or `powershell`, right-click the result, choose **Run as administrator**. If you forget, the tool just prints that it needs administrator privileges and stops — nothing breaks.

## Getting started

In your administrator terminal, `cd` into the folder where you unzipped the bundle, then run `.\tsdev_cli.exe` (in PowerShell the `.\` prefix is required; in cmd.exe plain `tsdev_cli` works too).

Your EasyLanguage code lives in ordinary text files (`.el` or `.txt`).

**Insert a new item** (adds it and compiles it; if the code does not compile, nothing is added and the compiler errors are printed):

```powershell
.\tsdev_cli.exe insert function MyFunction .\MyFunction.el
```

**Check whether an item exists:**

```powershell
.\tsdev_cli.exe exists function MyFunction
```

prints `exists=1` plus the stored name and ProgId, or `exists=0` when absent. The name must match exactly; add `--case-insensitive` to ignore letter case.

**Get an item's source back out:**

```powershell
.\tsdev_cli.exe get-source function MyFunction --output backup.el
```

Without `--output` the source is printed to the console. Items stored encrypted are decrypted automatically, exactly like TSDev does.

**Update or delete:**

```powershell
.\tsdev_cli.exe modify function MyFunction .\MyFunction.el   # replace source, recompile
.\tsdev_cli.exe upsert function MyFunction .\MyFunction.el   # modify if present, else insert
.\tsdev_cli.exe delete function MyFunction
```

**See which library items use a given item:**

```powershell
.\tsdev_cli.exe dependents function MyFunction
```

## Item types

`<type>` is one of `function`, `indicator`, `showme`, `strategy`, `tradingapp` (letter case does not matter). The TSDev types ActivityBar, PaintBar, and ProbabilityMap are not supported.

## Doing many items at once (batch)

Batch commands take a manifest file — a plain text file with one item per line; lines starting with `#` are comments. Insert/modify/upsert lines are `<type> <name> <source_file>`; delete lines are `<type> <name>`. A name containing spaces must be in double quotes.

```text
# manifest.txt
function MyFuncA .\src\MyFuncA.el
indicator "My Indicator" .\src\MyIndicator.el
strategy MyStrat .\src\MyStrat.el
```

```powershell
.\tsdev_cli.exe batch-insert manifest.txt    # also batch-modify / batch-upsert / batch-delete
```

To process a whole folder of `.el`/`.txt` files at once (file name becomes the item name, all of one type):

```powershell
.\tsdev_cli.exe batch-insert-dir function .\src    # also batch-modify-dir / batch-upsert-dir / batch-delete-dir
```

Each item is reported as it completes; a failed item does not stop the rest.

## Useful flags

- `--json` — print the result as JSON, for scripts.
- `--ts-path "<dir>"` — point at a non-standard TradeStation install.
- `--backup-cache` / `--restore-cache` — back up (or restore) TradeStation's internal cache before the command runs; can also be run on its own: `.\tsdev_cli.exe --backup-cache`.
- `--pbar` — show a progress bar during batch runs.
- `-v` / `-vv` — more detailed logging (repeatable).
- `--case-insensitive` (`exists` only), `--output <file>` (`get-source` only) — see above.

Run `.\tsdev_cli.exe` with no arguments to print the full command list.

## If something goes wrong

The tool is designed to be fail-safe: every operation is checked, and when TradeStation is in a bad state it **refuses, prints the reason, and stops** instead of risking your library. A refusal is safe — unprocessed items simply did not run. Common cases:

| What you see | What it means | What to do |
|---|---|---|
| "requires administrator privileges" | Terminal is not elevated | Re-open it via **Run as administrator** |
| "not found in the warehouse" | Wrong name or type | Check the exact name with `exists` |
| Compile errors printed | The EasyLanguage source has errors | Fix the source and run again |
| "init failed: ..." | TradeStation 9.5 not found | Install TS 9.5, or pass `--ts-path` |
| A run aborts midway with a watchdog or safety message | TradeStation's background process is in a bad state | Close TradeStation/TSDev (restart the PC if it persists), then re-run; already-completed items are fine |

If a TradeStation error or crash window pops up while a command runs, the operation did **not** succeed — read the message, close the popup, close TradeStation/TSDev, and try again.

## Using it from Python

The bundled `tsdev_automation` package wraps the CLI. Python 3.14+, standard library only. `TsDevClient` elevates itself automatically — you get **one UAC prompt** per session (none if Python is already running as administrator), no administrator terminal needed:

```python
from tsdev_automation import TsDevClient

client = TsDevClient()          # auto-elevation on by default

client.insert('function', 'MyFn', '.\\MyFn.el')
r = client.exists('function', 'MyFn')
print(r.exists, r.stored_name)  # True 'MyFn'
src = client.get_source('function', 'MyFn')
client.delete('function', 'MyFn')
```

Methods: `insert`, `modify`, `upsert`, `delete`, `exists`, `get_source`, plus `batch_insert`/`batch_modify`/`batch_upsert`/`batch_delete` (and `*_dir` variants). Full details: `docs/BUNDLE_README.md`.

---

## Advanced / technical

*Everything below this line is for developers and curious users — not needed to use the tool.*

- **Building from source:** C++, Visual Studio Build Tools, CMake — `cmake -S . -B build -A Win32`, then `cmake --build build --config Release`. The build assembles the `dist/` bundle and a timestamped zip in `artifacts/` (`tools/create_bundle_zip.ps1`). CI (`.github/workflows/ci.yml`) builds, tests, and attaches the bundle zip to `v*` releases.
- **How it works:** `tsdev_cli.exe` is 32-bit and drives the same `orlib20.dll`/WHServer machinery `TSDev.exe` uses — no UI automation. Built-in guards watch WHServer's health and abort rather than let a runaway batch damage the library.
- **Deep documentation:** project rules and process in `AGENTS.md`; reverse-engineered facts about TSDev/orlib20 in `docs/KNOWLEDGE_BASE.md`; WHServer internals and recovery design in `docs/HANDOFF_WHServer_VAS_recovery.md`; the complete flag/exit-code/Python reference in `docs/BUNDLE_README.md`.
- **Tests:** `tests\run_regression.ps1` (use `-NoTs` on machines without TradeStation); C++ unit-test inventory in `tests\run_unit_tests.ps1`.
- **Exit codes:** `0` success; `11` not elevated; `8` compile failure; `5` not found / already exists; `14`/`15`/`18`/`19` safety-abort family (the run stopped before it could do harm — see `docs/BUNDLE_README.md` for the full table).
