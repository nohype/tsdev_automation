# RE Findings: ShowMe Component Type — orlib20 Parameters

**Date:** 2026-08-16
**Analyst:** RE subagent (Ghidra static analysis)
**Scope:** Confirm orlib20 parameters needed to add ShowMe component type to the CLI
**Program analyzed:** orlib20.dll (VA 0x10000000, version 9.50.01.3344) in Ghidra project `tsdev.exe`

---

## 1. Ghidra Project State

**Project:** `tsdev.exe` (6 programs total)
**Open programs:** orlib20.dll, ELDebugger.dll
**Unavailable (DB files missing):** TSDev.exe, TSDev.exe.0, WHServer.exe, ORELUI20.dll
**Tools available:** 238 Ghidra MCP tools (all groups loaded: analysis, function, listing, xref, etc.)

**Key functions decompiled:**
- `FUN_101d43a0` (TypeConvert) — expands category to subtype list
- `ConvertToPowerType` @ `0x101584f0` — maps raw subtypes to power types
- `FUN_101d4280` — maps (category, rawSubtype) to expanded whType
- `GetFileTypeName1` @ `0x10157dc0` — maps (category, subtype) to type name string
- `FUN_101583c0` — alternate (category, subtype) to type name mapping
- `FUN_1019f510` — maps expanded whType to wide-char type name
- `_CreateEntryEX@24` @ `0x10159610` — entry creation
- `FUN_10158fb0` — record initialization (sets offset 0xE9)
- `_VerifyModule_New@20` @ `0x10169630` — compile/verify
- `FUN_10165a00` — CELCompiler constructor
- `VerifyATechnique` @ `0x10169510` — internal verify wrapper
- `_LoadMasterRecsEX@16` @ `0x101a5360` — record enumeration
- `_GetMasterRecByType@16` @ `0x101a0460` — single record lookup

---

## 2. Question 1: ShowMe whType

### Hypothesis
ShowMe = category 2, subtype 0x04, expanded whType = 1026 (0x0402).

### Verdict: **REJECTED — the hypothesis is WRONG**

ShowMe is **category=2, subtype=0x05** (or 0x03), **expanded whType = 1282 (0x0502)**.
The value **1026 (0x0402) is PaintBar**, not ShowMe.

### Evidence

#### 2a. GetFileTypeName1 (0x10157dc0) — (category, subtype) → name

Decompiled switch for category 2:
```c
case 2:
    switch(param_2) {
    case 0: case 1:  return "Indicator";
    case 2: case 4:  return "PaintBar";    // ← subtype 2 OR 4 = PaintBar
    case 3: case 5:  return "ShowMe";      // ← subtype 3 OR 5 = ShowMe
    case 6: case 7:  return "Expert";
    case 8: case 9:  return "Expiration Rule";
    case 10: case 0xb: return "Volatility Model";
    case 0xc: case 0xd: return "Pricing Model";
    case 0xe: case 0xf: return "Bid/Ask Model";
    case 0x10: case 0x11: return "Search Strategy";
    case 0x12: case 0x13: return "ProbabilityMap";
    case 0x14: return "TradingApp";
    }
```

**ShowMe = subtype 3 or 5. PaintBar = subtype 2 or 4.** Subtype 0x04 is PaintBar.

#### 2b. FUN_101583c0 — alternate (category, subtype) → name mapping

Identical mapping:
```c
case 2: case 4:  return "PaintBar";
case 3: case 5:  return "ShowMe";
```

#### 2c. FUN_1019f510 — expanded whType → wide-char name

```c
if (param_1 == 0x402) return L"PaintBar";   // 0x402 = 1026 = PaintBar
if (param_1 == 0x502) return L"ShowMe";     // 0x502 = 1282 = ShowMe
```

**0x0402 (1026) = PaintBar. 0x0502 (1282) = ShowMe.**

#### 2d. ConvertToPowerType (0x101584f0) — raw subtype → power type

```c
case 2:  // category 2
    switch(param_2) {
    case 2: case 4:  return 4;    // PaintBar power type
    case 3: case 5:  return 5;    // ShowMe power type
    case 6: case 7:  return 6;    // Expert
    case 8: case 9:  return 8;    // Expiration Rule
    case 10: case 0xb: return 10; // Volatility Model
    case 0xc: case 0xd: return 0xc;
    case 0xe: case 0xf: return 0xe;
    case 0x10: case 0x11: return 0x10;
    case 0x12: case 0x13: return 0x12;
    case 0x14: return 0x14;       // TradingApp
    }
```

Raw subtypes come in pairs (even, odd) that map to a single power type:
- PaintBar: raw {2,4} → power type 4
- ShowMe: raw {3,5} → power type 5

#### 2e. FUN_101d4280 — (category, rawSubtype) → expanded whType

```c
case 2:
    uVar2 = ConvertToPowerType(2, param_2);
    switch(uVar2) {
    case 0:  return 2;       // Indicator
    case 4:  return 0x402;   // PaintBar (1026)
    case 5:  return 0x502;   // ShowMe (1282)
    case 6:  return 0x602;   // Expert
    case 8:  return 0x802;
    case 10: return 0xa02;
    case 0xc: return 0xc02;
    case 0xe: return 0xe02;
    case 0x10: return 0x1002;
    case 0x12: return 0x1202;
    case 0x14: return 0x1402; // TradingApp (5122)
    }
```

**ShowMe expanded whType = 0x502 = 1282. PaintBar = 0x402 = 1026.**

#### 2f. TypeConvert / FUN_101d43a0 — category → expanded whType list

```c
case 2:
    param_2[0] = 2;       // base type
    param_2[1] = 0x502;   // ShowMe (1282)
    param_2[2] = 0x402;   // PaintBar (1026)
    param_2[3] = 0x602;   // Expert
    param_2[4] = 0x802;
    param_2[5] = 0xa02;
    param_2[6] = 0xc02;
    param_2[7] = 0xe02;
    param_2[8] = 0x1002;
    param_2[9] = 0x1202;
    param_2[10] = 0x1402;  // TradingApp
    param_2[0xb] = -1;     // terminator
```

The TypeConvert list uses 0x502 (subtype 5) as the canonical ShowMe expanded whType.

#### 2g. .ell filename set corroboration

FEASIBILITY.md §3 states: "X in {0,1,2,3,4,1026,1282,4610,5122}".
- 1026 = 0x0402 = **PaintBar** (subtype 4)
- 1282 = 0x0502 = **ShowMe** (subtype 5)
- 4610 = 0x1202 = ProbabilityMap
- 5122 = 0x1402 = TradingApp

The `elmodule_1026_*.ell` files cited as evidence for ShowMe are actually **PaintBar** files. The ShowMe files are `elmodule_1282_*.ell`.

#### 2h. FEASIBILITY.md §5.2 table is WRONG

The table says: "2,3 = PaintBar; 4,5 = ShowMe"
Ghidra confirms: "2,4 = PaintBar; 3,5 = ShowMe"

The PaintBar and ShowMe subtype pairs are **swapped** in the FEASIBILITY table.

#### 2i. Record initialization (FUN_10158fb0) confirms subtype handling

The record init function at `0x10158fb0` stores the raw subtype at offset 0xE9:
```c
*(char *)((int)param_3 + 0xe9) = (char)param_2;  // param_2 = raw subtype
```

For category 2, it then switches on the subtype:
```c
case 0: case 1: case 0x14:   // Indicator, TradingApp → scale settings, 0x9000
case 2: case 3: case 4: case 5:  // PaintBar, ShowMe → 0xf400
case 10...0x11:               // Volatility/Pricing/BidAsk → 0x8800
case 0x12: case 0x13:         // ProbabilityMap → special
```

ShowMe (subtype 3 or 5) falls into the `case 2: case 3: case 4: case 5` group with PaintBar, receiving `0xf400` flags. This is consistent — both are visual chart overlay components.

### Correct ShowMe parameters

| Parameter | Value |
|-----------|-------|
| Category (low 8 bits) | 2 (same as Indicator, TradingApp) |
| Raw subtype (CreateEntryEX param 2) | **5** (canonical; 3 also valid) |
| Power type (ConvertToPowerType output) | 5 |
| Expanded whType (enum value) | **1282** (0x0502 = (5 << 8) \| 2) |
| .ell filename pattern | `elmodule_1282_<progId>.ell` |
| getSubType(1282) = 1282 >> 8 | 5 |
| getCategory(1282) = 1282 & 0xFF | 2 |

### Why subtype 5 (not 3)?

Both raw subtypes 3 and 5 map to power type 5 and thus expanded whType 0x502 = 1282. However:
1. The TypeConvert function lists 0x502 (subtype 5) as the canonical ShowMe expanded whType.
2. The .ell filename set includes 1282 (0x502) but NOT 770 (0x0302).
3. Using subtype 5 gives expanded whType 1282, matching existing .ell files on disk.
4. The encoding formula `expanded = (subtype << 8) | category` with subtype 5 gives 1282, consistent with how TradingApp works (subtype 0x14 → 5122).

Using subtype 3 would give expanded whType 770 (0x0302), which is NOT in the .ell filename set and would create .ell files with a non-standard prefix.

---

## 3. Question 2: CELCompiler +0x10 field (historical `0xa02` finding; superseded)

> **CURRENT GUIDANCE — operation ProgId assignment:** The former guidance that
> `CELCompiler +0x10 = 0xa02` is superseded. `TsDevLib::verifyModule`
> writes the operation's `progId` to compiler-object `+0x10`: the TSDev-parity
> field layout is documented at `src/tsdev_lib.cpp:1641-1646` and the live
> assignments are at `src/tsdev_lib.cpp:1672-1679`. The same `progId` is passed
> through `callVerify` to `_VerifyModule_New` (`src/tsdev_lib.cpp:173-183`,
> call site `src/tsdev_lib.cpp:1711-1718`). For an insert, `TsDevLib::createEntry`
> obtains that value from `CreateEntryEX` (`src/tsdev_lib.cpp:1356-1375`); for an
> existing item, `TsDevLib::findExistPos` uses the value returned by
> `FindExistPos` (`src/tsdev_lib.cpp:1346-1354`). For ShowMe, retain the
> confirmed category 2, subtype 5, and expanded whType 1282 mapping above;
> `0xa02` is not the current compiler-object `+0x10` assignment.

### Historical RE finding (superseded for current CLI guidance)

### Historical hypothesis
The hardcoded `0xa02` value is the same for ShowMe as for the 4 current types.

### Historical verdict: **LIKELY — 0xa02 was a type-independent constant** (superseded)

### Evidence

#### 3a. CELCompiler constructor (FUN_10165a00) initializes +0x10 to 0

```c
*(undefined4 *)(param_1 + 0x10) = 0;  // zeroed by constructor
```

The +0x10 field is NOT set by the constructor — it's left to the caller.

#### 3b. VerifyATechnique (orlib20 internal) does NOT set +0x10

```c
pvVar5 = operator_new(0x58);          // allocate 88-byte CELCompiler
iVar4 = FUN_10165a00();               // construct (zeroes +0x10)
// NOTE: +0x10 is NOT set to 0xa02 or any other value
iVar7 = _VerifyModule_New_20(0, uVar3, uVar6, 0, iVar4);  // verify with +0x10=0
```

The internal orlib20 verify wrapper leaves +0x10 at 0 and still works.

#### 3c. Historical CLI state: +0x10 was set to 0xa02 (superseded)

A prior CLI implementation contained:
```cpp
*(reinterpret_cast<int*>(static_cast<char*>(compilerMem) + 0x10)) = 0xa02;
```

This historical value was observed from Frida traces of TSDev.exe and was
recorded as used for Strategy (cat 0), Indicator (cat 2), Function (cat 4),
and TradingApp (cat 2, subtype 0x14). It was recorded as identical across all
four types. The current tracked implementation instead assigns the operation
`progId`; see the current guidance at the start of this section.

#### 3d. Historical interpretation: 0xa02 was NOT a whType-specific value

Historically, `0xa02 = (0x0a << 8) | 2` was identified as the VolatilityModel
expanded whType. The earlier finding recorded it as used for all component types
(Strategy, Indicator, Function, TradingApp), so it was not treated as a
type-dependent parameter; it was instead interpreted as a flags/mode constant
that TSDev.exe set on the CELCompiler object. This interpretation is retained
for historical context only and is superseded by the current ProgId assignment.

#### 3e. Historical VerifyModule_New observation: +0x10 was not re-derived from whType

In `_VerifyModule_New@20`, the CELCompiler object (param_5) is passed to `CELBasicCompiler::CompileText`. The expanded whType is computed separately via `FUN_101d4280(param_2, local_44 >> 8)` and passed as a separate argument to CompileText. The +0x10 field is not re-derived from the whType inside VerifyModule_New.

### Historical conclusion (superseded)

The historical evidence recorded that 0xa02 was:
1. Identical for all 4 observed types (including 2 different categories and 2
   subtypes of category 2)
2. Not set by orlib20's own internal verify wrapper (VerifyATechnique leaves it
   at 0)
3. Not a whType-derived value (it was the same for Strategy cat=0 and Indicator
   cat=2)

The historical conclusion was that it was **almost certainly a type-independent
constant** that would be the same for ShowMe, with a recommendation to continue
using 0xa02. That recommendation is superseded. Current guidance is to assign
the operation's `progId` to compiler `+0x10`, as documented at the start of this
section and evidenced by `src/tsdev_lib.cpp:1641-1679`.

**Historical confidence: LIKELY** (not CONFIRMED because TSDev.exe could not be
opened in Ghidra to observe the actual value TSDev set for ShowMe).

---

## 4. Question 3: enumerateRecords subtype filtering (historical finding; current source status)

### Historical hypothesis
ShowMe (subtype 0x04 in the hypothesis, actually 0x05) needs the same kind of subtype filtering as TradingApp in Phase 1, and possibly Phase 2. This is a historical hypothesis, not current implementation guidance.

### Historical verdict: **Phase 1 needed subtype filtering; Phase 2 needed no change** (superseded for current Phase 1 guidance)

> **CURRENT GUIDANCE — category-2 filtering and Phase 2 buffer:** The historical
> incomplete-filter and three-element-buffer statements below do not describe
> the tracked source. `TsDevLib::enumerateRecords` is implemented at
> `src/tsdev_lib.cpp:1495-1627`. Its Phase 1 `_LoadMasterRecsEX@16` loop is at
> `src/tsdev_lib.cpp:1502-1563`; the current category-2 filter is the branch at
> `src/tsdev_lib.cpp:1543-1549`. That branch fully filters the currently
> supported category-2 queries: TradingApp keeps raw subtype `0x14`, Indicator
> keeps raw subtypes `0x00`/`0x01`, and ShowMe keeps raw subtypes `0x03`/`0x05`.
> The Phase 2 type-filter buffer is `int typeFilterBuf[16] = {};` at
> `src/tsdev_lib.cpp:1589-1593`, and it is passed to the BST search at
> `src/tsdev_lib.cpp:1601-1604`. There is no current `int typeFilterBuf[3]`
> overflow or pending ShowMe-filter change in the tracked source.

### Evidence

#### 4a. Historical RE observation: _LoadMasterRecsEX returns ALL category-2 subtypes

`_LoadMasterRecsEX@16` (0x101a5360) calls `FUN_101d43a0(category, typeArray)` to get ALL expanded whTypes for the category, then iterates each, loading records into a single buffer. For category 2, this returns records for ALL subtypes (Indicator, PaintBar, ShowMe, Expert, TradingApp, etc.) mixed together before the caller applies its query-specific filter. This remains the reason the current Phase 1 branch is required.

The subtype at record offset 0xE9 is the **raw subtype** (confirmed by FUN_10158fb0: `*(char*)(rec + 0xe9) = (char)param_2`).

#### 4b. Historical Phase 1 source snapshot (superseded)

The following was the older Phase 1 implementation (the former `tsdev_lib.cpp:1031-1036` range); it is retained as historical evidence only:

```cpp
if (category == 2) {
    unsigned char recSubType = (unsigned char)rec[kSubTypeOffset];
    if (subType == 0x14 && recSubType != 0x14) continue;  // TradingApp: keep only 0x14
    if (subType == 0x00 && recSubType == 0x14) continue;  // Indicator: exclude 0x14
}
```

That historical filter was **incomplete** — it only distinguished TradingApp from Indicator and did not exclude PaintBar, ShowMe, Expert, etc. from Indicator queries. It is not a current-source claim; the current branch is identified in the current-guidance block above.

#### 4c. Historical ShowMe Phase 1 filter recommendation (superseded; already implemented)

The historical recommendation for ShowMe (`subType == 0x05`) was:

```cpp
if (subType == 0x05 && recSubType != 0x05 && recSubType != 0x03) continue;
```

This matches both raw subtypes 3 and 5 (both map to ShowMe per ConvertToPowerType and GetFileTypeName1). The recommendation to add this condition is retained for provenance only; the condition is already present in the current `TsDevLib::enumerateRecords` category-2 branch at `src/tsdev_lib.cpp:1543-1549`. No additional ShowMe filtering change is pending.

**Historical note:** Since the implementation used subtype 5 for CreateEntryEX, newly created ShowMe records were expected to have 0x05 at offset 0xE9. Existing records created by TSDev.exe might use either 3 or 5, so the historical recommendation matched both.

#### 4d. Phase 2: BST search — no subtype narrowing is needed

Phase 2 uses `FUN_101d43a0` (TypeConvert) to build a type filter array, then calls `FUN_101d05e0` (BST search wrapper) with this array. The current implementation is the Phase 2 range at `src/tsdev_lib.cpp:1570-1624`; it builds the category-wide array at `src/tsdev_lib.cpp:1589-1593` and calls the BST wrapper at `src/tsdev_lib.cpp:1601-1604`. The BST search therefore searches across ALL category-2 subtypes (the type filter includes all expanded whTypes for the category), with no additional subtype narrowing in Phase 2.

The BST search finds a node by name, and the verbatim name is read from it. The ProgId is obtained separately via `TsDevLib::findExistPos` / `FindExistPos` (`src/tsdev_lib.cpp:1346-1354`), which uses `getCategory(type) = 2` for ShowMe.

This is the same behavior for all current category-2 types (Indicator, TradingApp, and ShowMe). ShowMe needs no special Phase 2 handling beyond the existing path. If a name collision exists (for example, an Indicator and a ShowMe with the same name), Phase 2 can match a category-2 record without subtype narrowing; this is a general category-2 behavior, not a ShowMe-specific pending fix.

#### 4e. Historical buffer-size note (superseded)

An earlier source snapshot allocated `int typeFilterBuf[3]` while TypeConvert for category 2 writes 12 dwords. That historical observation described a buffer overflow in the older implementation. It is not a current-source claim: the tracked Phase 2 code now allocates `int typeFilterBuf[16] = {};` at `src/tsdev_lib.cpp:1592` and passes that buffer through `callBuildTypeArray` at `src/tsdev_lib.cpp:1593`. No current three-element buffer overflow is supported by the repository.

---

## 5. Additional Findings

### 5a. Complete subtype mapping for category 2 (CONFIRMED via Ghidra)

| Raw Subtypes | Power Type | Expanded whType | Type Name | .ell Prefix |
|--------------|-----------|-----------------|-----------|-------------|
| 0, 1 | 0 | 2 (0x002) | Indicator | elmodule_2_ |
| 2, 4 | 4 | 1026 (0x402) | PaintBar | elmodule_1026_ |
| 3, 5 | 5 | 1282 (0x502) | ShowMe | elmodule_1282_ |
| 6, 7 | 6 | 1538 (0x602) | Expert | elmodule_1538_ |
| 8, 9 | 8 | 2050 (0x802) | Expiration Rule | elmodule_2050_ |
| 0xa, 0xb | 0xa | 2562 (0xa02) | Volatility Model | elmodule_2562_ |
| 0xc, 0xd | 0xc | 3074 (0xc02) | Pricing Model | elmodule_3074_ |
| 0xe, 0xf | 0xe | 3586 (0xe02) | Bid/Ask Model | elmodule_3586_ |
| 0x10, 0x11 | 0x10 | 4098 (0x1002) | Search Strategy | elmodule_4098_ |
| 0x12, 0x13 | 0x12 | 4610 (0x1202) | ProbabilityMap | elmodule_4610_ |
| 0x14 | 0x14 | 5122 (0x1402) | TradingApp | elmodule_5122_ |

### 5b. FEASIBILITY.md §5.2 table correction needed

The table currently says:
- "2,3 = PaintBar; 4,5 = ShowMe"

Should be:
- "2,4 = PaintBar; 3,5 = ShowMe"

### 5c. TSDev.exe not available in Ghidra

TSDev.exe is listed in the project (6 programs) but its database files are missing and cannot be opened. This prevented direct observation of TSDev.exe's ShowMe creation call site. All findings are from orlib20.dll static analysis.

### 5d. Record init (FUN_10158fb0) category-2 subtype groups

| Subtypes | Flags set | Component types |
|----------|-----------|-----------------|
| 0, 1, 0x14 | 0x9000 + scale settings | Indicator, TradingApp |
| 2, 3, 4, 5 | 0xf400 | PaintBar, ShowMe |
| 0xa-0x11 | 0x8800 | Volatility/Pricing/BidAsk/Search |
| 0x12, 0x13 | 0xe400 + special | ProbabilityMap |

ShowMe (3, 5) shares the same default record settings as PaintBar (2, 4), which makes sense as both are visual chart overlay components.

### 5e. FUN_1019f510 uses "Application" for TradingApp

The wide-char name function returns `L"Application"` for 0x1402 (TradingApp), while GetFileTypeName1 returns "TradingApp". This is a naming discrepancy, not a functional issue.

---

## 6. Confidence Levels

| Question | Confidence | Explanation |
|----------|-----------|-------------|
| Q1: ShowMe whType | **CONFIRMED** | 4 independent Ghidra functions (GetFileTypeName1, FUN_101583c0, FUN_1019f510, ConvertToPowerType + FUN_101d4280) all agree: ShowMe = subtype 3/5, expanded 0x502 = 1282. The .ell filename set corroborates. The hypothesis (subtype 4, expanded 1026) is definitively PaintBar. |
| Q2: CELCompiler +0x10 | **HISTORICAL LIKELY; SUPERSEDED** | The historical finding recorded 0xa02 as identical for all 4 observed types (2 categories, 2 subtypes); orlib20's own VerifyATechnique leaves it at 0, and the value was not type-derived in that historical analysis. Current `TsDevLib::verifyModule` guidance instead assigns the operation's `progId` to `+0x10` (`src/tsdev_lib.cpp:1629-1679`), then passes it through `callVerify` (`src/tsdev_lib.cpp:173-183`, call site `:1711-1718`). |
| Q3: enumerateRecords filtering | **HISTORICAL FINDING; CURRENT SOURCE CONFIRMED** | The historical Phase 1 snapshot was incomplete, but current `TsDevLib::enumerateRecords` applies the full supported category-2 filter in `src/tsdev_lib.cpp:1543-1549`. Phase 2 intentionally uses the category-wide TypeConvert array (`src/tsdev_lib.cpp:1589-1604`) and its current buffer is 16 ints, not 3 (`src/tsdev_lib.cpp:1592`). |

---

## 7. Recommendations for the Implementation Subagent

### 7a. ShowMe enum value

Add to `ComponentType` enum in `src/tsdev_lib.h`:
```cpp
ShowMe = 1282   // 0x0502 = (subType 0x05 << 8) | category 2
```

The existing `getCategory()` and `getSubType()` functions will correctly decompose this:
- `getCategory(ShowMe) = 1282 & 0xFF = 2`
- `getSubType(ShowMe) = 1282 >> 8 = 5`

### 7b. CLI type parsing

Add "showme" to the type parsing function (alongside "indicator", "tradingapp", etc.).

### 7c. CELCompiler +0x10 field — current guidance

Assign the operation's `progId` to compiler-object `+0x10`; do **not** use the
historical `0xa02` assignment. The tracked current `TsDevLib::verifyModule`
implementation writes
`*(reinterpret_cast<int*>(base + 0x10)) = progId` in
`src/tsdev_lib.cpp:1629-1679` (field layout at `:1641-1646`, assignment at
`:1672-1679`) and passes the same `progId` through `callVerify`
(`src/tsdev_lib.cpp:173-183`, call site `:1711-1718`) to
`_VerifyModule_New`. For ShowMe, the operation still passes category 2 to the
API while retaining expanded whType 1282 for the component mapping above.

### 7d. enumerateRecords Phase 1 subtype filtering — historical recommendation, already implemented

The earlier recommendation to add the ShowMe condition is retained in §4c as
historical provenance only; it is not a current change request. The current
`TsDevLib::enumerateRecords` implementation is at
`src/tsdev_lib.cpp:1495-1627`, and its complete supported category-2 filter is
already present at `src/tsdev_lib.cpp:1543-1549`:

```cpp
if (category == 2) {
    unsigned char recSubType =
        (unsigned char)rec[kSubTypeOffset];
    if (subType == 0x14 && recSubType != 0x14) continue;
    if (subType == 0x00 && recSubType != 0x00 && recSubType != 0x01) continue;
    if (subType == 0x05 && recSubType != 0x05 && recSubType != 0x03) continue;
}
```

No additional Phase 1 or ShowMe filtering change is pending.

### 7e. enumerateRecords Phase 2

**No change needed.** Phase 2 uses the same TypeConvert + BST search path for all
current category-2 types. The current `int typeFilterBuf[16] = {};` allocation
and `callBuildTypeArray` call are at `src/tsdev_lib.cpp:1589-1593`; the BST
search call is at `src/tsdev_lib.cpp:1601-1604`. ShowMe requires no special
handling, and the historical three-element-buffer overflow is superseded.

### 7f. .ell file path generation

**No change needed.** The existing `getEllPath()` and `getXmlPath()` use `static_cast<int>(type)` (the expanded whType), which will correctly produce `elmodule_1282_<progId>.ell` for ShowMe.

### 7g. deleteEntry

**No change needed.** The deleteEntry function uses `getCategory(type)` for the delete orchestrator and `static_cast<int>(type)` (expanded whType) for file path generation. Both work correctly with ShowMe = 1282.

### 7h. Documentation updates needed

1. **FEASIBILITY.md §5.2**: Fix the subtype table — swap PaintBar (2,4) and ShowMe (3,5).
2. **TS_COMPONENT_TYPES.md**: Update the type converter section and add ShowMe as CONFIRMED.
3. **FEASIBILITY.md §3**: Clarify that 1026 = PaintBar and 1282 = ShowMe in the .ell filename set.

### 7i. Testing

1. Create a ShowMe entry via the CLI and verify the .ell file is named `elmodule_1282_<progId>.ell`.
2. Verify the entry appears in TSDev.exe's ShowMe list (not Indicator or PaintBar list).
3. Test case-insensitive exists for ShowMe (both Phase 1 and Phase 2).
4. Test delete and modify for ShowMe.
5. **Historical follow-up only (superseded):** Frida-tracing TSDev.exe creating a ShowMe was proposed to confirm the old CELCompiler `+0x10` assumption. The tracked current source now provides the operation `progId` assignment (`src/tsdev_lib.cpp:1641-1679`); no new live trace is implied.

### 7j. Minimal ShowMe EasyLanguage source

ShowMe is a visual chart component (like PaintBar) that can use Plot statements. A minimal valid ShowMe source might be:
```easylanguage
{ ShowMe test }
if Close > Open then begin
    Plot1(High, "Up");
end;
```
This needs verification via TSDev.exe or TradeStation documentation, as ShowMe may have specific requirements (e.g., it plots markers at conditions, not continuous lines like indicators).
