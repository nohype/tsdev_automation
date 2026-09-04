# ORELUI20.dll — Enumeration + Delete (TVS #7 final research deliverable)

Final consolidated research deliverable for TVS #7. Combines subtask A
(static RE, [RE_ORELUI20_STATIC.md](RE_ORELUI20_STATIC.md)) and subtask B
(dynamic verification, [RE_ORELUI20_DYNAMIC.md](RE_ORELUI20_DYNAMIC.md)) into
the authoritative recipes and assessment.

**Proven facts live in the SSOT** ([docs/KNOWLEDGE_BASE.md](KNOWLEDGE_BASE.md),
"ORELUI20.dll + ORLIB20.dll ordinals" section). This document references the
SSOT and holds the consolidated reasoning + evidence appendix; it does NOT
restate the SSOT counts/lists as the canonical source.

Every claim cites A's Ghidra finding or B's Frida result. No speculation.

---

## 1. Summary

TVS #7 characterized the `ORELUI20.dll` item-enumeration surface and the
`ORELUI20.dll` delete routine, both **Frida-verified from within the live
TSDev.exe process** using exported/stable surfaces (ordinal exports). The
enumeration surface enumerates all library items with verbatim stored names,
ProgIds, type, subtype, and record id. The delete routine deletes items at the
**library-record level** (type + record id → `ORLIB20.dll`), with no
document-state manipulation.

**Implication:** this unblocks three subsequent implementation TVSs:
- **Reimplement `tsdevctl delete`** via `DeleteItem` (Ordinal 587) — replaces
  the current destructor-path delete's 7 binary-internal coupling points (3
  vtable offsets, 2 struct offsets, 2 function addresses) with one exported
  ordinal call that operates at the library-record level.
- **Implement `tsdevctl list`** (currently unimplemented; R2 requires it) via
  the enumeration surface.
- **Robustify `tsdevctl exists`** — the enumeration gives verbatim stored
  names + ProgIds + enables exact case-sensitive matching, fixing the current
  limitations (reports requested name not verbatim, no ProgId,
  case-insensitive matching, substring-rule false positives).

`getsrc`, `insert`, `modify`, and `upsert` do NOT benefit from the
`ORELUI20.dll` module-calling approach (see §4).

---

## 2. Enumeration surface

### 2.1 Final corrected invocation recipe

The recipe below incorporates the **three corrections** subtask B found to
subtask A's static recipes (see §2.2). This is the recipe to build on.

1. **Allocate a `std::map<std::string,long>`** (empty — it is the per-type
   open-document counter; not needed for enumeration). Frida-verified: a
   zero-initialized 0x40-byte buffer suffices (B §2.8).
2. **Construct a `CELFileOpenDialog`** via **Ordinal_17** (all-types
   constructor, `__thiscall CELFileOpenDialog(this, map*, CWnd*)`):
   - `this` must point to a buffer of at least ~0x3e0 bytes (constructor
     initializes fields up to `this+0x3dc`).
   - `CWnd*` parent = **NULL** for headless use. Frida-verified: NULL CWnd*
     does not crash, no dialog window is created (B §2.8).
3. **For each of the 13 type entries** in the type-category table at
   `DAT_1008fd04` (iterate index 0..12):
   - Read the packed value `packed = DAT_1008fd04[index]` (a `uint32`).
     Decompose: `category = packed & 0xff`, `subtype = (packed >> 8) & 0xff`.
     (Full type table in the SSOT and §2.3 below.)
   - **Construct a `CAnalysisTypeInfo`** (0x28 bytes) via **`FUN_10044820`**
     (`0x10044820`, NOT exported) with **4 parameters**:
     `__thiscall FUN_10044820(this, int category, int subtype, int index)`.
     This populates `this+0x00`=category, `this+0x04`=subtype (the power-type
     filter LoadList applies), `this+0x08`=index, `this+0x0c`=CMapWordToPtr
     (hash size 10). **Correction B-1:** the static recipe's 1-parameter form
     leaves the subtype filter as zero/garbage and LoadList rejects all items.
   - **Call `LoadList` (Ordinal_1291)**:
     `__thiscall LoadList(dialog, packed, &cati)`. **Correction B-2:** call
     LoadList **per-type with the packed value from `DAT_1008fd04`**, NOT with
     `type=0`. The static recipe's `type=0` "all types" path does NOT bypass
     the power-type filter (`cati+0x04`); it must be called once per type
     entry, matching `InitLists`.
   - This populates the `CMapWordToPtr` at `cati+0x0c` mapping
     `recordId → CELTechInfo*`.
4. **Walk each `CAnalysisTypeInfo`'s `CMapWordToPtr`** (`cati+0x0c`) to
   enumerate all `CELTechInfo*` records. **Correction B-3:** the `CAssoc`
   layout is **swapped** from standard MFC — it is
   `{ pNext@0x00, value@0x04, key@0x08 }`, NOT standard MFC's
   `{ pNext@0x00, key@0x04, value@0x08 }`. With the standard layout the
   "values" read as garbage (non-pointer integers); with the swapped layout
   they are valid `CELTechInfo*` pointers. Walking code must use:
   - `value = pAssoc->offset(0x04).readPointer()`  (`CELTechInfo*`)
   - `key   = pAssoc->offset(0x08).readU16()`       (recordId)
5. **For each `CELTechInfo*` record**, read the per-item fields (see §2.4).
6. **Destroy the dialog** via **destructor (Ordinal_166)**. Frida-verified: no
   crash on destruction (B §5).

**Avoid `InitLists` (Ordinal_1196)** — it touches UI controls (sends
`LVM_INSERTITEM`/`LVM_SETITEMDATA` to the ComboBox at `this+0x340` and CListCtrl
at `this+0x2a0`). Call `LoadList` directly for headless enumeration (A §1.6).

### 2.2 The three corrections (B → A)

| # | Static recipe (A) | Correction (B) | Evidence |
|---|-------------------|-----------------|----------|
| 1 | `CAnalysisTypeInfo` ctor `FUN_10044820` takes 1 param | Takes **4 params**: `(this, category, subtype, index)` | B §2.2.1; with 1 param the subtype filter is zero/garbage and LoadList rejects all items |
| 2 | `LoadList(type=0)` enumerates all types at once | Must call **per-type with packed values from `DAT_1008fd04`**; `type=0` does not bypass the power-type filter | B §2.2.2; matches `InitLists` iteration |
| 3 | `CMapWordToPtr` CAssoc = standard MFC `{ pNext@0, key@4, value@8 }` | **Swapped**: `{ pNext@0, value@4, key@8 }` | B §2.2.3; standard layout yields garbage non-pointer "values", swapped yields valid `CELTechInfo*` |

### 2.3 Type-category table (`DAT_1008fd04`) — dynamically confirmed

The full type stored in `CELTechInfo+0x08` is the **category** (low byte);
`CELTechInfo+0x0a` is the **subtype** (high byte). Full type =
`(subtype << 8) | category`. (SSOT holds this table.)

| Index | Packed | Category | Subtype | Type name |
|-------|--------|----------|---------|-----------|
| 0 | 0x1402 | 2 | 20 | TradingApp |
| 1 | 0x0002 | 2 | 0 | Indicator |
| 2 | 0x0502 | 2 | 5 | ShowMe |
| 3 | 0x0402 | 2 | 4 | PaintBar |
| 4 | 0x0a02 | 2 | 10 | VolatilityModel |
| 5 | 0x0c02 | 2 | 12 | PricingModel |
| 6 | 0x0e02 | 2 | 14 | BidAskModel |
| 7 | 0x1002 | 2 | 16 | SearchStrategy |
| 8 | 0x1202 | 2 | 18 | ProbabilityMap |
| 9 | 0x0000 | 0 | 0 | Strategy |
| 10 | 0x0003 | 3 | 0 | ActivityBar |
| 11 | 0x0001 | 1 | 0 | Function |
| 12 | 0x0004 | 4 | 0 | Function (built-in) |

### 2.4 Per-item data fields (`CELTechInfo`, 0x44 bytes)

| Offset | Type | Field | Source |
|--------|------|-------|--------|
| `+0x08` | `uint16` | **category** (type low byte) | A `FUN_10044b90`; B-confirmed (Strategy=0, Function=1, Indicator=2, TradingApp=2) |
| `+0x0a` | `uint16` | **subtype** (type high byte) | A `FUN_10044b90`; B-confirmed (TradingApp=20, ShowMe=5, Indicator=0) |
| `+0x0c` | `uint16` | **record id** | A `FUN_10044b90`; B-confirmed (matches CMap key) |
| `+0x10` | `CString` | **name** (verbatim stored name) | A `FUN_1006cb30`; B-confirmed (mixed-case preserved) |
| `+0x1c` | `CString` | **ProgId** | A `FUN_1006c790`; B-confirmed (matches name for user items) |
| `+0x40` | `void*` | info object pointer (ORLIB20 record handle) | A `FUN_100732e0`; B-confirmed (valid handle) |

Other fields (`+0x00` category info, `+0x14` short name, `+0x18` display name,
`+0x20` date, `+0x28` zero, `+0x2c` CStringArray) are documented in A §1.3 but
are not needed for automation.

### 2.5 Exported surfaces used (evidence)

| Surface | Ordinal | Address | Evidence |
|---------|---------|---------|----------|
| `CELFileOpenDialog` ctor (all-types) | **Ordinal_17** | `0x1000f2f0` | A §1.1: `list_class_members` + `get_xrefs_to` "Entry Point" EXTERNAL refs + Ordinal_XXX labels |
| `LoadList` | **Ordinal_1291** | `0x10012290` | A §1.1: same evidence chain |
| `CELFileOpenDialog` dtor | **Ordinal_166** | `0x1000fad0` | A §1.1 |

### 2.6 Is a live/DoModal'd dialog required?

**NO.** `LoadList` (Ordinal_1291) does not touch any UI control — it calls only
`ORLIB20.dll` library functions and stores results in the `CMapWordToPtr` at
`this+0x3bc`. A `CELFileOpenDialog` can be constructed (Ordinal_17) and
`LoadList` called directly on it without ever calling `DoModal` or creating a
window. **Frida-verified:** NULL `CWnd*` + empty map, no crash, no dialog
window (B §2.8). Headless construction works.

### 2.7 Non-exported dependency (version-brittle — purpose b)

`CAnalysisTypeInfo` construction uses **`FUN_10044820`** (`0x10044820`), which
is **NOT exported** by ordinal. Production code must compute its address from
the `ORELUI20.dll` module base + offset `0x44820`. This offset is stable for
the analyzed build (TSDev 9.5, build Nov 30 2018) but **may change across
TSDev/TradeStation versions** — flag as version-brittle per purpose (b). This
is the only non-exported surface the enumeration recipe requires; all other
surfaces are exported ordinals.

### 2.8 Dynamic verification result

- **2958 items enumerated across 9 types** (B §2.5; full per-type counts in
  the SSOT and the evidence appendix §6.2).
- **5 `E2eTvs7_Enum_*` test items found** with correct verbatim names + types
  (B §2.6): `E2eTvs7_EnumFn_MixedCase` (Function), `...EnumInd_MixedCase`
  (Indicator), `...EnumStr_MixedCase` (Strategy), `...EnumSm_MixedCase`
  (ShowMe), `...EnumTa_MixedCase` (TradingApp).
- **Mixed-case preserved** (B §2.7): `E2eTvs7_EnumFn_MixedCase` enumerated as
  exactly `E2eTvs7_EnumFn_MixedCase` — no normalization.

---

## 3. Delete routine

### 3.1 Final corrected invocation recipe

1. **Enumerate** to find the target item's `CELTechInfo*` by name + type
   (§2.1). This gives `type` (`+0x08`), `subtype` (`+0x0a`), and
   `recordId` (`+0x0c`).
2. **Delete via `DeleteItem`** (exported, Ordinal 587):
   `__thiscall DeleteItem(dialog, CELTechInfo*)` — `ECX=dialog`,
   `[ESP+4]=item`. The dialog instance from step 1 can be reused.
   `DeleteItem` returns `true` on success.
3. **Destroy the dialog** via destructor (Ordinal_166) if constructed.

`DeleteItem` internally performs the `ORLIB20.dll` delegation
(`Ordinal_59` library delete + `Ordinal_17` cache refresh) AND the dialog's
internal list cleanup. **Prefer `DeleteItem` over calling `Ordinal_59`/17
directly** — it is the higher-level path and adds the built-in-protection and
QE-open safety checks (B §3.2, §7).

**Alternative direct path (lower-level, not needed):** `Ordinal_59(NULL, type,
recordId, 0)` + `Ordinal_17(type, recordId)` — both stable `ORLIB20.dll`
ordinal exports. This requires only type + record id (from enumeration) and no
dialog instance. **Not Frida-tested** (B §5: redundant — `DeleteItem` already
calls these). Documented as a fallback; `DeleteItem` is the recommended path.

### 3.2 `DeleteItem` identity

| Item | Value | Evidence |
|------|-------|----------|
| Function | `CELFileOpenDialog::DeleteItem` | A §2.1: `get_xrefs_to(0x10010dd0)` → "Entry Point" EXTERNAL + call from `OnDelete` at `0x10012a74` |
| Address | `0x10010dd0` (analyzed image base `0x10000000`) | A §2.1 |
| **Export ordinal** | **587 (0x24A)** | B §3.1: `dumpbin /EXPORTS ORELUI20.dll` → `587  24A 00010DD0 ?DeleteItem@CELFileOpenDialog@@IAE_NPAVCELTechInfo@@@Z` (resolves A's `[DYNAMIC]` flag) |
| Calling convention | `__thiscall`, RET 0x4 (callee cleans) | A §2.2: decompiled + disassembled |
| Signature | `bool __thiscall DeleteItem(CELFileOpenDialog *this, CELTechInfo *item)` | A §2.2 |

### 3.3 Internal delegation (decompiled logic, A §2.3; B §3.2 confirmed)

1. Reads `type = *(ushort*)(item+0x08)`, `subtype = *(ushort*)(item+0x0a)`,
   `id = *(ushort*)(item+0x0c)`.
2. **Guard 1 — built-in function:** if `type == 4` and `this+0x278 == 0`
   (not developer mode), shows a "cannot delete built-in" message box and
   returns `false`.
3. **Guard 2 — open in QE:** calls `GetFileOpenInQE()` (`ORLIB20.dll` import,
   IAT `0x1008d860`). If the item's `(id<<8 | type)` matches the
   currently-open-in-QE item, shows a message box and returns `false`.
4. **Delete:** `Ordinal_59(parent_hwnd, type, id, 0)` — `ORLIB20.dll` ordinal
   59 (IAT `0x1008d850`; `get_external_location`: label "Ordinal_59", library
   "ORLIB20.DLL"). This is the library-record-level delete (internally handles
   the record-id-to-handle lookup and calls `_SetSigDeleted`).
5. **Cache refresh:** `Ordinal_17(type, id)` — `ORLIB20.dll` ordinal 17 (IAT
   `0x1008d854`).
6. **Internal cleanup:** `ConvertToPowerType(type, subtype)` then
   `DeleteTypeInfoItem(this, type, powerType, id)` removes the `CELTechInfo`
   from the internal `CMapWordToPtr`.
7. Returns `true`.

**`ORELUI20.dll` does NOT import `_SetSigDeleted@4`** (A §2.4:
`list_external_locations` for ORLIB20.DLL shows `_IsSigDeleted@4` but NOT
`_SetSigDeleted@4`). The delete is performed by `Ordinal_59`, a higher-level
function taking `(HWND, ushort type, ushort id, int reserved)`; the
record-id-to-handle lookup is internal to `ORLIB20.dll`. The invocation recipe
does NOT need to call `_SetSigDeleted` directly.

### 3.4 Preconditions

- **Item must not be a built-in function** (type 4) unless developer mode is
  active (`this+0x278 != 0`). Not an issue for user-created items (the
  automation target).
- **Item must not be currently open in the Query Editor.** `GetFileOpenInQE`
  checks this. For headless automation where documents are closed before
  delete, this is satisfied.
- **No document-state manipulation needed.** `DeleteItem` operates at the
  library-record level (type + id → `ORLIB20.dll`), NOT through a TSDev
  document object. The `doc+0x174`/`doc+0x17C` conditions that block the
  tree-view delete path for Functions (per `RE_DELETE_OPEN_DIALOG.md` §4) do
  NOT apply. **This is the key advantage over the current destructor-path
  delete**, which needs 7 binary-internal coupling points (3 vtable offsets,
  2 struct offsets, 2 function addresses) to manufacture the destructor
  conditions.

### 3.5 Is a live dialog required for delete?

**NO.** `DeleteItem` operates on a `CELTechInfo*` and calls `ORLIB20.dll`
directly. It uses `this+0x20` (the dialog's HWND) as the parent for potential
error message boxes — with a non-windowed dialog instance this is NULL.
Frida-verified: passing NULL/invalid HWND does not crash when the delete
succeeds (no error message box is shown on success) (B §3.4, §3.6).

### 3.6 Dynamic verification result

Per-type delete results (B §3.4; script `tmp/frida_delete.js`):

| Type | Item | Method | `DeleteItem` return | `tsdevctl exists` | Frida re-enum | Dialogs |
|------|------|--------|---------------------|-------------------|---------------|---------|
| Function | `E2eTvs7_DelFn_001` | DeleteItem (Ord 587) | **true** | timeout (tsdevctl issue) | **CONFIRMED_DELETED** | None |
| Indicator | `E2eTvs7_DelInd_001` | DeleteItem (Ord 587) | **true** | timeout (tsdevctl issue) | **CONFIRMED_DELETED** | None |
| ShowMe | `E2eTvs7_DelSm_001` | DeleteItem (Ord 587) | **true** | **exists=false** (exit 1) | **CONFIRMED_DELETED** | None |
| TradingApp | `E2eTvs7_DelTa_001` | DeleteItem (Ord 587) | **true** | **exists=false** (exit 1) | **CONFIRMED_DELETED** | None |
| Strategy | `E2eTvs7_DelStr_001` | N/A (insert failed) | N/A | N/A | N/A | None |

**Minimal bar exceeded.** The required minimal bar was TradingApp (mandatory —
highest-risk type; the destructor path needed a special-case for it) + at
least 2 other distinct types. Achieved: **TradingApp + 3 others** (Function,
Indicator, ShowMe) — all deleted and confirmed gone (B §3.5).

**Strategy was NOT Frida-tested** — the `tsdevctl insert --type Strategy`
production command timed out (exit 6) before a test item could be created, so
no Strategy item existed to Frida-delete. This is a **tsdevctl production
issue, NOT a Frida failure** (B §3.3). The `DeleteItem` recipe is type-agnostic
(operates on type + record id from enumeration); there is no
Strategy-specific code path in `DeleteItem`. See §5 for the production bugs.

**All deletions confirmed via Frida re-enumeration** (script
`frida_verify_del.js`); ShowMe + TradingApp also confirmed via
`tsdevctl exists` (B §3.4, §3.6).

---

## 4. Per-operation assessment (confirmed/corrected)

This is the answer to the user's question that motivated TVS #7: can other
operations than `delete` be simplified/robustified with the module-calling
approach?

| Op | Benefits? | One-line evidence |
|----|-----------|-------------------|
| `delete` | **YES** | `DeleteItem` (Ord 587, exported, `0x10010dd0`) + `Ordinal_59` (ORLIB20 ord 59) delete at library-record level (type+id), no document-state manipulation; Frida-verified for 4 types incl. TradingApp (B §3.4). Replaces the 7 brittle destructor-path coupling points. |
| `list` | **YES** | `LoadList` (Ord 1291, exported) enumerates all items into `CMapWordToPtr`; `CELTechInfo` provides verbatim name (`+0x10`), ProgId (`+0x1c`), type (`+0x08`); Frida-verified: 2958 items across 9 types, 5 test items found with correct names (B §2.5–2.6). No live dialog needed. |
| `exists` | **YES** | Same enumeration surface as `list`; verbatim names enable exact case-sensitive matching; ProgIds available at `+0x1c`; mixed-case preserved (B §2.7). Fixes current limitations (requested-name-not-verbatim, no ProgId, case-insensitive, substring false positives). |
| `getsrc` | **No** | `ORELUI20.dll` enumeration provides metadata (name, ProgId, type), not source content. Source is document content (`WM_GETTEXT` on editor), unrelated. (A §3) |
| `insert` | **No** | No library-level insert/create routine in `ORELUI20.dll`. `search_functions("InsertItem")` found only `CTSTreeListItem::InsertItem` (tree-list UI) and `CTransferExportSelectionPage::CanInsertItem` (transfer wizard UI) — neither creates library items. Insert remains via TSDev.exe's New-dialog + verify path. (A §3) |
| `modify` | **No** | Same as `getsrc` — operates on document content, not library records. (A §3) |
| `upsert` | **No (transitive)** | Composition of insert/modify/exists. Benefits transitively from `exists` robustification, but no direct `ORELUI20.dll` dependency. (A §3) |

---

## 5. Findings affecting the current production code

The following **tsdevctl production bugs** were discovered during subtask B's
dynamic verification. They are **OUT OF SCOPE for this research TVS to fix**
(no production code changes in TVS #7), but must be recorded for the next
implementation TVS.

> **production bug — not fixed in TVS #7 (research only); flagged for next
> implementation TVS.**

1. **`tsdevctl insert --type Strategy` timeout.** The current tsdevctl
   production insert command consistently times out (exit 6, "item did not
   open within the timeout") for Strategy type. Observed during B's delete-test
   item creation (B §3.3). The TSDev main window had "MyTestStrategyA :
   Strategy" open, which may have interfered. No dialog or crash appeared
   during the insert attempts. This blocked Frida-delete verification for
   Strategy (no test item could be created). **Not a Frida issue.**

2. **`tsdevctl exists` timeout for Function/Indicator.** During B's
   delete-verification, `tsdevctl exists` for Function and Indicator types
   timed out (B §3.4). Deletions were instead confirmed via Frida
   re-enumeration. **Not a Frida issue.**

3. **`tsdevctl delete --type Strategy` crash.** `tsdevctl delete --type
   Strategy` (the current destructor-path production delete) caused a
   `TSCrashReport` — TSDev PID 3140 crashed (B §4, dialog log entry #1). The
   crash dialog had no readable child text. TSDev was restarted (PID 16600)
   and cleanup completed via Frida `DeleteItem`. This is a **tsdevctl
   production bug** that must be fixed before the production E2E gate (TVS #7
   step E). **Not caused by Frida instrumentation** — no dialogs or crashes
   occurred during any Frida enumeration or delete operation.

These bugs are recorded as known issues in the SSOT
([docs/KNOWLEDGE_BASE.md](KNOWLEDGE_BASE.md), "ORELUI20.dll + ORLIB20.dll
ordinals" section). The next implementation TVS should address them (or
confirm they are obsoleted by the `ORELUI20.dll`-based reimplementation).

---

## 6. Evidence appendix

Key findings so a reviewer can verify without re-running. Full detail in
[RE_ORELUI20_STATIC.md](RE_ORELUI20_STATIC.md) (A) and
[RE_ORELUI20_DYNAMIC.md](RE_ORELUI20_DYNAMIC.md) (B).

### 6.1 Key Ghidra findings (subtask A)

| Finding | Address / ordinal | Evidence |
|---------|-------------------|----------|
| `CELFileOpenDialog` ctor (all-types) | Ordinal_17, `0x1000f2f0` | `list_class_members` (76 members) + `get_xrefs_to` "Entry Point" EXTERNAL refs + Ordinal_XXX labels (A §1.1) |
| `LoadList` | Ordinal_1291, `0x10012290` | same; decompiled (A §1.2) — calls `ORLIB20.dll` lib functions, stores in `CMapWordToPtr` at `this+0x3bc`, no UI |
| `CELFileOpenDialog` dtor | Ordinal_166, `0x1000fad0` | A §1.1 |
| `CELTechInfo` ctor | `FUN_10044960`, `0x10044960` | decompiled (A §1.3) — 0x44 bytes, inits CStrings at +0x10/+0x14/+0x18/+0x1c |
| `CELTechInfo` populator | `FUN_10044b90`, `0x10044b90` | decompiled (A §1.3) — field sources |
| `CAnalysisTypeInfo` ctor | `FUN_10044820`, `0x10044820` (NOT exported) | A §1.7; B corrected to 4 params (B §2.2.1) |
| `DeleteItem` | Ordinal 587 (0x24A), `0x10010dd0` | A §2.1 (`get_xrefs_to` "Entry Point" + call from `OnDelete` `0x10012a74`); B §3.1 (`dumpbin /EXPORTS`) |
| `DeleteItem` calling convention | `__thiscall`, RET 0x4 | A §2.2 (decompiled + disassembled) |
| `DeleteItem` → `Ordinal_59` | ORLIB20 ord 59, IAT `0x1008d850` | `get_external_location`: label "Ordinal_59", library "ORLIB20.DLL" (A §2.3) |
| `DeleteItem` → `Ordinal_17` | ORLIB20 ord 17, IAT `0x1008d854` | `get_external_location`: label "Ordinal_17", library "ORLIB20.DLL" (A §2.3) |
| `ORELUI20.dll` does NOT import `_SetSigDeleted@4` | — | `list_external_locations` for ORLIB20.DLL shows `_IsSigDeleted@4` but NOT `_SetSigDeleted@4` (A §2.4) |
| Type-category table | `DAT_1008fd04`, 13 entries | `GetAnalysisTypeCount` returns 13; `InitLists` iterates 0..12 (A §1.2); B confirmed mapping (B §2.3) |

### 6.2 Key Frida results (subtask B)

**Environment:** elevated shell; TSDev PID 3140 (crashed during cleanup via
tsdevctl, not Frida) → restarted PID 16600; Frida 17.17.0; `ORELUI20.dll`
runtime base `0x01a00000` (ASLR; static base `0x10000000`).

**Enumeration (B §2.5):** 2958 items across 9 types —

| Full type | Type name | Count |
|-----------|-----------|-------|
| 0x0000 | Strategy | 1655 |
| 0x0001 | Function | 92 |
| 0x0002 | Indicator | 244 |
| 0x0003 | ActivityBar | 5 |
| 0x0004 | Function (built-in) | 852 |
| 0x0402 | PaintBar | 29 |
| 0x0502 | ShowMe | 65 |
| 0x1202 | ProbabilityMap | 2 |
| 0x1402 | TradingApp | 14 |

**Test items found (B §2.6):** all 5 `E2eTvs7_Enum_*` items found with correct
verbatim names + types + ProgIds.

**Per-type delete return values (B §3.4):** Function `true`, Indicator `true`,
ShowMe `true`, TradingApp `true`; all confirmed deleted via re-enumeration.
Strategy: not tested (tsdevctl insert timeout — production bug, §5).

**Dialog/crash log (B §4):** exactly one crash — caused by `tsdevctl delete
--type Strategy` (production bug), NOT by any Frida operation. No dialogs or
crashes during any Frida enumeration or delete.

**Cleanup (B §6):** all 9 `E2eTvs7_*` test items deleted via Frida `DeleteItem`;
20 old `E2eDel_*`/`E2eModify_*` leftovers also Frida-deleted; 0 `E2e*` items
remaining; final dialog/crash check clean (TSDev PID 16600 running).

---

## References

- [RE_ORELUI20_STATIC.md](RE_ORELUI20_STATIC.md) — subtask A full static RE.
- [RE_ORELUI20_DYNAMIC.md](RE_ORELUI20_DYNAMIC.md) — subtask B full dynamic
  verification (scripts, raw output, dialog log, cleanup log).
- [KNOWLEDGE_BASE.md](KNOWLEDGE_BASE.md) — SSOT; "ORELUI20.dll + ORLIB20.dll
  ordinals" section holds the proven facts.
- [RE_DELETE_OPEN_DIALOG.md](RE_DELETE_OPEN_DIALOG.md) — prior TSDev.exe-side
  analysis of the Open dialog (context for the destructor-path delete being
  replaced).
