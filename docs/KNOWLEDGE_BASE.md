# TSDev.exe Knowledge Base

Authoritative knowledge base of discovered facts about **TSDev.exe** only. Current proven knowledge only: every statement cites its evidence; no assumptions, conclusions, history, or unevidenced claims. Scope is TSDev.exe exclusively — per `AGENTS.md`, no other binary or process is in scope, and facts about other binaries do not belong here. All addresses are from TSDev 9.5 and are version-specific.

The UI save path below is the definition of "the same verify (compile + save) code path the UI triggers" that the automation drives.

---

## TSDev.exe external control channel: named pipe `\\.\Pipe\TSDevPipe` (the platform→TSDev control channel)

TSDev.exe runs a **named-pipe server** that accepts commands from external processes. This is the channel TradeStation itself uses to drive TSDev (second-instance handoff), and it is the highest-level external access point discovered.

- **Server**: `FUN_004897c0` — `CreateNamedPipeW(L"\\\\.\\Pipe\\TSDevPipe", PIPE_ACCESS_DUPLEX=3, 6, 255, 32000, 32000, 0, 0)`, then loop: `ConnectNamedPipe` → `ReadFile` (64000-byte buffer) → dispatch → `DisconnectNamedPipe`. Runs on a thread created by `CMainFrame::OnCreate` (`FUN_00486a60`) with the main frame HWND.
- **Pipe name string**: `\\.\Pipe\TSDevPipe` @ `0x0056E6E0` (UTF-16).
- **Client side**: `FUN_004950c0(wchar_t *cmdline)` — `CreateFileW(pipe, GENERIC_READ|GENERIC_WRITE, …)`, `WriteFile` of the UTF-16 string (default `L"Start"` if empty), `CloseHandle`. Called by the second instance (`FUN_00491960` = InitInstance) when `TSDEV_MUTEX` already exists (`GetLastError()==ERROR_ALREADY_EXISTS`) — the second instance forwards its command line and exits.
- **Payload format** (parsed by `FUN_00485460`): comma-separated pairs `"EncodedName", <number>` (UTF-16). Parser: `wcstok_s` with delimiters `L","` and `L", "`; surrounding `"` stripped; each name decoded via `CELDocHelper::DecodeName(CTSString&)`; number via `_wtof`→`int`. Returns true only if ≥1 pair parsed.
- **Dispatch**: per parsed record → `SendMessageW(mainFrame, 0x8001 /*WM_APP+1*/, (WPARAM)&CString(name), (LPARAM)number)`; on parse failure → `SendMessageW(mainFrame, 0x8002, (WPARAM)rawBuffer, 0)`. Frame message map (built in `FUN_004857d0`): `0x8001 → FUN_00487e30` → `FUN_004944b0` (open document by name+type via the doc-template map keyed by the number; special numbers `0xe8e8/0xe9e9/0xeaea` handled specially), `0x8002 → FUN_00487eb0` (forwards raw string to the app object vtable+0xac).
- **After every pipe command** the thread raises the frame in z-order (`SetWindowPos` TOPMOST→NOTOPMOST with `SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE`) and calls `ShowWindow(SW_RESTORE)` only `if (IsIconic(hwnd))`. **Verified: a hidden window stays hidden** (no SWP_SHOWWINDOW; hidden ⇒ not iconic).
- **Open-or-create semantics (verified live)**: sending `"Name", <type>` for an EXISTING item opens it; for a NONEXISTENT name it opens a NEW EMPTY document titled `Name : <Type>` (no error, no dialog). `open` therefore cannot detect non-existence by itself.
- **Per-type numbers (verified live, PID-level observation)**: `0` = Strategy, `1` = Function. Indicator/ShowMe/TradingApp numbers: NOT yet established (doc-template map keys; ShowMe subType 0x05 / TradingApp subType 0x14 per the category facts below are candidates, unverified).
- **Name encoding**: plain alphanumeric names work unencoded (verified). `CELDocHelper::DecodeName` implies an encoding for special characters — not yet characterized.
- **Live verification (2026-09-03)**: Python ctypes client (CreateFileW + WriteFile, UTF-16LE + NUL) against a running TSDev.exe: `"E2eFn_202608291052", 1` → main frame title changed to `TradeStation Development Environment - E2eFn_202608291052 : Function`; `"MyTestStrategyA", 0` → `… - MyTestStrategyA : Strategy`; `"NoSuchItemZZZ999", 1` → new empty doc titled `NoSuchItemZZZ999 : Function`. With the frame hidden (`ShowWindow(SW_HIDE)`), the command still executed and the window remained hidden (`IsWindowVisible=false`, `IsIconic=false`).

Evidence: TSDev.exe Ghidra decompilation (`FUN_004897c0`, `FUN_004950c0`, `FUN_00485460`, `FUN_00487e30`, `FUN_004944b0`, `FUN_00487eb0`, message-map builder `FUN_004857d0`), plus live pipe probes from an external Python process (this machine, TSDev 9.5 build timestamp Fri Nov 30 18:39:00 2018, SHA-256 `9099AA600F0A035C0290107EB1852CD61690330C53B14C9F9D01899395DCAF9A`).

## TSDev.exe has NO other programmatic access point (negative evidence)

- **No COM automation surface**: imports are COM-client only (`CLSIDFromProgID`, `CLSIDFromString`, `OleRun`, `CoInitialize`, `CoCreateInstance`, `CoUninitialize`); no `CoRegisterClassObject`/`CoGetClassObject`/`RegisterTypeLib`/`LoadTypeLib`/`GetActiveObject`. Registry (this machine): no ProgID/CLSID/TypeLib/AppID referencing TSDev.exe in HKLM (64-bit + WOW6432Node) or HKCU; `.eld/.els/.ela/.elp` shell-open associations all launch `ORPlat.exe`, not TSDev.exe. Evidence: import-table dump (dumpbin + independent PE parse, identical results) + registry queries (see `tmp/research/` evidence files, 2026-09-03).
- **No DDE, no sockets/winsock/wininet, no WM_COPYDATA, no file-mapping IPC** (import table: zero hits; `DdeInitialize`, `WSAStartup`, `CreateFileMapping` absent).
- **No command-line automation beyond the transfer wizard**: switches `/import_location`, `/export_project`, `/export_location` are parsed (`FUN_00494c60`, `FUN_004948e0`); `/import_location` leads to `CTransferWizard::DoModal` in InitInstance (modal UI wizard); `/export_project` additionally requires `/export_location` and passes a project-context check (`FUN_004905c0`) — verified live: with `/export_project <file> /export_location <dir>` and no project open, TSDev starts normally, shows no wizard, and creates no file. The wizard path is UI-bound; not usable for headless automation.
- Manifest: `requestedExecutionLevel level="requireAdministrator"` (R8 confirmed). Single-instance mutex `TSDEV_MUTEX`; second instances forward their command line via the pipe and exit.
- Main frame window class (runtime-verified): `TSDEV.EXE TRADESTATION`.

---

## TSDev.exe UI save path (the verify/compile + save code path)

Entry point: `FUN_00451800` @ `0x00451800`. Ordered sequence (TSDev-internal addresses; steps named only semantically — pending-save queue add/remove, save, compiler alloc, notify — are calls whose callees lie outside TSDev.exe, and per `AGENTS.md` their internals are out of scope):

1. **Pending-save queue add** — `(category, progId, name)` added to the pending-save queue. The save is bracketed by this add and the queue remove in step 6.
2. **Save** — `FUN_00452290` @ `0x00452290` performs the save via an external save call, passing the document's category (see category facts below).
3. **Compiler alloc** — external compiler-allocation call.
4. **Compile process Start/End** — `FUN_004D9480` → `StartCompilingProcess` / `FUN_004D91E0` / `EndCompilingProcess`.
5. **Category-gated dependent-recalc block** (runs only for categories 0/1/4 — see gate below): `FUN_004D7140` + worker cmd `0x0D`; `FUN_004c9670` builds the dependent list; recursive `FUN_004D91E0` per dependent.
6. **Pending-save queue remove**.
7. **Property/cache persistence** — `FUN_004531A0`.
8. **Notify** — external notify call `(category, progId)` via `FUN_00453f70`.

Evidence: TSDev.exe Ghidra decompilation @ `0x00451800` (static), plus runtime Frida traces of the UI save path.

### Category gate in `FUN_004D91E0`

The post-verify dependent-recalc block (step 5) runs only for categories `0` (Strategy), `1` (Function), and `4` (built-in Function). Indicator/ShowMe/TradingApp (category `2`) skip it — the shared-warehouse path is sufficient because the modified item IS the applied item.

Decompiled gate condition:

```c
if ((uVar6 == 1) && (((uVar2 == 1 || (uVar2 == 4)) || (uVar2 == 0))))
```

Evidence: TSDev.exe Ghidra decompilation (`FUN_004D91E0`).

### Per-type category mapping

| Type | Category |
|------|----------|
| Strategy | `0` |
| Function | `1` (user) / `4` (built-in) |
| Indicator | `2` |
| ShowMe | `2`, subType `0x05` |
| TradingApp | `2`, subType `0x14` |

Evidence: decompilation-derived.

### Category assignment facts

| Fact | Address | Evidence |
|------|---------|----------|
| TSDev.exe creates user functions with warehouse category `1` | `CELFunctionDoc` ctor @ `0x00457eb0` | TSDev.exe Ghidra decompilation |
| TSDev.exe creates built-in functions with category `4` (read-only built-ins) | `CELBIFunctionDoc` ctor @ `0x004140c0` | TSDev.exe Ghidra decompilation |
| TSDev.exe's save path passes the document's category (`param_1[0x7a]`) from `FUN_00452290` into the external save call (callee outside TSDev.exe; internals out of scope) | `FUN_00452290` @ `0x00452290` | TSDev.exe Ghidra decompilation |

---

## TSDev.exe UI characteristics relevant to automation

- TSDev.exe has NO "Find Dependents" / "Find References" / "Where Used" / "Used By" UI feature for regular functions/indicators. Evidence: `strings` analysis of TSDev.exe (narrow + wide) found no such menu text; runtime menu exploration found no such feature.
- The TSDev.exe main window has NO standard Win32 HMENU: `win32gui.GetMenu` on the main window returns `hmenu=0x0`; TSDev uses a BCGSoft owner-drawn `BCGPToolBar` menu bar, not a standard Win32 HMENU. pywinauto `menu()` returns None; the UIA backend finds only the system MenuBar; Alt+key walks produce no `#32768` popup windows. Evidence: runtime UI inspection.
- The strings "Dependencies"/"Callers" in TSDev.exe exist ONLY as TradingApp project resource-tree node names (`TradingApp, UserFunction, SystemFunction, Icon, Form, Dependencies, Strings, Callers, Image`) — not a function/indicator feature. Evidence: `strings` analysis of TSDev.exe.
- The function editor Outline pane is a `SysTreeView32` showing only a `Document` node (children: `using`, `inputs`, and the input names — e.g. `Price`, `Length`) — no `Callers` or `Dependencies` nodes. Evidence: runtime UI inspection (built-in `Average` and user `E2eFn_202608291052` opened in the editor; Outline pane dumped).

---

## Expected observable behavior after modify/delete (E2E expectations)

The dependent recalc itself executes outside TSDev.exe; these are distilled end-to-end expectations, not TSDev.exe-internal mechanisms.

- An applied Indicator re-plots immediately after modify; deleting it while applied auto-removes it from the chart and from the Insert Analysis Techniques list.
- ShowMe markers move immediately after modify.
- Strategy signal labels update immediately after modify.
- An open TradingApp WinForms form reinitializes immediately when the TradingApp itself or a caller Function is modified.

Evidence: user-confirmed runtime observation.
