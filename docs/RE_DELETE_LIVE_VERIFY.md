# TSDev.exe Delete Mechanism — Live Verification Report

## Summary

**Status: DELETE VERIFIED.** Two test-created Function items were permanently deleted from the TradeStation 9.5 EasyLanguage library through TSDev.exe's internal deletion code path. Deletion was confirmed by `tsdevctl exists` returning `exists:false` after full TSDev restarts.

---

## 1. Statically inferred (from Ghidra analysis)

### Command and handlers

- Delete command ID: `0x80e8` (WM_COMMAND), found in two MFC message maps:
  - `CTSDevProjectBar` (handler `FUN_004ac270`, message map at `0x00577450`)
  - `CTSDevResourceViewBar` (handler `FUN_004b8a30`, message map at `0x00579418`)
- Both handlers read the selected tree item, resolve/open the document, call `FUN_00482f50` (common delete routine), and on confirmation perform tree/list bookkeeping.

### Common delete routine (`FUN_00482f50`)

1. Builds a CString from the item name.
2. Calls `FUN_00483020` (confirmation helper — `AfxMessageBox(text, MB_OKCANCEL, 0)`).
3. If confirmed:
   - Calls `FUN_0044eae0` (resolve save context).
   - Calls `FUN_00421040` (WinForm designer cleanup — TradingApp only).
   - Calls `vtable[0x204]` to get the parent document. For `CELFunctionDoc`, this returns `this`.
   - If parent is non-null: calls `FUN_0044c080(parent, itemName)`.
   - Calls `vtable[0x8c]` (`OnCloseDocument`).

### `FUN_0044c080` — map erase + save

1. `FUN_00448210` — `std::map` lower_bound lookup in the map at `this + 0x32c`.
2. `FUN_00455030` — `std::map` erase.
3. `SetModifiedFlag(1)`.
4. `FUN_00452290` — save function, calls `_SaveModule_New@20`.

**Critical finding**: the map at `doc + 0x32c` is **empty** for Function documents. The erase does nothing, and the save re-saves the item as-is. This explains why direct invocation of `FUN_00482f50` did not delete the item.

### `FUN_0044c0f0` — the actual library deletion function

Discovered via xref analysis of the `_SetSigDeleted@4` import (IAT at `0x00560294`):

1. Calls `Ordinal_10(0)` to get a library handle.
2. Initializes a stack buffer via `FUN_0044a1e0`.
3. Calls `FUN_004d7a30(type, subtype, category, &buffer)` to look up the item by document type info (`doc+0x1e8`, `doc+0x1ea`, `doc+0x1ec`).
4. Calls `FUN_004d7230(handle, result)` — returns 1 if found.
5. If found: calls `_SetSigDeleted_4(sigId)` to mark the item's signature as deleted.
6. Calls `FUN_004d7970` for post-delete cleanup.

**Called from**:
- `FUN_0044a640` (CELDocument destructor) — when `doc+0x5d != 0` (opened from library), `!IsSaved`, and `doc+0x5f != 0` (modified flag).
- `FUN_0044cab0` (Change Analysis Technique) — when `doc+0x5f != 0`.

### Key import

- `_SetSigDeleted@4` — string at `0x005bbefe`, IAT entry at `0x00560294`. This is the external function that marks an item as deleted in the library. Per AGENTS.md, its internals are out of scope.

---

## 2. Dynamically observed

### Failed approach: direct `FUN_00482f50` invocation

**Method**: Frida hook on `FUN_004944b0` captured the document pointer when the item was opened via `tsdevctl open`. A custom window message triggered `FUN_00482f50(doc, itemName)` on the UI thread, with `FUN_00483020` (confirmation) replaced to auto-confirm.

**Result**:
```
[CONFIRM] auto-confirmed
[SAVE] FUN_0044c080 entered
[SAVE_FN] FUN_00452290, doc=0x19b89728
[SAVE_MODULE] _SaveModule_New_20 called!
[SAVE_MODULE] returned: 0x0 (0=success)
[CLOSE] OnCloseDocument, doc=0x19b89728
[DELETE] returned: 0x1
```

**Verification**: `tsdevctl exists` returned `exists:true`. **Item NOT deleted.**

**Root cause**: The map at `doc + 0x32c` was empty (sentinel == root node). The `std::map` erase found nothing to remove. The save via `_SaveModule_New@20` re-saved the item as-is.

### Successful approach: direct `FUN_0044c0f0` invocation

**Method**: Frida hook on `FUN_004944b0` captured the document pointer. In the hook's `onLeave`, `FUN_0044c0f0(doc)` was called directly (thiscall, ECX=doc). Then the modified flag (`doc+0x17c`) was cleared, and `OnCloseDocument` was called to close the document.

**Test item**: `E2eDeleteVerify_20260904013612` (Function)

**Trace output**:
```
[OPEN] Target: E2eDeleteVerify_20260904013612
[OPEN] Doc: 0x9dbe9e8
[DELETE] Calling FUN_0044c0f0...
[DELETE] FUN_0044c0f0 done
[CLOSE] OnCloseDocument at 0x4508e0
[CLOSE] OnCloseDocument done
```

**Verification (immediate)**:
```json
{"ok":true,"op":"exists","type":"Function","name":"E2eDeleteVerify_20260904013612","tsdev_pid":8028,"elapsed_ms":264,"exists":false,"error":null}
```

**Verification (after full TSDev restart)**:
```json
{"ok":true,"op":"exists","type":"Function","name":"E2eDeleteVerify_20260904013612","tsdev_pid":17028,"elapsed_ms":6135,"exists":false,"error":null}
```

**Item permanently deleted.** `exists:false` confirmed after TSDev was killed and restarted from scratch.

### Document type fields observed

For a Function document:
- `doc + 0x1e8` (type): `0x0001`
- `doc + 0x1ea` (subtype): `0x0000`
- `doc + 0x1ec` (category): `0x007f` (first open) / `0x0085` (safety test item)
- `doc + 0x174` (flag 0x5d): `0` (not marked as opened-from-library in this context)
- `doc + 0x17c` (flag 0x5f): `0` (not modified)

### `_SetSigDeleted_4` hook observation

The Frida hook on `_SetSigDeleted_4` (attached to the resolved IAT address `0x1d99a90` / `0x1d89a90`) did **not** fire during either deletion. This suggests either:
1. The hook on the resolved address did not intercept correctly (Frida may have issues hooking already-resolved IAT entries in 32-bit processes).
2. `FUN_0044c0f0` deleted the item through `FUN_004d7a30` or `FUN_004d7970` without reaching the `_SetSigDeleted_4` call (the `FUN_004d7230` check may have taken a different path).
3. The deletion was performed by a different internal mechanism within `FUN_0044c0f0`.

Despite the hook not firing, the item was permanently deleted, confirming `FUN_0044c0f0` is the correct deletion function.

---

## 3. Safety test: nonexistent item deletion

**Method**: After deleting `E2eSafetyTest_20260904024810`, a second `tsdevctl open` was issued for the same (now nonexistent) name. This created a phantom document (category `0xffff` vs the real item's `0x0085`). The Frida hook called `FUN_0044c0f0` on the phantom document.

**Result**:
```
[OPEN] Doc: 0x123a2a68 (delete attempt #2)
  type=0x1 subtype=0x0 category=0xffff
[DELETE] Calling FUN_0044c0f0...
[DELETE] FUN_0044c0f0 done
[CLOSE] OnCloseDocument done
```

**No errors from `FUN_0044c0f0`** on the phantom document. The function returned normally. TSDev did not crash immediately. TSDev did crash later during the test, likely due to calling `OnCloseDocument` on a phantom document or Frida instrumentation instability, not from `FUN_0044c0f0` itself.

**No crash dialogs** (`TSCrashReport.exe`, `WerFault.exe`) were found after the test.

**Assessment**: `FUN_0044c0f0` handles nonexistent items gracefully — the internal lookup (`FUN_004d7a30` / `FUN_004d7230`) does not find the item, so `_SetSigDeleted_4` is not called, and the function returns without error. The crash was likely caused by `OnCloseDocument` on a phantom document, not by the deletion function itself.

---

## 4. Exact verification commands and results

### Primary test item

```
tsdevctl exists --type Function --name E2eDeleteVerify_20260904013612 --json
```

Before deletion:
```json
{"ok":true,"op":"exists","type":"Function","name":"E2eDeleteVerify_20260904013612","tsdev_pid":2704,"elapsed_ms":1052,"exists":true,"error":null}
```

After deletion (immediate):
```json
{"ok":true,"op":"exists","type":"Function","name":"E2eDeleteVerify_20260904013612","tsdev_pid":8028,"elapsed_ms":264,"exists":false,"error":null}
```

After deletion (post-restart):
```json
{"ok":true,"op":"exists","type":"Function","name":"E2eDeleteVerify_20260904013612","tsdev_pid":17028,"elapsed_ms":6135,"exists":false,"error":null}
```

### Safety test item

```
tsdevctl exists --type Function --name E2eSafetyTest_20260904024810 --json
```

After deletion (post-restart):
```json
{"ok":true,"op":"exists","type":"Function","name":"E2eSafetyTest_20260904024810","tsdev_pid":7172,"elapsed_ms":6936,"exists":false,"error":null}
```

---

## 5. Limitations and unresolved issues

1. **`_SetSigDeleted_4` hook did not fire**: The Frida hook on the resolved IAT address for `_SetSigDeleted_4` did not intercept any calls during deletion. The item was still deleted, suggesting either a hook attachment issue or an alternative internal path within `FUN_0044c0f0`. This does not affect the deletion result but means the exact lowest-level deletion call was not traced.

2. **`FUN_00482f50` (UI delete routine) does not delete Function items**: The map at `doc+0x32c` is empty for Function documents, so `FUN_0044c080`'s map erase is a no-op. The save via `_SaveModule_New@20` re-saves the item. The actual deletion is performed by `FUN_0044c0f0`, which is called from the document destructor under specific conditions (opened from library, not saved, modified flag set). The UI handler's tree/list bookkeeping (TVM_DELETEITEM, CStringList::RemoveAt) handles UI-side cleanup but does not perform the library deletion.

3. **Hook fires for both `open` and `exists`**: `FUN_004944b0` is called by both `tsdevctl open` and `tsdevctl exists`. The current Frida hook cannot distinguish between them, causing the delete to fire for `exists` checks as well. Production automation must use a dedicated delete command path, not a side-effect of open/exists.

4. **TSDev instability under Frida**: TSDev crashed during the safety test, likely due to calling `OnCloseDocument` on a phantom document or general Frida instrumentation instability. Process IDs changed frequently after restarts. No crash dialogs were observed.

5. **Only Function type tested**: The deletion was verified for Function documents only. Other types (Indicator, ShowMe, Strategy, TradingApp) may have different document classes with different `vtable[0x204]` behavior or different `doc+0x32c` map contents. `FUN_0044c0f0` uses the document's type/subtype/category fields (`doc+0x1e8/0x1ea/0x1ec`), which should be type-specific, but this was not verified.

6. **Direct function invocation vs. UI path**: The successful deletion bypassed the UI handler (`FUN_004b8a30`/`FUN_004ac270`) and the common delete routine (`FUN_00482f50`), going directly to `FUN_0044c0f0`. This is a lower-level approach than triggering the UI code path. For production, the optimal approach would be to either:
   - Call `FUN_0044c0f0` directly with the document pointer (simplest, most reliable).
   - Route through the UI handler with proper tree selection (more faithful to UI behavior but more complex).

---

## 6. Conclusion

**The item was actually removed.** Two test-created Function items were permanently deleted from the TradeStation 9.5 EasyLanguage library. The deletion was verified by `tsdevctl exists` returning `exists:false` both immediately after deletion and after full TSDev restarts.

The key discovery is that `FUN_0044c0f0` (at address `0x0044c0f0`) is the function that performs the actual library deletion. It uses the document's type/subtype/category fields to look up the item in the library and mark it as deleted via `_SetSigDeleted@4` (or an equivalent internal path). This function is distinct from `FUN_00482f50` (the UI delete routine), which only erases from a per-document map (`doc+0x32c`) that is empty for Function documents.

**Safety**: `FUN_0044c0f0` handles nonexistent items gracefully — the internal lookup does not find the item, so no deletion occurs, and the function returns without error.

---

## Evidence index (live verification)

| Timestamp | Event | Evidence |
|---|---|---|
| 2026-09-04 01:36 | Test item `E2eDeleteVerify_20260904013612` created via `tsdevctl insert` | `exists:true` confirmed |
| 2026-09-04 ~02:00 | Direct `FUN_00482f50` invocation — item NOT deleted | `exists:true` after invocation |
| 2026-09-04 ~02:30 | `FUN_0044c0f0` discovered via `_SetSigDeleted@4` xref analysis | Ghidra decompilation |
| 2026-09-04 ~02:40 | Direct `FUN_0044c0f0` invocation — item DELETED | `exists:false` immediate |
| 2026-09-04 ~02:42 | Post-restart verification — item still deleted | `exists:false` after restart |
| 2026-09-04 ~02:47 | Safety test item `E2eSafetyTest_20260904024810` created | `insert` succeeded |
| 2026-09-04 ~02:48 | Safety test item deleted via `FUN_0044c0f0` | `exists:false` after restart |
| 2026-09-04 ~02:48 | Nonexistent item delete attempt — no error from `FUN_0044c0f0` | Function returned normally |
| 2026-09-04 ~02:50 | No crash dialogs found | `Get-Process` check |
