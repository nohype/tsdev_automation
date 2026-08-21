# TradeStation Component Types

## Domain Knowledge (from user, 2026-08-16)

TradeStation's EasyLanguage supports **8 component types**, each with different
purposes and constraints (available/permitted keywords, libraries, functions):

| Type | Purpose |
|------|---------|
| ActivityBar | |
| Function | Reusable calculations (e.g., Average, MACD) |
| Indicator | Plots values on charts (can use Plot1, Plot2, etc.) |
| PaintBar | |
| ProbabilityMap | |
| ShowMe | Plots markers on chart bars meeting a condition (can use Plot1, Plot2, etc.) |
| Strategy | Trading strategies executed on market data to generate orders. **Cannot plot.** |
| TradingApp | |

## Key constraints
- **Strategies cannot use Plot statements** (Plot1, Plot2, etc.) — only indicators, ShowMe, and PaintBar can plot.
- Strategies generate orders (Buy, Sell, etc.).
- Functions are reusable calculations callable by other components.
- ShowMe uses Plot statements (like Indicators) but cannot use order statements (Buy/Sell).
- All are coded in EasyLanguage but with different permitted keywords/libraries.

## Mapping to orlib20 warehouse types

orlib20 uses numeric warehouse types in `CreateEntryEX`, `FindExistPos`, etc.
The mapping is partially confirmed. Current evidence:

| WHType | MasterRec count | Component type | Status | Evidence |
|--------|----------------|----------------|--------|----------|
| 0 | 1454 | Strategy | **CONFIRMED** | CreateEntryEX(0,...) creates entries visible in TSDev strategy list |
| 1 | 51 | ??? | UNCONFIRMED | |
| 2 | 406 | Indicator (base) + subtypes | **CONFIRMED** (base) | Type converter has 7 subtypes for type 2 |
| 3 | 5 | ??? | UNCONFIRMED | |
| 4 | 393 | Function | **CONFIRMED** | FindExistPos("Average", 4) = 76; "Average" is a Function |
| 5122 (0x1402) | — | TradingApp | **CONFIRMED** | Expanded whType = (subType 0x14 << 8) \| category 2. See `tmp/re_artifacts/tradingapp_whtype.md` + `tmp/re_artifacts/c1_adversarial_review.md` |
| 1282 (0x0502) | — | ShowMe | **CONFIRMED** | Expanded whType = (subType 0x05 << 8) \| category 2. Confirmed via Ghidra RE (GetFileTypeName1, FUN_1019f510, ConvertToPowerType, FUN_101d4280). See `docs/RE/RE_SHOWME_WHTYPE_FINDINGS.md`. Canonical subtype 0x05; raw subtypes 0x03 and 0x05 both map to ShowMe. |

### Encoding formula: expanded whType

The orlib20 warehouse type system uses an **expanded whType** encoding:

```
expanded = (subType << 8) | category
```

- **category** (low 8 bits): the base warehouse type passed to orlib20 API functions (`CreateEntryEX`, `FindExistPos`, `SaveModule_New`, `VerifyModule_New`, delete orchestrator).
- **subType** (high bits): the sub-type within the category. Passed as `CreateEntryEX`'s 2nd parameter.
- **.ell filenames** use the full expanded whType: `elmodule_<expanded>_<progId>.ell`.

For types with sub-type 0 (Strategy=0, Indicator=2, Function=4), the expanded whType equals the category, so no decomposition is needed. For **TradingApp** (sub-type 0x14, category 2), the expanded whType (5122) differs from the category (2) — the API takes category=2 (same as Indicator) while the `.ell` filename uses 5122. For **ShowMe** (sub-type 0x05, category 2), the expanded whType (1282) differs from the category (2) — the API takes category=2 while the `.ell` filename uses 1282.

### FindExistPos type parameter
FindExistPos takes `(name, warehouseType)` where warehouseType is the **category** (low 8 bits of the expanded whType):
- Type 0: Strategy — **CONFIRMED (whType=0, working in CLI)**
- Type 2: Indicator / TradingApp / ShowMe (TradingApp confirmed: FindExistPos with category=2 finds TradingApp entries; ShowMe confirmed: same category=2)
- Type 4: Function — confirmed: FindExistPos("Average", 4) = 76, FindExistPos("MACD", 4) = 20

### Type converter (from static analysis of orlib20.dll RVA 0x1D43A0)
- Type 0: {type=0, subtype=0x200, extra=-1}
- Type 1: {type=1, subtype=-1}
- Type 2: {type=2, subtypes=[0x502, 0x402, 0x602, 0x802, 0xA02, 0xC02, 0xE02]} (7 subtypes — Indicator/PaintBar/ShowMe/ActivityBar). **0x502 = ShowMe CONFIRMED** (Ghidra RE, TVS-K.1). **0x402 = PaintBar CONFIRMED** (Ghidra RE — the §5.2 subtype table in FEASIBILITY.md previously had PaintBar/ShowMe subtypes SWAPPED; corrected: subtypes 2,4 = PaintBar, subtypes 3,5 = ShowMe).
- Type 3: {type=3, subtype=-1}
- Type 4: {type=4, subtype=-1}

## Resolved questions
- **WHType for Strategies:** WHType=0 (category 0). CONFIRMED — insert/delete/modify/exists all work.
- **WHType for Functions:** WHType=4 (category 4). CONFIRMED — insert/delete/modify/exists all work.
- **WHType for Indicators:** WHType=2 (category 2). CONFIRMED — insert/delete/modify/exists all work.
- **WHType for TradingApp:** WHType=5122 (0x1402 = category 2 | sub-type 0x14). CONFIRMED — Workstream C.
- **WHType for ShowMe:** WHType=1282 (0x0502 = category 2 | sub-type 0x05). CONFIRMED — TVS-K.1. Ghidra RE confirmed via GetFileTypeName1, FUN_1019f510, ConvertToPowerType, FUN_101d4280. See `docs/RE/RE_SHOWME_WHTYPE_FINDINGS.md`.
- **CreateEntryEX WHType vs FindExistPos type:** Same numbering — both use the category (low 8 bits of the expanded whType encoding).

## Open questions
- **3 unconfirmed types:** ActivityBar, PaintBar, ProbabilityMap. These require Ghidra/Frida RE work to determine their WHType values. See `docs/FEASIBILITY.md` §5.2/§5.5. (ShowMe was previously in this list but is now CONFIRMED — see TVS-K.1 above.)
