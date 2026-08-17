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
| ShowMe | |
| Strategy | Trading strategies executed on market data to generate orders. **Cannot plot.** |
| TradingApp | |

## Key constraints
- **Strategies cannot use Plot statements** (Plot1, Plot2, etc.) — only indicators can plot.
- Strategies generate orders (Buy, Sell, etc.).
- Functions are reusable calculations callable by other components.
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

### Encoding formula: expanded whType

The orlib20 warehouse type system uses an **expanded whType** encoding:

```
expanded = (subType << 8) | category
```

- **category** (low 8 bits): the base warehouse type passed to orlib20 API functions (`CreateEntryEX`, `FindExistPos`, `SaveModule_New`, `VerifyModule_New`, delete orchestrator).
- **subType** (high bits): the sub-type within the category. Passed as `CreateEntryEX`'s 2nd parameter.
- **.ell filenames** use the full expanded whType: `elmodule_<expanded>_<progId>.ell`.

For types with sub-type 0 (Strategy=0, Indicator=2, Function=4), the expanded whType equals the category, so no decomposition is needed. For **TradingApp** (sub-type 0x14, category 2), the expanded whType (5122) differs from the category (2) — the API takes category=2 (same as Indicator) while the `.ell` filename uses 5122.

### FindExistPos type parameter
FindExistPos takes `(name, warehouseType)` where warehouseType is the **category** (low 8 bits of the expanded whType):
- Type 0: Strategy — **NOT YET CONFIRMED via FindExistPos**
- Type 2: Indicator / TradingApp (TradingApp confirmed: FindExistPos with category=2 finds TradingApp entries)
- Type 4: Function — confirmed: FindExistPos("Average", 4) = 76, FindExistPos("MACD", 4) = 20

### Type converter (from static analysis of orlib20.dll RVA 0x1D43A0)
- Type 0: {type=0, subtype=0x200, extra=-1}
- Type 1: {type=1, subtype=-1}
- Type 2: {type=2, subtypes=[0x502, 0x402, 0x602, 0x802, 0xA02, 0xC02, 0xE02]} (7 subtypes — likely Indicator/PaintBar/ShowMe/ActivityBar)
- Type 3: {type=3, subtype=-1}
- Type 4: {type=4, subtype=-1}

## Open questions
- What WHType should CreateEntryEX use for Strategies? (Currently using 0, which creates entries visible in strategy list, but FindExistPos(type=0) hasn't been tested on them yet)
- What WHType for Functions? (FindExistPos type=4 finds functions, but is CreateEntryEX WHType=4 for functions?)
- Are CreateEntryEX WHType and FindExistPos type the same numbering?
- **4 remaining unconfirmed types:** ActivityBar, PaintBar, ProbabilityMap, ShowMe — their warehouse-type codes have NOT been confirmed and must NOT be treated as known.
