# TSDev.exe Delete via View Activation — Reverse-Engineering Research

Static analysis of **TSDev.exe** (TradeStation 9.5 Development Environment, build timestamp Fri Nov 30 18:39:00 2018, SHA-256 `9099AA600F0A035C0290107EB1852CD61690330C53B14C9F9D01899395DCAF9A`) via the Ghidra MCP server. Every claim cites Ghidra addresses and decompilation evidence. Unproven conclusions are marked **[UNVERIFIED]**.

This document investigates whether a higher-level deletion approach — pipe-open, modify editor text, activate the document view, close without saving, and let the document destructor call the library deletion routine — can replace the current DLL injection approach (`src/delete_hook/dllmain.cpp`) and eliminate vtable calls and direct struct-offset manipulation.

---

## 1. `FUN_004417d0` — the `WM_ELCREATEDOCUMENT` callback

### 1.1 Installation as a message-map handler

`FUN_004417d0` is installed as an `ON_REGISTERED_MESSAGE` handler in a dynamically initialized MFC message map. The initialization code at `0x00440443`–`0x0044047f` writes a 24-byte `AFX_MSGMAP_ENTRY`:

| Field | Address | Value | Meaning |
|---|---|---|---|
| `nMessage` | `0x005d36b8` | `0xC000` | Registered-message marker |
| `nCode` | `0x005d36bc` | `0x0` | No notification code |
| `nID` | `0x005d36c0` | `0x0` | No control ID |
| `nLastID` | `0x005d36c4` | `0x0` | No range |
| `nSig` | `0x005d36c8` | `0x005EB968` | Address of `DAT_005eb968` (the registered message ID variable) |
| `pfn` | `0x005d36cc` | `0x004417D0` | `FUN_004417d0` — the handler |

The registered message is `WM_ELCREATEDOCUMENT`, registered at `0x004024d0` via `RegisterWindowMessageW` with the string at `0x005624a8` (`"WM_ELCREATEDOCUMENT"`). The resulting message ID is stored in `DAT_005eb968` (`0x004024db`).

The same message map contains `ON_COMMAND` entries for command IDs `0xC01C`, `0xC01E`, `0xC01F`, `0xC020`, `0xC021`, `0x8071`, `0x8096`, and `0x80AC` (confirmed by disassembly at `0x004400a1`–`0x0044042f`). These are view-level commands, consistent with a view-class message map. **[UNVERIFIED]** The exact class that owns this message map could not be definitively identified because the entries are zero in the static image (dynamically initialized at runtime) and no `AFX_MSGMAP` structure pointer to the entries array was found in the static image. However, the circumstantial evidence — `FUN_004417d0` receives a view pointer and extracts the document from `param_1[0x20]` (the standard MFC `CView::m_pDocument` offset at byte `0x80`) — strongly suggests this is the `CELBCGPEditView` class message map.

### 1.2 Full decompilation (`0x004417d0`)

```c
undefined4 __fastcall FUN_004417d0(int *param_1) {
    // param_1 = the VIEW (CBCGPEditView / CELBCGPEditView)
    BeginWaitCursor();

    piVar6 = (int *)param_1[0x20];       // view->m_pDocument (offset 0x80)
    if (piVar6 == NULL) return 0;

    if ((char)piVar6[0x6e] != '\x01')     // check gate byte at doc+0x1B8
        return 0;
    *(byte *)(piVar6 + 0x6e) = 0;         // CLEAR gate byte (one-shot)

    // Resolve parent frame windows
    pHVar3 = GetParent((HWND)param_1[8]);
    pCVar4 = CWnd::FromHandle(pHVar3);
    pHVar3 = GetParent(*(HWND *)(pCVar4 + 0x20));
    pCVar4 = CWnd::FromHandle(pHVar3);

    if (piVar6[0x5f] == 0) {              // doc+0x17C == 0 (NOT modified)
        iVar5 = FUN_0044f020();
        if (iVar5 == 0xeaea) goto end;    // skip for type 0xEAEA

        iVar5 = FUN_0044f020();
        DAT_005eb9e8 = 1;

        if (iVar5 == 0xe9e9) {            // type 0xE9E9 (TradingApp)
            (**(code **)(*param_1 + 500))();  // VIEW->vtable[0x1F4] (not doc)
            EndWaitCursor();
            DAT_005eb9e8 = 0;
            // ... WFDesignerManager::SelectForm ...
            return 0;
        }

        // Normal types (Function, Indicator, ShowMe, Strategy):
        (**(code **)(*piVar6 + 500))();   // DOC->vtable[0x1F4] = FUN_0044f8e0 (loader)
                                         // → sets doc+0x174 = 1 if item found
        DAT_005eb9e8 = 0;

        cVar1 = FUN_0044b870();
        if (cVar1 != '\0')
            (**(code **)(*piVar6 + 0x6c))();  // DOC->vtable[0x6c] = FUN_00453a30

        iVar5 = FUN_0044f020();
        if (iVar5 != 0xe8e8) {
            FUN_0044eae0();
            FUN_0042b420();
            FUN_004f3960();
        }
    }
    else {                                // doc+0x17C != 0 (MODIFIED)
        iVar5 = FUN_0044ba50();           // Create Entry → sets doc+0x174 = 1
                                          // ALSO creates a new library entry!
        if (iVar5 == 0) return 0;
        // ... manager/property operations ...
        FUN_00452290();                   // Save → clears doc+0x17C = 0
        // ... view notification (vtable[0x22c]) ...
    }

    FUN_0044b940();
    FUN_00439400();
    // Send registered message to main window
    SendMessageW(*(HWND *)(iVar5 + 0x20), DAT_005eb964, 1, (LPARAM)param_1);

end:
    EndWaitCursor();
    return 0;
}
```

### 1.3 Key observations

1. **`param_1` is the VIEW**, not the document. The document is obtained from `param_1[0x20]` (view+0x80 = `CView::m_pDocument`).

2. **Gate byte at `doc+0x1B8`** (`piVar6[0x6e]` with int* indexing): if not `1`, the handler returns immediately. If `1`, it is cleared to `0` (one-shot behavior). The constructor `FUN_00449ea0` initializes this byte to `1` (confirmed by decompilation summary; the constructor sets `param_1[0x6e] = 1` at `0x00449ea0`).

3. **When `doc+0x17C == 0` (not modified)** — the normal state after pipe-open:
   - For type `0xEAEA`: skips entirely.
   - For type `0xE9E9` (TradingApp): calls `VIEW->vtable[0x1F4]` (a different path).
   - For normal types (Function, Indicator, ShowMe, Strategy): calls `DOC->vtable[0x1F4]` = `FUN_0044f8e0` (loader), which **sets `doc+0x174 = 1`** if the item is found in the library (`0x0044ffd0`, confirmed in `RE_DELETE_UI_FLOW.md` §3).
   - Then optionally calls `DOC->vtable[0x6c]` = `FUN_00453a30` if `FUN_0044b870` returns true.

4. **When `doc+0x17C != 0` (modified)** — the "Create Entry" branch:
   - Calls `FUN_0044ba50` (Create Entry), which **sets `doc+0x174 = 1`** (`0x0044ba9e`) but also **creates a new library entry** via `_CreateEntryEX_24`.
   - Then calls `FUN_00452290` (save), which **clears `doc+0x17C = 0`**.
   - This branch is destructive and not suitable for deletion.

5. **`FUN_004417d0` does NOT directly write to `doc+0x174` or `doc+0x17C`.** It sets `doc+0x174 = 1` indirectly through the loader (`FUN_0044f8e0`) or Create Entry (`FUN_0044ba50`).

### 1.4 `vtable[0x6c]` (`FUN_00453a30`) does NOT set `doc+0x17C`

The `RE_DELETE_UI_FLOW.md` §2 states that `vtable[0x6c]` is "SetModifiedFlag" and sets `doc+0x17C = 1`. Decompilation of `FUN_00453a30` at `0x00453a30` (confirmed at `CELDocument` vtable `0x00567698` + `0x6c` = `0x00567704`, and `CELFunctionDoc` vtable `0x00568c98` + `0x6c` = `0x00568d04`, both containing `0x00453a30`) shows:

```c
void __thiscall FUN_00453a30(int *param_1, int param_2) {
    piVar2 = FUN_0044f0f0();              // get current view
    if (piVar2 != NULL)
        (**(code **)(*piVar2 + 0x1dc))(param_2);  // view->vtable[0x1dc](param_2)
    param_1[0x12] = param_2;              // doc+0x48 = param_2
    pwVar3 = ... (param_1 + 0x81);        // doc+0x204 = name string
    (**(code **)(*param_1 + 0x5c))(pwVar3);  // vtable[0x5c](name)
    param_1[0x12] = param_2;              // doc+0x48 = param_2
}
```

This function sets `doc+0x48` (likely MFC's `CDocument::m_bModified`), NOT `doc+0x17C` (CELDocument's own modified flag). The destructor `FUN_0044a640` checks `param_1[0x5f]` (doc+0x17C), not `param_1[0x12]` (doc+0x48). Therefore, `vtable[0x6c]` does NOT satisfy the destructor's modified-flag condition.

---

## 2. Pipe-opened documents and views

### 2.1 Pipe handler (`FUN_004897c0` at `0x004897c0`)

The named-pipe server at `\\.\Pipe\TSDevPipe` (`0x0056e6e0`) is created by `FUN_004897c0`. For each pipe message:

1. `FUN_00485460` parses the pipe data into structured items.
2. For each parsed item: `SendMessageW(param_1, 0x8001, (WPARAM)puVar5, puVar1[6])` — sends `WM_USER + 0x1` (`0x8001`) to the main frame window.
3. If parsing fails: `SendMessageW(param_1, 0x8002, ...)` — sends `0x8002`.

The main frame's message map (`FUN_004857d0` at `0x004857d0`) maps:
- `0x8001` → `FUN_00487e30` (pipe-open handler)
- `0x8002` → `FUN_00487eb0` (raw text handler)

### 2.2 Pipe-open handler (`FUN_00487e30` at `0x00487e30`)

```c
undefined4 FUN_00487e30(wchar_t *param_1, undefined4 param_2) {
    AfxGetModuleState();
    CStringT::CStringT(aCStack_14, param_1);
    FUN_004944b0();                       // pipe-open / document resolution
    return 0;
}
```

### 2.3 Document resolution (`FUN_004944b0` → `FUN_004942b0`)

`FUN_004944b0` locates an existing document by type/name, or calls `FUN_004942b0` to open/create one. `FUN_004942b0`:

- Enumerates existing documents. If found: obtains the view/frame, calls `GetParentFrame`, `BringWindowToTop`.
- If not found: calls the document template's `vtable+0x8c` (`OnOpenDocument`) with the requested name and `1`.
- After successful opening: calls `FUN_0044f410` (type resolver).

This confirms that pipe-opened documents are regular MFC documents opened through a document template, with associated frames and views. The document template creates the MDI child frame and editor view.

### 2.4 View initialization sends `WM_ELCREATEDOCUMENT`

`FUN_004427b0` (referenced from `0x00566ffc`) performs editor-view initialization. It operates on a `CBCGPEditView` and:

- If a thread flag at `thread+0x10b70` is set: `SendMessageW(viewHWND, DAT_005eb968, 0, 0)` — sends `WM_ELCREATEDOCUMENT` synchronously.
- Otherwise: `PostMessageW(viewHWND, DAT_005eb968, 0, 0)` — posts `WM_ELCREATEDOCUMENT` asynchronously.

Since pipe-open calls the document template's open method (which creates the frame and view), and `FUN_004427b0` is part of view initialization, **pipe-opened documents do receive `WM_ELCREATEDOCUMENT`**. **[UNVERIFIED]** The exact call chain from document-template open to `FUN_004427b0` was not fully traced; the inference is based on `FUN_004427b0` operating on `CBCGPEditView` and the document template creating the view.

### 2.5 `FUN_00487980` confirms the callback is relevant to pipe-opened documents

`FUN_00487980` (a debugger-related flow, in the main frame message map at `0x005d43b4`):

1. Checks whether a document is already open.
2. If not, calls `FUN_004944b0` (pipe-open) to open it.
3. **Explicitly clears the gate byte** `piVar3 + 0x6e` (doc+0x1B8) after opening.
4. Sets a global state, directly calls a vtable slot, then restores global state.

This explicitly suppresses the `WM_ELCREATEDOCUMENT` callback for pipe-opened documents in the debugger flow, demonstrating that the callback **would normally fire** for pipe-opened documents (the gate byte is initialized to `1` by the constructor and is not cleared by the pipe-open path itself).

---

## 3. Does `WM_ACTIVATE` / `WM_MDIACTIVATE` trigger `FUN_004417d0`?

**No.** `FUN_004417d0` is not an activation handler. It is specifically an `ON_REGISTERED_MESSAGE(WM_ELCREATEDOCUMENT, ...)` handler. It fires when the `WM_ELCREATEDOCUMENT` registered message is sent or posted to the view window, which happens during view initialization (`FUN_004427b0`), not during window activation.

`WM_ACTIVATE` and `WM_MDIACTIVATE` are standard Windows messages handled by MFC's `CView::OnActivateView` and `CMDIChildWnd` activation logic. There is no evidence in the message map containing `FUN_004417d0` of `WM_ACTIVATE` or `WM_MDIACTIVATE` entries. The message map entries are all `WM_COMMAND` (`0x111`) and the single `ON_REGISTERED_MESSAGE` for `WM_ELCREATEDOCUMENT`.

Sending `WM_ACTIVATE` or `WM_MDIACTIVATE` to a pipe-opened document's view would trigger standard MFC activation (focus, frame title updates, etc.) but would **not** trigger `FUN_004417d0` and therefore would **not** set `doc+0x174`.

The only way to trigger `FUN_004417d0` via a window message is to send `WM_ELCREATEDOCUMENT` (the registered message whose ID is in `DAT_005eb968`) to the view window. However, this only works if the gate byte at `doc+0x1B8` is still `1`, which it is after pipe-open (the constructor sets it to `1`, and the pipe-open path does not clear it).

---

## 4. Close-without-saving path

### 4.1 Existing implementation

The project already implements close-without-saving in `src/tsdevctl/main.cpp`:

- **`close_doc_window`** (line 385): `PostMessageW(doc.window, WM_CLOSE, 0, 0)` — closes a clean document.
- **`close_doc_discard`** (line 406): `PostMessageW(doc.window, WM_CLOSE, 0, 0)`, then polls for TSDev dialogs and sends `PostMessageW(dlg, WM_COMMAND, IDNO, 0)` to answer "No" to any "Save changes?" prompt.

This mechanism is already used in the modify/insert flows for cleanup on failure (lines 1398, 1401, 1435, 1438, 1705, 1741).

### 4.2 Close path in the binary

`FUN_004508e0` (`OnCloseDocument`, vtable `0x8c`):

1. Closes related documents through `vtable+0x8c`.
2. Handles associations at `+0x1BC` / `+0x1C0`.
3. Performs type-specific cleanup.
4. Clears `param_1 + 0x1C4`.
5. Calls `COleServerDoc::OnCloseDocument(param_1)` — standard MFC close, which destroys views and calls `delete this` (the destructor).

### 4.3 Destructor deletion condition (`FUN_0044a640` at `0x0044a640`)

```c
void __fastcall FUN_0044a640(undefined4 *param_1) {
    *param_1 = CELDocument::vftable;        // revert to base vtable (0x00567698)
    if (param_1[0x5d] != 0) {               // doc+0x174: opened-from-library
        // ... cleanup (FUN_00491460, FUN_00477b40) ...
        bVar1 = CELProperties::IsSaved((CELProperties *)(param_1 + 0x84));
        if ((!bVar1) && (param_1[0x5f] != 0)) {  // !IsSaved AND doc+0x17C (modified)
            FUN_0044c0f0();                  // ACTUAL LIBRARY DELETION
        }
    }
    // ... rest of destructor ...
}
```

The three conditions (all must be met) with decompiler index mapping:

| Condition | Decompiler index | Byte offset | Semantic |
|---|---|---|---|
| 1 | `param_1[0x5d] != 0` | `doc+0x174` | Opened from library |
| 2 | `!CELProperties::IsSaved(doc+0x84)` | `doc+0x84` (properties) | Properties not saved |
| 3 | `param_1[0x5f] != 0` | `doc+0x17C` | Modified flag |

Index-to-offset mapping: `0x5d * 4 = 0x174`, `0x5f * 4 = 0x17C` (confirmed by constructor disassembly at `0x00449edb`: `MOV [EBX+0x174], 0` and `0x00449ee5`: `MOV [EBX+0x17c], 1`).

### 4.4 "Save changes?" dialog

No "Save changes" string was found in the binary's string table (searched via Ghidra `search_strings` for "save", "Save changes", "SaveModified"). The dialog text is likely in a resource file not visible as a string in the `.data` section. The existing code comments at `main.cpp` line 404 confirm: "TSDev's 'Save changes?' prompt (#32770)". The dialog is a standard MFC `CDocument::SaveModified` prompt, triggered by `WM_CLOSE` → `OnClose` → `SaveModified`.

The existing `close_doc_discard` function handles this by sending `WM_COMMAND IDNO` (value `7`) to the dialog window, which triggers the same code path as the user clicking "No".

---

## 5. Does modifying editor text set the destructor conditions?

### 5.1 `doc+0x17C` (modified flag) — NOT set by `WM_SETTEXT`

A byte-pattern search for `C7 8? 7C 01 00 00 01 00 00 00` (MOV DWORD PTR [reg+0x17C], 1) found exactly **one** site:

| Address | Function | Context |
|---|---|---|
| `0x00449ee5` | `FUN_00449ea0` (constructor) | Initializes `doc+0x17C = 1` for new documents |

A search for `89 8? 7C 01 00 00` (MOV [reg+0x17C], reg) found one additional site:

| Address | Function | Context |
|---|---|---|
| `0x0044f924` | `FUN_0044f8e0` (loader, vtable[0x1F4]) | `MOV [EBX+0x17C], EDI` — but `EDI` is `0` in this context (the loader clears the flag) |

Sites that **clear** `doc+0x17C = 0`:

| Address | Function | Context |
|---|---|---|
| `0x0044125c` | `FUN_00441250` | `MOV [ESI+0x17C], 0` |
| `0x00451959` | `FUN_00451800` (verify handler, cmd `0x296a`) | `MOV [param_1+0x17C], 0` — clears at start |
| `0x00452467` | `FUN_00452290` (save) | `doc[0x5f] = 0` — clears on save success |
| `0x00453679` | `FUN_004535d0` (archive serialize) | `MOV [ESI+0x17C], 0` |
| `0x00450fb5` | `FUN_00450f30` (OnOpenDocument) | Clears during open |

**No function in the binary sets `doc+0x17C = 1` except the constructor.** The constructor's value is immediately cleared by `FUN_00450f30` (OnOpenDocument) during pipe-open. After pipe-open, `doc+0x17C = 0`.

The existing code comments confirm this: `main.cpp` line 1342–1343: "WM_SETTEXT may not set the document's modified flag, so the '*' may never appear." The `WM_SETTEXT` message is sent to the `BCGPEditCtrl` (the edit control), not to the MFC document. The `BCGPEditCtrl` does not call `CDocument::SetModifiedFlag` in response to `WM_SETTEXT` — it only does so in response to user-initiated text changes (keyboard input, cut/paste via `EN_CHANGE` notifications).

No `EN_CHANGE` handler that writes `doc+0x17C` was found in the `CELBCGPEditView` message map. The `EN_CHANGE` handlers found at message-map entries `0x005670f8` and `0x0056ae40` belong to dialog classes, not the editor view.

### 5.2 `!IsSaved` — NOT set by any high-level operation

`CELProperties::IsSaved` is a method in an external DLL (the `CELProperties` class is not fully visible from `TSDev.exe` static analysis). The existing DLL hook (`dllmain.cpp` lines 112–121) calls `vtable[0x1B4]` to invoke `CELProperties::SetSaved(false)`.

No pipe command, window message, or command ID in the binary is known to call `SetSaved(false)`. The loader (`FUN_0044f8e0`) loads properties from the library, which puts them in a "saved" state (`IsSaved` returns `true`). No subsequent high-level operation (text modification, verify, activation) calls `SetSaved(false)`.

**[UNVERIFIED]** The exact internal state of `IsSaved` after pipe-open + text modification cannot be determined from `TSDev.exe` static analysis alone because `IsSaved` is implemented in an external DLL. However, the existing DLL hook explicitly calls `SetSaved(false)` because the developers determined it was necessary — if it happened naturally, the hook would not need to do it.

### 5.3 `doc+0x174` (opened-from-library) — potentially set by `WM_ELCREATEDOCUMENT`

If `WM_ELCREATEDOCUMENT` fires for a pipe-opened document (see §2.4–2.5), `FUN_004417d0` takes the "not modified" branch (since `doc+0x17C == 0` after pipe-open) and calls `DOC->vtable[0x1F4]` (`FUN_0044f8e0`, loader), which sets `doc+0x174 = 1` if the item exists in the library.

This is the only destructor condition that might be met through high-level operations.

---

## 6. Existing modify/insert flows and `doc+0x174`

The existing modify/insert flows (`main.cpp` lines 1293–1325, 1609–1633):

1. Open the item via pipe (`FUN_004944b0`).
2. Set the source via `WM_SETTEXT` to the `BCGPEditCtrl` (`set_doc_text` in `doc_read.cpp` line 91).
3. Send `WM_COMMAND 0x296a` (verify/compile) to the main frame.
4. Wait for the "*" dirty marker to appear then disappear.
5. On success, close the document.
6. On failure, close with `close_doc_discard`.

These flows do **not** set `doc+0x174` because:
- Pipe-open does not call `vtable[0x1F4]` (confirmed in `RE_DELETE_UI_FLOW.md` §3).
- `WM_SETTEXT` does not trigger `FUN_004417d0` (it's not a `WM_ELCREATEDOCUMENT` message).
- The verify handler (`FUN_00451800`) does not call `vtable[0x1F4]`.
- `WM_ELCREATEDOCUMENT` may fire during view initialization, but this happens before the text is modified, when `doc+0x17C == 0`, so the "not modified" branch runs and calls the loader.

If `WM_ELCREATEDOCUMENT` fires during pipe-open, `doc+0x174` would be set to `1` for all pipe-opened documents, including those used in modify/insert flows. This would not affect modify/insert because those flows save (which clears `doc+0x17C`) and then close cleanly (the destructor sees `doc+0x17C == 0` and skips deletion).

---

## 7. Conclusion

### 7.1 Viability assessment

The higher-level deletion approach — pipe-open, modify text, activate view, close without saving — is **NOT viable** as a complete replacement for the current DLL injection approach.

The destructor requires three conditions (§4.3):

| Condition | Set by high-level operations? | Evidence |
|---|---|---|
| `doc+0x174 != 0` (opened-from-library) | **Potentially yes** [UNVERIFIED] | `WM_ELCREATEDOCUMENT` may fire during pipe-open view initialization, causing `FUN_004417d0` to call the loader (`FUN_0044f8e0`), which sets `doc+0x174 = 1` (§2.4, §5.3) |
| `!IsSaved` | **No** | No pipe command, window message, or command ID calls `CELProperties::SetSaved(false)`. The existing hook does this via `vtable[0x1B4]` (§5.2) |
| `doc+0x17C != 0` (modified) | **No** | `WM_SETTEXT` to `BCGPEditCtrl` does not set `doc+0x17C`. Only the constructor writes `doc+0x17C = 1`, and it is cleared to `0` by `OnOpenDocument` during pipe-open. No `EN_CHANGE` handler writes `doc+0x17C` (§5.1) |

Two of three conditions cannot be met through high-level operations alone.

### 7.2 Why the current DLL injection approach is necessary

The current hook (`src/delete_hook/dllmain.cpp`) manually sets all three conditions:

1. Calls `vtable[0x1F4]` (loader) → sets `doc+0x174 = 1` (line 108–110)
2. Calls `vtable[0x1B4]` (clear saved) → calls `SetSaved(false)` → `!IsSaved` (line 118–121)
3. Writes `doc+0x17C = 1` directly (line 125)
4. Calls `vtable[0x8c]` (OnCloseDocument) → destructor → `FUN_0044c0f0` (line 133–135)

Without injection, there is no way to set conditions 2 and 3.

### 7.3 Potential partial improvement

If `WM_ELCREATEDOCUMENT` is confirmed to fire for pipe-opened documents (requires dynamic verification), the hook could potentially skip the `vtable[0x1F4]` call (condition 1), as `doc+0x174` would already be set. However:

- This would require waiting for the posted `WM_ELCREATEDOCUMENT` to be processed before proceeding.
- The hook would still need `vtable[0x1B4]` (SetSaved(false)) and direct `doc+0x17C = 1` write.
- The complexity of timing the callback may exceed the simplicity of the current approach.
- **[UNVERIFIED]**: Whether `WM_ELCREATEDOCUMENT` actually fires for pipe-opened documents in all cases (e.g., when the document already exists and is just activated).

### 7.4 Risks of the higher-level approach (if attempted)

Even if the approach were viable, the following risks exist:

1. **Timing**: `WM_ELCREATEDOCUMENT` may be posted (asynchronous), not sent (synchronous). The gate byte and `doc+0x174` would not be set until the message is processed, requiring a wait/poll.
2. **Gate byte clearing**: If any code clears the gate byte (`doc+0x1B8`) before the message is processed (as `FUN_00487980` does), the callback will not fire.
3. **Type-dependent behavior**: `FUN_004417d0` skips type `0xEAEA` and uses a different path for type `0xE9E9` (TradingApp). The loader call only happens for normal types.
4. **Create Entry side effect**: If `doc+0x17C` is `1` when `WM_ELCREATEDOCUMENT` is processed, `FUN_0044ba50` (Create Entry) runs, creating a new library entry and saving — destructive and not reversible.
5. **`WM_SETTEXT` modified flag**: Even if a future TSDev version's `BCGPEditCtrl` calls `SetModifiedFlag` on `WM_SETTEXT`, the MFC `SetModifiedFlag` sets `doc+0x48` (via `FUN_00453a30`), not `doc+0x17C` — the destructor checks `doc+0x17C`, not `doc+0x48`.

### 7.5 Recommendation

The current DLL injection approach should be retained for deletion. It is the only confirmed method to establish all three destructor conditions. The higher-level approach cannot eliminate vtable calls or struct-offset writes because two of three conditions (`!IsSaved` and `doc+0x17C != 0`) have no high-level trigger.

If future work aims to reduce binary coupling, the most promising direction is to investigate whether `CELProperties::SetSaved(false)` can be triggered through a command ID or exported function (reducing reliance on the `vtable[0x1B4]` offset), and whether `doc+0x17C` can be set through a document method rather than a raw offset write. Both would require further reverse engineering of the `CELProperties` class and its interaction with `TSDev.exe` command handlers.
