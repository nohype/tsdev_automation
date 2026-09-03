# Working Memory — TSDev Automation

Live, real-time state for the PM. Keep minimal; drop stale/superseded facts.
Do not restate what AGENTS.md or the code documents (machine conventions,
paths, and process rules live in AGENTS.md).

## Current state

- Branch `exp/remote-control`. Requirements SSOT: AGENTS.md (R1–R15).
  TSDev.exe facts SSOT: docs/KNOWLEDGE_BASE.md (updated 2026-09-03 with the
  external control channel: named pipe `\\.\Pipe\TSDevPipe`, payload
  `"Name", <type>` pairs; verified live incl. hidden-window operation;
  no COM/DDE/sockets/CLI automation exists — pipe is THE access point).
- Access-point research complete (2026-09-03): Ghidra headless MCP server
  running (launch: `tmp/start_ghidra_mcp.cmd`, port 8089, analyzes
  `tmp\TSDev.exe` byte-identical copy — launch.bat cannot parse the
  `(x86)` path). Registry/strings/web/archive research evidence in
  `tmp/research/` (gitignored scratch).
- **TVS #1 (implemented, in review): `tsdevctl open` — TSDev lifecycle + pipe
  control channel.** C++20 CLI (src/tsdevctl + src/tsdevlib, CMake, /W4 /WX),
  verb `open --type <Strategy|Function> --name <n>`, `--json`, exit codes
  0/2/3/4/5/6/7/8/9, `--timeout` (≤0 disables), `--nohide`, `--ts-path`.
  Attach-or-launch-hidden (R5/R7), busy mutex `Local\TSDevAutomation.Busy`
  (R15), pipe write `"Name", <type>`, success = frame title contains
  ` - <name> : <Type>`, dialog/crash checks on every path. Verified green:
  unit tests (PS runner), E2E (launch hidden → open → stays hidden → WM_CLOSE
  cleanup), attach path, busy path (exit 9), usage error (exit 2).
  Known TSDev behaviors handled: pipe appears asynchronously after the frame
  (retry on ERROR_FILE_NOT_FOUND); TSDev re-shows its frame during startup
  (hide re-enforced at every exit); nonexistent name opens an empty phantom
  doc (documented in KB — Exists needs a different mechanism).
- Next after TVS #1: type-number mapping for Indicator/ShowMe/TradingApp;
  then editor-text set + verify trigger (Modify/Insert path).

## Still-valid machine facts

- UAC ConsentPromptBehaviorAdmin=5 → elevated launch from unelevated context
  costs ONE interactive UAC prompt; check elevation per session
  (`whoami /groups` → High Mandatory Level). Current shells are elevated.
- VS Build Tools present (dumpbin via vswhere; MSVC 14.51.36231). Python:
  `.venv\Scripts\python.exe`. Ghidra 12.1.2 + ghidra-mcp extension installed
  per-user (`AppData\Roaming\ghidra\ghidra_12.1.2_PUBLIC\Extensions`).
- Gitignored scratch: `build/`, `dist/`, `tmp/`, `.venv/`, etc.
  `ghidra_project/` holds the OLD Ghidra project (`tsdev.exe.rep`, .gpr
  marker missing — unrecoverable without the marker; fresh analysis used
  instead).
- Test items on this machine's TSDev library: `MyTestStrategyA` (Strategy),
  `E2eFn_202608291052` (Function). Closing an unmodified doc prompts nothing.
