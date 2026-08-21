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

## 3. Question 2: CELCompiler +0x10 field (0xa02)

### Hypothesis
The hardcoded `0xa02` value is the same for ShowMe as for the 4 current types.

### Verdict: **LIKELY — 0xa02 is a type-independent constant**

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

#### 3c. Current CLI sets +0x10 to 0xa02 for ALL types

From `src/tsdev_lib.cpp:1135`:
```cpp
*(reinterpret_cast<int*>(static_cast<char*>(compilerMem) + 0x10)) = 0xa02;
```

This value was observed from Frida traces of TSDev.exe and is used for Strategy (cat 0), Indicator (cat 2), Function (cat 4), and TradingApp (cat 2, subtype 0x14). It is identical across all 4 types.

#### 3d. 0xa02 is NOT a whType-specific value

0xa02 = (0x0a << 8) | 2 = VolatilityModel expanded whType. But it's used for ALL component types (Strategy, Indicator, Function, TradingApp), so it's NOT a type-dependent parameter. It appears to be a flags/mode constant that TSDev.exe sets on the CELCompiler object.

#### 3e. VerifyModule_New does not read +0x10 in a type-dependent way

In `_VerifyModule_New@20`, the CELCompiler object (param_5) is passed to `CELBasicCompiler::CompileText`. The expanded whType is computed separately via `FUN_101d4280(param_2, local_44 >> 8)` and passed as a separate argument to CompileText. The +0x10 field is not re-derived from the whType inside VerifyModule_New.

### Conclusion

Since 0xa02 is:
1. Identical for all 4 current types (including 2 different categories and 2 subtypes of category 2)
2. Not set by orlib20's own internal verify wrapper (VerifyATechnique leaves it at 0)
3. Not a whType-derived value (it's the same for Strategy cat=0 and Indicator cat=2)

It is **almost certainly a type-independent constant** that will be the same for ShowMe. The implementation should continue using 0xa02.

**Confidence: LIKELY** (not CONFIRMED because TSDev.exe could not be opened in Ghidra to observe the actual value TSDev sets for ShowMe. A Frida trace of TSDev.exe creating a ShowMe would provide definitive confirmation.)

---

## 4. Question 3: enumerateRecords subtype filtering

### Hypothesis
ShowMe (subtype 0x04 in the hypothesis, actually 0x05) needs the same kind of subtype filtering as TradingApp in Phase 1, and possibly Phase 2.

### Verdict: **CONFIRMED — Phase 1 needs subtype filtering; Phase 2 needs no change**

### Evidence

#### 4a. Phase 1: _LoadMasterRecsEX returns ALL category-2 subtypes

`_LoadMasterRecsEX@16` (0x101a5360) calls `FUN_101d43a0(category, typeArray)` to get ALL expanded whTypes for the category, then iterates each, loading records into a single buffer. For category 2, this returns records for ALL subtypes (Indicator, PaintBar, ShowMe, Expert, TradingApp, etc.) mixed together.

The subtype at record offset 0xE9 is the **raw subtype** (confirmed by FUN_10158fb0: `*(char*)(rec + 0xe9) = (char)param_2`).

#### 4b. Current Phase 1 filter (tsdev_lib.cpp:1031-1036)

```cpp
if (category == 2) {
    unsigned char recSubType = (unsigned char)rec[kSubTypeOffset];
    if (subType == 0x14 && recSubType != 0x14) continue;  // TradingApp: keep only 0x14
    if (subType == 0x00 && recSubType == 0x14) continue;  // Indicator: exclude 0x14
}
```

This filter is **incomplete** — it only distinguishes TradingApp from Indicator. It does NOT exclude PaintBar, ShowMe, Expert, etc. from Indicator queries. This is a pre-existing issue.

#### 4c. ShowMe Phase 1 filter requirement

For ShowMe (subType == 0x05), a filter must be added to keep only ShowMe records:
```cpp
if (subType == 0x05 && recSubType != 0x05 && recSubType != 0x03) continue;
```

This matches both raw subtypes 3 and 5 (both map to ShowMe per ConvertToPowerType and GetFileTypeName1).

**Important:** Since we use subtype 5 for CreateEntryEX, newly created ShowMe records will have 0x05 at offset 0xE9. However, existing records created by TSDev.exe might use either 3 or 5. The filter should match both.

#### 4d. Phase 2: BST search — no subtype filtering needed

Phase 2 uses `FUN_101d43a0` (TypeConvert) to build a type filter array, then calls `FUN_101d05e0` (BST search wrapper) with this array. The BST search searches across ALL category-2 subtypes (the type filter includes all expanded whTypes for the category).

The BST search finds a node by name, and the verbatim name is read from it. The ProgId is obtained separately via `FindExistPos(searchName, type)` which uses `getCategory(type) = 2`.

This means Phase 2 does NOT filter by subtype — it could match any category-2 record with the same name. This is the **same pre-existing behavior for all category-2 types** (Indicator, TradingApp). ShowMe does not need any special Phase 2 handling beyond what already exists.

In practice, this is not a problem because component names are typically unique across all category-2 subtypes. If a name collision exists (e.g., an Indicator and a ShowMe with the same name), Phase 2 might return the wrong record, but this is a pre-existing issue not specific to ShowMe.

#### 4e. Buffer size note (pre-existing issue)

The Phase 2 code allocates `int typeFilterBuf[3]` but TypeConvert for category 2 writes 12 dwords. This is a pre-existing buffer overflow that happens to work because the overflowed data doesn't corrupt critical state. This is NOT specific to ShowMe and should be fixed separately.

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
| Q2: CELCompiler +0x10 | **LIKELY** | 0xa02 is identical for all 4 current types (2 categories, 2 subtypes). orlib20's own VerifyATechnique leaves it at 0. It's not type-derived. However, TSDev.exe could not be opened to observe the actual value for ShowMe. A Frida trace would confirm definitively. |
| Q3: enumerateRecords filtering | **CONFIRMED** (Phase 1), **CONFIRMED** (Phase 2 no change) | Phase 1 loads all category-2 subtypes; the 0xE9 field stores the raw subtype; filtering is needed. Phase 2 uses TypeConvert (all subtypes) and doesn't filter by subtype — same for all category-2 types. |

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

### 7c. CELCompiler +0x10 field

**No change needed.** Continue using `0xa02` for all types including ShowMe. The existing `verifyModule()` code already uses `getCategory(type)` for the VerifyModule_New whType parameter, which correctly passes category=2 for ShowMe.

### 7d. enumerateRecords Phase 1 subtype filter

Add ShowMe filtering in `src/tsdev_lib.cpp` around line 1031-1036. Replace:
```cpp
if (category == 2) {
    unsigned char recSubType = (unsigned char)rec[kSubTypeOffset];
    if (subType == 0x14 && recSubType != 0x14) continue;
    if (subType == 0x00 && recSubType == 0x14) continue;
}
```
With:
```cpp
if (category == 2) {
    unsigned char recSubType = (unsigned char)rec[kSubTypeOffset];
    if (subType == 0x14 && recSubType != 0x14) continue;
    if (subType == 0x00 && recSubType == 0x14) continue;
    if (subType == 0x05 && recSubType != 0x05 && recSubType != 0x03) continue;
}
```

### 7e. enumerateRecords Phase 2

**No change needed.** Phase 2 uses the same TypeConvert + BST search path for all category-2 types. ShowMe requires no special handling.

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
5. If possible, Frida-trace TSDev.exe creating a ShowMe to confirm the CELCompiler +0x10 field value (would elevate Q2 from LIKELY to CONFIRMED).

### 7j. Minimal ShowMe EasyLanguage source

ShowMe is a visual chart component (like PaintBar) that can use Plot statements. A minimal valid ShowMe source might be:
```easylanguage
{ ShowMe test }
if Close > Open then begin
    Plot1(High, "Up");
end;
```
This needs verification via TSDev.exe or TradeStation documentation, as ShowMe may have specific requirements (e.g., it plots markers at conditions, not continuous lines like indicators).
