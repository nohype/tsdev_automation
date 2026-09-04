# TVS #8 — Mandatory E2E Gate

Final mandatory acceptance gate for TVS #8 (`tsdevctl list` — ORELUI20.dll
in-process enumeration; implementation + approved fix round). Per AGENTS.md
line 80: after every approved TVS, run a passing real production CLI E2E using
the latest code from an elevated direct CLI, against a real TSDev instance;
check for error/crash dialogs; clean up created items.

Gate requirements (from WORKING_MEMORY.md "TVS #8" → "Acceptance gate"):

- **A** — `tests/run_unit_tests.ps1` passes from the latest code.
- **B1** — the list scenarios in `tests/run_e2e.ps1` actually execute.
- **B2** — the standard insert/modify/delete regression across ALL 5 R13 types
  (Function, Indicator, ShowMe, Strategy, TradingApp): insert fresh uniquely
  named `E2eGate_*` items from valid EasyLanguage source files, verify exists,
  modify (re-verify), getsrc round-trip, delete, verify gone — via the
  production CLI exactly as users would.
- **B3** — zero error/crash dialogs; check after EVERY CLI invocation.
- **B4** — cleanup: every created item deleted and confirmed gone; no
  `E2eGate_*` residue.

---

## 1. Environment

| Item | Value |
|------|-------|
| Shell elevation | **Elevated (administrator)** — `whoami /groups` shows Mandatory Label\High Mandatory Level (S-1-16-12288); `net session` succeeds |
| Branch / commit | `exp/remote-control` @ `321ff8d` (HEAD; working tree clean before and after the gate — `git status --porcelain` empty; no source files modified) |
| Build command | `cmake -S <repo> -B build\e2e_gate -A Win32` then `cmake --build build\e2e_gate --config Release` (cmake resolved via vswhere, VS 18 BuildTools, as in the test scripts) |
| Build location | `build\e2e_gate\` — **FRESH build directory** per the rebuild-while-loaded hazard mitigation: on-disk DLL paths are new, so injection is safe even with a lingering TSDev instance holding the old `build\e2e` DLLs. Artifacts: `tsdevctl.exe` (194560 B), `delete_hook.dll` (10240 B), `enum_hook.dll` (10240 B), built 17:48:30–17:48:44 local, immediately before the E2E runs |
| Unit-test build | `build\unit\` (`cmake -A Win32`, Release; all targets under /W4 /WX) |
| `tsdevctl` used | `build\e2e_gate\Release\tsdevctl.exe` (the production CLI, driven exactly as users would — direct invocations from the elevated shell) |
| TSDev executable | `C:\Program Files (x86)\TradeStation 9.5\Program\TSDev.exe` |
| TSDev instances | Pre-existing **hidden** instance pid 7904 (tool-launched by the earlier session) — attached per R5/R6, later recycled by the E2E script's own graceful-restart hygiene. All subsequent instances were tool-launched during the gate and closed gracefully (WM_CLOSE) at the end. Final state: **no TSDev running** |
| Run timestamps (UTC) | B1: 15:48:59–15:54:00 (~5 min 15 s); B2: 15:59:41–16:03:24 (~3 min 43 s); cleanup sweep + final close: ~16:04–16:08 |
| Gate scripts | `tmp/e2e_gate/run_e2e_gate.ps1` (gate copy of `tests/run_e2e.ps1` — `tests/` untouched) + `tmp/e2e_gate/b2_regression.ps1`; logs `tmp/e2e_gate/b1_run.log`, `tmp/e2e_gate/b2_run.log` |

### Gate copy adaptations (documented; `tests/run_e2e.ps1` unmodified)

1. Builds into `build\e2e_gate` (fresh build dir — hazard mitigation #1).
2. Tolerates a pre-existing **hidden** TSDev (attach per R5/R6); only a
   **visible** window (user instance) aborts. Never closes/disturbs a
   pre-existing instance directly.
3. `Invoke-Tsdevctl` additionally checks `TSCrashReport` after **every** CLI
   invocation (strengthens B3; the original checks after key steps only).

---

## 2. Requirement A — unit tests

Command: `powershell -File tests/run_unit_tests.ps1` (configures + builds
`build\unit` Win32 Release, then runs `test_tsdevctl_units.exe`).

- Result: **`all unit tests passed`** — exit 0.
- Coverage: 143 `check(...)` assertions in `tests/test_tsdevctl_units.cpp`
  (per-verb arg validation for `list`, usage-text coverage, record
  marshalling → JSON formatting → type-filter logic on canned records,
  type-table mapping — no TSDev required).
- Build artifacts: `test_tsdevctl_units.exe` (128000 B), `tsdevctl.exe`
  (194560 B — same size as the E2E build; same sources).

**A: PASS.**

---

## 3. Requirement B1 — list scenarios (run_e2e gate copy)

The full `run_e2e.ps1` suite ran (open / exists / getsrc / **list** / modify /
delete sections). The list scenarios (the TVS #8 subject) all executed and
passed; the surrounding sections confirm no regression from TVS #8's changes.

| Scenario | Command | Exit | Key result | Dialog check |
|----------|---------|------|------------|--------------|
| list, all items (JSON) | `tsdevctl list --json` | 0 | `ok:true, op:list, type:null, count:2507`; items array; count == items length; tsdev_pid present; **all 5 R13 types represented**; known item `MyTestStrategyA` found exactly once with type `Strategy` and a ProgId; every item has non-empty name + type; no document residue | clean |
| list, per-type filter | `tsdevctl list --type Function --json` | 0 | `ok:true, type:Function, count>0`; **only Function items** returned | clean |
| list, human output | `tsdevctl list` | 0 | 2507 item lines + summary line matching `list: <N> item(s) (pid <pid>, <ms> ms)`; item lines match `<Type>  <name>  (ProgId: <progid>)` | clean |
| list, `--name` rejected | `tsdevctl list --name X --json` | 2 | Usage rejection (`--name is not valid with list`) | clean |
| list, unsupported type | `tsdevctl list --type PaintBar --json` | 2 | Usage rejection (`unsupported type 'PaintBar'`) | clean |

Non-list sections (regression confirmation, all exit codes as asserted by the
suite): open (exit 0, attached to hidden pid 7904, window stayed hidden),
exists real/wrong-case/phantom×5 (0/0/1×5), getsrc JSON/raw/`--output`/
phantom×5 (0/0/0/1×5, round-trip source non-empty, no phantom error page),
modify section (insert 9193 ms → exit 0; modify 7832 ms → exit 0; getsrc
round-trip contains the modify marker; modify nonexistent → exit 1
`not_found`; modify garbage → exit 12 `compile_error`, verbatim message:
`"This word cannot start a statement. [item 'E2eModify_20260904T154859' was
saved with uncompiled new source]"` — the CLI's own documented-state reporting
for the compile-failure path, asserted by the suite since TVS #5),
delete section (all 5 R13 types insert→exists→delete→exists-gone, exit
0/0/0/1 respectively; delete nonexistent → exit 1 `not_found`). Script
verdict: **`E2E passed`** (33 `ok:` assertion lines, 0 `FAIL:` lines).

Wrapper exit-code note: the background-run harness reported exit 1 for the B1
invocation despite the script's `E2E passed` (its exit-0 branch). A controlled
probe proved the harness reports 1 for **any** pipeline that emits stderr
(the two expected usage messages above go to stderr), regardless of the inner
exit code (`powershell -Command "exit 3" | Tee-Object` → reported 1; an
exit-0 script with stderr → reported 1; the same script without stderr → 0).
The script's own verdict lines are authoritative; recorded for transparency.

**B1: PASS.**

---

## 4. Requirement B2 — insert/modify/delete regression, all 5 R13 types

Dedicated gate script (`tmp/e2e_gate/b2_regression.ps1`) driving the production
CLI per type: pre-check absent → insert from a per-type valid EasyLanguage
source file → exists → modify (marker source) → getsrc round-trip → delete →
verify gone. Graceful TSDev restart between types (pipe-wedge prevention).
Items named `E2eGate_<Type>_<stamp>` with stamp `20260904T155941`.

| Type | Pre-check | Insert | Exists | Modify | Getsrc round-trip | Delete | Gone |
|------|-----------|--------|--------|--------|-------------------|--------|------|
| Function | exit 1, absent | exit 0, ok:true (7630 ms) | exit 0, exists:true, verbatim name match (1207 ms) | exit 0, ok:true (7852 ms) | exit 0, source contains `E2E_GATE_MODIFY_MARKER` (1213 ms) | exit 0, ok:true (1655 ms) | exit 1, exists:false (185 ms) |
| Indicator | exit 1, absent | exit 0 (7369 ms) | exit 0, true (1213 ms) | exit 0 (7874 ms) | exit 0, marker present (1210 ms) | exit 0 (1672 ms) | exit 1, false (186 ms) |
| ShowMe | exit 1, absent | exit 0 (7489 ms) | exit 0, true (1213 ms) | exit 0 (7876 ms) | exit 0, marker present (1214 ms) | exit 0 (1658 ms) | exit 1, false (185 ms) |
| TradingApp | exit 1, absent | exit 0 (7578 ms) | exit 0, true (1217 ms) | exit 0 (7875 ms) | exit 0, marker present (1211 ms) | exit 0 (1674 ms) | exit 1, false (184 ms) |
| Strategy | exit 1, absent | exit 0 (7314 ms) | exit 0, true (1214 ms) | exit 0 (7845 ms) | exit 0, marker present (1209 ms) | exit 0 (1636 ms) | exit 1, false (185 ms) |

All 35 steps returned the asserted exit codes with `ok:true` (or the asserted
`exists:false` for the gone-checks). The B4 cleanup sweep then re-verified all
5 items absent via `exists` (all exit 1, exists:false) and the final
`list --json` found **0 `E2eGate_*` items among 2507**. Script verdict:
**`B2 regression: ALL CHECKS PASSED`** (exit 0).

### Strategy known-bug note (escalation trigger — did NOT fire)

The recorded, state-dependent production bug (`tsdevctl insert --type
Strategy` timeout) did **not** reproduce in this gate. All three Strategy
inserts performed by the gate returned exit 0: B1 modify-section 9193 ms, B1
delete-section 8601 ms, B2 regression 7314 ms. No escalation.

### ShowMe delete note

The suite documents that ShowMe deletion may crash TSDev (the CLI handles the
crash internally). This run: no crash — ShowMe delete exit 0 in 1658 ms (B2) /
1640 ms (B1), no `TSCrashReport`, item confirmed gone.

**B2: PASS.**

---

## 5. Requirement B3 — dialog/crash log

| # | When | Type | Message | Corrective action |
|---|------|------|---------|-------------------|
| — | (entire gate) | — | — | **No TSDev/TS error dialogs, no crash reports observed across the entire gate run.** |

Mechanism: after **every** CLI invocation — (a) the B1 gate copy's
`Invoke-Tsdevctl` checks `Get-Process -Name TSCrashReport`; (b) the B2 script
additionally checks `TSCrashReport` + `WerFault` + any **visible** TSDev-owned
window (the tool keeps the frame hidden; a visible TSDev window would be an
error popup); (c) every direct manual invocation (cleanup delete, exists
checks, final list, final close) was followed by an explicit process scan.
Zero hits across the run. The CLI's own dialog detection (exit 7 / crash exit
8) never triggered either — every JSON result carried `error:null` except the
asserted negative paths (`not_found`, `compile_error`, the one recorded
timeout incident below).

**B3: PASS.**

---

## 6. Requirement B4 — cleanup log

### 6.1 Items created by this gate (all deleted, all confirmed gone)

| Item name | Type | Created by | Cleaned via | Verified gone |
|-----------|------|------------|-------------|---------------|
| E2eGate_Function_20260904T155941 | Function | B2 insert | B2 delete | yes — exists exit 1 (twice: B2 + cleanup sweep) + final list |
| E2eGate_Indicator_20260904T155941 | Indicator | B2 insert | B2 delete | yes — exists exit 1 (×2) + final list |
| E2eGate_ShowMe_20260904T155941 | ShowMe | B2 insert | B2 delete | yes — exists exit 1 (×2) + final list |
| E2eGate_TradingApp_20260904T155941 | TradingApp | B2 insert | B2 delete | yes — exists exit 1 (×2) + final list |
| E2eGate_Strategy_20260904T155941 | Strategy | B2 insert | B2 delete | yes — exists exit 1 (×2) + final list |
| E2eModify_20260904T154859 | Strategy | B1 modify-section insert | manual `tsdevctl delete` (exit 0, 3556 ms) | yes — exists exit 1, exists:false + final list |

Independent final confirmation: `tsdevctl list --json` → exit 0,
**count 2507** (the pre-gate baseline count from B1's list — zero net items
added by the gate), `E2eGate_*` residue: **0**, `E2eModify_20260904T154859`
residue: **0**.

### 6.2 Pre-existing out-of-scope residue

The library still contains older `E2e*`/`E2E*` items from TVS #4/#5/#6-era
work (documented in `docs/RE_ORELUI20_E2E_GATE.md` §5.3: 108 items as of that
gate). They pre-date this gate, were not created by it, and remain out of
scope here.

**B4: PASS.**

---

## 7. Incidents and corrective actions

| # | Incident | Evidence | Corrective action |
|---|----------|----------|-------------------|
| 1 | `tsdevctl exists --type ShowMe` timed out (exit 6, 60491 ms, `"item did not open within the timeout"`) in B1's cleanup probe for `E2eDel_ShowMe_20260904T154859` | B1 log; JSON error `{"code":6,"name":"timeout",...}`; tsdev_pid 17556 healthy before/after; no popup, no crash | None needed: the same item had already been confirmed gone by the delete section's own exists-gone check (exit 1, exists:false, 184 ms) minutes earlier, and the final B4 sweep re-confirmed absence. Consistent with the recorded state-dependent exists-timeout bug family (SSOT); the probe is advisory in the suite. Recorded; not a gate failure (no dialog, CLI honored the R10 timeout contract with a clean machine-usable error) |
| 2 | The B1 script's cleanup loop covers only its `E2eDel_*` items; the modify-section item `E2eModify_20260904T154859` was left in the library | Final list count 2508 before cleanup vs 2507 baseline | Manual `tsdevctl delete --type Strategy --name E2eModify_20260904T154859` (exit 0, ok:true) + exists verification (exit 1, exists:false) + final list (0 residue). Gate leaves no items it created |
| 3 | Background-run harness reported exit 1 for the B1 invocation although the script reached its `exit 0` branch (`E2E passed`) | Controlled probe: `powershell -Command "exit 3" \| Tee-Object` → 1; exit-0 script emitting stderr → 1; exit-0 script without stderr → 0 | Treated as a harness reporting artifact (stderr-bearing pipelines), not a tool result; the script's internal assertion counters (0 FAIL) and verdict lines are authoritative. No TSDev/tool corrective action |
| 4 | Pre-existing hidden TSDev instance pid 7904 (earlier session's tool-launched smoke-test instance) was running at gate start | `Get-Process` at 17:44 local: pid 7904, empty MainWindowTitle, started 17:40:29 | Per the hazard mitigation and R5/R6: not killed, not disturbed — the tool attached to it (first `open` returned `tsdev_pid:7904`); it was later recycled by the E2E script's own graceful-restart hygiene (WM_CLOSE, the established practice). No force-kill occurred; no crash reports or instability followed any restart |

---

## 8. Final process state (after the gate)

| Check | Result |
|-------|--------|
| TSDev.exe | **none running** (all gate-launched instances closed gracefully via WM_CLOSE; the pre-existing hidden instance was recycled by the script's restart hygiene) |
| TSCrashReport.exe | **none** |
| WerFault.exe | **none** |
| Repo state | `git status --porcelain` empty — no source files modified; gate scripts/logs under gitignored `tmp\e2e_gate\` |
| Build artifacts | `build\e2e_gate\Release\`: `tsdevctl.exe` (194560 B), `delete_hook.dll` (10240 B), `enum_hook.dll` (10240 B) — fresh on-disk paths per the hazard mitigation |

---

## 9. Gate verdict

### **PASS**

| Requirement | Result | Evidence |
|-------------|--------|----------|
| A — unit tests | **PASS** | `all unit tests passed`, exit 0; 143 assertions; latest code (clean tree @ 321ff8d) |
| B1 — list scenarios | **PASS** | All 5 list scenarios executed and passed (count 2507, all 5 R13 types, per-type filter, human format, usage rejections); full suite green (`E2E passed`, 33 ok / 0 FAIL) |
| B2 — 5-type insert/modify/delete regression | **PASS** | 35/35 steps at asserted exit codes with round-trip getsrc verification; `B2 regression: ALL CHECKS PASSED` (exit 0) |
| B3 — zero error/crash dialogs | **PASS** | Dialog/crash checks after every CLI invocation (both scripts + manual): zero hits; no TSCrashReport/WerFault at any point |
| B4 — cleanup | **PASS** | All 6 gate-created items deleted and confirmed gone (exists + final list); 0 `E2eGate_*` residue; count back to the 2507 baseline |

Escalations: **none** — the recorded `tsdevctl insert --type Strategy` timeout
did not reproduce (3 Strategy inserts, all exit 0: 9193 / 8601 / 7314 ms).
Incidents #1–#4 (§7) are recorded with corrective actions; none is a gate
failure.

The gate satisfies AGENTS.md line 80 and the WORKING_MEMORY.md "TVS #8"
acceptance-gate definition. **TVS #8 is DONE.**
