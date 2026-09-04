# TSDev.exe Delete Mechanism — Reverse-Engineering Findings

Static analysis of **TSDev.exe** (TradeStation 9.5 Development Environment, build timestamp Fri Nov 30 18:39:00 2018, SHA-256 `9099AA600F0A035C0290107EB1852CD61690330C53B14C9F9D01899395DCAF9A`) via the Ghidra MCP server. Every claim cites Ghidra addresses and decompilation evidence. Unproven conclusions are marked **[UNVERIFIED]** with live-verification requirements.

---

## 1. Delete command ID(s) with evidence

### Command ID: `0x80e8` (WM_COMMAND)

**Evidence**: Raw MFC message-map entry scan. Each MFC `ON_COMMAND` entry is 24 bytes: `{nCode=0x0111 (WM_COMMAND), nID, nLastID, nSig=0x3A, pfn}`. A byte-pattern search for the command-ID field `e8 80 00 00 e8 80 00 00` (little-endian `0x80e8` in both `nID` and `nLastID`) found exactly **two** matches:

| Match address | Message map entries start | Handler pointer | Handler function |
|---|---|---|---|
| `0x005775f0` | `0x00577450` | `0x004ac270` | `FUN_004ac270` |
| `0x00579510` | `0x00579418` | `0x004b8a30` | `FUN_004b8a30` |

**Full entry at `0x005775e8`** (first map):
```
11 01 00 00  00 00 00 00  e8 80 00 00  e8 80 00 00  3a 00 00 00  70 c2 4a 00
WM_COMMAND   0x00000000    0x80e8       0x80e8       nSig=0x3A    FUN_004ac270
```

**Full entry at `0x00579508`** (second map):
```
11 01 00 00  00 00 00 00  e8 80 00 00  e8 80 00 00  3a 00 00 00  30 8a 4b 00
WM_COMMAND   0x00000000    0x80e8       0x80e8       nSig=0x3A    FUN_004b8a30
```

**No other message map contains `0x80e8`**: the same byte-pattern search across the entire binary returned only the two addresses above. The command does not appear in the CELDocument map (`0x005678b8`), CELSignalDoc map (`0x0056c4e0`), app-class map (`0x005719b0`), or the main-frame map (built by `FUN_004857d0`).

**No `ON_UPDATE_COMMAND_UI` entry exists for `0x80e8`**: a byte-pattern search for `11 01 00 00 ff ff ff ff e8 80 00 00 e8 80 00 00` (the MFC update-handler signature with `nCode=0xFFFFFFFF`) returned zero matches. The delete command has no enable/disable guard in any message map. **[UNVERIFIED — live]**: whether the tree-view context-menu item is dynamically enabled/disabled by non-message-map code (e.g., a context-menu build function that checks tree selection state).

---

## 2. Handler address and decompilation summary

### Handler 1: `FUN_004ac270` — `CTSDevProjectBar`

**Class identity**: The message map at `0x00577450` has its `GetThisMessageMap` at `FUN_004ab6b0` (stores entries pointer `0x00577450` at global `0x005edf98`, evidence at `0x004ab6bc`). The vtable that references this GetThisMessageMap (at vtable offset `+0x20`, address `0x00577070`) starts at `0x00577040`. The destructor at vtable offset `+0x04` (`FUN_004aa980`, address `0x00577044`) writes `CTSDevProjectBar::vftable` and deallocates an object of size `0x11f0` (4592 bytes). RTTI type descriptor: `.?AVCTSDevProjectBar@@` at `0x005d87f8`. The nearby string `"TDE Project Files (*.elx)|*.tde|All Files (*.*)|*.*||"` at `0x00577618` confirms this is the **Project Bar** (the TDE project tree panel).

**Decompilation** (`FUN_004ac270`):

1. **Reads the selected tree item**: calls `SendMessageW(hwndTree@+0x3d8, 0x110a, 3, selectedItem@+0x11e0)` three times — `0x110a` is `TVM_GETNEXTITEM`, `3` is `TVGN_PARENT`, so it walks up the tree to the parent of the selected item's parent (i.e., the top-level category node). Stores the result at `+0x11e4`.
2. **Gets tree item data and text**: `CTreeCtrl::GetItemData(this+0x3b8, parentItem)` and `CTreeCtrl::GetItemText(this+0x3b8, parentItem)`.
3. **Resolves or opens the document**: calls `FUN_00491760` (check if document is already open by name); if not open, calls `FUN_004944b0` (the same open-document-by-name function used by the pipe `0x8001` path). Sets `doc[0x74]` and `doc[0x73]` (category/type fields).
4. **Compares document name with tree name**: calls `(**doc->vtable[0x204])()` (parent-doc getter; for `CELFunctionDoc` at vtable `0x00568c98`, this is `FUN_0044d2b0` which returns `this`), then `FUN_0044e6c0` (returns `CString` from doc offset `0x1d4` — the document name). Compares it with the tree item text. If they differ, calls `FUN_004ad5f0` (a name-mismatch cleanup).
5. **Calls the common delete routine**: `FUN_00482f50(doc, itemName)`.
6. **If confirmed (returns true)**:
   - `SendMessageW(hwndTree@+0x3d8, 0x1101, 0, selectedItem@+0x11e0)` — `0x1101` is `TVM_DELETEITEM`, deleting the tree item.
   - `FUN_004b7460` — finds the tree-item-to-list mapping entry.
   - `CStringList::Find` + `CStringList::RemoveAt` — removes the item name from an internal `CStringList`.
   - `FUN_004ada30` — post-delete list/panel refresh.
7. **Releases reference-counted tree item data** (decrement at `+0x84`, call vtable destructor if count reaches 1).

### Handler 2: `FUN_004b8a30` — `CTSDevResourceViewBar`

**Class identity**: The message map at `0x00579418` has its `GetThisMessageMap` at `FUN_004b7430` (stores entries pointer `0x00579418` at global `0x005ee108`, evidence at `0x004b743c`). The vtable that references this GetThisMessageMap (at vtable offset `+0x20`, address `0x0057903c`) starts at `0x0057900c`. The destructor at vtable offset `+0x04` (`FUN_004b61e0`, address `0x00579010`) calls `FUN_004b60a0`, which writes `CTSDevResourceViewBar::vftable` and deallocates an object of size `0x11f8` (4600 bytes). RTTI type descriptor: `.?AVCTSDevResourceViewBar@@` at `0x005d899c`. This is the **Resource View Bar** (the EL library tree panel).

**Decompilation** (`FUN_004b8a30`):

1. **Reads the selected tree item**: `CTreeCtrl::GetItemData(this+0x3b8, selectedItem@+0x11e8)` — note the different tree-item field (`+0x11e8` vs `+0x11e0`).
2. **Gets tree item text**: `CTreeCtrl::GetItemText(this+0x3b8, selectedItem)`.
3. **Resolves or opens the document**: calls `FUN_00491760` (check if open); if not open, calls `FUN_004944b0` (open by name). Sets `doc[0x74]` and `doc[0x73]`. Sets a local flag `local_11 = 1` indicating a new document was created.
4. **Compares document name with tree name**: same as handler 1 — `FUN_0044e6c0` returns doc name from `+0x1d4`, compares with tree text. If mismatch, calls `FUN_004b94a0` (equivalent cleanup).
5. **Calls the common delete routine**: `FUN_00482f50(doc, itemName)`.
6. **If NOT confirmed (returns false)**: if `local_11` (new doc was created), calls `(**doc->vtable[0x8c])()` — `OnCloseDocument` (`FUN_004508e0`) to close the temporary document opened in step 3.
7. **If confirmed (returns true)**:
   - `SendMessageW(hwndTree@+0x3d8, 0x1101, 0, selectedItem@+0x11e8)` — `TVM_DELETEITEM`, deleting the tree item.
   - `FUN_004b7460` — finds the list mapping.
   - `CStringList::Find` + `CStringList::RemoveAt` — removes the name from `CStringList` at `list+0xc`.
   - `FUN_004b97c0` — post-delete list/panel refresh.
8. **Releases reference-counted tree item data**.

### Common delete routine: `FUN_00482f50`

**Decompilation** (disassembly at `0x00482f50`–`0x0048300b`):

1. **Formats the confirmation message**: constructs a `CString` from the item name (via `[0x0055fc24]` → `CString::CString`), then calls `FUN_00483020` (the confirmation helper).
2. **If confirmed** (`FUN_00483020` returns true):
   - `FUN_0044eae0(doc)` — resolves/creates a document-associated object by walking the parent chain (follows `+0x1bc` for linked documents of type `0xE8E8`), then gets/creates a save-context object at `+0x1c4`.
   - `FUN_0044e6c0(doc)` — returns `BSTR` from doc name at `+0x1d4`.
   - `[0x0055f9e8]` — `CString::CString` copy.
   - `[0x0055fed0]` — likely `CString::AllocSysString` or a string-list add.
   - `FUN_00421040(doc, name)` — **WinForm designer cleanup**: if `doc+0x10c` (a `WFDesignerManager*`) is non-null and its form name matches the item name, calls `WFDesignerManager::~WFDesignerManager` and frees the object (size `0xb8`). This is only relevant for TradingApp/WinForm items.
   - `[0x0055febc]` — `CString::~CString`.
   - `(**doc->vtable[0x204])()` — gets the parent document. For `CELFunctionDoc`, `vtable[0x204]` = `FUN_0044d2b0` which returns `this`. For other doc types, it may return a different parent.
   - If parent is non-null: `FUN_0044c080(parent, itemName)` — **the actual library-item save/delete operation** (see below).
   - `(**doc->vtable[0x8c])()` — **`OnCloseDocument`** (`FUN_004508e0` for `CELFunctionDoc`): closes the document, its child documents, and associated views, then calls `COleServerDoc::OnCloseDocument`.
3. **Returns** the confirmation result.

### `FUN_0044c080` — the library-item save-and-remove

**Decompilation**:

1. `FUN_00448210` — `std::map` lower_bound lookup for the item name.
2. `FUN_00455030` — `std::map` erase/remove the entry.
3. `(**doc->vtable[0x6c])(1)` — marks the document modified (`SetModifiedFlag(1)`).
4. `FUN_00452290` — **the save function**: calls `_SaveModule_New@20` (imported, string at `0x005bbfa4`) with the document's category, type, flags, and password. This is the same external save function used by the UI save path (see KNOWLEDGE_BASE.md §"UI save path"). The save writes the library state **without** the deleted item.
5. `FUN_0044eae0` — re-resolves the save context.
6. `FUN_0042b420` — post-save view/editor refresh.
7. `(**view->vtable[0x21c])()` — view notification.
8. `FUN_004f3960` — additional post-save cleanup.

**Key finding**: the actual library deletion is performed by `_SaveModule_New@20` — an **imported function** (external to TSDev.exe). The delete path removes the item from TSDev's internal `std::map` (the in-memory library index) and then saves the library state through the same external save function the UI uses. Per `AGENTS.md`, the callee's internals are out of scope; only the import name is recorded: **`_SaveModule_New@20`** (string at `0x005bbfa4`).

### `FUN_004508e0` — `OnCloseDocument` (vtable+0x8c)

**Decompilation**: closes child documents (iterates `+0x32c` list, calls `vtable[0x8c]` on each), handles `0xE8E8` (partial-class) and `0xE9E8` (WFDesigner) document types specially, sends a `SendMessageW` notification to the main window, then calls `COleServerDoc::OnCloseDocument`. This is the standard MFC document closure path — it destroys the document's views and MDI child windows.

---

## 3. Routing classification

**`0x80e8` is a tree-view/context-menu command on two specific CBCGP control bars, NOT a frame, document, or app-class command.**

| Routing target | Applicable? | Evidence |
|---|---|---|
| Main frame | **No** | `0x80e8` not in frame map (`FUN_004857d0`) |
| MDI child / active document | **No** | `0x80e8` not in CELDocument or CELSignalDoc maps |
| App class | **No** | `0x80e8` not in app-class map (`0x005719b0`) |
| `CTSDevProjectBar` (Project Bar) | **Yes** | Message map at `0x00577450`, handler `FUN_004ac270` |
| `CTSDevResourceViewBar` (Resource View Bar) | **Yes** | Message map at `0x00579418`, handler `FUN_004b8a30` |

**MFC command routing for control bars**: MFC routes `WM_COMMAND` through `OnCmdMsg` in the order: active view → active document → frame → application. Control bars receive commands via the frame's `OnCmdMsg` dispatching to docked control bars. Therefore, posting `WM_COMMAND 0x80e8` to the **main frame** should route to whichever control bar (Project Bar or Resource View Bar) has the handler, if the bar's `OnCmdMsg` is called.

**However**, MFC control-bar command routing requires the command to not be handled by the view/document/frame first. Since `0x80e8` is not in any of those maps, the frame's `OnCmdMsg` will eventually offer it to control bars. **[UNVERIFIED — live]**: whether posting `0x80e8` to the main frame HWND actually reaches the control bar handler, or whether it must be posted directly to the control bar window. The control bars are CBCGP docked windows; their HWNDs are children of the frame.

**Critical routing dependency**: both handlers read the **currently selected tree item** from the control bar's internal state (`+0x11e0` or `+0x11e8`). The delete operates on whatever tree item is selected in the tree control at the time the command is dispatched. There is **no name parameter** — the item name is obtained from the tree selection.

---

## 4. Preconditions and guards

### No `ON_UPDATE_COMMAND_UI` handler

No update-command handler exists for `0x80e8` (evidence: byte-pattern search returned zero matches). The command is always enabled when routed. **[UNVERIFIED — live]**: whether the context-menu item is conditionally shown/enabled by the context-menu build code (not via `ON_UPDATE_COMMAND_UI`).

### Tree selection precondition

Both handlers require a valid selected tree item:
- Handler 1 (`FUN_004ac270`): reads `+0x11e0`, walks up the tree via `TVM_GETNEXTITEM(TVGN_PARENT)` three times to find the top-level category node. If the selected item has no parent at the expected level, the tree walk may produce a null/invalid item. **[UNVERIFIED — live]**: behavior when no item is selected or the selection is at an unexpected tree level.
- Handler 2 (`FUN_004b8a30`): reads `+0x11e8` and calls `FUN_004b98d0` to resolve the tree item data. If `FUN_004b98d0` returns null, the handler returns immediately without action.

### Document open/create behavior

Both handlers call `FUN_00491760` to check if a document for the item is already open. If not, they call `FUN_004944b0` (the pipe open path) to open it. For a **nonexistent** item, `FUN_004944b0` creates a **phantom document** (per KNOWLEDGE_BASE.md). Handler 2 sets a flag (`local_11`) when it creates a new document, and closes it via `OnCloseDocument` if the user cancels. **[UNVERIFIED — live]**: what happens when deleting a phantom document (the item doesn't exist) — whether the save call fails, produces an error dialog, or silently no-ops.

### Read-only / built-in items

No explicit read-only or built-in guard was found in the handler or common delete routine. **[UNVERIFIED — live]**: whether `_SaveModule_New@20` (the external save function) rejects deletion of built-in/read-only items with an error dialog.

### Dirty / unsaved documents

No dirty-document guard was found. The delete path calls `SetModifiedFlag(1)` on the parent document and then saves. **[UNVERIFIED — live]**: whether deleting an item from a dirty document prompts a save dialog.

---

## 5. Confirmation-dialog details

### Primary confirmation: `FUN_00483020`

**Decompilation**:
```c
bool FUN_00483020(undefined4 param_1) {
    // ... CString construction and Format with the item name ...
    int result = AfxMessageBox(text, 1, 0);
    // ... CString cleanup ...
    return result != 2;  // true unless IDCANCEL
}
```

- **Function called**: `AfxMessageBox` (thunk at `0x0053fbd4`, confirmed by Ghidra function name).
- **Style parameter**: `uVar3 = 1` → `MB_OKCANCEL` (0x01).
- **Return logic**: `return iVar2 != 2` → returns `true` for `IDOK` (1), `false` for `IDCANCEL` (2).
- **Message text**: formatted via `CString::Format` with the item name. The format string is passed as a parameter from `FUN_00482f50`. **[UNVERIFIED]**: the exact format string text. The string `"%s will be deleted and AnalysisTechnique saved"` at `0x0056d868` is a candidate, but its relationship to this specific call has not been confirmed by xref (no xrefs found to `0x0056d868`). **Live verification needed**: capture the dialog text during a live delete.
- **Dialog class**: `AfxMessageBox` creates a standard MFC message box, which is a `#32770` (dialog) class window. **[UNVERIFIED — live]**: confirm the window class via `Spy++` or `GetClassName` during a live delete.
- **Title**: MFC's `AfxMessageBox` uses the application name as the title by default. **[UNVERIFIED — live]**.
- **Button IDs**: `IDOK` = 1, `IDCANCEL` = 2 (standard Win32).

### Second removal string (unrelated to library delete)

The string `"' will be removed.\n\nContinue?"` at `0x00579ba8` and `"Items on tab '"` at `0x00579b88` are used in a **different** code path at `~0x004bf70b` (not in a recognized function). The disassembly at `0x004bf748` shows:
```asm
PUSH 0x31          ; MB_OKCANCEL (0x01) | MB_ICONEXCLAMATION (0x30)
PUSH EAX           ; formatted text
CALL AfxMessageBox
CMP EAX, 0x1       ; IDOK
JNZ cancel_label
```
This path uses `TSMessageBox` style `0x31` and is about removing **items from a tab** (WinForm designer tab removal), not deleting library items. It is **not** related to command `0x80e8`.

---

## 6. Success/failure observables

### On successful deletion (confirmed)

| Observable | Evidence |
|---|---|
| Tree item removed | `SendMessageW(tree, 0x1101, 0, item)` = `TVM_DELETEITEM` in both handlers |
| Item name removed from internal list | `CStringList::RemoveAt` in both handlers |
| Library state saved without the item | `FUN_0044c080` → `FUN_00452290` → `_SaveModule_New@20` |
| Document closed | `(**doc->vtable[0x8c])()` = `OnCloseDocument` (`FUN_004508e0`) in `FUN_00482f50` |
| MDI child window destroyed | `OnCloseDocument` calls `COleServerDoc::OnCloseDocument` which destroys views |
| Post-delete panel refresh | `FUN_004ada30` (handler 1) or `FUN_004b97c0` (handler 2) |
| WinForm designer cleaned up (TradingApp only) | `FUN_00421040` destroys `WFDesignerManager` if form name matches |

### On cancellation (IDCANCEL)

| Observable | Evidence |
|---|---|
| Tree item NOT removed | `FUN_00482f50` returns false; `TVM_DELETEITEM` not called |
| Document NOT saved | `FUN_0044c080` not called |
| Temporary document closed (handler 2 only) | If `local_11` flag set, `OnCloseDocument` called on the newly-created doc |
| Existing document left open (handler 2) | If doc was already open (`local_11` = 0), it remains open |

### On nonexistent item

**[UNVERIFIED — live]**: Both handlers call `FUN_004944b0` which creates a phantom document for nonexistent items. The delete path would then attempt to save with `_SaveModule_New@20` using the phantom document's data. The external save function's behavior with a phantom document is unknown. It may:
- Silently no-op (the item doesn't exist in the library to delete).
- Show an error dialog.
- Corrupt the library index. **This is a risk that must be verified live before any automation attempts to delete nonexistent items.**

### On external save failure

**[UNVERIFIED — live]**: `FUN_00452290` checks the return value of `_SaveModule_New@20`. If it returns 0 (failure), the function sets `doc[0x5f] = 0` and calls `vtable[0x6c](0)` (SetModifiedFlag(0)), but does not show an error dialog itself. The external save function may show its own error dialog. **Live verification needed**: monitor for error dialogs after a save failure.

---

## 7. Recommended external trigger sequence

**WARNING**: This sequence is based on static analysis only. Every step marked **[UNVERIFIED]** must be confirmed by live testing before production use. The sequence involves operations that could corrupt the library if the analysis is incorrect.

### Prerequisites

1. TSDev.exe is running (launched hidden or attached to an existing instance).
2. The target item exists in the library (verify first via the `exists` verb from KNOWLEDGE_BASE.md).

### Proposed sequence

1. **Open the target item via the pipe**: send `"EncodedName", <type>` to `\\.\Pipe\TSDevPipe`. This opens (or phantom-creates) the document and makes it the active MDI child. Wait for the editor content to stabilize (per the exists-verb polling technique).

2. **Verify the item exists**: use the `exists` discriminator (check editor content for the phantom error page). If it doesn't exist, abort with "item not found".

3. **Select the item in the tree control**: **This is the hardest step and the biggest gap in the automation.** The delete command operates on the **tree selection**, not the active document. The automation must:
   - Find the `CTSDevProjectBar` or `CTSDevResourceViewBar` window (a docked CBCGP control bar).
   - Find the `SysTreeView32` child control (class string at `0x0057309c`).
   - Locate the tree item corresponding to the target name and type.
   - Select it via `TVM_SELECTITEM` (`0x110b`, `TVGN_CARET`).
   - **[UNVERIFIED — live]**: whether the control bar's `+0x11e0`/`+0x11e8` field is updated by `TVM_SELECTITEM` or only by internal `TVN_SELCHANGED` handling. If the field is not updated, posting `0x80e8` will delete the wrong item or no item.

4. **Post the delete command**: `PostMessage(hwndTarget, WM_COMMAND, 0x80e8, 0)` where `hwndTarget` is either:
   - The main frame HWND (if MFC routes to the control bar). **[UNVERIFIED]**.
   - The control bar's HWND directly. **[UNVERIFIED — live]**: which HWND receives the command.

5. **Handle the confirmation dialog**: `AfxMessageBox` creates a `#32770` dialog. Detect it (e.g., `EnumWindows` for a child `#32770` of the frame) and send `WM_COMMAND IDOK (1)` to dismiss it. **[UNVERIFIED — live]**: the exact dialog detection criteria (title, text, class).

6. **Wait for completion**: wait for the MDI child window to close (the document's `OnCloseDocument` destroys it) and the tree item to disappear. **[UNVERIFIED — live]**: timing and synchronization.

7. **Verify deletion**: re-open the item via the pipe and check for the phantom error page (the `exists` discriminator). If the item is gone, deletion succeeded.

8. **Clean up**: if any phantom or temporary document remains open, close it.

### Alternative approach (lower risk)

Instead of driving the tree-view command, consider:
- **Injecting a call to `FUN_00482f50`** directly with the target document pointer and item name. This bypasses the tree-selection dependency but requires knowing the document object address (version-specific internal state). Per `AGENTS.md`, injection into TSDev.exe is permitted, but reliance on binary internals should be minimized.
- **Injecting a call to `FUN_0044c080`** (the save-and-remove function) with the parent document and item name. This is even more direct but more brittle.

Both alternatives require resolving the document object at runtime, which requires either hooking the open path or enumerating open documents — additional complexity.

---

## 8. Open questions and risks

### Must verify live before any automation

1. **Command routing**: Does `WM_COMMAND 0x80e8` posted to the main frame reach the control bar handler? Or must it be posted to the control bar's HWND? (§3)
2. **Tree selection sync**: Does `TVM_SELECTITEM` update the control bar's `+0x11e0`/`+0x11e8` field? If not, how is the field set? (§7 step 3)
3. **Confirmation dialog**: exact title, text, and class of the `AfxMessageBox` dialog. (§5)
4. **Nonexistent item behavior**: what happens when the delete path runs on a phantom document? Does `_SaveModule_New@20` error, no-op, or corrupt? (§6)
5. **Read-only/built-in guard**: does the external save function reject deletion of built-in items? (§4)
6. **Context-menu origin**: how is the delete command triggered in the UI? Is it a right-click context menu on the tree, a menu item, or a keyboard shortcut? This affects how to identify the correct tree control. **[UNVERIFIED]**.
7. **Which control bar to target**: both `CTSDevProjectBar` and `CTSDevResourceViewBar` have the handler. Which one is visible/active by default? Are both present? Does it matter which one receives the command? **[UNVERIFIED — live]**.
8. **Error dialogs**: does `_SaveModule_New@20` show error dialogs on failure? What about `TSMessageBox` (import at `EXTERNAL:00000010`)? (§6)

### Architectural risks

1. **Tree-selection dependency is fragile**: the delete command has no name parameter — it operates on the tree selection. Automating this requires finding and selecting the correct tree item, which involves tree enumeration and item matching. This is more complex and brittle than the pipe-based open path.
2. **No high-level external trigger for delete**: unlike open (which has the pipe), delete has no pipe command. The only external trigger is `WM_COMMAND 0x80e8` + tree selection + confirmation dialog handling. This is a multi-step Win32 message sequence, not a single pipe write.
3. **Phantom-document risk**: opening a nonexistent item via the pipe creates a phantom. If the delete path runs on a phantom, the behavior is unknown and potentially dangerous.
4. **Two handlers, same command**: the command exists in two control bars. If both are present and both handle the command, posting to the frame may dispatch to the wrong one (or both). **[UNVERIFIED]**: MFC control-bar routing order.
5. **External save function**: `_SaveModule_New@20` is outside TSDev.exe. Its behavior on delete (vs. save) is inferred from context (the item is removed from the internal map before the save), not directly verified. Per `AGENTS.md`, its internals are out of scope.

### Lower-priority open questions

1. The exact format string for the confirmation message (candidate: `"%s will be deleted and AnalysisTechnique saved"` at `0x0056d868`, but no xref confirmed).
2. Whether `FUN_004ad5f0` (handler 1) and `FUN_004b94a0` (handler 2) perform any significant work on name mismatch — they appear to be cleanup functions but were not fully decompiled.
3. The relationship between `FUN_004ada30` and `FUN_004b97c0` (post-delete refresh) — whether they trigger any additional side effects.

---

## Evidence index

| Address | Type | Description |
|---|---|---|
| `0x005775e8`–`0x00577607` | Data | Message-map entry: `WM_COMMAND 0x80e8 → FUN_004ac270` (CTSDevProjectBar) |
| `0x00579508`–`0x00579527` | Data | Message-map entry: `WM_COMMAND 0x80e8 → FUN_004b8a30` (CTSDevResourceViewBar) |
| `0x004ac270` | Code | Handler 1: CTSDevProjectBar delete handler |
| `0x004b8a30` | Code | Handler 2: CTSDevResourceViewBar delete handler |
| `0x00482f50` | Code | Common delete routine (confirmation + save + close) |
| `0x00483020` | Code | Confirmation helper (`AfxMessageBox(text, 1, 0)`) |
| `0x0044c080` | Code | Library-item save-and-remove (map erase + `_SaveModule_New@20`) |
| `0x00452290` | Code | Save function (calls `_SaveModule_New@20`) |
| `0x005bbfa4` | String | `_SaveModule_New@20` import name |
| `0x004508e0` | Code | `OnCloseDocument` (vtable+0x8c for CELFunctionDoc) |
| `0x00568c98` | Data | CELFunctionDoc vtable |
| `0x0044d2b0` | Code | vtable+0x204 for CELFunctionDoc (returns `this`) |
| `0x0044e6c0` | Code | Returns doc name BSTR from offset `+0x1d4` |
| `0x0044eae0` | Code | Resolves/creates save-context object at `+0x1c4` |
| `0x00421040` | Code | WinForm designer cleanup (WFDesignerManager destruction) |
| `0x004ab6b0` | Code | CTSDevProjectBar `GetThisMessageMap` |
| `0x004b7430` | Code | CTSDevResourceViewBar `GetThisMessageMap` |
| `0x004aa980` | Code | CTSDevProjectBar destructor (writes `CTSDevProjectBar::vftable`) |
| `0x004b60a0` | Code | CTSDevResourceViewBar destructor body (writes `CTSDevResourceViewBar::vftable`) |
| `0x005d87f8` | String | `.?AVCTSDevProjectBar@@` (RTTI) |
| `0x005d899c` | String | `.?AVCTSDevResourceViewBar@@` (RTTI) |
| `0x00577618` | String | `"TDE Project Files (*.elx)|*.tde|..."` (Project Bar context) |
| `0x0057309c` | String | `"SysTreeView32"` (tree control class) |
| `0x0056d868` | String | `"%s will be deleted and AnalysisTechnique saved"` (candidate confirmation format) |
| `0x00579b88` | String | `"Items on tab '"` (unrelated tab-removal path) |
| `0x00579ba8` | String | `"' will be removed.\n\nContinue?"` (unrelated tab-removal path) |
| `0x004bf70b` | Code | Unrelated tab-removal confirmation path (not library delete) |
| `0x0053fbd4` | Code | `AfxMessageBox` thunk |
