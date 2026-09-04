# TSDev.exe Delete UI Flow — Reverse-Engineering Research

Static analysis of **TSDev.exe** (TradeStation 9.5 Development Environment, build timestamp Fri Nov 30 18:39:00 2018, SHA-256 `9099AA600F0A035C0290107EB1852CD61690330C53B14C9F9D01899395DCAF9A`) via the Ghidra MCP server. Every claim cites Ghidra addresses and decompilation evidence. Unproven conclusions are marked **[UNVERIFIED]**.

This document supersedes the deletion-flow analysis in `docs/RE_DELETE_FINDINGS.md` §7 (recommended trigger sequence) and `docs/RE_DELETE_LIVE_VERIFY.md` §5.2, which speculated that the UI command path could be used for deletion. The evidence below shows why it cannot for Function documents.

---

## 1. Command ID `0x80e8` — MFC message maps and handlers

### Message-map entries (confirmed)

Two `ON_COMMAND` entries for `0x80e8` exist, exactly as documented in `RE_DELETE_FINDINGS.md` §1:

| Message map | Entry address | Handler | Class |
|---|---|---|---|
| `0x00577450` | `0x005775e8` | `FUN_004ac270` | `CTSDevProjectBar` |
| `0x00579418` | `0x00579508` | `FUN_004b8a30` | `CTSDevResourceViewBar` |

No other message map in the binary contains `0x80e8`. No `ON_UPDATE_COMMAND_UI` entry exists.

### Handler 1: `FUN_004ac270` (CTSDevProjectBar)

Full decompilation analyzed. Sequential flow:

1. **Tree-item walk**: `SendMessageW(tree@+0x3d8, TVM_GETNEXTITEM=0x110a, TVGN_PARENT=3, selectedItem@+0x11e0)` called three times, walking to the grandparent node. Result stored at `this+0x11e4`.
2. **Item data/text**: `CTreeCtrl::GetItemData(this+0x3b8, parentItem)` and `CTreeCtrl::GetItemText(this+0x3b8, parentItem)`.
3. **Document resolution**: `FUN_00491760()` checks if a document with the item name is already open. If not found:
   - Calls `FUN_004ca510()` and retries `FUN_00491760()` with adjusted type.
   - If still not found, calls `FUN_004944b0()` (pipe-open path) to open/create the document.
   - Sets `doc[0x74]` and `doc[0x73]` (category/type fields).
4. **Parent lookup**: `(**doc->vtable[0x204])()` — for `CELFunctionDoc`, vtable `0x00568c98` + `0x204` = `FUN_0044d2b0`, which returns `this` (the document itself).
5. **Name comparison**: `FUN_0044e6c0()` returns the doc name from `doc+0x1d4`; compared with tree item text. If mismatch, calls `FUN_004ad5f0()`.
6. **Delete call**: `FUN_00482f50(doc, itemName)`.
7. **If confirmed (returns true)**: `TVM_DELETEITEM` on tree, `CStringList::RemoveAt`, `FUN_004ada30()` refresh.
8. **Refcount release** on tree item data.

**Critical observation**: When the document is not already open, this handler opens it via `FUN_004944b0` (pipe-open). It does **NOT** call `vtable[0x1F4]` (`FUN_0044f8e0`), the function that sets the "opened from library" flag at `doc+0x174`. See §3 below.

### Handler 2: `FUN_004b8a30` (CTSDevResourceViewBar)

Full decompilation analyzed. Sequential flow:

1. **Tree-item read**: `CTreeCtrl::GetItemData(this+0x3b8, selectedItem@+0x11e8)`.
2. **Item data/text**: `FUN_004b98d0()` resolves tree data; `FUN_004c9be0()` gets encoded name; `CTreeCtrl::GetItemText` gets display text.
3. **Document resolution**: `FUN_00491760()` checks if open. If not found:
   - Sets `local_11 = 1` (flag: "we opened a temporary document").
   - Calls `FUN_004944b0()` (pipe-open).
   - Sets `doc[0x74]` and `doc[0x73]`.
   - Calls `(**doc->vtable[0x204])()`.
   - Gets doc name via `FUN_0044e6c0()`.
4. **Name comparison**: same as handler 1; mismatch calls `FUN_004b94a0()`.
5. **Delete call**: `FUN_00482f50(doc, itemName)`.
6. **If NOT confirmed**: if `local_11`, calls `vtable[0x8c]` (`OnCloseDocument`) to close the temporary document.
7. **If confirmed**: `TVM_DELETEITEM`, `CStringList::RemoveAt`, `FUN_004b97c0()` refresh.
8. **Refcount release**.

**Same critical observation**: Opens via `FUN_004944b0` (pipe-open), does **NOT** call `vtable[0x1F4]`.

### Differences between handlers

| Aspect | `FUN_004ac270` (ProjectBar) | `FUN_004b8a30` (ResourceViewBar) |
|---|---|---|
| Tree item field | `+0x11e0` (walks parent 3×) | `+0x11e8` (direct) |
| Temp-doc tracking | No explicit flag | `local_11` flag; closes temp doc on cancel |
| Name-mismatch cleanup | `FUN_004ad5f0` | `FUN_004b94a0` |
| Post-delete refresh | `FUN_004ada30` | `FUN_004b97c0` |
| Open path | `FUN_004944b0` (pipe) | `FUN_004944b0` (pipe) |
| Calls `vtable[0x1F4]`? | **No** | **No** |

Both handlers are functionally equivalent for the deletion path: they open the document via the pipe path and call `FUN_00482f50`. Neither sets `doc+0x174`.

---

## 2. `FUN_00482f50` — the common delete routine

### Full decompilation (`0x00482f50`)

```c
char __fastcall FUN_00482f50(int *param_1) {
    // 1. Build CString from item name (passed on stack)
    cVar1 = FUN_00483020();           // Confirmation dialog (AfxMessageBox, MB_OKCANCEL)
    if (cVar1 != '\0') {              // If confirmed (IDOK)
        iVar2 = FUN_0044eae0();       // Resolve save context
        if (iVar2 != 0) {
            // WinForm cleanup (FUN_00421040) — TradingApp only
        }
        iVar2 = (**(code **)(*param_1 + 0x204))();  // vtable[0x204]: parent/context
        if (iVar2 != 0) {
            FUN_0044c080();            // Map erase + SetModifiedFlag(1) + save
        }
        (**(code **)(*param_1 + 0x8c))();  // vtable[0x8c]: OnCloseDocument
    }
    return cVar1;
}
```

### What `FUN_00482f50` does to the document

1. **Confirmation**: `FUN_00483020` shows `AfxMessageBox(text, MB_OKCANCEL, 0)`. Returns `true` for IDOK, `false` for IDCANCEL.
2. **Save context**: `FUN_0044eae0` resolves the save-context chain.
3. **WinForm cleanup**: `FUN_00421040` destroys `WFDesignerManager` if applicable (TradingApp only).
4. **Parent lookup**: `vtable[0x204]` — for `CELFunctionDoc` (`FUN_0044d2b0` at vtable `0x00568c98` + `0x204`), returns `this` (always non-zero).
5. **Map erase + save**: `FUN_0044c080` is called because the parent is non-zero.
6. **Close**: `vtable[0x8c]` (`FUN_004508e0`, `OnCloseDocument`) is called unconditionally (if confirmed).

### `FUN_0044c080` — map erase + save

```c
void __thiscall FUN_0044c080(int *doc, undefined4 parentContext) {
    FUN_00448210(&local_c, parentContext);   // std::map lower_bound lookup
    FUN_00455030(&parentContext, local_c, local_8);  // std::map erase
    (**(code **)(*doc + 0x6c))(1);           // vtable[0x6c] (FUN_00453a30) sets doc+0x48 (MFC m_bModified), NOT doc+0x17C
    FUN_00452290();                          // Save (calls _SaveModule_New@20)
    FUN_0044eae0();                          // Re-resolve save context
    FUN_0042b420();                          // Post-save refresh
    piVar1 = FUN_0044f0f0();                 // Get view
    (**(code **)(*piVar1 + 0x21c))();       // View notification
    FUN_004f3960();                          // Additional cleanup
}
```

`FUN_00448210` (`0x00448210`) is a `std::map` lower_bound search by string key. `FUN_00455030` (`0x00455030`) is `std::map::erase` by iterator range. The map being searched is the document's child-item map at `doc+0x32c`.

**For Function documents, this map is empty** (confirmed live: `RE_DELETE_LIVE_VERIFY.md` §2, "sentinel == root node"). The erase is a no-op. The save via `FUN_00452290` re-saves the item as-is.

### `FUN_00452290` — the save function

```c
void __fastcall FUN_00452290(int *doc) {
    sVar6 = CONCAT11(doc[0x1ea], doc[0x7a]);  // type/subtype combined
    doc[200] = 0;                              // doc+0x1C8 (0xC8 * 4)
    if ((sVar6 == 0xE9E9) || (sVar6 == 0xEAEA)) {
        // Special types: recursive parent save
        (**(code **)(*doc + 0x204))();
        FUN_00452290();                        // Recursive
        (**(code **)(*doc + 0x6c))(0);         // SetModifiedFlag(0)
        doc[0x69] = 0;
    } else {
        // Normal types (Function=1, Indicator=2, etc.)
        this = (CELProperties *)(doc + 0x84);
        iVar2 = CELProperties::AreValid(this);
        if (iVar2 != 0) {
            // ... property setup, encryption, flags ...
            iVar2 = _SaveModule_New_20(type, subtype, flags, hwnd, password);
            if (iVar2 != 0) {                  // Save succeeded
                doc[0x5f] = 0;                 // doc+0x17C = 0 (clear modified flag)
                (**(code **)(*doc + 0x6c))(0); // SetModifiedFlag(0)
                doc[0x65] = 1;
                doc[0x69] = 0;
            }
            // ... post-save cleanup ...
        }
    }
}
```

**Critical**: If `_SaveModule_New_20` succeeds (returns non-zero), `doc+0x17C` is cleared to 0. This is the same modified flag the destructor checks.

### Does `FUN_00482f50` delete Function items from the library?

**No.** The evidence chain:

1. `FUN_0044c080` calls `vtable[0x6c](1)` (`FUN_00453a30`), which sets `doc+0x48` (MFC `m_bModified`), NOT `doc+0x17C` (CELDocument's own modified flag). See `RE_DELETE_VIEW_ACTIVATION.md` §1.4 for the correction.
2. `FUN_00452290` saves via `_SaveModule_New_20`. If save succeeds, `doc+0x17C` is cleared to 0 (it was already 0 from pipe-open; this is a no-op for the modified flag).
3. `FUN_00482f50` then calls `OnCloseDocument`, which eventually invokes the destructor `FUN_0044a640`.
4. The destructor checks `doc+0x174` (opened-from-library flag). If 0, it skips `FUN_0044c0f0` entirely.
5. Even if `doc+0x174` were 1, the destructor also checks `doc+0x17C != 0`. But `doc+0x17C` was never set to 1 by `FUN_0044c080` (it sets `doc+0x48`, not `doc+0x17C`), and was cleared to 0 by `OnOpenDocument` during pipe-open.

**Therefore**: `FUN_00482f50` does NOT trigger library deletion for Function documents, regardless of `doc+0x174` state. `doc+0x17C` was never set to 1 (vtable[0x6c] sets `doc+0x48`, not `doc+0x17C`), and was cleared to 0 by `OnOpenDocument` during pipe-open, so the destructor's modified-flag condition fails.

This was confirmed by live verification: `RE_DELETE_LIVE_VERIFY.md` §2, "Failed approach: direct `FUN_00482f50` invocation" — `_SaveModule_New_20` returned 0x0 (success), item NOT deleted.

### Does `FUN_00482f50` check whether the document is open?

No. `FUN_00482f50` receives a document pointer and operates on it directly. It does not check whether the document is open or in any particular state. It always calls `OnCloseDocument` if confirmed.

### Does `FUN_00482f50` close the document?

Yes. It calls `vtable[0x8c]` (`OnCloseDocument` / `FUN_004508e0`) unconditionally when confirmed. This closes the document, destroys its views and MDI child window, and invokes the destructor.

---

## 3. The `doc+0x174` flag — "opened from library"

### Discovery

The destructor `FUN_0044a640` gates `FUN_0044c0f0` (actual library deletion) on `param_1[0x5d] != 0` (decompiler int-pointer notation; byte offset `0x5d * 4 = 0x174`):

```c
void __fastcall FUN_0044a640(undefined4 *param_1) {
    *param_1 = CELDocument::vftable;           // Revert to base vtable
    if (param_1[0x5d] != 0) {                  // doc+0x174: opened-from-library flag
        // ... cleanup (FUN_00491460, FUN_00477b40) ...
        bVar1 = CELProperties::IsSaved((CELProperties *)(param_1 + 0x84));
        if ((!bVar1) && (param_1[0x5f] != 0)) { // !IsSaved AND doc+0x17C (modified)
            FUN_0044c0f0();                     // ACTUAL LIBRARY DELETION
        }
    }
    // ... rest of destructor (free strings, properties, etc.) ...
}
```

### Who writes `doc+0x174`?

A byte-pattern search for `C7 8? 74 01 00 00 01 00 00 00` (MOV DWORD PTR [reg+0x174], 1) found exactly three sites:

| Address | Function | Context |
|---|---|---|
| `0x0044ba9e` | `FUN_0044ba50` | "Create Entry" — called when a new item is saved to the library |
| `0x0044ffd0` | `FUN_0044f8e0` | Document loader (vtable[0x1F4]) — called after loading an existing item |
| `0x00457f1b` | `FUN_00457eb0` | `CELFunctionDoc` constructor — sets flag if `FUN_0044f670()` returns true |

A search for `C7 8? 74 01 00 00 00 00 00 00` (MOV DWORD PTR [reg+0x174], 0) found additional sites that clear the flag:

| Address | Function | Context |
|---|---|---|
| `0x00449edb` | `FUN_00449ea0` | `CELDocument` base constructor — initializes to 0 |
| `0x0044fe2f` | `FUN_0044f8e0` | Document loader — clears at start before potentially setting to 1 |
| `0x00451020` | `FUN_00450f30` | `OnOpenDocument` (vtable[0x84]) — clears to 0 |
| `0x00453683` | `FUN_004535d0` | Archive serialize — clears when archive version == 2 |

### `FUN_0044f8e0` (vtable[0x1F4]) — the flag setter

This is the key function. It is at vtable offset `0x1F4` (decimal 500) for `CELFunctionDoc` (vtable `0x00568c98` + `0x1F4` = `0x00568e8c`, confirmed by memory read).

Decompilation shows:

1. **Clears flags at start**: `param_1[0x5f] = 0` (doc+0x17C), `param_1[0x5d] = 0` (doc+0x174).
2. **Looks up item**: calls `_FindExistPosSubType_12()` to find the item in the library.
3. **If item found** (not `0xFFFFFFFF`):
   - Sets subtype, calls `FUN_004502f0()`.
   - Does various property checks (hidden, date, token-only).
   - In the "item loaded successfully" path: calls `vtable[0x1FC]()` and then **sets `param_1[0x5d] = 1`** (doc+0x174 = 1).
4. **If item not found**: shows error message, does not set the flag.

### `FUN_00450f30` (OnOpenDocument, vtable[0x84]) — the flag clearer

This is called by the pipe-open path (`FUN_004944b0` → `FUN_004942b0` → `vtable[0x8c]`). It:

1. Sets `param_1[0x5f] = 0` (clears modified flag).
2. Sets `param_1[0x5d] = 0` (clears opened-from-library flag).
3. Calls `_FindExistPosSubType_12()` to find the item.
4. If found, does various setup but does **NOT** call `vtable[0x1F4]` or set `doc+0x174 = 1`.

### The critical difference: tree-view open vs. pipe open

| Open path | Called by | Calls `vtable[0x1F4]`? | Sets `doc+0x174`? |
|---|---|---|---|
| `FUN_004b0800` (tree-view open) | Double-click in tree | **Yes** — explicitly calls `vtable[500]` after `vtable[0x8c]` | **Yes** (if item found) |
| `FUN_004944b0` (pipe open) | Pipe command, delete handlers | **No** | **No** (cleared to 0 by `FUN_00450f30`) |

**Evidence for `FUN_004b0800`** (decompilation at `0x004b0800`):
```c
piVar5 = (int *)(**(code **)(iVar2 + 0x8c))(pwVar3, uVar7);  // OnOpenDocument
if (piVar5 != (int *)0x0) {
    iVar2 = (**(code **)(*piVar5 + 500))();  // vtable[0x1F4] = FUN_0044f8e0
    // ...
}
```

`FUN_004b0800` is called from `FUN_004b0750` (tree-view open dispatcher), which is called from `FUN_004b0660` (project-file open handler).

**Evidence for pipe open** (`FUN_004942b0` at `0x004942b0`):
```c
piVar7 = (int *)(**(code **)(*param_2 + 0x8c))(param_4, 1);  // OnOpenDocument only
if (piVar7 != (int *)0x0) {
    FUN_0044f410(param_4, iVar1);  // Type resolver — NOT vtable[0x1F4]
    // ...
}
```

No call to `vtable[0x1F4]` anywhere in the pipe-open path.

### `FUN_004417d0` — view activation also sets the flag

`FUN_004417d0` (stored in a global function pointer at `0x005d36cc`) is called during view activation. It also sets `doc+0x174 = 1`:

- If `doc+0x17C == 0` (not modified): calls `vtable[0x1F4]` (FUN_0044f8e0), which sets `doc+0x174 = 1`.
- If `doc+0x17C != 0` (modified): calls `FUN_0044ba50` (Create Entry), which also sets `doc+0x174 = 1`.

This means that when a document's view is activated (e.g., user clicks on the editor), `doc+0x174` is set to 1. But this only happens for documents that have been opened through the tree-view open path (which creates a view). Documents opened via the pipe do not get view activation in the same way.

---

## 4. Tree-view flow

### Tree-view classes

- **CTSDevProjectBar**: Project Bar tree panel. Tree control at `this+0x3b8` (CTreeCtrl), tree HWND at `this+0x3d8`. Selected item stored at `this+0x11e0`. Handler `FUN_004ac270` for `0x80e8`.
- **CTSDevResourceViewBar**: Resource View Bar tree panel. Tree control at `this+0x3b8`, tree HWND at `this+0x3d8`. Selected item stored at `this+0x11e8`. Handler `FUN_004b8a30` for `0x80e8`.

### Tree selection storage

- `CTSDevProjectBar`: `this+0x11e0` stores the selected tree item handle. Handler `FUN_004ac270` reads this and walks up 3 parent levels via `TVM_GETNEXTITEM(TVGN_PARENT)`, storing the result at `this+0x11e4`.
- `CTSDevResourceViewBar`: `this+0x11e8` stores the selected tree item handle. Handler `FUN_004b8a30` reads this directly.

**[UNVERIFIED — live]**: Whether `TVM_SELECTITEM` (0x110b, TVGN_CARET) updates these internal fields, or whether they are only set by internal `TVN_SELCHANGED` handlers. If the fields are not updated by `TVM_SELECTITEM`, posting `WM_COMMAND 0x80e8` after selecting a tree item via Win32 messages would operate on the wrong item.

### Does tree selection set up a deletion context?

**No.** Tree selection only stores the tree item handle. It does not:
- Open the document.
- Set `doc+0x174`.
- Populate the map at `doc+0x32c`.
- Pass any name or type parameter to the delete handler.

The delete handler resolves the document from the tree item data and text at command-execution time, not at selection time.

### Does tree deletion differ from menu deletion?

Both handlers (`FUN_004ac270` and `FUN_004b8a30`) are invoked by the same `WM_COMMAND 0x80e8` message. There is no separate "tree deletion" vs. "menu deletion" path — the command ID is the same regardless of whether it was triggered by a right-click context menu, a keyboard shortcut, or a programmatic `WM_COMMAND` post.

The tree-view open path (`FUN_004b0800`, triggered by double-click) is a **separate** command from the delete command. It opens the document and sets `doc+0x174 = 1`, but it is not part of the delete flow.

---

## 5. Alternative deletion paths

### Cross-references to `FUN_0044c0f0` (actual library deletion)

Exactly two static callers (confirmed by `get_xrefs_to`):

| Call site | Calling function | Context |
|---|---|---|
| `0x0044a6bc` | `FUN_0044a640` (CELDocument destructor) | When `doc+0x174 != 0` AND `!IsSaved` AND `doc+0x17C != 0` |
| `0x0044cc23` | `FUN_0044cab0` (Change Analysis Technique) | When `doc+0x17C != 0` (modified) |

### Destructor path (`FUN_0044a640`)

The destructor is called during `OnCloseDocument` → `COleServerDoc::OnCloseDocument` → `delete this`. It calls `FUN_0044c0f0` only when all three conditions are met:

1. `doc+0x174 != 0` — opened from library (set by `FUN_0044f8e0` / vtable[0x1F4])
2. `!CELProperties::IsSaved(doc+0x84)` — properties are not saved
3. `doc+0x17C != 0` — modified flag is set

As shown in §2, `FUN_0044c080` (called by `FUN_00482f50`) sets `doc+0x17C = 1` but then `FUN_00452290` saves and clears `doc+0x17C = 0`. So condition 3 fails after a successful save.

### Change Analysis Technique path (`FUN_0044cab0`)

This is not a general delete command. It is invoked when changing the analysis technique of an existing document. It calls `FUN_0044c0f0` when the document is modified, to delete the old technique entry before creating a new one. This is not usable as a general-purpose delete API.

### No delete-by-name API exists

`FUN_0044c0f0` accepts a document pointer (`this` in ECX), not a name string. It reads type/subtype/category from `doc+0x1e8/0x1ea/0x1ec` and uses those to look up the item in the library. There is no wrapper that accepts a name and type, resolves the document, and calls `FUN_0044c0f0`.

No function in the binary accepts a name string and deletes the corresponding library item. The only paths to `FUN_0044c0f0` require a document pointer with specific field values.

### Does a command handler close a document and reach the destructor deletion path?

Yes, `FUN_00482f50` calls `OnCloseDocument`, which reaches the destructor. But as shown above, the save in `FUN_0044c080` clears the modified flag, preventing the destructor from calling `FUN_0044c0f0`. The destructor path is only reachable if:

1. `doc+0x174` is set (requires `vtable[0x1F4]` to have been called).
2. The document is modified (`doc+0x17C != 0`).
3. `IsSaved` returns false.

These conditions are met when a user:
1. Opens a document by double-clicking in the tree (sets `doc+0x174 = 1`).
2. Modifies the document (sets `doc+0x17C = 1`).
3. Closes the document without saving (IsSaved returns false).

In this scenario, the destructor calls `FUN_0044c0f0`, which deletes the item from the library. This is the "unsaved modified document closed" path, not the "delete command" path.

**[UNVERIFIED — live]**: Whether the UI delete command (0x80e8) on an already-open, modified, unsaved document would trigger library deletion through the destructor. The analysis suggests it would not, because `FUN_0044c080` saves and clears the modified flag before `OnCloseDocument` is called. But if the save fails (e.g., `_SaveModule_New_20` returns 0), `doc+0x17C` would remain 1, and the destructor could call `FUN_0044c0f0`.

---

## 6. Delete-by-name / tree-selection APIs

### Search results

- **No function accepts a name string and type number and deletes the library item.** Searched for functions receiving `wchar_t*` parameters that call `FUN_0044c0f0` or `_SetSigDeleted@4` — none found.
- **No pipe command for deletion.** The pipe (`\\.\Pipe\TSDevPipe`) only supports open-by-name (`0x8001`) and raw-string (`0x8002`). No delete command exists in the frame message map (`FUN_004857d0`).
- **No exported/ordinal function for deletion.** `_SetSigDeleted@4` is an import (called by TSDev, not exported by it). `FUN_0044c0f0` is internal and not exported.

### What tree selection passes to deletion handlers

Tree selection passes:
1. A tree item handle (HTREEITEM) stored in the control bar's internal state.
2. Tree item data (a reference-counted object containing type/category info), retrieved via `CTreeCtrl::GetItemData`.
3. Tree item text (the display name), retrieved via `CTreeCtrl::GetItemText`.

The handler uses these to resolve/open the document and pass its name to `FUN_00482f50`. No API exists above this level.

### Whether an API exists above raw struct/function-level invocation

**No.** The highest-level deletion trigger is `WM_COMMAND 0x80e8` with a tree selection. But this trigger:
- Does not set `doc+0x174` (opens via pipe, not tree-view open).
- Calls `FUN_00482f50`, which saves and clears the modified flag.
- Does not result in library deletion for Function documents.

The only path that reliably deletes Function items from the library is direct invocation of `FUN_0044c0f0` with a document pointer, as proven by live verification (`RE_DELETE_LIVE_VERIFY.md` §2).

---

## 7. Why the UI appears to delete Function documents

### Hypothesis: the UI requires the document to be open first

The typical UI flow for deleting a Function document may be:

1. **User double-clicks the tree item** → `FUN_004b0800` → `vtable[0x1F4]` → `doc+0x174 = 1`.
2. **User modifies the document** (e.g., edits source) → `doc+0x17C = 1`.
3. **User closes the document without saving** → destructor sees `doc+0x174 = 1`, `!IsSaved`, `doc+0x17C = 1` → calls `FUN_0044c0f0` → item deleted.

In this scenario, the deletion happens through the destructor during normal document closure, not through the `0x80e8` command.

Alternatively:

1. **User double-clicks the tree item** → `doc+0x174 = 1`.
2. **User right-clicks and selects Delete** → `FUN_004ac270` or `FUN_004b8a30`.
3. Handler finds document already open (`FUN_00491760` returns non-null, `doc+0x174 = 1`).
4. Handler calls `FUN_00482f50` → `FUN_0044c080` → `SetModifiedFlag(1)` → save.
5. **If save fails** (`_SaveModule_New_20` returns 0): `doc+0x17C` remains 1.
6. `OnCloseDocument` → destructor → `doc+0x174 = 1`, `!IsSaved`, `doc+0x17C = 1` → `FUN_0044c0f0` → item deleted.

**[UNVERIFIED — live]**: Whether the save actually fails during a UI delete operation. The live verification showed `_SaveModule_New_20` returning 0x0 (success) when `FUN_00482f50` was called directly, which did NOT delete the item. But the live test used a document opened via the pipe (doc+0x174 = 0), so the destructor path was never reached regardless.

### Hypothesis: tree item removal without library deletion

It is possible that the UI delete command removes the tree item and closes the document, but does NOT delete the library item for Function documents. The item would reappear in the tree on the next TSDev restart (when the tree is repopulated from the library). **[UNVERIFIED — live]**: this would require testing the actual UI delete (right-click > Delete in TSDev) and checking whether the item survives a restart.

---

## 8. `FUN_0044c0f0` — the actual library deletion function

### Decompilation summary (from `RE_DELETE_LIVE_VERIFY.md` §1)

1. Calls `Ordinal_10(0)` to get a library handle.
2. Initializes a stack buffer via `FUN_0044a1e0`.
3. Calls `FUN_004d7a30(type, subtype, category, &buffer)` to look up the item by document type info (`doc+0x1e8`, `doc+0x1ea`, `doc+0x1ec`).
4. Calls `FUN_004d7230(handle, result)` — returns 1 if found.
5. If found: calls `_SetSigDeleted_4(sigId)` to mark the item's signature as deleted.
6. Calls `FUN_004d7970` for post-delete cleanup.

### Parameters

- `this` (ECX): document pointer.
- Reads `doc+0x1e8` (type), `doc+0x1ea` (subtype), `doc+0x1ec` (category) to identify the library item.
- Does NOT accept a name string.

### Safety

Live verification confirmed: `FUN_0044c0f0` handles nonexistent items gracefully — the internal lookup does not find the item, so `_SetSigDeleted_4` is not called, and the function returns without error (`RE_DELETE_LIVE_VERIFY.md` §3).

---

## 9. CELFunctionDoc vtable layout

Vtable base: `0x00568c98` (set by constructor `FUN_00457eb0` at `0x00457ee8`: `MOV DWORD PTR [ESI], 0x00568c98`).

| Offset | Address | Function | Significance |
|---|---|---|---|
| `0x6c` | `0x00568d04` | `FUN_00453a48` | `SetModifiedFlag` |
| `0x84` | `0x00568d1c` | `FUN_00450f30` | `OnOpenDocument` — clears `doc+0x174` |
| `0x8c` | `0x00568d24` | `FUN_004508e0` | `OnCloseDocument` |
| `0x1F4` | `0x00568e8c` | `FUN_0044f8e0` | Document loader — sets `doc+0x174 = 1` if item found |
| `0x204` | `0x00568e9c` | `FUN_0044d2b0` | Parent/context — returns `this` |

---

## 10. Document field reference

All offsets are byte offsets from the document object base. Decompiler int-pointer notation in parentheses.

| Offset | Decompiler | Size | Meaning | Evidence |
|---|---|---|---|---|
| `0x174` | `param_1[0x5d]` | DWORD | Opened-from-library flag | Destructor gates `FUN_0044c0f0` on this; set by `FUN_0044f8e0`, cleared by `FUN_00450f30` |
| `0x17C` | `param_1[0x5f]` | DWORD | Modified flag | Set by `SetModifiedFlag(1)`; cleared by `FUN_00452290` on successful save |
| `0x1e8` | `param_1[0x7a]` (low word) | WORD | Document type | Function=1, Strategy=0, etc. |
| `0x1ea` | `param_1[0x7a]` (high word) / `+0x1ea` | WORD | Document subtype | |
| `0x1ec` | `+0x1ec` | WORD | Document category | |
| `0x1d4` | `+0x1d4` | CString | Document name | Read by `FUN_0044e6c0` |
| `0x32c` | `+0x32c` | std::map | Child-document map | Empty for Function docs; iterated by `OnCloseDocument` |
| `0x84` | `+0x84` | CELProperties | Properties object | `IsSaved` called on this by destructor |

---

## 11. Complete deletion flow diagram

```
UI Delete Command (0x80e8)
│
├── Handler (FUN_004ac270 or FUN_004b8a30)
│   ├── Read tree selection (item handle + text)
│   ├── Check if doc open (FUN_00491760)
│   ├── If not open: open via FUN_004944b0 (pipe-open)
│   │   └── vtable[0x84] (OnOpenDocument / FUN_00450f30)
│   │       └── doc+0x174 = 0  ← FLAG CLEARED
│   ├── vtable[0x204] (returns this for Function docs)
│   └── FUN_00482f50(doc, name)
│       ├── FUN_00483020() — confirmation dialog
│       ├── If confirmed:
│       │   ├── FUN_0044c080(doc, parentContext)
│       │   │   ├── std::map erase (doc+0x32c) — NO-OP for Function (empty map)
│       │   │   ├── SetModifiedFlag(1) → doc+0x17C = 1
│       │   │   ├── FUN_00452290() — save
│       │   │   │   └── _SaveModule_New_20()
│       │   │   │       └── If success: doc+0x17C = 0  ← MODIFIED FLAG CLEARED
│       │   │   └── Post-save cleanup
│       │   └── vtable[0x8c] (OnCloseDocument / FUN_004508e0)
│       │       └── COleServerDoc::OnCloseDocument
│       │           └── delete this
│       │               └── Destructor (FUN_0044a640)
│       │                   ├── if (doc+0x174 != 0)  ← FALSE (pipe-open cleared it)
│       │                   │   └── FUN_0044c0f0()  ← NOT REACHED
│       │                   └── ... destructor cleanup ...
│       └── Return confirmation result
│
└── Tree/list bookkeeping (TVM_DELETEITEM, CStringList::RemoveAt)
    └── Tree item removed, but LIBRARY ITEM NOT DELETED
```

```
Tree-View Open (double-click) — SEPARATE command
│
└── FUN_004b0800
    ├── vtable[0x8c] (OnOpenDocument / FUN_00450f30)
    │   └── doc+0x174 = 0  ← initially cleared
    └── vtable[0x1F4] (FUN_0044f8e0)  ← KEY DIFFERENCE
        ├── doc+0x174 = 0  ← cleared at start
        ├── _FindExistPosSubType_12() — look up item
        └── If found: doc+0x174 = 1  ← FLAG SET
```

---

## 12. Final recommendation

### Can command-ID/message-level triggering work for deletion?

**No.** `WM_COMMAND 0x80e8` (the delete command) does not delete Function items from the library. The evidence is conclusive:

1. The delete handlers open documents via the pipe path (`FUN_004944b0`), which does NOT set `doc+0x174`.
2. `FUN_00482f50` calls `FUN_0044c080`, which saves and clears the modified flag (`doc+0x17C = 0`).
3. The destructor requires `doc+0x174 != 0` AND `doc+0x17C != 0` AND `!IsSaved` to call `FUN_0044c0f0`.
4. All three conditions fail: `doc+0x174 = 0` (pipe-open), `doc+0x17C = 0` (save cleared it), and IsSaved may be true (save succeeded).
5. Live verification confirmed: direct `FUN_00482f50` invocation did NOT delete the item (`RE_DELETE_LIVE_VERIFY.md` §2).

### Is there a higher-level route?

**No higher-level route exists for Function document deletion.** The evidence:

- No pipe command for deletion.
- No exported/ordinal function for deletion.
- No function accepting a name string that performs deletion.
- `WM_COMMAND 0x80e8` does not perform library deletion for Function documents.
- The only path to `FUN_0044c0f0` (actual library deletion) is through the destructor, which requires specific document state that the delete command does not establish.

### Recommended implementation approach

The destructor path is now the production deletion approach. It was implemented in `src/delete_hook/dllmain.cpp` and replaces the previous direct `FUN_0044c0f0` call. The destructor path drives TSDev.exe's own document lifecycle: it sets up the three conditions the `CELDocument` destructor (`FUN_0044a640`) requires, then calls `OnCloseDocument` which triggers the destructor. The destructor calls `FUN_0044c0f0` (library deletion) and performs all cleanup in the correct order.

The implemented sequence (in `do_delete`):

1. **Open the target item via the pipe**: send `"EncodedName", <type>` to `\\.\Pipe\TSDevPipe`. This opens the document and provides a document pointer (captured via hook on `FUN_004944b0`).

2. **Call `vtable[0x1F4]` (loader / `FUN_0044f8e0`)**: this is the same vtable call the tree-view open handler (`FUN_004b0800`) makes. It clears `doc+0x17C` and `doc+0x174` at the start, looks up the library item, and sets `doc+0x174 = 1` if the item exists. Must be called before setting the modified flag (it clears it).

3. **Call `vtable[0x1B4]` (clear-saved / `FUN_00458160` for CELFunctionDoc)**: this calls `FUN_0044f3d0`, which calls `CELProperties::SetSaved(false)`, `SetVerified(false)`, `SetProtected(false)`, and `SetProtectedPlus(false)` on the `CELProperties` object at `doc+0x210`. This makes the destructor's `IsSaved` check return false.

4. **Set `doc+0x17C = 1` (modified flag)**: must be after the loader (which clears it). The destructor requires this to be non-zero.

5. **Call `vtable[0x8c]` (`OnCloseDocument` / `FUN_004508e0`)**: closes the document, destroys views, and calls `delete this`. The destructor sees `doc+0x174 = 1`, `!IsSaved = true`, `doc+0x17C = 1` → calls `FUN_0044c0f0` (library deletion) and performs all cleanup. After this call returns, the doc pointer is invalid (freed by the destructor).

This approach requires:
- Three vtable offsets: `vtable[0x1F4]` (loader), `vtable[0x1B4]` (clear-saved), `vtable[0x8c]` (OnCloseDocument).
- One struct offset: `doc+0x17C` (modified flag).
- One hook point: `FUN_004944b0` (to capture the document pointer).

### IsSaved evidence and state manipulation

The destructor (`FUN_0044a640`) assembly confirms:

```text
CMP dword ptr [EDI + 0x174],0       ; doc+0x174: opened-from-library flag
...
LEA ECX,[EDI + 0x210]               ; ECX = doc+0x210 (CELProperties object)
CALL dword ptr [0x0055eaec]         ; CELProperties::IsSaved
TEST AL,AL
JNZ ...                              ; if IsSaved == true, skip deletion
CMP dword ptr [EDI + 0x17c],0       ; doc+0x17C: modified flag
JZ ...                               ; if modified == 0, skip deletion
MOV ECX,EDI
CALL 0x0044c0f0                      ; FUN_0044c0f0: actual library deletion
```

**CELProperties object location**: The destructor uses `LEA ECX,[EDI + 0x210]`, proving the `CELProperties` object is at byte offset `doc+0x210`. The research document's earlier mention of `doc+0x84` was an offset error caused by Ghidra's `undefined4*` pointer indexing (`0x84 * 4 = 0x210`). This is now corrected.

**IsSaved function**: `CELProperties::IsSaved` is an external function pointer stored at `[0x0055EAEC]`, targeting address `0x001BA490` in an external DLL (outside TSDev.exe's memory range `0x00400000–0x00619BFF`). The external DLL is not loaded in the Ghidra project, so the `IsSaved` implementation could not be decompiled directly. However, its behavior is fully controlled via the `SetSaved` method:

**SetSaved method**: `CELProperties::SetSaved` is an external function pointer stored at `[0x0055EA2C]`. It is called from `FUN_0044f3d0` (a `CELDocument` method) with argument `false`:

```c
// FUN_0044f3d0 decompilation:
this = (**(code **)(*param_1 + 0x1f0))();  // vtable[0x1F0] → returns doc+0x210
CELProperties::SetSaved(this, false);
CELProperties::SetVerified(this, false);
CELProperties::SetProtected(this, false);
CELProperties::SetProtectedPlus(this, false);
```

`FUN_0044f3d0` is the base-class implementation at vtable offset `0x1B4`. `CELFunctionDoc` overrides it with `FUN_00458160`, which calls `FUN_0044f3d0()` first, then `CELProperties::SetReturnType`. Calling `vtable[0x1B4]` therefore clears the saved flag (and verified/protected/protected-plus flags) on the `CELProperties` object, making `IsSaved` return false.

**How IsSaved is made to return false**: The implementation calls `vtable[0x1B4]` on the document, which calls `CELProperties::SetSaved(false)` on the properties at `doc+0x210`. The specific `CELProperties` field offset that `SetSaved` writes (and that `IsSaved` reads) is internal to the external DLL and not visible from TSDev.exe static analysis. However, the `SetSaved(false)` → `IsSaved == false` relationship is confirmed by the existence of the `SetSaved` function pointer and its use in `FUN_0044f3d0` to clear saved state. This is the same mechanism TSDev's own code uses when it needs to mark a document as unsaved (e.g., before closing).

### Version-sensitivity assessment

| Approach | Binary coupling | Portability risk |
|---|---|---|
| Direct `FUN_0044c0f0` call (previous) | Function address + 3 struct offsets | Medium — function address changes per version; struct offsets may change |
| `WM_COMMAND 0x80e8` + tree selection | Command ID + tree control interaction | Low for command ID, high for tree selection automation |
| Destructor path (implemented) | 3 vtable offsets + 1 struct offset | Medium — vtable offsets are more stable than raw function addresses; no direct function address needed |

The destructor path uses vtable offsets (which are struct-relative and more stable across versions than absolute function addresses) and avoids calling `FUN_0044c0f0` directly. The previous direct-call approach required a hardcoded function address (`FUN_0044c0f0`) that changes with every build. The destructor path's vtable offsets are determined by class layout, which tends to be more stable.

---

## Evidence index

| Address | Type | Description |
|---|---|---|
| `0x004ac270` | Code | CTSDevProjectBar delete handler (0x80e8) |
| `0x004b8a30` | Code | CTSDevResourceViewBar delete handler (0x80e8) |
| `0x00482f50` | Code | Common delete routine (confirmation + save + close) |
| `0x00483020` | Code | Confirmation helper (AfxMessageBox, MB_OKCANCEL) |
| `0x0044c080` | Code | Map erase + SetModifiedFlag(1) + save |
| `0x00448210` | Code | std::map lower_bound lookup |
| `0x00455030` | Code | std::map erase |
| `0x00452290` | Code | Save function (calls _SaveModule_New@20) |
| `0x004508e0` | Code | OnCloseDocument (vtable+0x8c) |
| `0x00450f30` | Code | OnOpenDocument (vtable+0x84) — clears doc+0x174 |
| `0x0044f8e0` | Code | Document loader (vtable+0x1F4) — sets doc+0x174 = 1 |
| `0x0044f670` | Code | Checks if technique name is configured in Startup\Technique |
| `0x0044a640` | Code | CELDocument destructor — gates FUN_0044c0f0 |
| `0x0044c0f0` | Code | Actual library deletion (calls _SetSigDeleted@4) |
| `0x0044cab0` | Code | Change Analysis Technique — second caller of FUN_0044c0f0 |
| `0x0044d2b0` | Code | vtable+0x204 for CELFunctionDoc (returns this) |
| `0x0044ba50` | Code | Create Entry — sets doc+0x174 = 1 |
| `0x00457eb0` | Code | CELFunctionDoc constructor — sets doc+0x174 if FUN_0044f670 returns true |
| `0x00449ea0` | Code | CELDocument base constructor — initializes doc+0x174 = 0 |
| `0x004535d0` | Code | Archive serialize — clears doc+0x174 when version == 2 |
| `0x004417d0` | Code | View activation — sets doc+0x174 = 1 via vtable[0x1F4] or FUN_0044ba50 |
| `0x004b0800` | Code | Tree-view open handler — calls vtable[0x1F4] after OnOpenDocument |
| `0x004b0750` | Code | Tree-view open dispatcher |
| `0x004b0660` | Code | Project-file open handler |
| `0x004944b0` | Code | Pipe-open path (does NOT call vtable[0x1F4]) |
| `0x004942b0` | Code | Document creation/open helper (called by FUN_004944b0) |
| `0x00491760` | Code | Check if document is open by name |
| `0x00568c98` | Data | CELFunctionDoc vtable base |
| `0x00568e8c` | Data | vtable[0x1F4] entry → FUN_0044f8e0 |
| `0x00568e9c` | Data | vtable[0x204] entry → FUN_0044d2b0 |
| `0x00577450` | Data | CTSDevProjectBar message map |
| `0x00579418` | Data | CTSDevResourceViewBar message map |
| `0x005775e8` | Data | Message-map entry: WM_COMMAND 0x80e8 → FUN_004ac270 |
| `0x00579508` | Data | Message-map entry: WM_COMMAND 0x80e8 → FUN_004b8a30 |
| `0x0044ffd0` | Data | Instruction: MOV [EBX+0x174], 1 (in FUN_0044f8e0) |
| `0x0044ba9e` | Data | Instruction: MOV [reg+0x174], 1 (in FUN_0044ba50) |
| `0x00457f1b` | Data | Instruction: MOV [reg+0x174], 1 (in FUN_00457eb0) |
| `0x0044f3d0` | Code | CELDocument base clear-saved (vtable+0x1B4) — calls SetSaved/Verified/Protected/ProtectedPlus(false) |
| `0x00458160` | Code | CELFunctionDoc clear-saved override (vtable+0x1B4) — calls FUN_0044f3d0 + SetReturnType |
| `0x0044ec00` | Code | vtable[0x1F0] — returns doc+0x210 (CELProperties accessor) |
| `0x0055ea2c` | Data | PTR_SetSaved — CELProperties::SetSaved function pointer (external: 0x001BA1DC) |
| `0x0055ea28` | Data | PTR_SetVerified — CELProperties::SetVerified function pointer (external) |
| `0x0055ea34` | Data | PTR_SetProtected — CELProperties::SetProtected function pointer (external) |
| `0x0055ea3c` | Data | PTR_SetProtectedPlus — CELProperties::SetProtectedPlus function pointer (external) |
| `0x0055eaec` | Data | PTR_IsSaved — CELProperties::IsSaved function pointer (external: 0x001BA490) |
| `0x0055eb0c` | Data | PTR_Load — CELProperties::Load function pointer (external: 0x001BA5DC) |
| `0x0055eb10` | Data | PTR_Save — CELProperties::Save function pointer (external: 0x001BA5FA) |
