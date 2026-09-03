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
- **Per-type numbers — complete doc-template map (static + live-verified)**: the open path resolves the type number through a `std::map<int, CDocTemplate*>` at `CTSDevApp+0x2a0`; all 16 insertions are in `FUN_00491960` (InitInstance), each `CMultiDocTemplate` → `SetServerInfo` → `AddDocTemplate` → map insert (sites `0x0049223c`…`0x00492a2f`). Keys are `(subType<<8)|category`, matching the category facts below. Complete table:

  | Number | Type | Doc class | Verification |
  |--------|------|-----------|--------------|
  | `0` | Strategy | `CELSignalDoc` | live (phantom title) |
  | `1` | Function (user) | `CELFunctionDoc` | live |
  | `2` | Indicator | (Indicator doc) | live |
  | `3` | ActivityBar | (ActivityBar doc) | live |
  | `4` | Function (built-in, read-only) | `CELBIFunctionDoc` | live |
  | `0x402` (1026) | PaintBar | (PaintBar doc) | live |
  | `0x502` (1282) | ShowMe | (ShowMe doc) | live |
  | `0xa02` (2562) | VolatilityModel | (VolatilityModel doc) | static only |
  | `0xc02` (3074) | PricingModel | (PricingModel doc) | static only |
  | `0xe02` (3586) | BidAskModel | (BidAskModel doc) | static only |
  | `0x1002` (4098) | SearchStrategy | (SearchStrategy doc) | static only |
  | `0x1202` (4610) | ProbabilityMap | (ProbabilityMap doc) | live |
  | `0x1402` (5122) | TradingApp | `CELApplicationDoc` (INFERRED) | live (title `Trading Application`) |
  | `0xe8e8` | PartialClass | `CELPartialClassDoc` | static only — see specials below |
  | `0xe9e9` | WFDesigner | `CELWFDesignerDoc` | static only — see specials below |
  | `0xeaea` | StringResources | `CELStringResourcesDoc` | static only — see specials below |

  Cross-validated against a per-type file-extension table (`FUN_0047be40`: .esd/.efd/.eid/.ead/.efb/.ebd/.eed/.evd/.end/.ekd/.emd/.wfd/.str/.dgc). Numbers 5..32 are silent no-ops (no doc, no dialog; unknown map keys return 0 with no UI).
- **Special numbers must never be emitted by automation**: `0xe8e8` attaches a partial-class doc to its parent (requires a 4th argument the pipe never supplies — a complete no-op via the pipe, but internally meaningful); `0xe9e9` (WFDesigner) and `0xeaea` (StringResources) OPEN or phantom-create internal TSDev documents. Evidence: decompilation of `FUN_004944b0` special-number paths.
- **Raw-string pipe surface (`FUN_00494640`, the `0x8002` path) — complete**: (1) a string parseable as a `.eld/.els/.ela/.elp` file path (predicate `FUN_00492fd0`) triggers the MODAL `CTransferWizard` import inside the pipe thread — a hang risk; never emit such a payload; (2) `Name:A:B` opens-or-creates with type `(B<<8)|A` (vswscanf `L":%d:%d"`) — redundant with the pair form; (3) anything else (e.g. `Start`) is a no-op. **No List/Exists/Export/query command exists in this surface.** Evidence: decompilation + extension strings.
- **Frame message map is fully enumerated (`FUN_004857d0`)**: exactly two custom messages (`0x8001` open-by-pair, `0x8002` raw string); the rest are WM_COMMAND entries and BCGSoft registered messages, none pipe-reachable.
- **Open dialog is NOT externally reachable**: `CELFileOpenDialog` is imported from ORELUI20.dll (boundary); its IDOK handler calls the same `FUN_004944b0` the pipe uses, but the item enumeration happens inside ORELUI20.dll. The map TSDev passes to the dialog is a per-type open-document counter, not the item list. No external trigger exists for the enumeration.
- **Exists discriminator (verified live, 2026-09-03)**: after a pipe open, the document's editor control (`BCGPEditCtrl`, a descendant of the MDI child) contains, read via `WM_GETTEXT` (buffer sized by `WM_GETTEXTLENGTH` first): a REAL item → its EasyLanguage source; a NONEXISTENT name → TSDev's own error page `'{!!!…\n\nError loading document\n\n…!!!\n\nThe technique <name> no longer exists in your EasyLanguage workarea. \n\n…!!!}'` (85/86/85 `!` runs; 383 chars with a 32-char name; requested name embedded verbatim). This is TSDev's own load-failure judgment — the basis of the `exists` verb. Documented limitation: any real item whose stored source contains both signature substrings classifies as phantom (substring rule); matching follows TSDev's own lookup, which is CASE-INSENSITIVE (verified: `myteststrategya` opens the real `MyTestStrategyA`).
- **Exists output boundary (PM-accepted, 2026-09-03)**: the `exists` verb reports the REQUESTED name, not TSDev's verbatim stored spelling, and no ProgId. Both are unobtainable through the pipe channel: the frame/child title carries the spelling of the open request (verified live), and TSDev's stored item metadata (name spelling, ProgId) is exposed only by the item enumeration, which has no external trigger (see the Open-dialog negative evidence above). A case-sensitive answer and verbatim-name/ProgId output therefore require a future enumeration mechanism.
- **Editor render race (verified live, 2026-09-03)**: the editor content is NOT decidable from a single read. A fresh document's editor is empty immediately after its window appears, a fresh phantom's editor transiently holds a SINGLE SPACE before the error page replaces it, and under load (session-restore flood) both window appearance and content rendering lag the pipe command by seconds. A sound read must poll until the phantom error page appears (item does not exist) or the text is unchanged across consecutive reads ~500 ms apart (item exists — its rendered source); a single non-empty read misclassifies a phantom as existing.
- **WM_GETTEXT trailing NUL on BCGPEditCtrl (verified live, 2026-09-03)**: `WM_GETTEXT` on the `BCGPEditCtrl` editor control returns a copied count that includes a trailing NUL character in the text. The NUL is invisible to substring matching (the `exists` classifier) but corrupts verbatim text emission (`getsrc` source output — JSON, raw stdout, and `--output` file all contained a spurious `\u0000`). `WM_GETTEXT` on MDI child title windows (`Afx:` class) does NOT exhibit this. Readers must strip trailing NUL characters from `WM_GETTEXT` results when the text is emitted verbatim.
- **Frame-title behavior (verified live)**: the title carries the spelling TSDev opened the document with — for a fresh open that is the requested spelling; for an already-open item it is the ORIGINAL spelling, so title matching must be case-insensitive. The trailing `*` is a state-dependent UI marker (appears on fresh opens, clears on deactivation, real and phantom alike) — never an existence or dirty flag.
- **Session restore is asynchronous**: after the frame appears, TSDev continues to re-open previous-session documents for a while (phantoms included — a crashed session's phantoms return on the next start). Automation must close what it opens; observers must wait for the document set to stabilize before snapshotting.
- **Late pipe-command execution (observed once, 2026-09-03, under rapid repeated pipe writes)**: a TSDev instance accepted pipe writes and executed the command only after the caller's timeout had expired, leaving an opened document behind; under the same rapid probing the instance then stopped processing pipe commands entirely while remaining alive (recovering only after a fresh launch). Automation must therefore re-scan for late-executed opens on failed waits and close them (the `exists` verb does), and a timed-out invocation must be treated by callers as "state unknown — the command may still execute". The wedge condition itself is unexplained; treat rapid back-to-back pipe probing as a possible trigger.
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

### External trigger for the verify/compile+save path (WM_COMMAND 0x296a)

`FUN_00451800` is the handler for **WM_COMMAND with command ID `0x296a` (10666)** — the Verify command. The entry is in the **CELDocument** MFC message map (static, .data section at `0x005678d0`): `ON_COMMAND(0x296a, FUN_00451800)`, `ON_UPDATE_COMMAND_UI(0x296a, FUN_00451530)`. A second `ON_COMMAND(0x296a, FUN_0047b0e0)` entry exists in the **CELSignalDoc** message map (at `0x0056c4f8`); `FUN_0047b0e0` calls `FUN_00451800` unconditionally, then refreshes signal properties. The frame's own message map (`FUN_004857d0`) does NOT contain `0x296a`; the command routes through MFC's `CFrameWnd::OnCmdMsg` → active view → active document → CELDocument's message map.

**CRITICAL: BCGSoft's OnCmdMsg suppresses disabled commands.** The frame (`CBCGPMDIFrameWnd`) and MDI child (`CBCGPMDIChildWnd`) both override `OnCmdMsg` (decorated symbols found at `0x5c95f6` and `0x5c0106`). Unlike standard MFC (which does NOT check `ON_UPDATE_COMMAND_UI` during `WM_COMMAND` dispatch), BCGSoft's `OnCmdMsg` consults the update-UI state and returns TRUE (handled) without dispatching `CN_COMMAND` when the command is disabled. This was confirmed by Frida tracing: `WM_COMMAND 0x296a` reaches the frame's `WndProc` (at `0x73c1d170` in the BCGSoft DLL), `SendMessageW` returns 1 (handled), but `FUN_00451800` is NEVER called.

**The ON_UPDATE_COMMAND_UI handler disables Verify for phantom documents.** `FUN_00451530` calls `FUN_0044f720`, which checks:
1. If the doc type (offset `0x1e8`) is `0xE9E9` or `0xEAEA` (phantom markers), it walks up the parent chain (`vtable+0x204`). A phantom with no parent returns 1 (enable) from `FUN_0044f720`, BUT `FUN_00451530` then checks additional conditions on the original `this`:
2. `field_0x1a4 != 0` (a flag that is 0 for phantom docs)
3. `field_0x31c == 0`
4. `short_0x1e8 != 1000 (0x3e8)`
A phantom document fails condition 2 (`field_0x1a4 == 0`), so the Verify command is **DISABLED** for phantom documents. BCGSoft's `OnCmdMsg` then suppresses the command.

**FUN_00451800 also has its own phantom guard**: if the doc type is `0xE9E9`/`0xEAEA`, it calls `vtable+0x204(this)` to get the parent doc and recurses. A phantom with no parent would call `FUN_00451800(NULL)` (crash). So even if the command routing reached `FUN_00451800`, it would not verify a phantom doc directly.

**Implication for insert**: The approach of "open a nonexistent name → phantom doc → set text → verify via WM_COMMAND 0x296a" does NOT work because TSDev explicitly disables verification for phantom documents. A different approach is needed to create a real (non-phantom) document, set its source, and verify it.

### Working insert path (proven live, 2026-09-03): New-document dialog via MDI-child command routing

**Command routing discovery (Frida-verified)**: WM_COMMAND sent to the main frame only dispatches commands handled by the frame's own message map (e.g. `0x8037` Windows dialog, `0x80ac` new DGC doc). Document-level commands (`0x296a` Verify, `0x296b`, `0xe104` Save As) DO route frame → active view → active document, but ONLY when the target document is the MDI-active child (`WM_MDIACTIVATE` to the MDIClient; verify with `WM_MDIGETACTIVE`). App-class commands (`0x2950` New Strategy, `0x2951` New Function, `0x2947` New Indicator, `0x2949` New ShowMe, `0x3D21` New TradingApp, `0xE100` ID_FILE_NEW) are NOT reachable via the frame — but ARE reachable via any **MDI child window** (`CMDIChildWnd` inherits `CFrameWnd::OnCmdMsg`: view → child frame → app). Proven live: `PostMessageW(mdi_child_hwnd, WM_COMMAND, 0x2950, 0)` opens the "New Strategy" dialog.

**"New \<Type\>" dialog** (class `TS_IDD_COMMONCREATE`, title "New Strategy" etc.): children are Name Edit (control ID 10515), Notes Edit (control ID 10513), "Select Template" ComboBox (ID 10727, default "(None)"), OK/Cancel/Help buttons. Automation: `WM_SETTEXT` the target name into the Name edit, then `PostMessageW(dialog, WM_COMMAND, IDOK /*1*/, 0)`. On OK, TSDev creates a REAL (non-phantom) document titled `Name : Type`.

**CRITICAL — the TSDev main frame must be VISIBLE when the New dialog's OK is dispatched.** With the frame hidden (`SW_HIDE`), the dialog still appears, accepts `WM_SETTEXT` (readback confirms the text is in the control), and the OK handler still reads both edits via `GetWindowTextW` (Frida-traced) — but the created document comes out with an EMPTY name (the name propagation is skipped). `SetForegroundWindow` on the dialog does NOT fix it; `ShowWindow(frame, SW_SHOW)` before dispatching the New command DOES (proven 3/3). Note TSDev IGNORES `STARTUPINFO.wShowWindow=SW_HIDE` at process creation — the frame becomes visible during TSDev's own startup regardless (proven 3/3: `visible=1` after a STARTUPINFO-hidden launch). Consequence: a LAUNCHED TSDev instance is visible during operations (TSDev re-shows its frame during startup) and the tool re-hides it at exit; an ATTACHED instance hidden by a previous command's exit-hide must be re-shown for insert. The dialog itself briefly flashes visible — TSDev's own behavior.

**Quiet-UX: WS_VISIBLE off-screen is sufficient (verified live, 2026-09-03).** The "frame must be VISIBLE" requirement means `IsWindowVisible() == true` (the `WS_VISIBLE` flag is set), NOT that the frame is actually on the user's screen. A frame moved off-screen (`SetWindowPos(-32000, -32000)` with `SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE`) while keeping `WS_VISIBLE` satisfies the requirement: the New dialog's name propagation works (4/4 configs tested), and the verify "*" removal works (4/4 configs tested). This enables flicker-free operation: `SW_SHOWNA` (show without activating — no focus steal) + off-screen positioning keeps the frame `WS_VISIBLE` without the user seeing it.

**SW_SHOWNA does not steal focus (verified live, 2026-09-03).** With another application (notepad) in the foreground, `ShowWindow(frame, SW_SHOWNA)` + off-screen move did NOT steal focus (notepad remained foreground). The New dialog appearing on-screen also did NOT steal focus. However, the New dialog creation repositions the frame back on-screen (from -32000 to -7,-7 — TSDev's own `SetWindowPos` call), and this repositioning CAN steal focus. A background "frame guard" thread that polls the frame position every 10ms and immediately moves it back off-screen minimizes the on-screen time to ~10ms. Focus is also restored at exit via `AttachThreadInput` + `SetForegroundWindow` as a safety net.

**Proven end-to-end insert flow (hidden TSDev, 2026-09-03)**:
1. Pipe-open the target name → phantom doc (provides the MDI child command target; closed in cleanup).
2. `WM_MDIACTIVATE` the phantom's MDI child (via MDIClient). No `SetForegroundWindow` needed.
3. `PostMessageW(phantom_mdi_child, WM_COMMAND, <NewCmdId>, 0)` → "New \<Type\>" dialog appears.
4. `WM_SETTEXT` the name into the dialog's first Edit child; `PostMessageW(dialog, WM_COMMAND, IDOK, 0)`.
5. A real document window titled `Name : Type` appears (the phantom remains open behind it).
6. `WM_SETTEXT` the EasyLanguage source into the new doc's `BCGPEditCtrl`.
7. `WM_MDIACTIVATE` the new doc; `PostMessageW(frame, WM_COMMAND, 0x296a, 0)` → verify/compile+save runs (title "*" removed on completion).
8. Post-verify existence check confirms the item was saved; close the phantom (not dirty — no save prompt).

**Save As path (also proven, alternative)**: `WM_COMMAND 0xe104` to the FRAME with a real doc MDI-active opens the Save As dialog (class `TS_IDD_SAVEDLG`, Name Edit pre-filled, OK/Cancel/"Save as template"). Renames + saves the current doc as a new library item. Suppressed for phantoms. Usable as a copy-based insert when a seed item of the target type exists.

**Custom dialog classes**: TSDev's own dialogs use custom class names (`TS_IDD_COMMONCREATE`, `TS_IDD_SAVEDLG`, `TS_IDD_TSCRASHREPORT_DIALOG`), NOT the standard `#32770`. Dialog detection that only matches `#32770` misses them. (The compile-error MessageBox IS `#32770`.)

**New-command IDs per type** (from the app-class message map, thunk handlers FUN_00494010/FUN_00493F30/FUN_00493F50/FUN_00493FF0/FUN_00493EF0 → common dispatcher FUN_00496770 → `CDocTemplate::OpenDocumentFile(NULL, TRUE)`):

| Type | WM_COMMAND ID |
|------|---------------|
| Strategy | `0x2950` |
| Function | `0x2951` |
| Indicator | `0x2947` |
| ShowMe | `0x2949` |
| TradingApp | `0x3D21` |

Evidence: TSDev.exe Ghidra decompilation (app-class message map at `0x005719b0`–`0x00571ad8`; FUN_00496770; FUN_0047B9A0 create-dialog propagation inlined post-DoModal; FUN_004942B0 silent-open path; FUN_004511d0/FUN_0044C950 Save As); live Frida + Win32 message tracing (routing suppression, MDI-child routing, dialog classes, full hidden-mode insert flow).

**External invocation (for non-phantom docs only)**: `PostMessageW(frame_hwnd, WM_COMMAND, 0x296a, 0)` — provided the active document is NOT a phantom (type != `0xE9E9`/`0xEAEA`), `field_0x1a4 != 0`, `field_0x31c == 0`, and `short_0x1e8 != 1000`. `PostMessageW` (not `SendMessageW`) because the failure path shows a modal `MessageBox` that would block `SendMessageW`.

**Preconditions inside `FUN_00451800`**: (1) if `CELDebugger::IsDebugging()` is true, a MessageBoxW asks "Do you want to stop debugging?" — if the user clicks No, verify is aborted; (2) the function calls `FUN_0044f0f0` to get the active view, `CBCGPEditView::GetEditCtrl()` to get the edit control, and `CWnd::SetFocus()` to focus it — an active view with an editor is required.

**The pipe channel does NOT route to `FUN_00451800`**: `FUN_00487e30` (the frame's `0x8001` handler) calls `FUN_004944b0` (open-by-pair), not `FUN_00451800`. Confirmed by decompilation.

Evidence: TSDev.exe Ghidra decompilation (CELDocument message map at `0x005678b8`–`0x00567ab0`; AFX_MSGMAP header at `0x00567ac8`; entry at `0x005678d0`; CELSignalDoc message map at `0x0056c4e0`–`0x0056c510`; AFX_MSGMAP header at `0x0056c528`; `FUN_00451530`/`FUN_0044f720` update-UI handler; `FUN_00451800` phantom guard; BCGSoft `OnCmdMsg` symbols at `0x5c95f6`/`0x5c0106`); Frida runtime tracing (WM_COMMAND 0x296a reaches WndProc but FUN_00451800 never called; SendMessageW returns 1).

### Compile success/failure observable signals

After the compile step (`FUN_004D9480`), the result is checked: `*piVar10 == 1` → success; else → failure.

**Success path** (`*piVar10 == 1`): calls `vtable+0x1f8` = `FUN_00453f70` (`ShowRecalculationWarning()` [imported] + `Ordinal_57(category, subcategory)` for dependent recalc). No error dialog or error-list entries are produced. `ShowRecalculationWarning` is an imported function (import name at `0x005bbf22`) — it MAY show a dialog (unverified; the callee is outside TSDev.exe).

**Failure path** (`*piVar10 != 1`): if the Output/Error List window is NOT visible (`IsWindowVisible(frame+0x60a8)` returns false), `FUN_00453d40` displays errors via `GlobalMessageBox` (a `MessageBoxW` wrapper at `0x004c5d34`) or `DisplayError` (imported, `0x005bbf12`) — a modal error dialog appears. If the Output/Error List window IS visible, the error list control bar (at `frame+0x6088`) is shown and populated with error entries via `FUN_0044c410` (no MessageBox). `FUN_0044f150` adds error markers to the editor and navigates to error locations. Live-verified 2026-09-03: with the frame HIDDEN, compile failures raise a modal #32770 MessageBox (e.g. title "Grammar error", body "Semicolon (;) expected here.", one OK button); with the frame VISIBLE, errors go to the Output error list silently (a SysListView32 whose summary row reads "N error(s), M warning(s)").

**Both paths (common cleanup)**: `FUN_004502f0` (pending-save queue remove) → `FUN_004531A0` (property/cache persistence) → `CDocument::UpdateAllViews()` → `*(param_1 + 0x1a4) = 0` (clears a flag) → `FUN_00453a30(0)` (SetModifiedFlag: clears the modified flag at `param_1[0x12]`, updates the view and frame title). **Title "*" correction (live-verified 2026-09-03, supersedes the earlier claim): the "*" removal is visibility-dependent. With the frame VISIBLE during the verify, the "*" is removed quickly (<100 ms — before a 500 ms poll check) on BOTH the success and failure paths. With the frame HIDDEN, the "*" is NEVER removed (neither path updates the title) — the frame must be visible during the verify. The failed document is still dirty internally: closing it raises TSDev's "Save changes?" prompt.**

**Most reliable external detection**: (1) The Output error list is the authoritative compile-result signal: after the verify it holds one summary row " N error(s), M warning(s) " (the row containing "error(s)") plus one row per error (columns: text, item name, "Error (#n)", line). N > 0 → failure; no summary row / N == 0 → success. With the frame visible, failures are routed there SILENTLY (no dialog). (2) With the frame HIDDEN, failures raise a modal #32770 MessageBox (e.g. title "Grammar error", body "Semicolon (;) expected here.", one OK button) instead — and the "*" is never removed. (3) A post-verify existence check (re-open the item and check if it loads as a real item vs phantom) definitively confirms whether the item was saved: saved → success; not saved → failure.

| Signal | Success | Failure |
|--------|---------|---------|
| MessageBox dialog | No (unless ShowRecalculationWarning shows one) | Only with the frame HIDDEN (frame visible → silent, errors go to the list) |
| Output/Error window | Unchanged | Error list populated: summary row "N error(s), M warning(s)" + one row per error |
| Error markers in editor | No | Yes |
| Document title "*" | Removed (frame visible) | Also removed, <100 ms (frame visible); never removed (frame hidden) |
| Item exists in library after verify | Yes | Yes (the verify path SAVES before compiling — the item is saved uncompiled; verified live 2026-09-03) |

Evidence: TSDev.exe Ghidra decompilation (`FUN_00451800` compile result branch; `FUN_00453f70` success path; `FUN_00453d40`/`FUN_0044f150`/`FUN_0044c410` failure path; `FUN_00453a30` title cleanup; `FUN_0044f500` output window visibility check; `GlobalMessageBox` at `0x004c5d34`).

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
