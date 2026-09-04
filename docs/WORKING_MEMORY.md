# Working Memory — TSDev Automation

Live, real-time state for the PM. Keep minimal; drop stale/superseded facts.
Do not restate what AGENTS.md or the code documents (machine conventions,
paths, and process rules live in AGENTS.md).

## Current state

- Branch `exp/remote-control`. Requirements SSOT: AGENTS.md (R1–R15).
  TSDev.exe + loaded-module facts SSOT: docs/KNOWLEDGE_BASE.md.
- **TVS #1 DONE (commit ad1f93a): `tsdevctl open`** — lifecycle + pipe channel.
- **TVS #2 DONE (commit b94e63f): `tsdevctl exists`** — phantom-discriminator check.
- **TVS #3 DONE (commit cadbccd): `tsdevctl getsrc`** — open-read-close, source emission.
- **TVS #4 DONE (commit 35976b4): `tsdevctl insert`** — the first write operation.
- **TVS #5 DONE (commit 4d96592): `tsdevctl modify`** — replace source + reverify.
- **Quiet-UX + foundation fixes DONE (commit 27f6412).**
- **TVS #6 DONE (commit 67ac757): `tsdevctl delete`** — destructor-path library
  deletion. **TEMPORARY — to be replaced.** Current implementation uses 7
  binary-internal coupling points (3 vtable offsets, 2 struct offsets, 2
  function addresses). E2E passed for all 5 R13 types. The replacement
  (reimplement via ORELUI20.dll `DeleteItem`) is unblocked by TVS #7 — see
  "TVS #7 research findings" below. See `docs/RE_DELETE_OPEN_DIALOG.md` for
  the prior TSDev.exe-side analysis.
- **Constraint (d) relaxed (commit 896d8a5):** the process boundary is the
  safety boundary. Code injected into the TSDev.exe process may call and hook
  functions in any module loaded into that process (`ORELUI20.dll`,
  `ORLIB20.dll`, `mfc140u.dll`, etc.). Separate processes remain off-limits.
- **TVS #7 (research) DONE (2026-09-04).** All subtasks A–E complete;
  adversarial review APPROVED (26 PASS / 0 FAIL / 3 non-blocking NOTEs); E2E
  gate PASSED (all 5 R13 types clean insert/modify/delete, zero dialogs, no
  regression). No production code changed. See "TVS #7 research findings"
  below and `docs/RE_ORELUI20.md` (final deliverable).
- **TVS #8 SCOPED (PM session 2, 2026-09-04): `tsdevctl list` via ORELUI20.dll
  in-process enumeration.** Every scope premise verified against the repo
  (list verb absent; delete_hook/injection/exit-code/test/RE-doc facts
  confirmed). Adversarial review: APPROVED (round 1 DISAPPROVED — 2 blocking
  + 4 non-blocking findings, all fixed; round 2 APPROVED — its one
  non-blocking finding [stale nlohmann path] fixed). Exact scope: "TVS #8"
  section below. Next team
  implements it; do NOT re-plan TVS #7 and do NOT re-run its A–E subtasks.
- Ghidra: project `tsdev`, programs `/TSDev.exe` and `/ORELUI20.dll` open,
  MCP bridge connected (restart via the headless analyzer if stale). Frida
  17.17.0 available for RE/research only — NOT for production (users don't
  have RE tools).

### TVS #7 research findings (A+B+C done 2026-09-04)

Both enumeration and delete are **Frida-verified from within the TSDev.exe
process** using exported/stable surfaces. Proven facts are in the SSOT
(`docs/KNOWLEDGE_BASE.md`, "ORELUI20.dll + ORLIB20.dll ordinals" section);
consolidated reasoning + recipes + evidence appendix in `docs/RE_ORELUI20.md`.

- **Enumeration:** `CELFileOpenDialog` (Ordinal_17 ctor, NULL CWnd OK
  headless) + `LoadList` (Ordinal_1291, per-type with packed values from
  `DAT_1008fd04`) + walk `CMapWordToPtr` (CAssoc swapped: value@+0x04,
  key@+0x08) of `CELTechInfo*` (0x44 bytes: name CString@+0x10 verbatim,
  ProgId CString@+0x1c, type short@+0x08, subtype ushort@+0x0a, record id
  ushort@+0x0c). `CAnalysisTypeInfo` ctor = `FUN_10044820` (NOT exported —
  module base + offset, version-brittle). Frida-verified: 2958 items across 9
  types; 5 test items found with correct verbatim names + types; mixed-case
  preserved.
- **Delete:** `DeleteItem` (ordinal 587 / 0x24A, `0x10010dd0`,
  `__thiscall bool(this, CELTechInfo*)`, RET 0x4) → internally delegates to
  `ORLIB20.dll` Ordinal_59 (library delete) + Ordinal_17 (cache refresh). No
  document-state manipulation (operates at library-record level — key
  advantage over the 7-coupling-point destructor path). Preconditions: not
  built-in (type 4), not open in QE. Frida-verified: Function ✓, Indicator ✓,
  ShowMe ✓, TradingApp ✓ (minimal bar exceeded); Strategy NOT Frida-tested
  (tsdevctl insert timeout — production bug, not a Frida failure). All
  deletions confirmed via re-enumeration.
- **Per-operation assessment (confirmed):** delete YES, list YES, exists YES
  (benefit via ORELUI20.dll); getsrc/insert/modify/upsert NO.
- **Production bugs flagged (NOT fixed in TVS #7 — research only; for next
  implementation TVS):** (1) `tsdevctl insert --type Strategy` timeout; (2)
  `tsdevctl exists` timeout for Function/Indicator; (3) `tsdevctl delete
  --type Strategy` crash (killed TSDev PID 3140; state-dependent — manifests
  when a Strategy doc is open in QE; did NOT reproduce in the E2E gate's
  clean-state run). All are tsdevctl issues, not Frida/ORELUI20 issues.
  Recorded in the SSOT.

## TVS #7 — DONE (research; all subtasks A–E complete 2026-09-04)

**TVS #7: Research — characterize the `ORELUI20.dll` item enumeration + delete
routine, and assess which other operations benefit from the module-calling
approach.** No production code changes. Deliverable: `docs/RE_ORELUI20.md`
+ SSOT updates in `docs/KNOWLEDGE_BASE.md`.

### Subtask status (all DONE)

- **A — Static RE: DONE.** `docs/RE_ORELUI20_STATIC.md`. Enumeration +
  delete routines characterized from Ghidra; 10 `[DYNAMIC]` flags raised.
- **B — Dynamic verification: DONE.** `docs/RE_ORELUI20_DYNAMIC.md`. Both
  enumeration and delete Frida-verified from within the TSDev.exe process;
  3 corrections found to A's static recipes; per-type delete results;
  dialog/crash log; cleanup log.
- **C — Documentation: DONE.** `docs/RE_ORELUI20.md` written (final
  consolidated deliverable); `docs/KNOWLEDGE_BASE.md` SSOT updated
  ("ORELUI20.dll + ORLIB20.dll ordinals" section); this file updated.
- **D — Adversarial review: DONE — APPROVED.** `docs/RE_ORELUI20_REVIEW.md`.
  26 PASS / 0 FAIL / 3 non-blocking NOTEs. No required fixes. Independent
  Ghidra verification confirmed DeleteItem @ 0x10010dd0, LoadList @
  0x10012290, and FUN_44820's 4-param signature (correction B-1).
- **E — E2E gate: DONE — PASS.** `docs/RE_ORELUI20_E2E_GATE.md`. All 5 R13
  types clean insert/modify/delete from elevated production CLI against real
  TSDev; zero dialogs/crashes; no regression. Strategy delete did NOT crash
  this run (the known bug is state-dependent — manifests when a Strategy doc
  is open in QE; clean-state hygiene avoided it). All `E2eGate_*` items
  cleaned up and confirmed gone.

### Reviewer NOTEs (non-blocking; for the next implementation TVS to consider)

1. Reduce inline SSOT restatement in `RE_ORELUI20.md` §2.3/§2.4.
2. Spell out `GetModuleHandleW` for runtime-base resolution in the
   implementation TVS.
3. Flag `DAT_1008fd04` (offset 0x8fd04) as a second version-brittle offset
   alongside 0x44820 (FUN_10044820).

## TVS #8 — SCOPED, VERIFIED, REVIEW-APPROVED (PM session 2, 2026-09-04): `tsdevctl list` via ORELUI20.dll in-process enumeration

**Decision: implement `tsdevctl list`** — the first production consumer of the
TVS #7-proven ORELUI20.dll enumeration surface. This continues the TVS #7 arc
(user directive: "continue on TVS 7" = this workstream; the research itself is
DONE and must not be re-run). Scope premises verified against the repo on
2026-09-04 (verb table `args.cpp:7,10-11`; dispatcher `main.cpp:1811-1817`;
delete_hook pattern `src/delete_hook/dllmain.cpp:279,113-167` +
`shared.h:8-17`; injection `src/tsdevlib/inject.cpp` inject_dll /
call_remote_function / get_export_rva; exit codes `src/tsdevlib/exit_codes.h`;
tests `tests/test_tsdevctl_units.cpp`, `tests/run_e2e.ps1`,
`tests/run_unit_tests.ps1`; RE facts in `docs/RE_ORELUI20.md` §2 and
`docs/KNOWLEDGE_BASE.md` "ORELUI20.dll + ORLIB20.dll ordinals").

### Why `list` is the minimal next TVS

1. **Fills a missing required operation.** `list` is one of the seven R2
   operations and is entirely absent today (only `upsert` is also missing).
2. **Builds the shared foundation.** The enumeration surface is the common
   infrastructure that delete-reimpl AND exists-robustification will reuse.
   Building it with the simplest read-only consumer is the highest-leverage
   foundation work.
3. **Lowest risk = best first exercise of the new mechanism.** `list` is
   read-only; it is the first use of the "inject a DLL that calls ORELUI20.dll
   ordinals and marshals structured results back" pattern (delete_hook today
   marshals only a single status long).
4. **End-to-end verifiable** against known items and TVS #7's Frida-verified
   per-type counts.
5. **Minimal.** Enumeration cannot be split from a consumer and remain a
   verifiable TVS; the mechanism is identical per type, so the slice covers
   all types present (9 observed).

Deferred (in order): delete-reimpl via `DeleteItem` (NEXT TVS; small delta on
this foundation, removes 7 brittle coupling points, may obsolete the Strategy
delete crash) → exists-robustification (reuses enumeration; expected to
supersede the exists-timeout path) → upsert (pure composition) → flagged
Strategy production bugs (insert timeout / exists timeout / delete crash —
recorded in the SSOT; not blocked by this TVS).

### Exact scope of TVS #8 (build this, nothing more)

IN SCOPE:

1. **New injected DLL `enum_hook.dll`** (new `src/enum_hook/`, mirroring
   `delete_hook`'s ReArmHook / named file-mapping / done-event /
   `__try__except` + crash_flag pattern — see `src/delete_hook/shared.h:8-17`,
   `dllmain.cpp:279,113-167`; new CMake target, Win32). Its exported entry,
   called via `call_remote_function` (`src/tsdevlib/inject.cpp:74`):
   - Resolve `ORELUI20.dll` in-process via `GetModuleHandleW(L"ORELUI20.dll")`.
   - Resolve exported surfaces in-process via
     `GetProcAddress(hMod, MAKEINTRESOURCEA(n))`: Ordinal_17
     (CELFileOpenDialog ctor), Ordinal_1291 (LoadList), Ordinal_166 (dtor).
     No on-disk PE parsing inside the DLL (the CLI's `get_export_rva` is
     name-based and stays as-is).
   - Compute the two NON-exported surfaces from the module base, both flagged
     version-brittle in comments (purpose b): `CAnalysisTypeInfo` ctor =
     base + 0x44820 (FUN_10044820), type-category table = base + 0x8fd04
     (DAT_1008fd04, 13 entries).
   - Execute the enumeration recipe exactly per `docs/RE_ORELUI20.md` §2.1
     (with the three dynamic-verification corrections): empty map buffer;
     construct dialog via Ordinal_17 with NULL CWnd (headless-proven); walk
     the 13 DAT entries; per entry construct CAnalysisTypeInfo and call
     LoadList; walk each CMapWordToPtr with the SWAPPED CAssoc
     (value@0x04 = CELTechInfo*, key@0x08 = recordId); read CELTechInfo
     fields (name CString@+0x10, ProgId CString@+0x1c, category@+0x08,
     subtype@+0x0a, record id@+0x0c); destroy dialog via Ordinal_166. No
     message pump, no window creation (R7: never change visibility). An MFC
     CString here is a single wchar_t* to a CStringData buffer: read the
     string via that pointer; length lives at data−0x0C (32-bit MFC140).
   - Remote-entry return contract (mirrors ReArmHook's contract with
     `call_remote_function`): the exported entry returns nonzero on success,
     0 on setup failure; the full enumeration runs inside the remote thread
     and overall completion is governed by the done-event wait (configured
     `--timeout`), not the remote-thread exit code alone.
   - Marshal via named file mapping `Local\TSDevAutomation.Enum.<pid>`:
     header {count, status, crash_flag, truncated_flag} + fixed-size records
     `{ wchar_t name[256]; wchar_t progid[256]; ushort category; ushort
     subtype; ushort record_id; }` (~1030 B/record; observed max 2958 items
     ≈ 3 MB — size capacity 8192 records ≈ 8.4 MB for headroom). On
     would-be overflow: stop, set truncated_flag, still signal done. Copies
     into the fixed 256-wchar_t fields are truncation-safe (always
     NUL-terminated, consistent with the existing 255-cap convention,
     `main.cpp:749-750`); a stored name/ProgId longer than 255 wchar_t sets
     truncated_flag → clean fail (never silently truncate user-visible
     names).
   - Signal completion via event `Local\TSDevAutomation.EnumDone.<pid>`.
2. **CLI `run_list`** (`src/tsdevctl/main.cpp`) + **per-verb arg validation**
   (`src/tsdevlib/args.cpp`):
   - `list` added to the verb table (`args.cpp:7,10-11`) and dispatcher
     (`main.cpp:1811-1817`).
   - **The ONLY change to existing shared code:** `args.cpp:70-71` currently
     requires `--type` AND `--name` for ALL verbs. Make validation per-verb:
     `--name` required for all verbs EXCEPT `list`; `--type` optional for
     `list` (absent = all items; present = per-type filter). Existing verbs'
     behavior must not change (unit tests cover parse_args per verb).
     Invalid `--type` value → Usage (2), as today (`args.cpp:72-74`).
     `--name` supplied with `list` → reject with Usage (2), consistent with
     the per-verb flag-rejection convention (`args.cpp:58-64`).
   - Lifecycle identical to other verbs: elevation (3), BusyGuard (9),
     ensure_tsdev_ready (auto-start hidden / attach-only / --nohide
     respected) (4). NO pipe use — enumeration is in-process via the DLL;
     exit 5 N/A.
   - Create mapping + done event, inject `enum_hook.dll`, call its entry via
     `call_remote_function`, wait with timeout + dialog/crash checks (same
     pattern as `run_delete`, `main.cpp:730-836`).
   - Read records; filter by `--type` when given; emit:
     - Human: one line per item `<Type>  <verbatim name>  (ProgId: <progid>)`
       (omit the ProgId part when empty) + a summary count line.
     - JSON: `{"ok":true,"op":"list","type":"<Type>"|null,"count":N,
       "items":[{"name":..,"type":..,"prog_id":..},...],"tsdev_pid":..,
       "elapsed_ms":..,"error":null}`. Extend `OpResult`/`make_result_json`
       or add a dedicated builder — implementer's call (the vendored
       `third_party/nlohmann/json.hpp` may be used — NOTE: that path exists
       only under `archive/third_party/` (discarded pre-overhaul approach);
       re-vendor if used); the JSON shape above is
       the contract.
   - Type labels: reverse-map the FULL type — the (category, subtype) pair,
     equivalently the packed DAT value — → label for ALL enumerated types
     from TSDev's own category table (13 entries; 9 observed with items —
     see KB "ORELUI20.dll + ORLIB20.dll ordinals"); unknown type → stable
     numeric fallback label (never crash, never guess from content). The
     `--type` filter matches the exact full type against `type_number`
     (`src/tsdevlib/type_table.h:15-21`: Function=1, Indicator=2,
     ShowMe=0x0502, TradingApp=0x1402, Strategy=0) — so the
     built-in-Functions DAT entry
     (0x0004) is NOT matched by `--type Function`; document that choice in
     the help text.
   - `usage_text()` (`args.cpp:81-183`): add the `list` usage line,
     description, and the `--type`-optional / `--name`-not-required note;
     add a usage-text unit test for `list` mirroring
     `test_usage_contains_delete`.
   - Exit codes: 0 success (count 0 is success); 2 usage; 3 not elevated;
     4 launch failed; 6 timeout; 7 error dialog; 8 crash; 9 busy; 10
     unexpected (injection/enumeration failure, truncated_flag set,
     version-brittle-surface guard failure). 1/5/11/12 N/A for list.
   - Check for error/crash dialogs after the operation (mandatory rule).
3. **Version-brittle guard.** ORELUI20.dll SHA-256 known-hash check before
   injection → clean fail exit 10 (mirrors delete's TSDev.exe check,
   `main.cpp:609-714`; both ship together). Compute the known hash from the
   verified installation during implementation. The two non-exported offsets
   (0x44820, 0x8fd04) must be flagged version-brittle in code.
4. **Tests (TDD red-green):**
   - Non-TS unit tests in `tests/test_tsdevctl_units.cpp`: per-verb arg
     validation for `list` (--type optional, --name not required, --name
     with `list` rejected, invalid type → usage), usage-text coverage for
     `list`, record marshalling → JSON formatting → type-filter logic,
     fed canned records (no TSDev).
   - TS-dependent E2E in `tests/run_e2e.ps1`: `list --json` → exit 0,
     ok:true, count>0, items is an array, all 5 R13 types represented, a
     known item appears with correct type + verbatim name; `list --type
     Function --json` returns only Function items; `list` without --json
     emits human lines + summary; crash-dialog check. Reuse existing
     scaffolding.
5. **Docs:** update `WORKING_MEMORY.md` (mark `list` DONE when complete);
   update `KNOWLEDGE_BASE.md` SSOT only if new proven facts emerge;
   `RE_ORELUI20.md` §2 is the implementation input (already done).

OUT OF SCOPE (keep it minimal — separate TVSs):
- delete-reimpl via `DeleteItem` (NEXT; reuses this enumeration foundation).
- exists-robustification (reuses enumeration; expected to supersede the
  exists-timeout path).
- upsert.
- Flagged Strategy production bugs (insert timeout; exists timeout; delete
  crash — delete-reimpl may obsolete the crash).
- Batch/manifest/stdin for `list` (R4 is cross-cutting; separate TVS).
- Python package wiring (R1; CLI-first like TVSs 1–6; later cross-cutting TVS).

### Acceptance gate (mandatory, AGENTS.md)
- `tests/run_unit_tests.ps1` passes (cmake -A Win32; CI builds without
  TradeStation).
- Real production CLI E2E from an elevated direct CLI against a real TSDev
  instance: the list scenarios above AND the standard insert/modify/delete
  regression (all 5 R13 types), zero error/crash dialogs. Clean up created
  items.

### Handoff
This session = PM scoping only (premises repo-verified; adversarial review
APPROVED — round-1 blocking findings fixed, round-2 non-blocking finding
fixed). No execution. The
next team implements TVS #8 (TDD red-green), then runs the acceptance gate.
If the recorded Strategy insert timeout reproduces during the gate's
insert/modify/delete regression: STOP and escalate to the PM — do NOT
expand TVS #8 scope to fix it (it is a recorded SSOT bug, out of scope
here). Do NOT re-plan TVS #7 (research is done); do NOT expand scope.
