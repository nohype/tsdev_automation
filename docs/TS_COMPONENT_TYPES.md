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
The mapping is partially confirmed. The five confirmed component-to-warehouse mappings are Strategy=0, Indicator=2, Function=4, TradingApp=5122 (0x1402), and ShowMe=1282 (0x0502). The three unconfirmed component mappings remain ActivityBar, PaintBar, and ProbabilityMap; this document does not assign warehouse types to them.

The `MasterRec` values below are retained only as historical/transient observations from a mutable warehouse snapshot. They are not stable current inventory facts.

| WHType | Historical/transient MasterRec observation | Component type | Status | Evidence |
|--------|--------------------------------------------|----------------|--------|----------|
| 0 | 1454 (historical/transient) | Strategy | **CONFIRMED** | CreateEntryEX(0,...) creates entries visible in TSDev strategy list |
| 1 | 51 (historical/transient) | ??? | UNCONFIRMED | |
| 2 | 406 (historical/transient) | Indicator (base) + subtypes | **CONFIRMED** (base) | Type converter has category-2 outputs listed below |
| 3 | 5 (historical/transient) | ??? | UNCONFIRMED | |
| 4 | 393 (historical/transient) | Function | **CONFIRMED** | Historical/transient observation: `FindExistPos("Average", 4) = 76`; "Average" is a Function |
| 5122 (0x1402) | — | TradingApp | **CONFIRMED** | Expanded whType = (subType 0x14 << 8) \| category 2. Durable Ghidra RE: `docs/RE/RE_SHOWME_WHTYPE_FINDINGS.md` §§2a, 2d–2f, 5a; source mapping/decomposition: `src/tsdev_lib.h:45-67`, `src/tsdev_lib.cpp:1346-1362` |
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
- Type 4: Function — confirmed: historical/transient observations `FindExistPos("Average", 4) = 76` and `FindExistPos("MACD", 4) = 20`

### Type converter (from static analysis of orlib20.dll RVA 0x1D43A0)
- Type 0: {type=0, subtype=0x200, extra=-1}
- Type 1: {type=1, subtype=-1}
- Type 2: {type=2, expanded outputs=[0x0002, 0x0502, 0x0402, 0x0602, 0x0802, 0x0A02, 0x0C02, 0x0E02, 0x1002, 0x1202, 0x1402]} (11 outputs, followed by the `-1` terminator). **0x0502 = ShowMe CONFIRMED** and **0x1402 = TradingApp CONFIRMED**. Other category-2 subtype attributions are not confirmed as CLI component mappings here. Complete converter evidence: `docs/RE/RE_SHOWME_WHTYPE_FINDINGS.md` §2f and §5a.
- Type 3: {type=3, subtype=-1}
- Type 4: {type=4, subtype=-1}

## Resolved questions
- **WHType for Strategies:** WHType=0 (category 0). CONFIRMED — insert/delete/modify/exists all work.
- **WHType for Functions:** WHType=4 (category 4). CONFIRMED — insert/delete/modify/exists all work.
- **WHType for Indicators:** WHType=2 (category 2). CONFIRMED — insert/delete/modify/exists all work.
- **WHType for TradingApp:** WHType=5122 (0x1402 = category 2 | sub-type 0x14). CONFIRMED — Workstream C.
- **WHType for ShowMe:** WHType=1282 (0x0502 = category 2 | sub-type 0x05). CONFIRMED — TVS-C.8. Ghidra RE confirmed via GetFileTypeName1, FUN_1019f510, ConvertToPowerType, FUN_101d4280. See `docs/RE/RE_SHOWME_WHTYPE_FINDINGS.md`.
- **CreateEntryEX WHType vs FindExistPos type:** Same numbering — both use the category (low 8 bits of the expanded whType encoding).

## Open questions
- **3 unconfirmed types:** ActivityBar, PaintBar, ProbabilityMap. These require Ghidra/Frida RE work to determine their WHType values. See `docs/FEASIBILITY.md` §5.2/§5.5. (ShowMe was previously in this list but is now CONFIRMED — see TVS-C.8 above.)
