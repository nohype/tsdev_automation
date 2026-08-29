# Working Memory — TSDev Automation

Live, real-time state for the PM. Keep minimal; drop stale/superseded facts.
Do not restate what the code itself documents.

## Current objective

**TS Desktop interaction acceptance.** The six operations (Insert/Delete/Modify/
Exists/Get-source/Upsert) are implemented in C++ CLI + Python `TsDevClient` with
batch (manifest + stdin) support. The user has clarified that production
acceptance REQUIRES proper interaction with a running TS Desktop, matching
TSDev.exe:
- (a) all operations work WHILE TS Desktop is open
- (b) inserted items immediately available, usable, and FUNCTION when applied
- (c) modifications immediately picked up and applied to items already on
  charts, or already opened within TS Desktop as a tradingapp
- (d) deletions handled like TSDev: TS Desktop detects applied items, removes
  them, shows a "deleted" warning

The `dependents` workstream (M) is paused; it is beyond this acceptance target.

## Current state (not derivable from code)

- TradeStation 9.5 installed: `C:\Program Files (x86)\TradeStation 9.5\Program\
  TSDev.exe` and `orlib20.dll` (v9.50.01.3344). TS Desktop process is
  `orchart.exe` (imports `_OrlibRegisterNotificationHandle@4`).
- `tsdev_cli.exe` built (32-bit Win32 Release) at `build\Release\tsdev_cli.exe`
  and `dist\tsdev_cli.exe`. NOTE: `cmake --build` exit code is non-zero due to a
  POST_BUILD packaging step missing `docs/BUNDLE_README.md`; the exe itself
  builds and runs fine. Repo-content issue, not toolchain.
- TS Desktop cross-process notification mechanism: see KB §19 (SSOT). Summary:
  the CLI's per-process `NotifyLibChanged` 0x40A reaches no windows and is NOT
  the cross-process path; the real mechanism is shared warehouse memory
  (SHARE_WO) + WHServer.exe COM `_IWHouseEvents`, already exercised by orlib20's
  COM vtable calls during CLI operations.

## Read-only function cleanup

- Two functions from an earlier test workstream were read-only in TSDev and
  could not be deleted through the TSDev UI:
  - `TS_AUTO_M4_FRIDA_FUNC` (warehouse category 4 / Function, ProgId 1439)
  - `TS_AUTO_M4_TRAV_FUNC` (warehouse category 4 / Function, ProgId 1441)
- The read-only appearance is consistent with an application/warehouse metadata
  classification (type/property). Whether the files also carried an NTFS
  `FILE_ATTRIBUTE_READONLY` bit is not known — no pre-delete `GetFileAttributes`
  dump exists, so the on-disk attribute is not asserted either way.
- Deleted cleanly via the production `TsDevLib::deleteEntry` path
  (`FUN_101a8360` delete orchestrator → `DeleteFromTree` → warehouse close →
  `.ell`+`.xml` file removal):
  - `deleteEntry` returned true, which per `src/tsdev_lib.cpp` includes removal of
    `elmodule_4_1439.ell` / `elmodule_4_1439.xml`
  - `deleteEntry` returned true, which per `src/tsdev_lib.cpp` includes removal of
    `elmodule_4_1441.ell` / `elmodule_4_1441.xml`
- Post-delete `tsdev_cli exists` for both returned `exists=false`, `progId=0`.
- Production E2E passed:
  `tsdev_cli --json insert/modify/delete function TS_AUTO_PM_E2E_FUNC` →
  ProgId 1594, modify success, delete success, post-delete `exists=false`,
  no `TSCrashReport.exe`.
- `diag_m4_raw_rec` inserted a fresh function `TS_AUTO_M4_DIAG_RFUNC`
  (ProgId 1593) and dumped its 0x10A-byte master record; key bytes were
  `+0x7B` USHORT = `0x0404` and `+0x109` byte = `0x18`.
- Strongest defensible conclusion: the two deleted functions carried a read-only
  type/property classification in their warehouse record; the precise byte-level
  flag is inferred from orlib20 string/type-mapping evidence (see KB §20), not
  directly observed.
- Evidence limitation: the exact pre-delete master-record/XML byte(s) for the
  two deleted functions were not captured, so the precise byte-level flag that
  produced the read-only display is inferred from orlib20 string/type-mapping
  and property-string evidence, not directly observed.
- No production code change was required.

## TVS just completed (2026-08-29): modify TS Desktop interaction probe (objective c)

User-supervised probe (user applied the indicator to a chart and observed; no
restart, no manual refresh). Indicator `TsDeskModProbe_20260829`.

**Result:** For an indicator, CLI modify ALREADY satisfies TS Desktop
interaction (objective c) with NO code change — the shared warehouse memory +
WHServer COM path (KB §19) is sufficient. Two modifies while the indicator was
applied to a live chart, both picked up immediately with NO manual reload:
1. `Plot1(Close,"ProbeV1")` -> `Plot1(200,"ProbeV2")`: Close line became a flat
   line at y=200, label ProbeV2.
2. `Plot1(200,"ProbeV2")` -> `Plot1(100,"ProbeV3")`: flat line moved to y=100,
   label ProbeV3.
CLI delete afterward was auto-detected; TS Desktop auto-removed the indicator
from the chart (objective d re-confirmed). No crashes at any step.

All four TS Desktop interaction sub-objectives (a/b/c/d) are now verified for
the indicator type with NO code change.

Post-TVS E2E gate (insert→modify→get-source→delete, elevated direct CLI,
indicator `E2eGateDeskMod_*`): PASSED, all EXIT=0, get-source after modify
returned the modified source, no crashes. Log: `tmp/e2e_gate_post_tvs_modify.log`.
Supervised probe log: `tmp/tsdesk_modify_probe.log`. Probe pattern (phased
script + v1/v2 sources) in `tmp/probe_modify_desk.ps1`, `tmp/probe_modify_v1.el`,
`tmp/probe_modify_v2.el`.

**Still unverified (future TVSs):** type coverage (strategy/tradingapp/showme/
function) for TS Desktop interaction; items with dependents; encrypted items;
batch operations; the tradingapp-opened variant of modify (a running tradingapp
reflects a CLI modify). Note: delete-side 0x40A is skipped for functions
(category == 4) — function-delete TS Desktop interaction is unverified.

## Next step (one TVS ahead, for the next team)

**ShowMe TS Desktop interaction probe (second type — generalization check).**
Reuse the established probe pattern (insert -> apply to chart -> modify ->
observe chart update without manual reload -> delete) for a ShowMe (plottable,
like indicator). Expected: NO code change needed — confirms the shared
warehouse + WHServer COM mechanism (KB §19) generalizes beyond indicator to
another plottable type. User-supervised TS Desktop required. Reuse
`tmp/probe_modify_desk.ps1` (phased) with ShowMe-typed CLI args and fresh
source files.

## Key references

- CLI insert/modify/delete impl: `src/tsdev_cli.cpp` (`doInsert`, `doModify`,
  `doDelete`).
- NotifyLibChanged call sites: `src/tsdev_lib.cpp` (`verifyModule` success,
  `deleteEntry` success — category != 4 guard).
- NotifyLibChanged resolver: `src/orlib_resolver.h` / `src/orlib_resolver.cpp`
  (ordinal 57).
- Warehouse offsets + dispatch fns: `docs/KNOWLEDGE_BASE.md` §3, §5, §6, §7,
  §10, §17, §18, §19.
- Probe log: `tmp/tsdesk_probe.log`. Post-TVS E2E gate:
  `tmp/e2e_gate_post_tvs_desk.log`.
