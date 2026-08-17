# TSDev Automation

Automated management of the TradeStation Development Environment (TSDev) Library without using the TSDev UI (TSDev.exe). Insert, delete, and modify EasyLanguage strategies, indicators, functions, and trading apps via a standalone CLI.

## Supported component types

| Type | WHType | Status |
|------|--------|--------|
| Strategy | 0 | Supported |
| Indicator | 2 | Supported |
| Function | 4 | Supported |
| TradingApp | 5122 (0x1402) | Supported |
| ActivityBar | — | Unconfirmed |
| PaintBar | — | Unconfirmed |
| ProbabilityMap | — | Unconfirmed |
| ShowMe | — | Unconfirmed |

## CLI usage

```
tsdev_cli insert <type> <name> <source-file>
tsdev_cli delete <type> <name>
tsdev_cli modify <type> <name> <source-file>
tsdev_cli [--ts-path <dir>] <subcommand> ...
```

- `insert` — Compile and insert a new component. Fails loudly on compile errors (exit 8).
- `delete` — Delete an existing component. Notifies if it doesn't exist (exit 5).
- `modify` — Replace source code of an existing component and recompile. Preserves ProgId.

## Build

See [docs/BUILD.md](docs/BUILD.md) for build prerequisites, CMake commands, and test instructions.

## Project rules

See [AGENTS.md](AGENTS.md) for project rules, acceptance criteria, and conventions.

## Architecture

Signature data lives in `signatures/*.json` (authoritative source of truth) → `tools/sigcodegen.py` generates `signatures.gen.h` at build time → `src/orlib_resolver.cpp` consumes it. No runtime JSON dependency in the production CLI.

`src/sigtool.cpp` is a standalone developer tool for verifying orlib20.dll signatures against a DLL file on disk.

## Next workstream

- [Workstream D: Python Integration](docs/HANDOFF_D_PYTHON_INTEGRATION.md) — Python wrapper around the CLI for automated library management.

## Regression suite

```powershell
powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1
```

Runs 60 tests: sigtool verification (12) + C++ unit tests (14) + E2E CLI tests (34).
