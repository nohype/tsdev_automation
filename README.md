# TSDev Automation

Automates management of the TradeStation Development Environment (TSDev) Library by **driving TSDev.exe itself**: triggering the same code paths its UI controls trigger, without GUI automation and without requiring a visible window.

**This branch (`exp/remote-control`) is an overhaul.** The previous approach — interfacing with the TS library directly, without TSDev.exe — is discarded, and most of the existing codebase will be replaced.

- Requirements (SSOT): [AGENTS.md](AGENTS.md)
- TSDev.exe knowledge base (SSOT): [docs/KNOWLEDGE_BASE.md](docs/KNOWLEDGE_BASE.md)
- PM working memory: [docs/WORKING_MEMORY.md](docs/WORKING_MEMORY.md)
- `archive/` contains the discarded previous approach (direct library interfacing without TSDev.exe) — reference only, see `archive/README.md`.

CLI and Python package documentation will be written here as the new implementation lands.

## tsdevctl (work in progress)

`tsdevctl` drives TSDev.exe through its own named-pipe control channel
(`\\.\Pipe\TSDevPipe`) — the same channel TradeStation itself uses. No UI
automation, no clipboard, no injection; TSDev.exe is the only process touched.

```
tsdevctl open --type <Strategy|Function> --name <Name> [--json] [--timeout <ms>] [--nohide] [--ts-path <dir>]
```

Attaches to a running TSDev.exe or launches it hidden (default), sends the
open command, and reports the outcome as JSON (`--json`) or human-readable
text. Requires an elevated shell. Exit codes are documented in
`tsdevctl --help`.

Build: `cmake -S . -B build -A x64` then
`cmake --build build --config Release` (CMake ships with the VS Build Tools;
see `tests/run_unit_tests.ps1` for the locator logic).

- `tests/run_unit_tests.ps1` — pure-logic unit tests, no TradeStation needed.
- `tests/run_e2e.ps1` — end-to-end against a real TSDev.exe; requires an
  elevated shell and TradeStation 9.5; skips cleanly in CI otherwise.
