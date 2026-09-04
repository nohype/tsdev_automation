# TSDev.exe Delete — TradingApp (CELApplicationDoc) Failure Analysis

Static analysis of **TSDev.exe** (TradeStation 9.5 Development Environment, build timestamp Fri Nov 30 18:39:00 2018, SHA-256 `9099AA600F0A035C0290107EB1852CD61690330C53B14C9F9D01899395DCAF9A`) via the Ghidra MCP server. Every claim cites Ghidra addresses and decompilation/disassembly evidence.

---

## 1. Root cause (summary)

The delete hook (`src/delete_hook/dllmain.cpp`) uses a destructor-path approach that requires three conditions in the common CELDocument destructor `FUN_0044a640` (`0x0044a640`) to trigger library deletion via `FUN_0044c0f0` (`0x0044c0f0`):

1. `doc+0x174 != 0` — "opened from library" flag, set by vtable slot `[0x1F4]` (`FUN_0044f8e0`).
2. `!CELProperties::IsSaved(doc+0x210)` — properties not saved, established by vtable slot `[0x1B4]` calling `CELProperties::SetSaved(false)`.
3. `doc+0x17C != 0` — modified flag, set directly by the hook.

For TradingApp (`CELApplicationDoc`, type `0x1402`), **condition 2 fails**. `CELApplicationDoc`'s vtable slot `[0x1B4]` points to `FUN_00413900` (`0x00413900`), an **empty stub** that does nothing — it does not call `CELProperties::SetSaved(false)`. The other two conditions are established correctly. As a result, the destructor never calls `FUN_0044c0f0`, the library item is not removed, and the hook reports `DeleteDone` without actual deletion.

---

## 2. TradingApp document class identification

### 2.1 Type code

The CLI sends pipe type number `5122` (`0x1402`) for TradingApp (`src/tsdevlib/type_table.h` line 20: `{L"TradingApp", 5122, ...}`). The pipe payload is `"name", 5122` (`src/tsdevctl/main.cpp` line 263, line 800).

### 2.2 Constructor

`FUN_00413590` (`0x00413590`) is the `CELApplicationDoc` constructor. Decompilation confirms:

- Writes vtable pointer `CELApplicationDoc::vftable` (`0x005638B0`) at `0x004135C6`: `MOV dword ptr [ESI], 0x5638B0`.
- Writes `doc+0x1E8 = 0x140002` at `0x004135CC`: `MOV dword ptr [ESI + 0x1e8], 0x140002`.
  - The combined type read by `FUN_0044f020` (`0x0044f020`) is `CONCAT11(byte@0x1EA, byte@0x1E8)` = `(0x14 << 8) | 0x02` = `0x1402` = 5122, matching the CLI type number.
- Writes `doc+0x1FC = 0x29BE` at `0x004135D6`.
- Sets display name `"TradingApp"` (decompilation: `operator=(param_1 + 0x80, L"TradingApp")`).
- Sets `doc+0x1B4 = 0x3D21` (decompilation: `param_1[0x6d] = 0x3d21`), matching the CLI `new_cmd_id` `0x3D21`.
- Calls `CELProperties::SetSection(param_1 + 0x84, "Application")`.

### 2.3 Template registration

The document template for type `0x1402` is registered during `FUN_00491960` (`0x00491960`, `COmegaApp::InitInstance`). The template's document runtime class is obtained via `FUN_004833c0` (`0x004833c0`), which returns `&PTR_s_CELWFDesignerDoc_0056d5e8` — **not** `CELApplicationDoc`.

**Correction of prior analysis**: the previous working memory stated "TradingApp library type: 0xE9E9" and "TradingApp document class: CELApplicationDoc". These are two separate types:

| Type code | Document class | Constructor | Vtable | Display name |
|---|---|---|---|---|
| `0x1402` (5122) | `CELApplicationDoc` | `FUN_00413590` (`0x00413590`) | `0x005638B0` | `"TradingApp"` |
| `0xE9E9` | `CELWFDesignerDoc` | `FUN_00482ab0` (`0x00482ab0`) | `0x0056D608` | `"Partial Class"` |

The CLI's TradingApp type (`0x1402`) maps to `CELApplicationDoc`. Type `0xE9E9` is `CELWFDesignerDoc` ("Partial Class"), a different document class. The template registration at `0x00492A1E` (`MOV dword ptr [EBP + -0x20], 0xe9e9`) creates a `CELWFDesignerDoc` template, not a TradingApp template.

The pipe-open function `FUN_004944b0` (`0x004944b0`) receives the type parameter and looks up the matching template in the map at `this+0x2A0`. For type `0x1402`, it finds the `CELApplicationDoc` template and calls `FUN_004942b0` (`0x004942b0`) to open/create the document.

---

## 3. CELApplicationDoc vtable layout

Vtable base: `0x005638B0` (confirmed by constructor write at `0x004135C6` and memory read).

| Slot (byte offset) | Vtable address | Function | Purpose |
|---|---|---|---|
| `[0x00]` | `0x005638B0` | `FUN_004138E0` (`0x004138E0`) | `GetRuntimeClass` — returns `&PTR_s_CELApplicationDoc_00563890` |
| `[0x04]` | `0x005638B4` | `FUN_00413620` (`0x00413620`) | Deleting destructor — sets vtable, calls `FUN_0044a640` |
| `[0x8C]` | `0x0056393C` | `FUN_004508E0` (`0x004508E0`) | `OnCloseDocument` |
| `[0x1B4]` | `0x00563A64` | `FUN_00413900` (`0x00413900`) | **Empty stub — does nothing** |
| `[0x1F0]` | `0x00563AA0` | `FUN_0044EC00` (`0x0044EC00`) | Returns `doc+0x210` (CELProperties object) |
| `[0x1F4]` | `0x00563AA4` | `FUN_0044F8E0` (`0x0044F8E0`) | Loader — sets `doc+0x174 = 1` if item found |

Memory reads confirming each slot (little-endian byte data):

- `0x005638B0`: `E0 38 41 00` → `0x004138E0` ✓
- `0x005638B4`: `20 36 41 00` → `0x00413620` ✓
- `0x0056393C`: `E0 08 45 00` → `0x004508E0` ✓
- `0x00563A64`: `00 39 41 00` → `0x00413900` ✓
- `0x00563AA0`: `00 EC 44 00` → `0x0044EC00` ✓
- `0x00563AA4`: `E0 F8 44 00` → `0x0044F8E0` ✓

---

## 4. Vtable slot analysis

### 4.1 `[0x1F4]` — Loader (`FUN_0044F8E0`)

`FUN_0044F8E0` (`0x0044F8E0`) is the common document loader, shared between `CELApplicationDoc` and `CELFunctionDoc` (both vtable slots point to the same address, confirmed by memory reads at `0x00563AA4` and `0x00568E8C`).

Decompilation of `FUN_0044F8E0`:

1. Calls vtable `[0x1F0]` to get the properties object (`0x0044F90A`: `CALL dword ptr [EAX + 0x1F0]`). For `CELApplicationDoc`, this is `FUN_0044EC00`, which returns `doc+0x210`.
2. Clears `doc+0x17C = 0` and `doc+0x174 = 0` at the start (decompilation: `param_1[0x5f] = 0; param_1[0x5d] = 0;`).
3. Calls `_FindExistPosSubType_12()` to look up the item in the library.
4. Checks `if ((short)param_1[0x7a] != 1000)` — for TradingApp, `param_1[0x7a]` (doc+0x1E8) is `0x140002`; as a short this is `0x0002`, so the condition is true (normal path).
5. If the item is found (`local_20 != 0xFFFFFFFF`): calls `FUN_004502f0`, checks properties (hidden, date, token-only), calls vtable `[0x1FC]`, then **sets `doc+0x174 = 1`** at `0x0044FFD0`: `MOV dword ptr [EBX + 0x174], 0x1`.

**Conclusion**: `[0x1F4]` correctly sets `doc+0x174 = 1` for TradingApp. Condition 1 is satisfied.

### 4.2 `[0x1B4]` — Clear-saved (`FUN_00413900`) — THE PROBLEM

`FUN_00413900` (`0x00413900`) is `CELApplicationDoc`'s implementation of vtable slot `[0x1B4]`. Decompilation:

```c
void FUN_00413900(void)
{
  return;
}
```

It is an **empty function**. It does not call `CELProperties::SetSaved(false)` or any other function. Calling this slot has no effect on the document's saved state.

**For comparison**, `CELFunctionDoc`'s vtable slot `[0x1B4]` at `0x00568E4C` points to `FUN_00458160` (`0x00458160`, confirmed by memory read: `60 81 45 00`). Decompilation of `FUN_00458160`:

```c
void FUN_00458160(int *param_1) {
    FUN_0044f3d0();                                           // common clear-saved helper
    this = (CELProperties *)(**(code **)(*param_1 + 0x1f0))(); // get properties via vtable[0x1F0]
    CELProperties::SetReturnType(this, param_1[0x7c]);        // Function-specific
}
```

`FUN_00458160` calls `FUN_0044f3d0` (the actual clear-saved helper), then does a Function-specific `SetReturnType` call.

### 4.3 The actual clear-saved helper: `FUN_0044f3d0`

`FUN_0044f3d0` (`0x0044f3d0`) is the common helper that establishes the `!IsSaved` condition. Decompilation:

```c
void FUN_0044f3d0(int *param_1) {
    CELProperties *this = (CELProperties *)(**(code **)(*param_1 + 0x1f0))();
    CELProperties::SetSaved(this, false);
    CELProperties::SetVerified(this, false);
    CELProperties::SetProtected(this, false);
    CELProperties::SetProtectedPlus(this, false);
}
```

It obtains the properties object via vtable `[0x1F0]` (for `CELApplicationDoc`, `FUN_0044EC00` returns `doc+0x210`), then calls `CELProperties::SetSaved(this, false)`. This is what makes the destructor's `IsSaved` check return false.

### 4.4 `[0x8C]` — OnCloseDocument (`FUN_004508E0`)

`FUN_004508E0` (`0x004508E0`) is the common `OnCloseDocument` handler, shared between `CELApplicationDoc` and `CELFunctionDoc` (both vtable slots point to the same address, confirmed by memory reads at `0x0056393C` and `0x00568D24`).

Decompilation shows it handles document closure: closes child documents, calls `COleServerDoc::OnCloseDocument`, which eventually invokes `delete this` (the destructor). It has special handling for types `0xE9E9` and `0xEAEA` (calls vtable `[0x204]`), but for TradingApp (`0x1402`) it takes the normal path.

**Conclusion**: `[0x8C]` correctly triggers destruction for TradingApp.

---

## 5. Destructor chain

### 5.1 CELApplicationDoc deleting destructor: `FUN_00413620`

`FUN_00413620` (`0x00413620`) is `CELApplicationDoc`'s deleting destructor (vtable slot `[0x04]`). Decompilation:

```c
undefined4 * __thiscall FUN_00413620(undefined4 *param_1, uint param_2) {
    *param_1 = CELApplicationDoc::vftable;   // set vtable to CELApplicationDoc
    FUN_0044a640();                           // call common CELDocument destructor
    if ((param_2 & 1) != 0) {
        if ((param_2 & 4) == 0) {
            operator_delete(param_1);         // scalar delete
        } else {
            FUN_00540e1c(param_1, 0x35c);     // vector delete (size 0x35C)
        }
    }
    return param_1;
}
```

The object size is `0x35C` (860 bytes), confirmed by the vector delete size parameter.

### 5.2 Common CELDocument destructor: `FUN_0044a640`

`FUN_0044a640` (`0x0044a640`) is the common base destructor shared by all CELDocument-derived classes. Decompilation (key section):

```c
void FUN_0044a640(undefined4 *param_1) {
    *param_1 = CELDocument::vftable;          // revert to base vtable
    if (param_1[0x5d] != 0) {                 // doc+0x174: opened-from-library flag
        // ... cleanup (FUN_00491460, FUN_00477b40) ...
        bVar1 = CELProperties::IsSaved((CELProperties *)(param_1 + 0x84));
        if ((!bVar1) && (param_1[0x5f] != 0)) { // !IsSaved AND doc+0x17C (modified)
            FUN_0044c0f0();                     // ACTUAL LIBRARY DELETION
        }
    }
    // ... rest of destructor (free strings, properties, etc.) ...
}
```

Note: `param_1 + 0x84` in decompiler int-pointer notation = byte offset `0x84 * 4 = 0x210`, i.e., `doc+0x210`, the CELProperties object. This is the same object returned by vtable `[0x1F0]` (`FUN_0044EC00`).

### 5.3 Library deletion: `FUN_0044c0f0`

`FUN_0044c0f0` (`0x0044c0f0`) performs the actual library deletion. It reads type/subtype/category from `doc+0x1E8/0x1EA/0x1EC` and uses those to look up and remove the item from the library. For TradingApp, these fields are `0x1402` (type), `0x29BE` (subtype at `doc+0x1FC`), which are valid library identifiers.

---

## 6. Destructor conditions analysis

| Condition | How established | Works for TradingApp? |
|---|---|---|
| `doc+0x174 != 0` | Hook calls vtable `[0x1F4]` (`FUN_0044F8E0`), which sets `doc+0x174 = 1` at `0x0044FFD0` if item found | **Yes** — same loader as other types |
| `!CELProperties::IsSaved(doc+0x210)` | Hook calls vtable `[0x1B4]`, which should call `CELProperties::SetSaved(false)` | **No** — `FUN_00413900` is an empty stub |
| `doc+0x17C != 0` | Hook directly sets `doc+0x17C = 1` | **Yes** — direct memory write |

**Condition 2 is the failing condition.** Because `FUN_00413900` does nothing, `IsSaved` remains true (the document was saved during pipe-open), and the destructor skips `FUN_0044c0f0`.

---

## 7. Comparison with working types

### 7.1 CELFunctionDoc (Function, type 0x0001)

Vtable base: `0x00568C98`.

| Slot | Vtable address | Function | Calls `SetSaved(false)`? |
|---|---|---|---|
| `[0x1B4]` | `0x00568E4C` | `FUN_00458160` (`0x00458160`) | **Yes** — calls `FUN_0044f3d0` |
| `[0x1F4]` | `0x00568E8C` | `FUN_0044F8E0` (`0x0044F8E0`) | N/A (sets `doc+0x174`) |
| `[0x8C]` | `0x00568D24` | `FUN_004508E0` (`0x004508E0`) | N/A (triggers destruction) |

`FUN_00458160` calls `FUN_0044f3d0`, which calls `CELProperties::SetSaved(false)`. All three destructor conditions are met. Deletion works.

### 7.2 CELApplicationDoc (TradingApp, type 0x1402)

Vtable base: `0x005638B0`.

| Slot | Vtable address | Function | Calls `SetSaved(false)`? |
|---|---|---|---|
| `[0x1B4]` | `0x00563A64` | `FUN_00413900` (`0x00413900`) | **No** — empty stub |
| `[0x1F4]` | `0x00563AA4` | `FUN_0044F8E0` (`0x0044F8E0`) | N/A (sets `doc+0x174`) |
| `[0x8C]` | `0x0056393C` | `FUN_004508E0` (`0x004508E0`) | N/A (triggers destruction) |

`FUN_00413900` is empty. Condition 2 fails. Deletion silently does not occur.

### 7.3 Shared slots

`[0x1F4]` and `[0x8C]` point to the same functions for both classes (`FUN_0044F8E0` and `FUN_004508E0` respectively), confirmed by memory reads. Only `[0x1B4]` differs.

---

## 8. Recommended fix

### 8.1 Approach

For TradingApp (`CELApplicationDoc`, type `0x1402`), the hook must call `FUN_0044f3d0` (`0x0044f3d0`) directly instead of relying on vtable slot `[0x1B4]`, which is a no-op. `FUN_0044f3d0` is the common clear-saved helper that:
1. Gets the properties object via vtable `[0x1F0]` (`FUN_0044EC00`, returns `doc+0x210`).
2. Calls `CELProperties::SetSaved(this, false)` — establishes the `!IsSaved` destructor condition.
3. Also calls `SetVerified(false)`, `SetProtected(false)`, `SetProtectedPlus(false)` — harmless additional resets.

This is the same function called by `CELFunctionDoc`'s vtable `[0x1B4]` (`FUN_00458160`), minus the Function-specific `SetReturnType` call, which is irrelevant for deletion.

### 8.2 Type identification

The hook can identify TradingApp by reading the document's type field:

```c
unsigned char type_lo = *((unsigned char*)doc + 0x1e8);
unsigned char type_hi = *((unsigned char*)doc + 0x1ea);
unsigned short doc_type = (type_hi << 8) | type_lo;
```

This replicates `FUN_0044f020` (`0x0044f020`): `CONCAT11(*(byte*)(param_1 + 0x1ea), *(byte*)(param_1 + 0x1e8))`. For TradingApp, `doc_type == 0x1402`.

### 8.3 Hook change (in `do_delete`, after step 1, replacing step 2)

Current code (`src/delete_hook/dllmain.cpp` lines 128–131):

```c
vtable = *reinterpret_cast<void***>(doc);
const ThiscallVoidFn clear_saved =
    reinterpret_cast<ThiscallVoidFn>(vtable[g_shared->vtable_clear_saved_offset / sizeof(void*)]);
clear_saved(doc);
```

Recommended change:

```c
// Check if this is a TradingApp (CELApplicationDoc, type 0x1402).
// Its vtable[0x1B4] is FUN_00413900 — an empty stub that does NOT call
// CELProperties::SetSaved(false). For this type, call the common
// clear-saved helper FUN_0044f3d0 directly.
unsigned char type_lo = *((unsigned char*)doc + 0x1e8);
unsigned char type_hi = *((unsigned char*)doc + 0x1ea);
unsigned short doc_type = (type_hi << 8) | type_lo;

if (doc_type == 0x1402) {
    // TradingApp: call FUN_0044f3d0 directly (thiscall, ECX = doc)
    const ThiscallVoidFn clear_saved_direct =
        reinterpret_cast<ThiscallVoidFn>(0x0044f3d0);
    clear_saved_direct(doc);
} else {
    // Other types: vtable[0x1B4] works correctly
    vtable = *reinterpret_cast<void***>(doc);
    const ThiscallVoidFn clear_saved =
        reinterpret_cast<ThiscallVoidFn>(vtable[g_shared->vtable_clear_saved_offset / sizeof(void*)]);
    clear_saved(doc);
}
```

The address `0x0044f3d0` is version-specific (TSDev 9.5, build Nov 30 2018). The CLI already verifies the TSDev.exe SHA-256 before injection (`src/delete_hook/dllmain.cpp` lines 6–8), so this hardcoded address is safe — a mismatched build is rejected before the DLL loads.

### 8.4 Why not always call `FUN_0044f3d0`?

Calling `FUN_0044f3d0` directly for all types would also work — it is the common helper that all working types' vtable `[0x1B4]` implementations call. However, some types' vtable `[0x1B4]` implementations do additional type-specific work (e.g., `FUN_00458160` calls `CELProperties::SetReturnType` for Functions). While this additional work is irrelevant for deletion (the destructor only checks `IsSaved`), the type-specific approach is more conservative and avoids changing behavior for types that already work.

### 8.5 Why not directly call `CELProperties::SetSaved`?

`CELProperties::SetSaved` is an imported function in an external DLL (orlib). Its address is not visible from TSDev.exe static analysis alone — it is resolved through the IAT at runtime. `FUN_0044f3d0` is a TSDev.exe-internal function that calls `SetSaved` through the properties object's own method table, which is the correct and safe way to invoke it. Using `FUN_0044f3d0` avoids dependence on external DLL internals.

### 8.6 Alternative: add a shared field for the direct function address

Instead of hardcoding `0x0044f3d0` in the DLL, the CLI could pass it through a new field in `DeleteShared` (`src/delete_hook/shared.h`), similar to how `fn_open` is passed. This would keep all version-specific addresses in one place (the CLI) and make the DLL fully data-driven. This is a design choice; the hardcoded approach is consistent with the existing `kFnOpen` pattern in the DLL.

---

## 9. Additional notes

### 9.1 `FUN_004138E0` (vtable slot `[0x00]`)

`FUN_004138E0` (`0x004138E0`) is `CELApplicationDoc::GetRuntimeClass` — it returns `&PTR_s_CELApplicationDoc_00563890`. It is not a destructor; it is MFC's `CRuntimeClass` accessor. The actual deleting destructor is at vtable slot `[0x04]` (`FUN_00413620`).

### 9.2 `FUN_004417d0` and TradingApp

`FUN_004417d0` (`0x004417d0`, the `WM_ELCREATEDOCUMENT` callback) has special handling for type `0xE9E9` (calls `VIEW->vtable[0x1F4]` instead of `DOC->vtable[0x1F4]`). This applies to `CELWFDesignerDoc` ("Partial Class"), not to TradingApp (`CELApplicationDoc`, type `0x1402`). For TradingApp, `FUN_004417d0` takes the normal path and calls `DOC->vtable[0x1F4]` (`FUN_0044F8E0`), same as other types. This is consistent with the hook's approach.

### 9.3 `CELWFDesignerDoc` (type 0xE9E9) — not affected

For completeness, `CELWFDesignerDoc` (vtable `0x0056D608`) has:
- `[0x1B4]` at `0x0056D7BC` → `FUN_0044F3D0` (the actual clear-saved helper, not a stub).
- `[0x1F4]` at `0x0056D7FC` → `FUN_0044F8E0` (same loader).
- `[0x8C]` at `0x0056D694` → `FUN_004508E0` (same OnCloseDocument).

Its destructor chain is `FUN_00482C40` → `FUN_0047B8B0` (sets `CELStudyDoc` vtable) → `FUN_0044A640` (common destructor). All three destructor conditions would be established. However, `CELWFDesignerDoc` is "Partial Class" (type `0xE9E9`), not TradingApp, and is not in the CLI's current type set.

### 9.4 `doc+0x1B4` field vs. vtable slot `[0x1B4]`

These are unrelated. `doc+0x1B4` is a data field (the `new_cmd_id` / command ID, `0x3D21` for TradingApp, set by the constructor at `param_1[0x6d] = 0x3d21`). Vtable slot `[0x1B4]` is a function pointer at byte offset `0x1B4` from the vtable base (`vtable_base + 0x1B4`), which for `CELApplicationDoc` is `0x005638B0 + 0x1B4 = 0x00563A64` → `FUN_00413900`. The similar offsets are coincidental.
