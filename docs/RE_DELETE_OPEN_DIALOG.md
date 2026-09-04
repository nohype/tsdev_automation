# TSDev.exe "Open" Dialog Delete Flow — Reverse-Engineering Research

Static analysis of **TSDev.exe** (TradeStation 9.5 Development Environment, build timestamp Fri Nov 30 18:39:00 2018, SHA-256 `9099AA600F0A035C0290107EB1852CD61690330C53B14C9F9D01899395DCAF9A`) via the Ghidra MCP server. Every claim cites Ghidra addresses and decompilation evidence. Unproven conclusions are marked **[UNVERIFIED]**.

Scope note: per `AGENTS.md`, reverse engineering is bounded to **TSDev.exe only**. `CELFileOpenDialog` and its UI live in `ORELUI20.dll`, which is out of scope. This document therefore traces everything TSDev.exe does around the dialog and establishes, with negative evidence, what TSDev.exe does **not** do.

---

## 1. How the "Open" dialog is opened (command IDs, functions)

`CELFileOpenDialog` is imported from `ORELUI20.dll`. The import symbols (demangled) and their IAT slots:

| Method | External symbol | IAT slot |
|---|---|---|
| `CELFileOpenDialog::CELFileOpenDialog` | `EXTERNAL:00000169` | `0x0055ebe4` |
| `CELFileOpenDialog::~CELFileOpenDialog` | `EXTERNAL:0000016a` | `0x0055ebe8` |
| `GetSelectedCount` | `EXTERNAL:0000016b` | `0x0055ebec` |
| `GetSelectedItem` | `EXTERNAL:0000016c` | `0x0055ebf0` |
| `GetSelectedType` | `EXTERNAL:0000016d` | `0x0055ebf4` |
| `NeedToImport` | `EXTERNAL:0000016e` | `0x0055ebf8` |
| `GetImportType` | `EXTERNAL:0000016f` | `0x0055ebfc` |
| `GetSelectedProgID` | `EXTERNAL:00000181` | `0x0055ec00` |

Evidence: Ghidra symbol scan (`run_script_inline`) returned exactly these 8 external symbols plus two labels `PTR_CELFileOpenDialog_0055ebe4` / `PTR_~CELFileOpenDialog_0055ebe8`. IAT layout confirmed by `read_memory` at `0x0055ebe4` (8 consecutive 4-byte thunks `0x001bb964…0x001bbc92`).

### Constructor call sites — exactly two

`get_xrefs_to(0x0055ebe4)` (constructor IAT) returns exactly two READ references:

| Call site | Calling function |
|---|---|
| `0x00493909` | `FUN_004938c0` |
| `0x004ab992` | `FUN_004ab920` |

No other function in TSDev.exe constructs a `CELFileOpenDialog`. These two functions are the entire TSDev.exe surface around the Open dialog.

### Command IDs that invoke them (MFC message-map entries)

A byte-scan of `.data` (`0x00500000`–`0x005d0000`) for the two function pointers found the message-map entries:

| Function pointer | Entry address | `nMessage` | `nID` | Message map |
|---|---|---|---|---|
| `0x004938c0` | `0x00571848` | `0x111` (WM_COMMAND) | **`0xE101`** (MFC `ID_FILE_OPEN`) | main frame (`CMainFrame`) |
| `0x004ab920` | `0x00577510` | `0x111` (WM_COMMAND) | **`0x80DB`** | `CTSDevProjectBar` (map at `0x00577450`, per `RE_DELETE_UI_FLOW.md` §1) |

Entry layout (24-byte `AFX_MSGMAP_ENTRY`): `nMessage, nCode, nID, nLastID, nSig, pfn`.
- `0x00571848`: `11 01 00 00 | 00 00 00 00 | 01 E1 00 00 | 01 E1 00 00 | 3A 00 00 00 | C0 38 49 00` → ID `0xE101`, handler `0x004938c0`.
- `0x00577510`: `11 01 00 00 | 00 00 00 00 | DB 80 00 00 | DB 80 00 00 | 3A 00 00 00 | 20 B9 4A 00` → ID `0x80DB`, handler `0x004ab920`.

So the Open dialog is opened by:
- **`WM_COMMAND 0xE101`** (`ID_FILE_OPEN`, the File > Open menu) → `FUN_004938c0` (frame message map).
- **`WM_COMMAND 0x80DB`** (a `CTSDevProjectBar` command — the Project Bar's "Open" toolbar/button) → `FUN_004ab920`.

---

## 2. What TSDev.exe does with the dialog (decompilation of both handlers)

### `FUN_004938c0` (frame, `0xE101`)

Decompilation (`0x004938c0`):
1. `AfxGetThread()` → `(*vtable+0x7c)()` to get the main window (`CWnd*`).
2. `CELFileOpenDialog::CELFileOpenDialog(local, (map<string,long>*)(param_1 + 0xa0), mainWnd)` — constructs the dialog. **The only data TSDev passes is the `map<string,long>` at `this+0xa0` (the per-type open-document counter, per `KNOWLEDGE_BASE.md`) and the parent `CWnd*`. No callback function pointer, no vtable override, no item list.**
3. `CDialog::DoModal(local)`.
4. **If DoModal returns `1` (IDOK)**:
   - `(*vtable+0xc0)()` (frame method).
   - Loop `i = 0 .. GetSelectedCount()-1`: `GetSelectedType(i)`, `GetSelectedItem(i)`, then **`FUN_004944b0(type, name)`** — the open-by-name+type function (the same function the named pipe uses; `KNOWLEDGE_BASE.md`).
   - Post-open MDI tab bookkeeping.
5. **Else (not IDOK)**: `NeedToImport()` → if set, run `CTransferWizard` import.
6. `CELFileOpenDialog::~CELFileOpenDialog(local)`.

### `FUN_004ab920` (ProjectBar, `0x80DB`)

Decompilation (`0x004ab920`):
1. `AfxGetModuleState()`; builds a local `map<string,long>` via `FUN_00479110()`.
2. `CELFileOpenDialog::CELFileOpenDialog(local, &localMap, mainWnd)` — same constructor, same two parameters.
3. `CDialog::DoModal(local)`.
4. **If DoModal returns `1` (IDOK)**:
   - Loop `i = 0 .. GetSelectedCount()-1`: `GetSelectedType(i)`, `GetSelectedItem(i)`, **`FUN_004944b0()`** (open), then `(*vtable+500)()` (`vtable[0x1F4]` = `FUN_0044f8e0`, the "opened-from-library" flag setter — see `RE_DELETE_UI_FLOW.md` §3), then `GetSelectedProgID(i)`, `FUN_004ade00`, `FUN_004a9f20`, `FUN_004b02d0`, and tree-refresh `FUN_004b99f0`.
5. `CELFileOpenDialog::~CELFileOpenDialog(local)`; frees the local map.

### Key observation — both handlers only OPEN items

On `IDOK`, both handlers iterate the selected items and call `FUN_004944b0` (open-or-create). **There is no branch in either handler that deletes.** `FUN_004944b0`'s callees (`get_function_callees`): `FUN_0044f7b0`, `FUN_0047be40`, `FUN_004942b0` (pipe-open), plus string helpers — no deletion function. Neither handler calls `FUN_0044c0f0`, `FUN_00482f50`, `_SetSigDeleted@4`, or any other delete routine.

The non-IDOK branch only handles `NeedToImport` (launches `CTransferWizard`). There is **no "delete" return code** from the dialog handled by TSDev.exe.

---

## 3. The Delete button's code path back into TSDev.exe — **there is none**

### Negative evidence: no TSDev.exe callback for deletion

1. **Constructor takes no callback.** The only arguments are `map<string,long>*` and `CWnd*` (§2). TSDev.exe passes no function pointer, no vtable, no delegate. The dialog is constructed on the stack and used as-is (no subclassing visible in either handler).
2. **No delete return code is handled.** Both handlers branch only on `DoModal == 1` (IDOK → open) vs. else (import). No other return value triggers deletion.
3. **`_SetSigDeleted@4` (the import that marks a library item deleted) has exactly one TSDev.exe caller.** `get_function_xrefs("_SetSigDeleted@4")` → `0x0044c18d` inside `FUN_0044c0f0` (a computed call through IAT `0x00560294`), plus the IAT data ref. No other caller.
4. **`FUN_0044c0f0` (actual library deletion) has exactly two callers** — unchanged from prior research. `get_xrefs_to(0x0044c0f0)`:
   - `0x0044a6bc` in `FUN_0044a640` (CELDocument destructor)
   - `0x0044cc23` in `FUN_0044cab0` (Change Analysis Technique)
   No third caller from any Open-dialog path.
5. **No delete-confirmation strings exist in TSDev.exe.** `search_strings` for `"Yes to all"` and `"Yes to All"` → 0 matches. `"Are you sure"` → only `"The project %s will be closed. Are you sure ?"` (`0x005776d0`, project-close, unrelated). `"Delete"` → `"%s will be deleted and AnalysisTechnique saved"` (`0x0056d868`, the Change-AT path) and unrelated CBCGP/library strings. The "Yes to all" / "Are you sure you want to delete" confirmation the user sees is therefore rendered by `ORELUI20.dll` (or its resources), not TSDev.exe.

### Conclusion

The Open dialog's **Delete button is implemented entirely inside `ORELUI20.dll`**. When clicked, the dialog enumerates the selected items (it already holds the item list internally — see §6) and deletes them directly through `ORLIB20.dll` (the same library layer `_SetSigDeleted@4` belongs to), without calling back into TSDev.exe. TSDev.exe's only role is to construct the dialog, hand it the per-type open-document counter map, and process the `IDOK` (Open) result.

This is consistent with `KNOWLEDGE_BASE.md`: *"the item enumeration happens inside ORELUI20.dll… No external trigger exists for the enumeration."* The delete action uses that same internal enumeration and stays inside `ORELUI20.dll`/`ORLIB20.dll`.

---

## 4. Does this path work for Function items?

**[UNVERIFIED — the delete logic is inside `ORELUI20.dll`, out of scope]** but the indirect evidence is strong:

- The user reports the Open-dialog Delete works for Functions, unlike the tree-view right-click Delete (`0x80e8`).
- The tree-view path fails for Functions because it operates through a **document object**: it opens the doc via the pipe path (`FUN_004944b0` → `FUN_004942b0`), which clears `doc+0x174` (opened-from-library) and `doc+0x17C` (modified), then calls `FUN_00482f50`, whose save (`FUN_00452290`) re-saves the item as-is, so the destructor's three conditions (`doc+0x174 != 0`, `!IsSaved`, `doc+0x17C != 0`) never all hold (`RE_DELETE_UI_FLOW.md` §2, §3).
- The Open-dialog Delete does **not** go through a TSDev document object at all. `FUN_0044c0f0` (the only TSDev.exe function that calls `_SetSigDeleted@4`) operates at the **library-record level**: it reads type/subtype/category from `doc+0x1e8/0x1ea/0x1ec`, calls `FUN_004d7a30` (library lookup) and `FUN_004d7230`, obtains a record handle (`local_1f9` via `FUN_004c9ef0`), and passes that handle to `_SetSigDeleted@4` — no document modified-flag check is involved at this layer. The dialog's in-`ORELUI20.dll` delete presumably calls the same `ORLIB20.dll` record-level API, bypassing the `doc+0x174`/`doc+0x17C` document-state gating that blocks Function deletion in the tree-view path.

**Why it works for Functions (inferred):** the dialog deletes at the library-record level, not the document-object level, so the Function-specific document-state conditions that defeat `FUN_00482f50` do not apply.

---

## 5. Can this path be triggered programmatically?

### Opening the dialog — yes (WM_COMMAND)

The dialog can be opened at the MFC command level, consistent with the existing insert flow (which uses `WM_COMMAND` to open the "New" dialog, `KNOWLEDGE_BASE.md`):
- `PostMessageW(main_frame, WM_COMMAND, 0xE101, 0)` → `FUN_004938c0` (File > Open).
- `PostMessageW(project_bar, WM_COMMAND, 0x80DB, 0)` → `FUN_004ab920`.

### Triggering the Delete — no clean way within the rules

The delete action itself is inside `ORELUI20.dll`. The only ways to reach it are:

1. **UI automation on the modal dialog** — select items in the dialog's list, click its Delete button, click "Yes"/"Yes to all" in the confirmation. This is **prohibited** by `AGENTS.md` ("No UI automation, no clipboard: never synthesize clicks/keys on UI controls"). Sending `WM_COMMAND`/`BM_CLICK` to the dialog's controls to drive the delete would constitute UI automation on a modal dialog that TSDev.exe does not own the logic for.
2. **Injecting into `ORELUI20.dll`** to call its internal delete routine directly — **prohibited** by `AGENTS.md` constraint (d): "never touch any process or binary other than TSDev.exe."
3. **Calling a TSDev.exe function that performs the delete** — **none exists.** There is no TSDev.exe function that takes a name/type (or item list) and deletes it. The only TSDev.exe deletion function (`FUN_0044c0f0`) requires a document pointer with `doc+0x174`/`doc+0x17C` set, and is not reachable from the Open-dialog flow (§3).

### Abstraction level vs. current destructor-path approach

- The **current destructor-path approach** (open via tree-view to set `doc+0x174`, mark modified, close) at least stays inside TSDev.exe and calls `FUN_0044c0f0` through the destructor — but it requires artificial precondition setup on document struct fields (`doc+0x174`, `doc+0x17C`), which is low-level and brittle (raw struct offsets, per `AGENTS.md` purpose (b)).
- The **Open-dialog Delete** is higher-level conceptually (it's the real UI delete), but its implementation lives in `ORELUI20.dll`, so it is **not reachable at the TSDev.exe level** without UI automation or out-of-scope injection. It therefore offers no usable higher-level TSDev.exe surface.

**Bottom line:** the Open-dialog Delete cannot be triggered programmatically under the project's constraints. It is not a viable replacement for the destructor-path approach. The only TSDev.exe-internal deletion surface remains `FUN_0044c0f0` (reachable via the destructor conditions, or via the Change-AT path `FUN_0044cab0`).

---

## 6. Item enumeration (relevant to the future `list` operation)

`KNOWLEDGE_BASE.md` states the Open dialog's item enumeration happens inside `ORELUI20.dll` and "No external trigger exists for the enumeration." This research confirms and sharpens that:

- TSDev.exe passes the dialog **only** a `map<string,long>` (per-type open-document counter) and a `CWnd*` parent (§2). It does **not** pass an item list, an enumeration callback, or a library handle.
- `CELFileOpenDialog`'s public TSDev-imported API is limited to: constructor, destructor, `GetSelectedCount`, `GetSelectedItem`, `GetSelectedType`, `GetSelectedProgID`, `NeedToImport`, `GetImportType`. There is **no enumeration accessor** imported by TSDev.exe — TSDev can only read what the user selected after `DoModal` returns, not enumerate the full list.
- `GetSelectedProgID` exists, which means the dialog internally has access to ProgIds — but that data is not exposed to TSDev.exe except for user-selected items, post-`DoModal`.

**Implication for `list`:** the Open dialog cannot be repurposed as an enumeration mechanism for the `list` operation. The enumeration is internal to `ORELUI20.dll` and is not exposed through any TSDev.exe-imported function. A future `list`/`exists`-with-verbatim-name-and-ProgId operation cannot use this dialog; it must find another TSDev.exe-internal enumeration surface (none discovered so far) or remain dependent on the current pipe-open + editor-read existence discriminator (`KNOWLEDGE_BASE.md`), which cannot yield verbatim stored names or ProgIds.

---

## 7. Summary

| Question | Answer |
|---|---|
| How is the Open dialog opened? | `WM_COMMAND 0xE101` (File > Open, frame) → `FUN_004938c0`; `WM_COMMAND 0x80DB` (ProjectBar) → `FUN_004ab920`. Both construct `CELFileOpenDialog` (from `ORELUI20.dll`) and call `DoModal`. |
| What does TSDev.exe do on IDOK? | Iterates selected items and calls `FUN_004944b0` (open-by-name+type). Nothing else. |
| Delete button's code path back into TSDev.exe? | **None.** The Delete button is handled entirely inside `ORELUI20.dll`. No callback, no delete return code, no TSDev.exe delete function is invoked. `_SetSigDeleted@4` and `FUN_0044c0f0` have no caller reachable from this flow. |
| Does it work for Function items? | **[UNVERIFIED]** but strongly indicated yes — the dialog deletes at the library-record level (via `ORLIB20.dll`), bypassing the document-state conditions (`doc+0x174`/`doc+0x17C`) that block Function deletion in the tree-view path. |
| Triggerable programmatically? | The dialog can be opened via `WM_COMMAND 0xE101`/`0x80DB`, but the delete itself is unreachable without UI automation (prohibited) or injecting into `ORELUI20.dll` (prohibited by constraint (d)). No TSDev.exe function performs the delete. |
| Abstraction level vs. destructor path? | Conceptually higher (the real UI delete), but implemented out-of-scope, so it provides **no usable TSDev.exe-level surface**. The destructor-path (`FUN_0044c0f0` via `doc+0x174`/`doc+0x17C`) remains the only TSDev.exe-internal deletion surface. |
| Item enumeration for `list`? | Not exposed. The dialog enumerates internally in `ORELUI20.dll`; TSDev.exe only receives user-selected items post-`DoModal`. No `list` surface here. |

### Evidence index (Ghidra addresses)

- Open-dialog constructor IAT: `0x0055ebe4`; destructor IAT: `0x0055ebe8`; getter IATs `0x0055ebec`–`0x0055ec00`.
- Constructor callers: `FUN_004938c0` (`0x00493909`), `FUN_004ab920` (`0x004ab992`).
- Message-map entries: `0x00571848` (ID `0xE101` → `0x004938c0`), `0x00577510` (ID `0x80DB` → `0x004ab920`).
- Open function called on IDOK: `FUN_004944b0` (callees: `FUN_0044f7b0`, `FUN_0047be40`, `FUN_004942b0` — no delete).
- `_SetSigDeleted@4` sole TSDev.exe caller: `0x0044c18d` in `FUN_0044c0f0`.
- `FUN_0044c0f0` callers: `0x0044a6bc` (`FUN_0044a640` destructor), `0x0044cc23` (`FUN_0044cab0` Change AT). No third caller.
- Delete-confirmation strings in TSDev.exe: none (`"Yes to all"` → 0 matches).
