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

orlib20 uses numeric warehouse types (0-4) in `CreateEntryEX`, `FindExistPos`, etc.
The mapping is NOT yet fully confirmed. Current evidence:

| WHType | MasterRec count | Likely component type | Evidence |
|--------|----------------|----------------------|----------|
| 0 | 1454 | Strategy | CreateEntryEX(0,...) creates entries visible in TSDev strategy list |
| 1 | 51 | ??? | |
| 2 | 406 | Indicator + subtypes | Type converter has 7 subtypes for type 2 |
| 3 | 5 | ??? | |
| 4 | 393 | Function | FindExistPos("Average", 4) = 76; "Average" is a Function |

### FindExistPos type parameter
FindExistPos takes `(name, warehouseType)` where warehouseType maps to:
- Type 0: Strategy (subtype=0x200) — **NOT YET CONFIRMED via FindExistPos**
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
