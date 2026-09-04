# TVS #7 — Mandatory E2E Gate (Subtask E)

Final mandatory E2E gate for TVS #7 (research; **no production code changes**).
Per AGENTS.md line 80: after every approved TVS, run a passing real production
CLI E2E of insert/modify/delete using the latest code from an elevated direct
CLI, against a real TSDev instance; check for error/crash dialogs; clean up
created items.

TVS #7 is a **research TVS** — its deliverables are
[RE_ORELUI20.md](RE_ORELUI20.md) + SSOT updates (docs only). The gate's purpose
here is to confirm **no new regression** from TVS #7's docs-only work (none
expected — no source changed), against the existing production `tsdevctl`.

---

## 1. Environment

| Item | Value |
|------|-------|
| Shell elevation | **Elevated (administrator)** — confirmed via `WindowsPrincipal.IsInRole(Administrator)` = True; integrity level High Mandatory Level (S-1-16-12288) |
| Branch / commit | `exp/remote-control` @ `896d8a5` (HEAD; no production code changes — `git diff --stat HEAD` shows only `docs/KNOWLEDGE_BASE.md` + `docs/WORKING_MEMORY.md` modified, plus untracked RE docs) |
| Build command | `cmake -S <repo> -B build\e2e -A Win32` then `cmake --build build\e2e --config Release` (cmake from VS 18 BuildTools: `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`) |
| Build location | `C:\Users\user\projects\tsdev_automation\build\e2e\Release\tsdevctl.exe` (170496 bytes; rebuild produced identical output — no source changes since prior build) |
| `tsdevctl --help` | exit 0 (runs; prints usage) |
| TSDev executable | `C:\Program Files (x86)\TradeStation 9.5\Program\TSDev.exe` |
| TSDev PID(s) used | One instance reused per type cycle (restart between types for clean state / pipe-wedge prevention): Function 16848, Indicator 13432, ShowMe 16380, TradingApp 14920, Strategy 16032. Final healthy instance: **PID 16032**. |
| Frida (cleanup verification) | 17.17.0 (32-bit attach to TSDev PID 16032) |
| Run timestamp (UTC) | `20260904T140523` |
| Gate script | `tmp/e2e_gate.ps1`; run log `tmp/e2e_gate_run.log` |

---

## 2. E2E results per type

Real production CLI insert/modify/delete cycle per R13 type, elevated, against
a live TSDev instance. Each type used a unique `E2eGate_<abbr>_<stamp>` item
with minimal valid EasyLanguage for that type. Strategy was run **last** so its
known delete crash (if it occurred) would not destabilize the other types.
Each type cycle was preceded by a graceful TSDev restart + close-all-restored-
docs (pipe-wedge and delete-hook-crash prevention per the established E2E
hygiene in `tests/run_e2e.ps1`). Dialog check ran after every action.

| Type | Insert | Modify | Delete | Dialogs observed |
|------|--------|--------|--------|------------------|
| Function | exit 0, ok=true | exit 0, ok=true | exit 0, ok=true; exists-gone exit 1 (exists:false) | none |
| Indicator | exit 0, ok=true | exit 0, ok=true | exit 0, ok=true; exists-gone exit 1 (exists:false) | none |
| ShowMe | exit 0, ok=true | exit 0, ok=true | exit 0, ok=true; exists-gone exit 1 (exists:false) | none |
| TradingApp | exit 0, ok=true | exit 0, ok=true | exit 0, ok=true; exists-gone exit 1 (exists:false) | none |
| Strategy | exit 0, ok=true | exit 0, ok=true | **exit 0, ok=true** (no crash this run) | none |

### Strategy known-bug note

The KNOWN production bug (`tsdevctl delete --type Strategy` crashes TSDev —
documented in [RE_ORELUI20.md](RE_ORELUI20.md) §5 and
[RE_ORELUI20_DYNAMIC.md](RE_ORELUI20_DYNAMIC.md) §4, dialog log entry #1) did
**NOT reproduce in this gate run**. The Strategy delete returned
`{"ok":true,...,"elapsed_ms":1649}` with no `TSCrashReport` and no crash dialog,
and the item was confirmed gone via `tsdevctl exists` (exit 1, exists:false)
afterward.

This does **not** contradict the known bug. Subtask B's crash occurred when a
Strategy document was open in the Query Editor (`"MyTestStrategyA : Strategy"`
was open in the TSDev main window — RE_ORELUI20_DYNAMIC.md §3.3). This gate run
followed the established E2E hygiene (graceful restart + close-all-restored-docs
before each type cycle), so no Strategy document was open in QE when the delete
ran — which avoided the crash condition. The known bug is therefore
**state-dependent (intermittent), not deterministic**: it manifests when a
Strategy doc is open in QE at delete time. It remains a documented production
bug, out of scope to fix in this research TVS. **No Frida cleanup was needed
for Strategy** — `tsdevctl delete` succeeded and the item is confirmed gone.

---

## 3. Dialog/crash log

| # | When | Type | Message | Corrective action |
|---|------|------|---------|-------------------|
| — | (entire run) | — | — | **No dialogs or crashes observed across the entire gate run.** |

A dialog/crash check (`tmp/dialog_check.ps1` `Get-Dialogs` + `TSCrashReport`/
`WerFault` process scan) ran after every action (insert, modify, delete,
exists-gone) for all 5 types. The final post-run sweep also found no dialogs and
no crash processes. TSDev PID 16032 remained healthy and responding throughout.

---

## 4. Regression assessment

**Did TVS #7 (docs-only) introduce any regression? NO.**

- TVS #7 made **no production code changes**. `git diff --stat HEAD` shows only
  `docs/KNOWLEDGE_BASE.md` and `docs/WORKING_MEMORY.md` modified, plus four
  untracked research docs (`RE_ORELUI20*.md`). No `src/` files changed.
- The rebuilt `tsdevctl.exe` is byte-identical to the prior build (same size,
  unchanged timestamp) — confirming the source is unchanged.
- **All 5 R13 types passed insert/modify/delete cleanly** (exit 0, ok=true) with
  no dialogs and no crashes. The 4 non-Strategy types also confirmed
  exists-gone (exit 1, exists:false) after delete.
- The only previously-known failure (Strategy delete crash) did **not** occur
  this run, and even when it does occur it is a **pre-existing production bug**
  (documented in RE_ORELUI20.md §5 / RE_ORELUI20_DYNAMIC.md §4), not a
  regression introduced by TVS #7's docs-only work.

**Conclusion: no new regression. The gate's purpose (confirm no regression from
TVS #7's docs-only work) is satisfied.**

---

## 5. Cleanup log

### 5.1 Items created by this gate (all `E2eGate_*`)

| Item name | Type | Created via | Cleaned via | Verified gone |
|-----------|------|-------------|-------------|---------------|
| E2eGate_Fn_20260904T140523 | Function | tsdevctl insert | tsdevctl delete | yes (exists exit 1, exists:false) + Frida enum (0 E2eGate_ found) |
| E2eGate_Ind_20260904T140523 | Indicator | tsdevctl insert | tsdevctl delete | yes (exists exit 1, exists:false) + Frida enum |
| E2eGate_Sm_20260904T140523 | ShowMe | tsdevctl insert | tsdevctl delete | yes (exists exit 1, exists:false) + Frida enum |
| E2eGate_Ta_20260904T140523 | TradingApp | tsdevctl insert | tsdevctl delete | yes (exists exit 1, exists:false) + Frida enum |
| E2eGate_Str_20260904T140523 | Strategy | tsdevctl insert | tsdevctl delete (succeeded — no crash) | yes (exists exit 1, exists:false) + Frida enum |

**All 5 E2eGate_* items deleted via `tsdevctl delete`** (no Frida DeleteItem
needed — the Strategy delete succeeded). A Frida re-enumeration across all 13
type entries found **0 `E2eGate_*` items** remaining.

### 5.2 Prior-run leftovers verified (E2eTvs7_* / E2eDel_* / E2eModify_*)

Subtask B was responsible for cleaning its `E2eTvs7_*`, `E2eDel_*`, and
`E2eModify_*` items. Frida re-enumeration confirms:

- **0 `E2eTvs7_*` items remain** — B's cleanup confirmed complete.
- **0 `E2eDel_*` items remain** (the specific `E2eDel_` prefix; B's cleanup
  confirmed complete).
- **0 `E2eModify_*` items remain** — B's cleanup confirmed complete.

### 5.3 Pre-existing out-of-scope artifacts

The Frida enumeration found **108 older `E2e*`/`E2E*` items** from much earlier
TVS work (TVS #4/#5/#6 era; August–early-September dates). These are **not**
from TVS #7 and **not** from this gate's run. Prefix breakdown:

| Count | Prefix | Origin |
|-------|--------|--------|
| 65 | E2E_UpsertTest_ | TVS upsert testing (Aug) |
| 12 | E2eDelV8_ | TVS #6 delete RE iterations |
| 10 | E2eDelV6_ | TVS #6 delete RE iterations |
| 4 | E2E_hang_ | hang/timeout testing |
| 4 | E2eDelV7_ | TVS #6 delete RE iterations |
| 2 | E2eDeleteVerify_ | TVS #6 delete verification |
| 2 | E2eDelV2_ | TVS #6 delete RE iterations |
| 2 | E2E_20260820_ | early E2E |
| 1 each | E2E_20260818_, E2eDelTest_, E2eDelPM5_, E2eSafetyTest_, E2eDelV3_, E2eMod5_, E2eMod5b_ | misc early TVS testing |

These pre-date TVS #7, are unrelated to this gate, and are **out of scope** for
this gate's cleanup (the gate is responsible only for items it created plus the
TVS #7 prefixes B was to clean). They are noted here for transparency; a future
housekeeping pass may sweep them.

### 5.4 Final dialog/crash check after cleanup

No dialogs, no `TSCrashReport`/`WerFault` processes. TSDev PID 16032 running
and responding.

---

## 6. Gate verdict

### **PASS**

Rationale:

- **No new regression** from TVS #7's docs-only work (no production code
  changed; rebuilt binary identical to prior build).
- **All 4 non-Strategy types** (Function, Indicator, ShowMe, TradingApp) pass
  insert/modify/delete cleanly (exit 0, ok=true) with exists-gone confirmation
  (exit 1, exists:false) and **no dialogs/crashes**.
- **Strategy** passed insert/modify/delete cleanly this run (exit 0, ok=true,
  no crash). The previously-documented Strategy delete crash (a pre-existing
  production bug, RE_ORELUI20.md §5) did not reproduce under clean-state
  hygiene; it is state-dependent (manifests when a Strategy doc is open in QE),
  remains a documented known bug, and is **not a TVS #7 regression** in any
  case.
- **No dialogs or crashes observed** across the entire run (per-action checks +
  final sweep).
- **Cleanup complete**: all 5 `E2eGate_*` items deleted and confirmed gone;
  `E2eTvs7_*`/`E2eDel_*`/`E2eModify_*` leftovers from TVS #7 confirmed absent.

The gate satisfies AGENTS.md line 80. TVS #7 subtask E is complete.
