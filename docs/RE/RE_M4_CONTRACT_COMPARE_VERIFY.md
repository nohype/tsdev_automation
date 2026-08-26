# RE Verification: FUN_004d9040 Contract-Change Comparison Logic

**Source binary:** `TSDev.exe` (Ghidra project `TSDevAnalysis`)
**Function under test:** `FUN_004d9040` @ `0x004d9040`
**Caller (gate logic):** `FUN_004d91e0` @ `0x004d91e0`
**Method:** Ghidra MCP `decompile_function` (read-only). No source code modified.
**Status:** All 5 questions answered with decompilation evidence.

---

## 0. Function signature and parameter meaning

```c
uint __thiscall FUN_004d9040(int *param_1, int *param_2)
```

`param_1` and `param_2` are **pointers to pointers** (each points to a slot holding a
warehouse master-record pointer). The function dereferences once to get the record:

```c
iVar3 = *param_1;          // record A (warehouse master rec pointer)
iVar4 = *param_2;          // record B (warehouse master rec pointer)
```

All offsets below (`+0xd6`, `+0x15`, `+0x7f`, `+0x81`, `+0x29`, `+0x4f`) are relative
to `iVar3` / `iVar4` (the dereferenced record pointers), not to `param_1`/`param_2`.

---

## 1. CONFIRMED: +0x81 bitmask is **0xF900**, NOT 0xFF00

### Evidence (decompilation lines)

```c
uVar1 = *(ushort *)(iVar3 + 0x81);
uVar2 = *(ushort *)(iVar4 + 0x81);
in_EAX = (int *)(uint)(uVar1 ^ uVar2);
if ((((((uVar1 ^ uVar2) & 0x100) == 0) &&                       // bit 8
     (in_EAX = (int *)(uint)(uVar1 ^ uVar2), ((uVar1 ^ uVar2) & 0x800) == 0)) &&   // bit 11
    (in_EAX = (int *)(uint)(uVar1 ^ uVar2), ((uVar1 ^ uVar2) & 0x1000) == 0)) &&   // bit 12
   (((in_EAX = (int *)(uint)(uVar1 ^ uVar2), ((uVar1 ^ uVar2) & 0x2000) == 0 &&    // bit 13
     (in_EAX = (int *)(uint)(uVar1 ^ uVar2), ((uVar1 ^ uVar2) & 0x4000) == 0)) &&  // bit 14
    ((((uVar1 ^ uVar2) & 0x8000) == 0 && ...))))))                                // bit 15
```

### Conclusion

- **Read type:** `ushort` (2 bytes) at offset `+0x81`.
- **Comparison style:** masked XOR. The two ushorts are XORed, then specific bits of
  the XOR result are tested. If any tested bit is non-zero → mismatch → return 0.
- **Exact mask:** `0x100 | 0x800 | 0x1000 | 0x2000 | 0x4000 | 0x8000` = **`0xF900`**.
- **Bits tested:** 8, 11, 12, 13, 14, 15.
- **Bits NOT tested:** 9 and 10 (i.e. `0x200` and `0x400` are excluded). Bits 0–7 are
  also not tested.

### Discrepancy with previous implementation

The previous implementation used `0xFF00` (all bits 8–15). **This is WRONG.** The
correct mask is **`0xF900`** — bits 9 and 10 are explicitly excluded from the
contract-change comparison. A change confined to bits 9 or 10 of the `+0x81` ushort
does NOT count as a contract change.

---

## 2. CONFIRMED: +0x4f is dereferenced and array CONTENTS are compared element-by-element

### Evidence (decompilation lines)

```c
iVar11 = 0;                          // element index
iVar10 = 0;                          // byte offset into array
while( true ) {
  if (*(int *)(iVar3 + 0x29) <= iVar11) {        // element count reached
    return CONCAT31((int3)((uint)in_EAX >> 8),1); // all equal -> return 1
  }
  iVar9 = *(int *)(iVar4 + 0x4f) + iVar10;       // element pointer B (dereferenced)
  in_EAX = *(int **)(*(int *)(iVar3 + 0x4f) + 0xd + iVar10);  // deref A, field +0xd
  iVar7 = *(int *)(iVar3 + 0x4f) + iVar10;       // element pointer A (dereferenced)
  if (((in_EAX != *(int **)(iVar9 + 0xd)) || ... ) || ... ) break;
  iVar11 = iVar11 + 1;
  iVar10 = iVar10 + 0xb5;                        // STRIDE = 0xb5
}
```

### Conclusion

- `*(int *)(rec + 0x4f)` is **dereferenced** for both records. The pointers are NOT
  compared against each other; they are used as base addresses into two parallel
  element arrays.
- The loop iterates `*(int *)(rec + 0x29)` times (element count, compared as int —
  see §3). Byte offset `iVar10` advances by **`0xb5`** each iteration → **stride = 0xb5**.
- The previous implementation that compared only the pointer values at `+0x4f` is
  **WRONG**. The function compares array CONTENTS.

### Per-element fields compared (within each 0xb5-stride element)

| Elem offset | Read type | Comparison | Evidence line |
|-------------|-----------|------------|---------------|
| `+0xd`  | `int *` (4-byte pointer) | pointer equality | `in_EAX != *(int **)(iVar9 + 0xd)` |
| `+0x11` | `int *` (4-byte pointer) | pointer equality | `in_EAX = *(int **)(iVar7 + 0x11), in_EAX != *(int **)(iVar9 + 0x11)` |
| `+0x15` | `int *` (4-byte pointer) | pointer equality | `in_EAX = *(int **)(iVar7 + 0x15), in_EAX != *(int **)(iVar9 + 0x15)` |
| `+0x83` | `uint` (4 bytes) | **bit 2 only** (`& 4`) of XOR | `*(uint *)(iVar9 + 0x83) ^ *(uint *)(iVar7 + 0x83)`, `((uint)in_EAX & 4) != 0` |
| `+0x87` | `short` (2 bytes) | full short equality (only checked if `+0x83` bit 2 did NOT differ) | `*(short *)(iVar7 + 0x87) != *(short *)(iVar9 + 0x87)` |
| `+0xb1` | `int *` (4-byte pointer) | pointer equality | `in_EAX = *(int **)(iVar7 + 0xb1), in_EAX != *(int **)(iVar9 + 0xb1)` |

### +0x83 / +0x87 short-circuit detail

```c
(  ((uint)in_EAX & 4) != 0                                       // +0x83 bit 2 differs -> break
|| (in_EAX = ...*(short *)(iVar7 + 0x87),
    *(short *)(iVar7 + 0x87) != *(short *)(iVar9 + 0x87)))        // OR +0x87 short differs -> break
```

- `+0x83` is a `uint`; only **bit 2** (`mask 0x4`) of the XOR is tested. If bit 2
  differs → break immediately (the `+0x87` short is NOT examined in that case).
- If `+0x83` bit 2 is equal, then `+0x87` (a 2-byte `short`) is compared for full
  equality.

### Confirmation of RE note's per-element offsets

All six per-element offsets in the RE note (`+0xd`, `+0x11`, `+0x15`, `+0x83`,
`+0x87`, `+0xb1`) are confirmed present in the decompilation. The `+0x83` "bit 2"
claim is confirmed (`& 4`).

---

## 3. CONFIRMED: Complete list of comparison fields

| Rec offset | Read type | Comparison style | Exact mask / notes | Evidence |
|------------|-----------|------------------|--------------------|----------|
| `+0xd6` | `int *` (4-byte pointer) | **pointer equality** (NOT dereferenced) | full 4-byte pointer compare | `in_EAX = *(int **)(iVar3 + 0xd6), in_EAX == *(int **)(iVar4 + 0xd6)` |
| `+0x15` | `ushort` read, cast to `byte` (low byte used) | masked XOR on low byte | **`0x7E`** (bits 1,2,3,4,5,6) | `bVar5 = (byte)*(undefined2 *)(iVar3 + 0x15)`; checks `& 6`, `& 0x18`, `& 0x20`, `& 0x40` |
| `+0x7f` | `ushort` (2 bytes) | masked XOR | **`0xB007`** (bits 0,1,2,12,13,15) | `uVar1 = *(ushort *)(iVar3 + 0x7f)`; checks `& 7` (low byte), `& 0x1000`, `& 0x2000`, `& 0x8000` |
| `+0x81` | `ushort` (2 bytes) | masked XOR | **`0xF900`** (bits 8,11,12,13,14,15) — see §1 | see §1 |
| `+0x29` | `int` (4 bytes) | full equality (element count) | direct `==` | `*(int *)(iVar3 + 0x29) == *(int *)(iVar4 + 0x29)` |
| `+0x4f` | `int *` (4-byte pointer) | **dereferenced**, array contents compared element-by-element (stride 0xb5, count from `+0x29`) | see §2 | see §2 |

### Notes on +0xd6

`+0xd6` is compared as a **raw 4-byte pointer value** (`int *`). The pointers are
NOT dereferenced at this stage — it is an identity check on whatever pointer is
stored at `+0xd6` in each record. If the two pointers differ, the function returns
0 (change detected) without examining any further fields.

### Notes on +0x15

The decompiler reads a `ushort` (`undefined2`) at `+0x15` but immediately casts to
`byte`, so only the **low byte** (offset `+0x15` itself) participates. The high byte
at `+0x16` is NOT tested. Mask on the low byte: `0x6 | 0x18 | 0x20 | 0x40` = **`0x7E`**
(bits 1–6). Bit 0 and bit 7 of the low byte are NOT tested.

### Notes on +0x7f

Full `ushort` XOR. Tested bits: `0x7` (low byte bits 0,1,2), `0x1000` (bit 12),
`0x2000` (bit 13), `0x8000` (bit 15). Combined mask = **`0xB007`**. Bits 3–11 and 14
are NOT tested.

---

## 4. CONFIRMED: Return value semantics — 1 = no change, 0 = change detected

### Evidence (return statements)

There are exactly two return paths:

**Path A — all comparisons passed, loop completed (count reached):**
```c
if (*(int *)(iVar3 + 0x29) <= iVar11) {
  return CONCAT31((int3)((uint)in_EAX >> 8),1);   // low byte = 1
}
```
→ returns **1** (no contract change detected; all fields equal).

**Path B — any comparison failed (early exit or loop break):**
```c
return (uint)in_EAX & 0xffffff00;   // low byte cleared
```
This path is hit:
- when `*param_1` or `*param_2` is null,
- when `+0xd6` pointers differ,
- when any of the `+0x15` / `+0x7f` / `+0x81` masked XOR tests fail,
- when `+0x29` counts differ,
- or when the per-element loop `break`s on a field mismatch.

In all these cases the low byte of the return value is 0 → returns **0** (contract
change detected).

### Conclusion

| Return value | Meaning |
|--------------|---------|
| `1` | NO contract change detected (records compare equal on all contract-relevant fields) |
| `0` | Contract change detected (at least one field mismatch) |

This matches the RE note's semantics.

---

## 5. CONFIRMED: Boolean gate logic in caller FUN_004d91e0

### Evidence (decompilation of FUN_004d91e0)

```c
cVar3 = FUN_004d9040(&local_18);
local_24 = (uint)(cVar3 == '\0');
```

So `local_24 = (cVar3 == 0)`. Given §4 (return 1 = no change, 0 = change):

| `cVar3` (FUN_004d9040 return) | Meaning | `local_24 = (cVar3 == 0)` |
|-------------------------------|---------|---------------------------|
| `1` | no contract change | `0` |
| `0` | contract change detected | `1` |

**Therefore `local_24 == 1` iff a contract change was detected.** The PM's
correction is verified: `local_24 = (cVar3 == 0)` means `local_24 = 1` when the
contract CHANGED.

### Recursion gate

The gate that decides whether to recurse into a dependent component:

```c
iVar7 = FUN_004ca5c0();
if ((iVar7 == 0) || (local_24 == 1)) {
  ...
  FUN_004d91e0(uVar4);   // RECURSE
}
```

### Exact condition

- **Recurse** when: `FUN_004ca5c0() == 0` **OR** `local_24 == 1` (contract changed).
- **Skip recursion** when: `FUN_004ca5c0() != 0` **AND** `local_24 == 0` (no
  contract change).

In plain terms: a contract change (`local_24 == 1`) **always** forces recursion
into the dependent component, regardless of `FUN_004ca5c0()`. When there is no
contract change, recursion happens only if `FUN_004ca5c0()` returns 0.

---

## 6. Summary of corrections to previous implementation

| Item | Previous implementation | Verified correct value | Source |
|------|--------------------------|------------------------|--------|
| `+0x81` mask | `0xFF00` (all bits 8–15) | **`0xF900`** (bits 8,11,12,13,14,15) | §1 |
| `+0x4f` comparison | pointer values only | **dereference + compare array contents** element-by-element, stride `0xb5`, count from `+0x29` | §2 |
| `+0xd6` | (undocumented type) | `int *` pointer equality (4 bytes, not dereferenced) | §3 |
| `+0x15` mask | (unspecified) | **`0x7E`** on low byte (bits 1–6); high byte ignored | §3 |
| `+0x7f` mask | (unspecified) | **`0xB007`** (bits 0,1,2,12,13,15) | §3 |
| `+0x29` | element count | `int` (4-byte) full equality | §3 |
| `+0x83` per-elem | "bit 2" | confirmed: `uint` at `+0x83`, **bit 2 only** (`& 4`) | §2 |
| `+0x87` per-elem | (unspecified) | `short` (2 bytes) full equality, checked only if `+0x83` bit 2 equal | §2 |
| Return semantics | (assumed) | `1` = no change, `0` = change | §4 |
| Gate `local_24` | `(cVar3 == 0)` → 1 when changed | **confirmed** | §5 |
| Recursion condition | (corrected by PM) | `FUN_004ca5c0() == 0 OR local_24 == 1` | §5 |

---

## 7. Raw decompilation excerpts (for audit)

### FUN_004d9040 (full)

```c
uint __thiscall FUN_004d9040(int *param_1,int *param_2)
{
  ushort uVar1;
  ushort uVar2;
  int iVar3;
  int iVar4;
  byte bVar5;
  int *in_EAX;
  undefined3 uVar6;
  int iVar7;
  byte bVar8;
  int iVar9;
  int iVar10;
  int iVar11;

  iVar3 = *param_1;
  if (((iVar3 != 0) && (iVar4 = *param_2, in_EAX = param_2, iVar4 != 0)) &&
     (in_EAX = *(int **)(iVar3 + 0xd6), in_EAX == *(int **)(iVar4 + 0xd6))) {
    uVar6 = (undefined3)((uint)in_EAX >> 8);
    bVar5 = (byte)*(undefined2 *)(iVar3 + 0x15);
    bVar8 = (byte)*(undefined2 *)(iVar4 + 0x15);
    in_EAX = (int *)CONCAT31(uVar6,bVar5 ^ bVar8);
    if (((((bVar5 ^ bVar8) & 6) == 0) &&
        (in_EAX = (int *)CONCAT31(uVar6,bVar5 ^ bVar8), ((bVar5 ^ bVar8) & 0x18) == 0)) &&
       ((in_EAX = (int *)CONCAT31(uVar6,bVar5 ^ bVar8), ((bVar5 ^ bVar8) & 0x20) == 0 &&
        (((bVar5 ^ bVar8) & 0x40) == 0)))) {
      uVar1 = *(ushort *)(iVar3 + 0x7f);
      uVar2 = *(ushort *)(iVar4 + 0x7f);
      bVar5 = (byte)uVar1 ^ (byte)uVar2;
      in_EAX = (int *)CONCAT31(uVar6,bVar5);
      if ((((bVar5 & 7) == 0) &&
          (in_EAX = (int *)(uint)(uVar1 ^ uVar2), ((uVar1 ^ uVar2) & 0x1000) == 0)) &&
         ((in_EAX = (int *)(uint)(uVar1 ^ uVar2), ((uVar1 ^ uVar2) & 0x2000) == 0 &&
          (((uVar1 ^ uVar2) & 0x8000) == 0)))) {
        uVar1 = *(ushort *)(iVar3 + 0x81);
        uVar2 = *(ushort *)(iVar4 + 0x81);
        in_EAX = (int *)(uint)(uVar1 ^ uVar2);
        if ((((((uVar1 ^ uVar2) & 0x100) == 0) &&
             (in_EAX = (int *)(uint)(uVar1 ^ uVar2), ((uVar1 ^ uVar2) & 0x800) == 0)) &&
            (in_EAX = (int *)(uint)(uVar1 ^ uVar2), ((uVar1 ^ uVar2) & 0x1000) == 0)) &&
           (((in_EAX = (int *)(uint)(uVar1 ^ uVar2), ((uVar1 ^ uVar2) & 0x2000) == 0 &&
             (in_EAX = (int *)(uint)(uVar1 ^ uVar2), ((uVar1 ^ uVar2) & 0x4000) == 0)) &&
            ((((uVar1 ^ uVar2) & 0x8000) == 0 && (*(int *)(iVar3 + 0x29) == *(int *)(iVar4 + 0x29))
            )))) {
          iVar11 = 0;
          iVar10 = 0;
          while( true ) {
            if (*(int *)(iVar3 + 0x29) <= iVar11) {
              return CONCAT31((int3)((uint)in_EAX >> 8),1);
            }
            iVar9 = *(int *)(iVar4 + 0x4f) + iVar10;
            in_EAX = *(int **)(*(int *)(iVar3 + 0x4f) + 0xd + iVar10);
            iVar7 = *(int *)(iVar3 + 0x4f) + iVar10;
            if (((in_EAX != *(int **)(iVar9 + 0xd)) ||
                (in_EAX = *(int **)(iVar7 + 0x11), in_EAX != *(int **)(iVar9 + 0x11))) ||
               ((in_EAX = *(int **)(iVar7 + 0x15), in_EAX != *(int **)(iVar9 + 0x15) ||
                (((in_EAX = (int *)(*(uint *)(iVar9 + 0x83) ^ *(uint *)(iVar7 + 0x83)),
                  ((uint)in_EAX & 4) != 0 ||
                  (in_EAX = (int *)CONCAT22((short)((uint)in_EAX >> 0x10),*(short *)(iVar7 + 0x87)),
                  *(short *)(iVar7 + 0x87) != *(short *)(iVar9 + 0x87))) ||
                 (in_EAX = *(int **)(iVar7 + 0xb1), in_EAX != *(int **)(iVar9 + 0xb1))))))) break;
            iVar11 = iVar11 + 1;
            iVar10 = iVar10 + 0xb5;
          }
          return (uint)in_EAX & 0xffffff00;
        }
      }
    }
  }
  return (uint)in_EAX & 0xffffff00;
}
```

### FUN_004d91e0 (gate excerpt)

```c
cVar3 = FUN_004d9040(&local_18);
local_24 = (uint)(cVar3 == '\0');
for (iVar5 = 0; iVar5 < local_20; iVar5 = iVar5 + 1) {
  ...
  iVar7 = FUN_004ca5c0();
  if ((iVar7 == 0) || (local_24 == 1)) {
    ...
    FUN_004d91e0(uVar4);   // recurse
  }
  ...
}
```

---

## 8. Items that could NOT be determined from decompilation

All five questions were fully answerable from the Ghidra decompilation. Nothing is
left as UNDETERMINED for this scope. (Semantic meaning of each bit/field — e.g.
*which* contract attribute bit 12 of `+0x81` represents — is outside the scope of
this verification and was not asked.)
