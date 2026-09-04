# ORELUI20.dll Static RE — Enumeration + Delete (TVS #7 Subtask A)

Static reverse-engineering of **ORELUI20.dll** (TradeStation 9.5, build Nov 30 2018,
`C:\Program Files (x86)\TradeStation 9.5\Program\ORELUI20.dll`, 1,184,248 bytes)
via the Ghidra MCP server. Image base `0x10000000`, 7486 functions, x86:LE:32.
Every claim cites a Ghidra finding (function address / ordinal / decompilation / xref).
Items that cannot be confirmed statically are flagged **[DYNAMIC]** for subtask B.

Scope: ORELUI20.dll + its ORLIB20.dll ordinal imports (characterized from the
ORELUI20.dll call sites — ORLIB20.dll was NOT imported into Ghidra; see §4).

---

## 1. Enumeration surface

### 1.1 Class and exported methods

`CELFileOpenDialog` is a C++ class exported from ORELUI20.dll. Ghidra resolved 76
member methods (`list_class_members`). All methods — including protected ones —
are **exported by ordinal** (confirmed via `get_xrefs_to` showing "Entry Point"
EXTERNAL refs + Ordinal_XXX labels). Key methods and their export ordinals:

| Method | Address | Ordinal | Calling convention | Signature |
|--------|---------|---------|--------------------|-----------|
| Constructor (all-types) | `0x1000f2f0` | Ordinal_17 | `__thiscall` | `CELFileOpenDialog(this, map<string,long>*, CWnd*)` |
| Constructor (single-type) | `0x1000f4e0` | Ordinal_18 | `__thiscall` | `CELFileOpenDialog(this, wchar_t* title, ushort type, map<string,long>*, CWnd*)` |
| Destructor | `0x1000fad0` | Ordinal_166 | `__thiscall` | `~CELFileOpenDialog(this)` |
| **LoadList** | `0x10012290` | **Ordinal_1291** | `__thiscall` | `void LoadList(this, int type, CAnalysisTypeInfo*)` |
| InitLists | `0x10011cf0` | Ordinal_1196 | `__thiscall` | `void InitLists(this)` |
| GetSelectedCount | `0x10011920` | Ordinal_964 | `__thiscall` | `int GetSelectedCount(this)` |
| GetSelectedItem | `0x10011930` | Ordinal_965 | `__thiscall` | `CTSString GetSelectedItem(this, int index)` |
| GetSelectedType | `0x10011990` | Ordinal_968 | `__thiscall` | `long GetSelectedType(this, int index)` |
| GetSelectedProgID | `0x10011960` | Ordinal_967 | `__thiscall` | `CTSString GetSelectedProgID(this, int index)` |
| GetAnalysisType | `0x100111f0` | Ordinal_706 | `__thiscall` | `long GetAnalysisType(this, long index)` |
| GetAnalysisTypeCount | `0x10011220` | Ordinal_707 | `__thiscall` | `long GetAnalysisTypeCount(this)` |

Evidence: `list_class_members("CELFileOpenDialog")` returned 76 members with
addresses; `get_xrefs_to` on each address returned "Entry Point" EXTERNAL refs;
symbol lookup script confirmed Ordinal_XXX labels (e.g. LoadList → Ordinal_1291,
GetSelectedProgID → Ordinal_967).

### 1.2 How items are enumerated

`OnInitDialog` (`0x10012f80`, decompiled) calls `InitLists(this)`.
`InitLists` (`0x10011cf0`, decompiled) iterates over 13 type categories
(`GetAnalysisTypeCount` returns `0xd` = 13; type table at `DAT_1008fd04`), and for
each type calls:

```
LoadList(this, typeCategory, CAnalysisTypeInfo*)
```

`LoadList` (`0x10012290`, decompiled + disassembled) is the core enumeration
function. It does NOT touch any UI control — it only uses ORLIB20.dll library
calls and stores results in the CMap at `this+0x3bc`. Its logic:

1. `FUN_10044ab0(CAnalysisTypeInfo*)` — initialize the type-info container.
2. `pvVar3 = Ordinal_10(0)` — get the ORLIB20.dll library object
   (ORLIB20.DLL ordinal 10, confirmed via `get_external_location`; thunk at
   `0x10080df6` → `JMP [0x1008d83c]`).
3. `FUN_10072eb0(&CELApplicationList)` — initialize a CELApplicationList.
4. If `type == 0`: `FUN_10074470(lib, 0, &appList)` — enumerate ALL types
   (decompiled: iterates type categories via `FUN_10074a20`, calls
   `FUN_10074310` per type, appends results to appList).
   Else: `FUN_10074310(lib, type, &appList)` — enumerate one type
   (decompiled: thread-marshaled call to ORLIB20 via WaitForSingleObject/SetEvent;
   receives a SAFEARRAY of record handles, populates appList via
   `FUN_100735e0`).
5. `count = CELApplicationList::GetItemCount(&appList)`.
6. For each item `i = 0..count-1`:
   - `local_28 = new(0x44)` — allocate a CELTechInfo record (0x44 = 68 bytes).
   - `FUN_10044960(local_28)` — construct CELTechInfo (decompiled: initializes
     CStrings at +0x10, +0x14, +0x18, +0x1c, double at +0x20, CStringArray at
     +0x2c, pointer at +0x40).
   - `FUN_10044b90(CELTechInfo*, category, type, &appList, i)` — populate
     CELTechInfo from the application list item (decompiled, see §1.3).
   - Filtering by flags (developer mode, stateless, protected) — some items are
     skipped.
   - Stores the CELTechInfo* into the CMapWordToPtr at `CAnalysisTypeInfo+0xc`:
     `CMapWordToPtr::operator[](cati+0xc, recordId) = CELTechInfo*`.

### 1.3 Per-item record layout (CELTechInfo, 0x44 bytes)

Determined from `FUN_10044b90` (`0x10044b90`, decompiled) which populates the
record, and `FUN_10044960` (`0x10044960`, decompiled) which constructs it:

| Offset | Type | Field | Source (from FUN_10044b90) |
|--------|------|-------|---------------------------|
| `+0x00` | `uint32` | category info | `param_1` (from CAnalysisTypeInfo+8) |
| `+0x08` | `short` | **type** (analysis type number) | `param_2` (the type index) |
| `+0x0a` | `ushort` | **subtype** | from info_obj+0xe9 (via FUN_1006cde0 or direct) |
| `+0x0c` | `ushort` | **record id** | from info_obj's first word (record handle low) |
| `+0x10` | `CString` | **name** (verbatim stored name) | `FUN_1006cb30(info_obj)` — copied directly, no case transform |
| `+0x14` | `CString` | short name | `FUN_1006cc20(info_obj)` |
| `+0x18` | `CString` | display name | for type 1/4: copy of name; else `FUN_1006cfc0(info_obj)` |
| `+0x1c` | `CString` | **ProgId** | `FUN_1006c790(info_obj)` |
| `+0x20` | `double` | date | `SigRecDateToOleDate(info_obj+0xc6, info_obj+0xca)` |
| `+0x28` | `uint32` | 0 | zero-initialized |
| `+0x2c` | `CStringArray` | (7 strings) | initialized empty |
| `+0x40` | `void*` | **info object pointer** (ORLIB20 record handle) | `FUN_100732e0(appList, index, &this->info_ptr)` |

**Only the fields needed for automation:** type (`+0x08`), subtype (`+0x0a`),
record id (`+0x0c`), name (`+0x10`), ProgId (`+0x1c`), info object pointer
(`+0x40`). Other fields are documented for completeness but are not needed.

### 1.4 Verbatim-name preservation

**Names are stored verbatim (case-preserved).** `FUN_10044b90` copies the name
from `FUN_1006cb30(info_obj)` directly into the CString at `CELTechInfo+0x10`
via `CString::operator=` — no case conversion, no truncation, no normalization.
The `OnOK` handler (`0x10013330`, decompiled) similarly copies names verbatim
from `CELTechInfo+0x10` into the selected-items array. The only case-insensitive
operation found is `CanDeleteItem` which uses `CompareNoCase` to detect renamed
items — but that is a comparison, not a storage transformation.

### 1.5 Selected-items array (post-DoModal public API)

The public exported getters (`GetSelectedCount`, `GetSelectedItem`,
`GetSelectedType`, `GetSelectedProgID`) read from the array at `this+0x164`,
populated by `OnOK` (only after `DoModal` returns IDOK). Each entry is 12 bytes:

| Offset | Field | Getter |
|--------|-------|--------|
| `+0x00` | `long` type (packed `CONCAT11(subtype, type)`) | `GetSelectedType` |
| `+0x04` | `CString` name | `GetSelectedItem` |
| `+0x08` | `CString` ProgId | `GetSelectedProgID` |

Evidence: decompilation of `GetSelectedType` (`*(long*)(*(int*)(this+0x164) + index*0xc)`),
`GetSelectedItem` (`*(int*)(this+0x164) + index*0xc + 4`), `GetSelectedProgID`
(`*(int*)(this+0x164) + index*0xc + 8`).

**This array only contains user-selected items after DoModal — NOT all items.**
For enumerating ALL items, the CMap at `this+0x3bc` (populated by LoadList) must
be walked directly.

### 1.6 Whether a live CELFileOpenDialog is required for enumeration

**A live (DoModal'd) dialog is NOT required.** `LoadList` (Ordinal_1291) does not
touch any UI control — it calls only ORLIB20.dll library functions and stores
results in the CMap at `this+0x3bc`. A `CELFileOpenDialog` instance can be
constructed (constructor Ordinal_17) and `LoadList` called directly on it without
ever calling `DoModal` or creating a window.

`InitLists` (Ordinal_1196) DOES touch UI controls (sends `LVM_INSERTITEM`/
`LVM_SETITEMDATA` to the ComboBox at `this+0x340` and CListCtrl at `this+0x2a0`)
— it should be avoided for headless enumeration. Call `LoadList` directly
instead.

### 1.7 Enumeration invocation recipe (from injected code in TSDev.exe process)

**Recommended approach (highest-level, exported ordinals):**

1. Allocate a `std::map<std::string,long>` (empty — it's the per-type open-doc
   counter; not needed for enumeration). **[DYNAMIC]**: verify an empty map
   suffices and doesn't cause issues.
2. Construct a `CELFileOpenDialog` via **Ordinal_17** (all-types mode):
   `__thiscall CELFileOpenDialog(this, &emptyMap, NULL)` — `this` must point to
   a buffer of at least ~0x3e0 bytes (the constructor initializes fields up to
   `this+0x3dc`). The `CWnd*` parent can be NULL for headless use.
   **[DYNAMIC]**: verify NULL CWnd* doesn't crash.
3. For each type category 0..12 (or type=0 for all types at once):
   - Construct a `CAnalysisTypeInfo` (0x28 bytes, via `FUN_10044820` at
     `0x10044820` — NOT exported; **[DYNAMIC]**: determine if a simpler
     initialization suffices, or if calling `LoadList` with type=0 avoids the
     need for per-type CAnalysisTypeInfo objects).
   - Call **LoadList (Ordinal_1291)**: `__thiscall LoadList(dialog, type, &cati)`.
     This populates the CMapWordToPtr at `cati+0xc` mapping recordId →
     CELTechInfo*.
4. Walk each CAnalysisTypeInfo's CMapWordToPtr (`cati+0xc`) to enumerate all
   CELTechInfo* records. For each record, read:
   - name: `CString` at `+0x10` (call `.operator wchar_t*()` or read the CString
     internals)
   - ProgId: `CString` at `+0x1c`
   - type: `short` at `+0x08`
   - subtype: `ushort` at `+0x0a`
   - record id: `ushort` at `+0x0c`
5. Destroy the dialog via **destructor (Ordinal_166)**.

**Alternative (lower-level, ORLIB20.dll direct):** Call `Ordinal_10(0)` to get
the library object, then `FUN_10074470(lib, 0, &appList)` to enumerate all,
then walk the CELApplicationList. This avoids constructing a CELFileOpenDialog
but uses non-exported ORELUI20.dll functions (`FUN_10074470`, `FUN_100732e0`,
`FUN_1006cb30`, `FUN_1006c790`) by address — more version-brittle (purpose b).
The CELFileOpenDialog approach is preferred because it uses exported ordinals.

**Type category table:** `GetAnalysisTypeCount` returns 13 (`0xd`). The type
category table is at `DAT_1008fd04` (array of `uint32` packed as
`(subtype << 8) | category`). `InitLists` iterates indices 0..12 and reads
`DAT_1008fd04[index]`. **[DYNAMIC]**: map these 13 categories to the R13 type
names (Function, Indicator, ShowMe, Strategy, TradingApp, etc.) via live
enumeration.

---

## 2. Delete routine

### 2.1 Function identity

| Function | Address | Export ordinal | Evidence |
|----------|---------|----------------|----------|
| **DeleteItem** | `0x10010dd0` | Exported (Entry Point ref confirmed; ordinal label not auto-generated by Ghidra — **[DYNAMIC]**: determine exact ordinal) | `get_xrefs_to(0x10010dd0)` → "Entry Point" EXTERNAL + DATA ref `0x100cda30` + call from `OnDelete` at `0x10012a74` |
| OnDelete (button handler) | `0x10012670` | Ordinal_1348 | `list_class_members` + symbol lookup |
| CanDeleteItem | `0x100105a0` | Ordinal_490 | `list_class_members` + symbol lookup |
| DeleteTypeInfoItem | `0x10010f50` | (internal, not exported) | `list_class_members` |

### 2.2 Calling convention and parameters

**DeleteItem** (`0x10010dd0`, decompiled + disassembled):
- **Calling convention:** `__thiscall` — `this` in `ECX`, one explicit stack
  parameter (`CELTechInfo*`), callee cleans stack (`RET 0x4`).
- **Signature:** `bool __thiscall CELFileOpenDialog::DeleteItem(CELFileOpenDialog *this, CELTechInfo *item)`
- **Parameters:**
  - `this` (ECX): the CELFileOpenDialog instance (needed for `this+0x20` parent
    HWND and `this+0x270` developer flag and `this+0x278` developer-mode flag).
  - `item` ([ESP+4]): a `CELTechInfo*` with at least type (`+0x08`), subtype
    (`+0x0a`), and record id (`+0x0c`) populated.

### 2.3 What DeleteItem does (decompiled logic)

1. Reads `type = *(ushort*)(item+8)`, `subtype = *(ushort*)(item+0xa)`,
   `id = *(ushort*)(item+0xc)`.
2. **Guard 1 — built-in function:** if `type == 4` and `this+0x278 == 0`
   (not developer mode), shows a "cannot delete built-in" message box and
   returns false.
3. **Guard 2 — open in QE:** calls `GetFileOpenInQE()` (ORLIB20.dll import at
   IAT `0x1008d860`, thunk `0x000f4934`). If the item's `(id<<8 | type)` matches
   the currently-open-in-QE item, shows a message box and returns false.
4. **Delete:** calls `Ordinal_59(parent_hwnd, type, id, 0)` — **ORLIB20.DLL
   ordinal 59** (IAT `0x1008d850`, confirmed via `get_external_location`:
   label "Ordinal_59", library "ORLIB20.DLL", address `8000003b`).
5. **Cache refresh:** calls `Ordinal_17(type, id)` — **ORLIB20.DLL ordinal 17**
   (IAT `0x1008d854`, confirmed: label "Ordinal_17", library "ORLIB20.DLL",
   address `80000011`).
6. **Internal cleanup:** calls `ConvertToPowerType(type, subtype)` (ORLIB20.dll
   import, IAT `0x1008d85c`, thunk `0x000f4902`) then
   `DeleteTypeInfoItem(this, type, powerType, id)` which removes the CELTechInfo
   from the internal CMapWordToPtr.
7. Returns `true`.

### 2.4 Relationship to _SetSigDeleted

**ORELUI20.dll does NOT import `_SetSigDeleted@4`.** Confirmed:
`list_external_locations` for ORLIB20.DLL shows `_IsSigDeleted@4` (address
`000f4b3e`) but NOT `_SetSigDeleted@4`. The delete is performed by
`Ordinal_59` (ORLIB20.dll ordinal 59), which is a **higher-level** function
taking `(HWND parent, ushort type, ushort id, int reserved)` — it internally
handles the record lookup and calls `_SetSigDeleted` (which takes a record
handle). This means:
- The invocation recipe does NOT need to call `_SetSigDeleted` directly.
- `Ordinal_59` is the stable, exported, higher-level delete surface (purpose b).
- The record-id-to-handle lookup is internal to ORLIB20.dll's `Ordinal_59`.

### 2.5 Preconditions

- **The item must not be a built-in function** (type 4) unless developer mode is
  active (`this+0x278 != 0`). For user-created items (the automation target),
  this is not an issue.
- **The item must not be currently open in the Query Editor.** `GetFileOpenInQE`
  checks this. For headless automation where documents are closed before delete,
  this is satisfied. **[DYNAMIC]**: verify this check passes when no documents
  are open.
- **No document-state manipulation needed.** DeleteItem operates at the
  library-record level (type + id → ORLIB20.dll), NOT through a TSDev document
  object. The `doc+0x174`/`doc+0x17C` conditions that block the tree-view delete
  path for Functions (per RE_DELETE_OPEN_DIALOG.md §4) do NOT apply.

### 2.6 Whether a live dialog is required for delete

**A live (DoModal'd) dialog is NOT required for DeleteItem itself.** DeleteItem
operates on a `CELTechInfo*` and calls ORLIB20.dll directly. It does use
`this+0x20` (the dialog's HWND) as the parent for potential error message boxes
— with a non-windowed dialog instance, this would be NULL or invalid.
**[DYNAMIC]**: verify that passing NULL/invalid HWND doesn't crash when the
delete succeeds (no error message box is shown on success).

Alternatively, the delete can be performed **without any CELFileOpenDialog
instance** by calling `Ordinal_59` and `Ordinal_17` directly (both are stable
ORLIB20.dll ordinal exports):
```
Ordinal_59(NULL, type, recordId, 0);   // ORLIB20.dll ordinal 59
Ordinal_17(type, recordId);            // ORLIB20.dll ordinal 17
```
This requires only the type and record id, which come from the enumeration
(CELTechInfo+0x08 and +0x0c). **[DYNAMIC]**: verify this direct approach works
and doesn't miss any necessary state setup that DeleteItem performs.

### 2.7 Delete invocation recipe (from injected code in TSDev.exe process)

**Recommended approach:**

1. **Enumerate** to find the target item's CELTechInfo* by name + type
   (see §1.7). This gives `type` (+0x08) and `recordId` (+0x0c).
2. **Delete via DeleteItem** (exported, `0x10010dd0`):
   `__thiscall DeleteItem(dialog, CELTechInfo*)` — `ECX=dialog`, `[ESP+4]=item`.
   The dialog instance from step 1 can be reused.
3. **Or delete via ORLIB20.dll directly** (stable ordinal exports, no dialog
   needed):
   - `Ordinal_59(NULL, type, recordId, 0)` — ORLIB20.dll ordinal 59
   - `Ordinal_17(type, recordId)` — ORLIB20.dll ordinal 17
4. Destroy the dialog via destructor (Ordinal_166) if constructed.

**OnDelete (the full UI button handler, Ordinal_1348)** is NOT needed — it adds
confirmation dialogs and selected-list iteration that are unnecessary for
programmatic delete. Call DeleteItem (or Ordinal_59+17) directly.

---

## 3. Per-operation assessment (confirmed/corrected)

| Op | Benefits? | Evidence |
|----|-----------|----------|
| `delete` | **YES — confirmed** | `DeleteItem` (exported, `0x10010dd0`) + `Ordinal_59` (ORLIB20.dll ord 59) delete at library-record level (type+id), no document-state manipulation. Replaces the 7 brittle destructor-path coupling points. |
| `list` | **YES — confirmed** | `LoadList` (Ordinal_1291, exported) enumerates all items into CMap; CELTechInfo provides verbatim name (+0x10), ProgId (+0x1c), type (+0x08). No live dialog needed. |
| `exists` | **YES — confirmed** | Same enumeration surface as `list`; verbatim names enable exact case-sensitive matching; ProgIds available. |
| `getsrc` | **No — confirmed** | ORELUI20.dll enumeration provides metadata (name, ProgId, type), not source content. Source is document content (WM_GETTEXT on editor), unrelated. |
| `insert` | **No — confirmed** | No library-level insert/create routine in ORELUI20.dll. `search_functions("InsertItem")` found only `CTSTreeListItem::InsertItem` (tree-list UI, `0x1005da50`) and `CTransferExportSelectionPage::CanInsertItem` (transfer wizard UI, `0x1004de80`) — neither creates library items. Insert remains via TSDev.exe's New-dialog + verify path. |
| `modify` | **No — confirmed** | Same as getsrc — operates on document content, not library records. |
| `upsert` | **No (transitive) — confirmed** | Composition of insert/modify/exists. Benefits transitively from `exists` robustification, but no direct ORELUI20.dll dependency. |

---

## 4. ORLIB20.dll import decision

ORLIB20.dll was **NOT imported** into Ghidra. The delete delegates to ORLIB20.dll
via `Ordinal_59` (ordinal 59) and `Ordinal_17` (ordinal 17), but the invocation
recipe is fully characterized from the ORELUI20.dll call sites (DeleteItem's
decompilation + disassembly shows the exact arguments). The internal behavior of
Ordinal_59 (whether/how it calls `_SetSigDeleted`) does not matter for the
invocation recipe — we know its signature: `Ordinal_59(HWND, ushort type, ushort
id, int reserved)`. Importing ORLIB20.dll would add RE work (purpose a) without
changing the recipe. If subtask B's dynamic verification reveals unexpected
behavior (e.g. Ordinal_59 requires additional state), ORLIB20.dll can be imported
then.

---

## 5. Items flagged for subtask B (dynamic verification)

1. **[DYNAMIC]** Constructing a CELFileOpenDialog with NULL CWnd* and an empty
   map — verify no crash and LoadList works.
2. **[DYNAMIC]** Calling LoadList with type=0 (all types) — verify it populates
   the CMap with all items, and determine whether per-type CAnalysisTypeInfo
   objects are needed or if a single call suffices.
3. **[DYNAMIC]** CAnalysisTypeInfo construction — `FUN_10044820` (`0x10044820`)
   is not exported; determine the minimum initialization needed, or whether
   LoadList's internal `FUN_10044ab0` initializes it sufficiently.
4. **[DYNAMIC]** Walking the CMapWordToPtr at `cati+0xc` — verify the iteration
   approach and that all CELTechInfo* records are accessible.
5. **[DYNAMIC]** DeleteItem with NULL/invalid HWND (`this+0x20`) — verify no
   crash on successful delete (error message boxes are only shown on failure).
6. **[DYNAMIC]** Direct `Ordinal_59(NULL, type, id, 0)` + `Ordinal_17(type, id)`
   without a dialog — verify this works standalone.
7. **[DYNAMIC]** Map the 13 type categories (DAT_1008fd04) to R13 type names
   (Function, Indicator, ShowMe, Strategy, TradingApp, etc.).
8. **[DYNAMIC]** DeleteItem's exact export ordinal (Ghidra didn't auto-label it;
   the Entry Point ref confirms it's exported).
9. **[DYNAMIC]** `FUN_10074310` uses thread marshaling (WaitForSingleObject/
   SetEvent) — verify this works from injected code in the TSDev.exe process
   (the ORLIB20 worker thread should already be running).
10. **[DYNAMIC]** Verify names are truly verbatim by comparing enumerated names
    against known items with mixed-case names.

---

## Appendix A — Evidence: key Ghidra MCP calls

### A.1 Import + program info
- `import_file("C:\Program Files (x86)\TradeStation 9.5\Program\ORELUI20.dll", auto_analyze=true)` → success, 7486 functions, base 0x10000000.
- `list_open_programs()` → TSDev.exe + ORELUI20.dll (current).

### A.2 Class discovery
- `list_classes()` → found `CELFileOpenDialog` and `CELConfirmDeleteDlg` among 410 classes.
- `list_class_members("CELFileOpenDialog")` → 76 members with addresses.

### A.3 Key decompilations
- `decompile_function(0x10012670)` → **OnDelete**: calls UpdateSelectedList, iterates selected CELTechInfo*, calls CanDeleteItem + confirmation dialog + DeleteItem.
- `decompile_function(0x10010dd0)` → **DeleteItem**: reads type/subtype/id from CELTechInfo, calls Ordinal_59 + Ordinal_17 + DeleteTypeInfoItem, returns true.
- `decompile_function(0x10012290)` → **LoadList**: Ordinal_10(0) → FUN_10074470/FUN_10074310 → CELApplicationList → FUN_10044b90 per item → CMapWordToPtr.
- `decompile_function(0x10044b90)` → **populate CELTechInfo**: FUN_100732e0 gets info_obj, copies name (FUN_1006cb30), ProgId (FUN_1006c790), type, subtype, id.
- `decompile_function(0x10044960)` → **CELTechInfo constructor**: initializes CStrings at +0x10/+0x14/+0x18/+0x1c, double at +0x20, CStringArray at +0x2c, ptr at +0x40.
- `decompile_function(0x1000f2f0)` → **constructor (all-types)**: `__thiscall(this, map<string,long>*, CWnd*)`, initializes CMap at +0x3bc, CList at +0x3a0, sets +0x28c=0.
- `decompile_function(0x1000f4e0)` → **constructor (single-type)**: `__thiscall(this, wchar_t*, ushort, map*, CWnd*)`, sets +0x28c=1.
- `decompile_function(0x10011960)` → **GetSelectedProgID**: reads `*(this+0x164) + index*0xc + 8` (CString ProgId).
- `decompile_function(0x10011930)` → **GetSelectedItem**: reads `*(this+0x164) + index*0xc + 4` (CString name).
- `decompile_function(0x10011990)` → **GetSelectedType**: reads `*(this+0x164) + index*0xc` (long type).
- `decompile_function(0x10011220)` → **GetAnalysisTypeCount**: returns 0xd (13) or 1 (single-type mode).
- `decompile_function(0x100105a0)` → **CanDeleteItem**: IsDocumentOpen check, built-in check, name CompareNoCase.
- `decompile_function(0x10012f80)` → **OnInitDialog**: calls InitLists.
- `decompile_function(0x10011cf0)` → **InitLists**: iterates 13 types, calls LoadList per type, populates ComboBox (UI).
- `decompile_function(0x10013330)` → **OnOK**: UpdateSelectedList, builds selected-items array at this+0x164 (12-byte records: type/name/ProgId).
- `decompile_function(0x10015d00)` → **UpdateSelectedList**: iterates CListCtrl selected items via LVM_GETNEXTITEM, builds CList<CELTechInfo*> at this+0x3a0.
- `decompile_function(0x10074470)` → **FUN_10074470 (enumerate all)**: iterates type categories, calls FUN_10074310 per type.
- `decompile_function(0x10074310)` → **FUN_10074310 (enumerate by type)**: thread-marshaled ORLIB20 call, SAFEARRAY of record handles.
- `decompile_function(0x1005da50)` → **CTSTreeListItem::InsertItem**: tree-list UI, NOT a library insert.
- `decompile_function(0x1004de80)` → **CTransferExportSelectionPage::CanInsertItem**: transfer wizard UI, NOT a library insert.

### A.4 IAT / external location lookups
- `disassemble_function(0x10010dd0)` → DeleteItem calls: `[0x1008d850]` (Ordinal_59), `[0x1008d854]` (Ordinal_17), `[0x1008d85c]` (ConvertToPowerType), `[0x1008d860]` (GetFileOpenInQE).
- `read_memory(0x1008d850, 16)` → `3b000080 11000080 02490f00 1e490f00` → ext ord 59, ext ord 17, thunk 0x100f4902, thunk 0x100f491e.
- `get_external_location(ORLIB20.DLL, 8000003b)` → "Ordinal_59", ORLIB20.DLL.
- `get_external_location(ORLIB20.DLL, 80000011)` → "Ordinal_17", ORLIB20.DLL.
- `get_external_location(ORLIB20.DLL, 8000000a)` → "Ordinal_10", ORLIB20.DLL.
- `list_external_locations(filter="Ordinal")` → confirmed Ordinal_10 from ORLIB20.DLL; `_IsSigDeleted@4` present but `_SetSigDeleted@4` ABSENT.
- `disassemble_bytes(0x10080df6, 6)` → `JMP [0x1008d83c]` (Ordinal_10 thunk).

### A.5 Export ordinal confirmation
- `get_xrefs_to(0x10010dd0)` → DATA `0x100cda30` + "Entry Point" EXTERNAL + call from OnDelete → DeleteItem is exported.
- `get_xrefs_to(0x10012290)` → DATA `0x100ce530` + "Entry Point" EXTERNAL + calls from InitLists → LoadList is exported.
- `get_xrefs_to(0x10011960)` → DATA + "Entry Point" EXTERNAL → GetSelectedProgID is exported.
- Symbol lookup script → ordinal labels: LoadList=Ordinal_1291, InitLists=Ordinal_1196, CanDeleteItem=Ordinal_490, OnDelete=Ordinal_1348, GetAnalysisType=Ordinal_706, GetAnalysisTypeCount=Ordinal_707, constructor=Ordinal_17, destructor=Ordinal_166, GetSelectedCount=Ordinal_964, GetSelectedItem=Ordinal_965, GetSelectedType=Ordinal_968, GetSelectedProgID=Ordinal_967.

### A.6 Insert surface (negative evidence)
- `search_functions("InsertItem")` → 6 results: `CTSTreeListItem::InsertItem` (0x1005da50, tree-list UI), `CTransferExportSelectionPage::CanInsertItem` (0x1004de80, transfer wizard). No library-level insert/create function.
