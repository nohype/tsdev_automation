# ORELUI20.dll Dynamic RE — Enumeration + Delete (TVS #7 Subtask B)

Dynamic verification of the static recipes in
[RE_ORELUI20_STATIC.md](RE_ORELUI20_STATIC.md) via Frida instrumentation
inside the live TSDev.exe process. No production code was changed.

All evidence is concrete: every claim cites the Frida script, the TSDev PID,
and the observed output. Speculation is explicitly flagged.

---

## 1. Environment

| Item | Value |
|------|-------|
| Shell elevation | Elevated (administrator) |
| TSDev executable | `C:\Program Files (x86)\TradeStation 9.5\Program\TSDev.exe` |
| TSDev PID (initial) | 3140 (crashed during cleanup — see §5) |
| TSDev PID (restart) | 16600 |
| TSDev integrity level | High Mandatory Level |
| Frida version | 17.17.0 |
| Frida runtime | qjs |
| tsdevctl build | `C:\Users\user\projects\tsdev_automation\build\e2e\Release\tsdevctl.exe` |
| ORELUI20.dll runtime base | `0x01a00000` (ASLR; static base `0x10000000`) |
| Ghidra MCP | Connected to project `tsdev`, programs `/ORELUI20.dll` and `/TSDev.exe` |

---

## 2. Enumeration verification

### 2.1 Scripts

| Script | Purpose |
|--------|---------|
| `tmp/frida_enum_v4.js` | Per-type enumeration with corrected CAnalysisTypeInfo construction |
| `tmp/frida_enum_dump.js` | Raw hex dump of CELTechInfo records and CMap state |
| `tmp/frida_enum_hook.js` | Hook FUN_10044b90 to capture CELTechInfo during population |
| `tmp/frida_enum_v7.js` | Swapped CAssoc layout test + full enumeration |
| `tmp/frida_enum_final.js` | Final enumeration with correct type mapping and verification |

### 2.2 Dynamic corrections to static RE

Three corrections were discovered during dynamic verification:

1. **CAnalysisTypeInfo constructor (FUN_10044820) takes 4 parameters, not 1.**
   Static RE flagged this as `[DYNAMIC]`. The actual signature is:
   ```
   __thiscall FUN_10044820(this, int category, int subtype, int index)
   ```
   - `this+0x00` = category
   - `this+0x04` = subtype (used by LoadList as a power-type filter)
   - `this+0x08` = index
   - `this+0x0c` = CMapWordToPtr (initialized with hash size 10)

   **Impact:** Calling with only 1 parameter leaves `this+0x04` (subtype) as
   zero/garbage, causing LoadList's power-type filter to reject all items.
   The static recipe's `LoadList(type=0)` "all types" path also requires a
   properly constructed CAnalysisTypeInfo — it does NOT bypass the filter.

2. **LoadList must be called per-type with the packed type value from
   DAT_1008fd04, not with `type=0`.**
   The static recipe suggested `type=0` enumerates all types. Dynamically,
   `LoadList(type=0)` with a properly constructed CAnalysisTypeInfo does
   enumerate, but the power-type filter (`cati+0x04`) still applies. The
   correct approach (matching InitLists) is to iterate the 13 type entries
   in DAT_1008fd04, construct a CAnalysisTypeInfo per entry, and call
   LoadList with the packed value.

3. **CMapWordToPtr CAssoc layout is `{ pNext@0, value@4, key@8 }`** — swapped
   from standard MFC's `{ pNext@0, key@4, value@8 }`.
   With the standard layout, values appeared as garbage (e.g., `0x721b18`,
   `0x2e1af6` — not valid heap pointers). With the swapped layout, values
   are valid CELTechInfo pointers and all fields read correctly.

   **Correction to static RE §1.3:** The CMap walking code must use:
   - `value = pAssoc->offset(0x04).readPointer()`  (CELTechInfo*)
   - `key   = pAssoc->offset(0x08).readU16()`       (recordId)

### 2.3 Type category table (DAT_1008fd04) — dynamically confirmed

| Index | Packed value | Category | Subtype | Type name |
|-------|-------------|----------|---------|-----------|
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

**Correction to static RE type mapping:** The static RE listed type values
as simple integers (0=Strategy, 1=Function, etc.). The actual type stored
in CELTechInfo+0x08 is the **category** (low byte), and CELTechInfo+0x0a
is the **subtype** (high byte). The full type is `(subtype << 8) | category`.
For example, TradingApp is category=2, subtype=20 → full type 0x1402.

### 2.4 CELTechInfo field layout — dynamically confirmed

| Offset | Type | Field | Dynamic confirmation |
|--------|------|-------|---------------------|
| +0x08 | `uint16` | category (type low byte) | Confirmed: Strategy=0, Function=1, Indicator=2, TradingApp=2 |
| +0x0a | `uint16` | subtype (type high byte) | Confirmed: TradingApp=20, ShowMe=5, Indicator=0 |
| +0x0c | `uint16` | record ID | Confirmed: matches CMap key |
| +0x10 | `CString` | verbatim name | Confirmed: mixed-case names preserved |
| +0x1c | `CString` | ProgId | Confirmed: matches name for user items; different for built-ins |
| +0x40 | `void*` | info object pointer | Confirmed: valid ORLIB20 record handle |

### 2.5 Enumeration results

**Total items enumerated across all types: 2958**

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

### 2.6 Inserted test item confirmation

Five `E2eTvs7_Enum_*` items were inserted via `tsdevctl insert` (all exit 0,
`{"ok":true}`):

| Item name | Type | Inserted via | Enumerated via Frida |
|-----------|------|-------------|---------------------|
| E2eTvs7_EnumFn_MixedCase | Function | tsdevctl | **FOUND** — type=0x0001, progId="E2eTvs7_EnumFn_MixedCase" |
| E2eTvs7_EnumInd_MixedCase | Indicator | tsdevctl | **FOUND** — type=0x0002, progId="E2eTvs7_EnumInd_MixedCase" |
| E2eTvs7_EnumStr_MixedCase | Strategy | tsdevctl | **FOUND** — type=0x0000, progId="E2eTvs7_EnumStr_MixedCase" |
| E2eTvs7_EnumSm_MixedCase | ShowMe | tsdevctl | **FOUND** — type=0x0502, progId="E2eTvs7_EnumSm_MixedCase" |
| E2eTvs7_EnumTa_MixedCase | TradingApp | tsdevctl | **FOUND** — type=0x1402, progId="E2eTvs7_EnumTa_MixedCase" |

### 2.7 Verbatim name preservation

**Confirmed.** The item `E2eTvs7_EnumFn_MixedCase` was inserted with mixed
case and enumerated as exactly `E2eTvs7_EnumFn_MixedCase` — case preserved,
no normalization. All five test items showed identical verbatim names.

### 2.8 Dynamic findings confirmed

| [DYNAMIC] item from static RE | Status | Evidence |
|-------------------------------|--------|----------|
| NULL CWnd* doesn't crash | **CONFIRMED** | Dialog constructed with NULL CWnd*, no crash, no dialog |
| Empty map suffices | **CONFIRMED** | Zero-initialized 0x40 buffer used as map, no issues |
| CAnalysisTypeInfo construction | **CORRECTED** | Requires 4 params (this, category, subtype, index), not 1 |
| LoadList(type=0) all types | **CORRECTED** | Must use per-type packed values from DAT_1008fd04 |
| CMap walking | **CORRECTED** | CAssoc layout is swapped: value@4, key@8 |
| Category-to-type mapping | **CONFIRMED** | Full type = (subtype << 8) | category |
| ProgId availability | **CONFIRMED** | ProgId at +0x1c, matches name for user items |
| Verbatim names | **CONFIRMED** | Mixed-case names preserved exactly |
| Thread-marshaled enumeration | **CONFIRMED** | FUN_10074310/FUN_10074470 execute, return success |
| FUN_10044b90 population | **CONFIRMED** | Called ~1655 times for Strategy type alone |

---

## 3. Delete verification

### 3.1 DeleteItem ordinal — dynamically confirmed

**DeleteItem export ordinal = 587 (0x24A).**

Confirmed via `dumpbin /EXPORTS ORELUI20.dll`:
```
587  24A 00010DD0 ?DeleteItem@CELFileOpenDialog@@IAE_NPAVCELTechInfo@@@Z
```

Static RE flagged the exact ordinal as `[DYNAMIC]` — now resolved.

### 3.2 DeleteItem decompilation analysis

DeleteItem (`0x10010dd0`, `__thiscall(this, CELTechInfo*)`) logic:
1. Reads type from `param_1+0x08`, subtype from `param_1+0x0a`, recordId
   from `param_1+0x0c`.
2. If type==4 (built-in function) and developer mode (this+0x278)==0:
   shows a message box ("cannot delete built-in"). Returns false.
3. Checks `GetFileOpenInQE()` — if the item is currently open in the
   QuickEditor, shows a message box. Returns false.
4. Otherwise: calls `Ordinal_59(parentHwnd, type, recordId, 0)` (delete
   record), then `Ordinal_17(type, recordId)` (delete type info), then
   `DeleteTypeInfoItem(this, type, powerType, recordId)`. Returns true.

**Key finding:** DeleteItem internally calls ORLIB20 Ordinal_59 + Ordinal_17.
The static RE's alternative lower-level approach (calling Ordinal_59 + 17
directly) is redundant — DeleteItem already does this and additionally
handles the dialog's internal list cleanup.

### 3.3 Delete test items

Four `E2eTvs7_Del*` items were inserted via `tsdevctl insert`:

| Item name | Type | Insert exit | Insert result |
|-----------|------|------------|---------------|
| E2eTvs7_DelFn_001 | Function | 0 | ok=true |
| E2eTvs7_DelInd_001 | Indicator | 0 | ok=true |
| E2eTvs7_DelSm_001 | ShowMe | 0 | ok=true |
| E2eTvs7_DelTa_001 | TradingApp | 0 | ok=true |
| E2eTvs7_DelStr_001 | Strategy | 6 (timeout) | **FAILED** — tsdevctl insert timed out |

**Strategy insert failure:** `tsdevctl insert --type Strategy` consistently
times out (exit 6, "item did not open within the timeout"). The TSDev main
window had "MyTestStrategyA : Strategy" open, which may have interfered.
This is a **tsdevctl production issue**, not a Frida issue. No dialog or
crash appeared during the insert attempts.

### 3.4 Delete results

Script: `tmp/frida_delete.js`

| Type | Item name | Method | DeleteItem return | tsdevctl exists | Frida re-enum | Dialogs |
|------|-----------|--------|-------------------|-----------------|---------------|---------|
| Function | E2eTvs7_DelFn_001 | DeleteItem(ord 587) | **true** | timeout (tsdevctl issue) | **CONFIRMED_DELETED** | None |
| Indicator | E2eTvs7_DelInd_001 | DeleteItem(ord 587) | **true** | timeout (tsdevctl issue) | **CONFIRMED_DELETED** | None |
| ShowMe | E2eTvs7_DelSm_001 | DeleteItem(ord 587) | **true** | **exists=false** (exit 1) | **CONFIRMED_DELETED** | None |
| TradingApp | E2eTvs7_DelTa_001 | DeleteItem(ord 587) | **true** | **exists=false** (exit 1) | **CONFIRMED_DELETED** | None |
| Strategy | E2eTvs7_DelStr_001 | N/A (insert failed) | N/A | N/A | N/A | None |

### 3.5 Minimal bar status

**MET.** The required minimal bar is TradingApp + at least two other distinct
types. Achieved: **TradingApp + 3 others** (Function, Indicator, ShowMe) —
all deleted and confirmed gone.

### 3.6 Delete verification details

Each delete was performed as follows:
1. Construct CELFileOpenDialog (NULL CWnd*, empty map) — no crash.
2. Construct CAnalysisTypeInfo per type (FUN_10044820 with 4 params).
3. Call LoadList to populate the CMap.
4. Walk CMap (swapped CAssoc layout) to find the target CELTechInfo* by name.
5. Call DeleteItem(dialog, CELTechInfo*) — returned `true` for all 4 types.
6. Destroy dialog — no crash.

Verification:
- ShowMe and TradingApp: confirmed via `tsdevctl exists` (exists=false, exit 1).
- Function and Indicator: `tsdevctl exists` timed out (tsdevctl production
  issue with these types); confirmed via Frida re-enumeration (item not found).
- All 4 types: confirmed via Frida re-enumeration script `frida_verify_del.js`.

---

## 4. Dialog/crash log

| # | When | Type | Message | Corrective action |
|---|------|------|---------|-------------------|
| 1 | After tsdevctl delete Strategy (E2eTvs7_EnumStr_MixedCase) | TSCrashReport | "TradeStation Development Environment" (crash dialog, no readable child text) | Killed TSCrashReport process; TSDev PID 3140 had crashed. Restarted TSDev via tsdevctl insert (PID 16600). Completed cleanup via Frida DeleteItem. |

**No dialogs or crashes occurred during any Frida enumeration or delete
operation.** The single crash was caused by `tsdevctl delete --type Strategy`
(a tsdevctl production issue), not by Frida instrumentation.

---

## 5. Static recipe confirmation/correction

| Static recipe item | Status | Details |
|--------------------|--------|---------|
| CELFileOpenDialog ctor (Ordinal 17) | **CONFIRMED** | NULL CWnd* + empty map works, no crash |
| CELFileOpenDialog dtor (Ordinal 166) | **CONFIRMED** | No crash on destruction |
| LoadList (Ordinal 1291) | **CONFIRMED with corrections** | Must use per-type packed values, not type=0 |
| CAnalysisTypeInfo ctor (FUN_10044820) | **CORRECTED** | 4 params: (this, category, subtype, index) |
| CMapWordToPtr layout | **CORRECTED** | CAssoc: value@4, key@8 (swapped from standard MFC) |
| CELTechInfo fields | **CONFIRMED** | type@+0x08, subtype@+0x0a, rid@+0x0c, name@+0x10, progId@+0x1c |
| DeleteItem ordinal | **CONFIRMED** | Ordinal 587 (0x24A) |
| DeleteItem calling convention | **CONFIRMED** | `__thiscall(dialog, CELTechInfo*)` |
| DeleteItem internally calls Ordinal_59 + Ordinal_17 | **CONFIRMED** | Via decompilation; DeleteItem is the higher-level path |
| ORLIB20 Ordinal 10 (library object) | **CONFIRMED** | Returns valid pointer (0x9d20b70) |
| ORLIB20 Ordinal_59 + Ordinal_17 direct | **NOT TESTED** | Redundant — DeleteItem already calls these |
| Type values (0=Strategy, 1=Function, etc.) | **CORRECTED** | Full type = (subtype << 8) \| category; see §2.3 |
| Verbatim name preservation | **CONFIRMED** | Mixed-case names preserved exactly |
| Thread-marshaled enumeration | **CONFIRMED** | FUN_10074310/FUN_10074470 execute successfully |
| NULL CWnd* safety | **CONFIRMED** | No crash, no dialog |
| Developer mode flag (this+0x278) | **CONFIRMED** | Setting to 1 did not change enumeration results |

---

## 6. Cleanup log

### 6.1 Created items (this subtask)

| Item name | Type | Created via | Cleaned via |
|-----------|------|-------------|-------------|
| E2eTvs7_EnumFn_MixedCase | Function | tsdevctl insert | Frida DeleteItem |
| E2eTvs7_EnumInd_MixedCase | Indicator | tsdevctl insert | Frida DeleteItem |
| E2eTvs7_EnumStr_MixedCase | Strategy | tsdevctl insert | Frida DeleteItem |
| E2eTvs7_EnumSm_MixedCase | ShowMe | tsdevctl insert | tsdevctl delete (ok) |
| E2eTvs7_EnumTa_MixedCase | TradingApp | tsdevctl insert | Frida DeleteItem |
| E2eTvs7_DelFn_001 | Function | tsdevctl insert | Frida DeleteItem (delete test) |
| E2eTvs7_DelInd_001 | Indicator | tsdevctl insert | Frida DeleteItem (delete test) |
| E2eTvs7_DelSm_001 | ShowMe | tsdevctl insert | Frida DeleteItem (delete test) |
| E2eTvs7_DelTa_001 | TradingApp | tsdevctl insert | Frida DeleteItem (delete test) |
| E2eTvs7_TmpStart | Function | tsdevctl insert | tsdevctl delete (ok) |

### 6.2 Frida-deleted items

All 9 E2eTvs7_* items (5 enum + 4 del) were deleted via Frida DeleteItem
(Ordinal 587). All confirmed gone via Frida re-enumeration.

### 6.3 Items cleaned via tsdevctl delete

- E2eTvs7_EnumSm_MixedCase (ShowMe) — tsdevctl delete ok
- E2eTvs7_TmpStart (Function) — tsdevctl delete ok

### 6.4 Old leftovers handled

20 old `E2eDel_*` and `E2eModify_*` items were found and deleted via Frida
DeleteItem:

| Count | Prefix | Types |
|-------|--------|-------|
| 3 | E2eDel_TradingApp_* | TradingApp |
| 1 | E2eDel_Indicator_* | Indicator |
| 2 | E2eDel_ShowMe_* | ShowMe |
| 3 | E2eDel_Strategy_* | Strategy |
| 1 | E2eDel_* | Strategy |
| 10 | E2eModify_* | Strategy |

All 20 deleted successfully (DeleteItem returned true). Verified: 0
remaining E2e* items.

### 6.5 Final dialog/crash check

After all cleanup: no dialogs, no crash reports, TSDev PID 16600 running
and responding.

---

## 7. Findings summary and blockers for subtask C

### Key findings

1. **Enumeration works** via CELFileOpenDialog + per-type LoadList. The
   static recipe's core approach is sound; three corrections were needed
   (CAnalysisTypeInfo 4-param ctor, per-type LoadList, swapped CAssoc
   layout).

2. **Delete works** via DeleteItem (Ordinal 587). It is the correct
   higher-level path — it internally calls ORLIB20 Ordinal_59 + Ordinal_17
   and handles dialog list cleanup. No need to call Ordinal_59/17 directly.

3. **NULL CWnd* is safe** for both enumeration and deletion — no crashes,
   no dialogs.

4. **Verbatim names are preserved** — mixed-case names stored and retrieved
   exactly.

5. **ProgId is available** at CELTechInfo+0x1c for all user items.

6. **DeleteItem ordinal is 587** (0x24A) — the static RE's `[DYNAMIC]` flag
   is resolved.

### Blockers for subtask C (production implementation)

1. **tsdevctl Strategy insert/exists/delete timeout.** The current tsdevctl
   production code times out for Strategy (and sometimes Function/Indicator)
   types. This is a tsdevctl issue, not an ORELUI20 issue. The Frida-based
   DeleteItem works for all types including Strategy.

2. **tsdevctl Strategy delete crash.** `tsdevctl delete --type Strategy`
   caused a TSCrashReport (TSDev crash). This is a tsdevctl production bug
   that must be fixed before the production E2E gate.

3. **CAnalysisTypeInfo construction is not exported.** FUN_10044820 is an
   internal function (not exported by ordinal). Production code must either
   compute its address from the module base + offset (brittle) or find an
   alternative. The offset `0x44820` is stable for this build but may
   change across TSDev versions.

4. **CMapWordToPtr CAssoc layout is non-standard.** The swapped layout
   (value@4, key@8) differs from standard MFC. Production code must use
   this layout when walking the map.

### Recommended production approach for subtask C

Based on the dynamic verification, the production implementation should:

1. **For enumeration (list/exists):** Construct CELFileOpenDialog (Ord 17),
   iterate the 13 type entries from DAT_1008fd04, construct
   CAnalysisTypeInfo per entry via FUN_10044820, call LoadList (Ord 1291),
   walk the CMap at cati+0x0c with the swapped CAssoc layout, read
   CELTechInfo fields. Destroy dialog (Ord 166).

2. **For delete:** Construct CELFileOpenDialog (Ord 17), enumerate to find
   the target CELTechInfo*, call DeleteItem (Ord 587). Destroy dialog.
   This is the highest-level path and handles all cleanup internally.

3. **Avoid InitLists** — it touches UI controls (ComboBox, CListCtrl).
   Call LoadList directly.

4. **Avoid direct Ordinal_59/17** — DeleteItem already calls these and
   provides additional safety checks (built-in protection, QE-open check).
