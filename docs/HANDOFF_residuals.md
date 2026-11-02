# Hand-off -- residual issues (post `list` feature, 2026-11-02)

Cleanup hand-off for the NEXT team: fix ALL known residual issues --
pre-existing ones and leftovers from the 2026-11-01/02 `list`-feature
session. Every entry is evidence-anchored (file:line or artifact path) and
was verified against the tree at 0abe7c0 (see Appendix F for the per-RC
verification status). Format follows `docs/HANDOFF_update.md` (map, not a
copy) and `docs/HANDOFF_WHServer_VAS_recovery.md` (evidence discipline).

## 1. Purpose + scope

Goal: drive the residual inventory RC-01..RC-29 to zero.

Non-goals (explicit):

- ActivityBar / PaintBar / ProbabilityMap support stays OUT of scope
  (project decision; `src/tsdev_lib.h` enum is the SSOT).
- The six-operation acceptance target is unchanged; no new feature work.
- No renumbering of shipped exit codes (13 and 20-24 taken; see
  `docs/HANDOFF_update.md` "Exit codes").

## 2. How to work through this document

The mandatory project process applies in full (AGENTS.md):

- One minimal TVS at a time; TDD red-green; split anything splittable.
- Every substantive result gets an independent adversarial review
  (100% completion, scope/drift, hacks/shortcuts) until APPROVED.
- After EVERY approved TVS: a passing real production CLI E2E of
  `tsdev_cli insert/modify/delete` from an elevated direct CLI with the
  latest code; clean up before closure. This gate is mandatory and
  supplements unit/regression tests.
- Popup/crash-dialog audit after EVERY live action (TSDev and our own
  executables): read the popup text, dismiss, correct course, record it.
  The popup-audit regex must exclude `explorer` windows (they carry the
  TradeStation path in their title -- `docs/WORKING_MEMORY.md`).
- ASCII only in docs and CLI output; byte-verify new/edited files at the
  end (grep for bytes > 127).
- Root-cause fixes only. Never suppress a compiler warning or mask a
  problem; if no clean form exists use an explicit/raw form with an
  explanatory comment (AGENTS.md no-hacks rule).
- Elevation: direct `tsdev_cli` requires an elevated shell
  (`isElevated()` -> exit 11). For UAC auto-elevation tests, disable UAC
  prompts first (`ConsentPromptBehaviorAdmin=0`, restore to 5 after).
- Live recycle/WHServer experiments require the desktop CLOSED (foreign
  clients hold `m_nLock`); the machine runs TS in offline mode
  (`docs/WORKING_MEMORY.md`).
- Regression: `powershell -ExecutionPolicy Bypass -File
  tests\run_regression.ps1` (`-NoTs` for the non-TS gate); suite inventory
  SSOT is `tests/run_unit_tests.ps1` `$TestDefinitions`.
- Order of attack: P1 first (RC-02 and RC-13 are small and pure-correctness;
  RC-18 and RC-11 are the hard ones), then P2 test strength, then P3.

## 3. Inventory

Effort: S <= half day, M = 1-2 days, L = 3+ days.
Priority: P1 correctness/reliability/leaks, P2 test strength/diagnostics,
P3 cosmetics/docs/housekeeping. RC-23 is an owner decision (no code).

| ID | Area | Pri | Effort | Summary |
|---|---|---|---|---|
| RC-01 | C++ COM (whserver_names / whserver_recycle) | P2 | S | CoInitializeEx not balanced by CoUninitialize at two bind sites (pattern debt) |
| RC-02 | C++ COM (whserver_names) | P1 | S | EXCEPINFO BSTRs leak on the SEH path of the IDispatch Invoke helper |
| RC-03 | Diagnostics (tsdev_lib) | P2 | S | listItems logs COM bind failure but stays silent on MyWork-resolution failure |
| RC-04 | Test strength (test_list_cmd) | P2 | S | sort tie-stability not discriminated (all tie runs N<=4) |
| RC-05 | Test strength (test_list_cmd) | P2 | S | check 5 message pin is self-referential; production text unpinned |
| RC-06 | Test strength (test_serve_dispatch) | P2 | S | check 24f cannot tell the batch/policy gates from the list grammar |
| RC-07 | Test strength (test_serve_dispatch) | P2 | S | check 24d pins error.code but not the frame message |
| RC-08 | Test strength (test_enumerate) | P2 | M | step-7b fixtures are live TS dictionary strings; degrade silently |
| RC-09 | Test strength (pytest) | P2 | S | test_cli_list_rejects_extra_flags is vacuously green in the red state |
| RC-10 | CLI/serve parity | P3 | S | serve unknown_type message lacks the direct path's hint (and quoting) |
| RC-11 | Test reliability (pytest) | P1 | M | one-off access violation in the fake-tsdev-cli mock-harness thread |
| RC-12 | Test reliability (pytest) | P2 | S | one-off zipfile.__del__ OSError at interpreter shutdown |
| RC-13 | C++ leak (tsdev_lib) | P1 | S | _LoadMasterRecsEX@16 SEH-failure path can leak the HGLOBAL buffer; GlobalUnlock return values ignored at the same-lifecycle HGLOBAL sites |
| RC-14 | Build hygiene | P3 | S | MSVC warning debt: C4244 confirmed in Release+Debug; C4476/C4996 shapes present but unreproduced at default level |
| RC-15 | Source hygiene | P3 | M | non-ASCII characters in code comments (~7.5 KB across src/ + tests/) |
| RC-16 | Tooling (tmp\check_ts_state.ps1) | P3 | S | "WHServer ABSENT (ANOMALY)" label false-alarms on normal self-reap |
| RC-17 | UX text (recycle markers) | P3 | S | deferred boundary reuses the "WHServer recycled" marker text |
| RC-18 | Reliability (recycle boundary) | P1 | M | close-hang kill path loses the partial results array |
| RC-19 | Serve contract | P3 | S | serve never-attached site exits 18 after a successful engine run |
| RC-20 | Docs | P3 | S | no consolidated exit-code table; README summary incomplete |
| RC-21 | Evidence / tmp hygiene | P3 | M | load-bearing evidence lives in gitignored tmp\; scratch cleanup |
| RC-22 | Ops (git / CI) | P2 | S | two local commits unpushed; CI green unconfirmed for new tests |
| RC-23 | Ops (legal) | owner | S | LICENSE decision stalled (all-rights-reserved default) |
| RC-24 | Update feature | P3 | S | --pbar download bar unimplemented; synthesized download-fail message |
| RC-25 | Housekeeping (git) | P3 | S | stale exp/remote-control branch (28 unmerged superseded commits) |
| RC-26 | Housekeeping (git) | P3 | S | stale user_bug_batch_ops_hang_1 branch (fully merged; safe delete) |
| RC-27 | Python wrapper (results.py) | P2 | S | from_json silently coerces a malformed non-list `names` frame to `[]` (masks a contract violation as an empty result) |
| RC-28 | CLI result contract (tsdev_cli) | P3 | S | CliResult defaults success=false, exitCode=0: an unfilled worker result would emit a success-code lie (judged unreachable; documented exposure) |
| RC-29 | Ops (broker verification) | P3 | S | elevated-shell broker bypass by documented design (_serve_active) silently redirects broker-contract verification to the direct path |

RC-26 was found by the verification sweep (Appendix B) and is new versus
the PM inventory. RC-27..RC-29 were added by the 2026-11-02 adversarial
review of this handoff.

## 4. Per-item sections

### RC-01 -- CoInitializeEx not balanced by CoUninitialize (2 sites)

Current state + evidence (verified present at all cited sites):

- `src/whserver_names.cpp:299-313`: `WhComNameSource::bind` calls
  `CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)` (:309); S_OK and
  S_FALSE both proceed (`FAILED(coHr) && coHr != S_FALSE` gate, :310) and
  no `CoUninitialize` runs on any exit path. `release()` documents the
  deliberate non-balancing at `src/whserver_names.cpp:285-288` ("the
  apartment belongs to TsDevLib's COM guard"); the header contract repeats
  it at `src/whserver_names.h:97-100` ("CoUninitialize is NEVER called").
- Same pattern: `src/whserver_recycle.cpp:724-728`
  (`ComRecycleAttach::attachAndForceShutDown`), documented non-balancing at
  `:558-560` and `:865-866`.
- The governing rule is stated at `src/com_apartment_guard.cpp:24-36`
  (quoting :26-28): each successful CoInitializeEx -- INCLUDING the S_FALSE
  one -- must be balanced by exactly one CoUninitialize on the same thread.

Why it matters: an S_OK on a thread the guard does not own leaks a COM
init count and keeps the apartment alive past the worker's teardown; even
the guard-owned S_FALSE bumps the per-thread count the guard balances only
once. It is silent pattern debt that future thread moves can turn into a
real leak/hang.

Fix direction: (a) use the existing RAII `ComApartmentGuard`
(`src/com_apartment_guard.h`) at both bind sites -- it implements the exact
rule and is seam-testable (ComApi injection, `src/tsdev_lib_test_hooks.h`:
72) -- or (b) drop the redundant "confirm" CoInitializeEx entirely (the
guard contract already guarantees the apartment), or (c) if the
non-balancing is truly intentional, document it AT BOTH SITES with an
explicit justification that covers the S_FALSE counting rule and prove
every call thread is guard-owned. (a) or (b) is preferred; (c) must not
contradict `com_apartment_guard.cpp:26-28`.

Definition of done: unit evidence that the chosen form balances exactly
once per successful init (mock ComApi counts), no behavior change on the
live `list`/recycle paths (production E2E per Section 2), and both sites'
comments match the rule.

### RC-02 -- EXCEPINFO BSTR leak on the SEH Invoke path

Current state + evidence (verified present at `src/whserver_names.cpp`):

- `callInvokeImpl` (:186-202) declares `EXCEPINFO exc = {}` INSIDE the
  `__try` (:190). The normal path frees `bstrSource`/`bstrDescription`/
  `bstrHelpFile` (:194-197, comment "EXCEPINFO BSTRs are owned by the
  caller even on failure"). The `__except (EXCEPTION_EXECUTE_HANDLER)` path
  (:198-199) returns false WITHOUT freeing them: if the server populated
  the BSTRs before raising, they leak. The except handler cannot even see
  `exc` (declared inside the `__try` scope).
- Scope check ("all Invoke helpers"): the only SEH-wrapped Invoke with an
  EXCEPINFO is `callInvokeImpl` (call sites :232, :385, :455, :494). The
  two inline IDispatch::Invoke calls in `src/whserver_recycle.cpp`
  (:801-807 and :833-839) are NOT SEH-wrapped and already free the
  EXCEPINFO BSTRs inline -- keep those frees when touching them.

Why it matters: an SEH from a dying/wedged WHServer mid-Invoke is a real
field path (the whole call family exists for it); each occurrence leaks
server-allocated BSTRs in our process.

Fix direction: hoist `EXCEPINFO` (POD) out of the `__try`, free the three
BSTRs in the `__except` path exactly as the normal path does (the
POD-only/no-destructor constraint that keeps `__try` legal under MSVC
C2712 is preserved -- see the helper-family convention comments in
`src/tsdev_lib.cpp:340-372`).

Definition of done: a unit test with a fake IDispatch whose Invoke
populates the EXCEPINFO BSTRs and then raises (SEH) proves the strings are
freed on the except path (tracking SysFreeString seam or CRT debug heap);
red before, green after; the recycle inline Invokes keep their frees.

### RC-03 -- listItems failure-stage logging asymmetry

Current state + evidence (verified at `src/tsdev_lib.cpp:2260-2291`):

- MyWork-dir unresolved: `return false` at :2274-2277 with NO log.
- COM bind failure: Warning logged at :2281-2287
  ("tsdev: listItems: %s").
- Bonus asymmetry found while verifying: the `catch (...)` at :2288-2291
  is also silent.

Why it matters: `list` exit 4 is the fail-loud contract ("could not
enumerate items"); an operator seeing exit 4 with an empty log cannot tell
which stage failed. The direct/serve surfaces already fail loudly; the
library layer should not swallow the first failure stage.

Fix direction: log a Warning (same `tsdev_log_message` call shape as
:2284) at the MyWork-resolution failure and in the catch-all, naming the
stage ("mywork_unresolved" / "unknown exception"), keeping the return
contract (false, outNames empty) untouched.

Definition of done: unit/live evidence that all three failure stages emit
a log line (drive with `--log-file` + `-v`: unresolvable MyWork via a bad
`--ts-path`, bind failure via a non-running/absent WHServer path), zero
output-shape change on success (existing list tests stay green).

### RC-04 -- sort tie-stability not discriminated

Current state + evidence (verified):

- Production sort is `std::stable_sort` at `src/tsdev_cli.cpp:3972`
  (PM citation :3959 drifted; the helper is `sortNamesForOutput` at
  :3951-3973).
- `tests/test_list_cmd.cpp` tie runs: check 6 (:210-223, N=4), check 7
  (:229-242, N=3), check 8 (:248-261, N=4), check 10 (:289-301, N=3). All
  N<=4 -- far below MSVC `std::sort`'s insertion-sort cutoff (16), where
  introsort is de facto stable. Swapping `std::stable_sort` for
  `std::sort` would keep every check green.

Why it matters: "STABLE ties" is the pinned `list` ordering contract
(`src/tsdev_cli.cpp:3939-3950`); the test suite does not currently protect
it.

Fix direction: add a tie run with >16 elements (e.g. 20+ names equal
ignoring case in a known non-sorted source order) to the check 7/8 family.
Deterministic data only; assert exact output order.

Definition of done: the new check FAILS against a deliberately swapped-in
`std::sort` (red proof recorded) and passes with `std::stable_sort`;
`-NoTs` suite green.

### RC-05 -- check 5 message pin is self-referential

Current state + evidence (verified):

- `tests/test_list_cmd.cpp:175-203` (check 5) sets `r.errorMessage` to the
  literal itself (:181) and then asserts the JSON carries that same
  literal (:198-201). Changing the production message breaks nothing.
- The production doList failure text "init failed: could not enumerate
  items" (`src/tsdev_cli.cpp:4005-4006`; PM citation :3992-3993 is the
  comment above) is pinned by NO test.

Why it matters: the exit-4 fail-loud wording is part of the user-facing
contract (BUNDLE_README list section); today it can change silently.

Fix direction: expose the message as a file-local constant next to
`doList` and have check 5 assert against the production constant (the
same-TU include convention already gives the test access), or drive
`doList` against a failing TsDevLib seam. Extend the same convention to
the check-5 error code string if convenient.

Definition of done: editing the production literal turns the test red;
suite green after restore.

### RC-06 -- serve check 24f cannot distinguish gate from grammar

Current state + evidence (verified at `tests/test_serve_dispatch.cpp`):

- check 24f (:1326-1351) asserts only `contains(errorMessage, "batch")` /
  `contains(errorMessage, "policy")` (:1344-1347).
- The real gate texts are "invalid 'batch' metadata (insert/modify/
  delete/upsert only; ..." (`src/tsdev_cli.cpp:7303`) and "'policy' is
  only valid for op 'backup-cache'" (`src/tsdev_cli.cpp:7325`). If the
  gates were bypassed and the list "unexpected field '<key>'" path
  (`src/tsdev_cli.cpp:7386-7394`) handled the key instead, the message
  would still contain "batch"/"policy" and the check would stay green.

Why it matters: the gate-before-grammar ordering is a deliberate contract
("their gates above reject them first", `src/tsdev_cli.cpp:7376-7377`);
the test currently cannot detect its loss.

Fix direction: assert the gate-specific text ("invalid 'batch' metadata"
and "only valid for op 'backup-cache'") and/or explicitly negate
"unexpected field" in this check.

Definition of done: a mutation that routes batch/policy into the list
grammar turns 24f red; suite green after restore.

### RC-07 -- check 24d frame message unpinned

Current state + evidence (verified at `tests/test_serve_dispatch.cpp:
1292-1311`): check 24d pins `exitCode == 2` and
`errorCodeString(...) == "unknown_type"` (:1305-1306) but never inspects
`rr.errorMessage`.

Why it matters: the frame message is client-visible; it is also the very
text RC-10 wants to change -- change it without a pin and parity can drift
again.

Fix direction: after RC-10 settles the wording, pin the exact
`rr.errorMessage` in check 24d (both cases in `unknownType[]`).

Definition of done: editing the serve unknown-type message turns the
check red.

### RC-08 -- live-dictionary description fixtures in test_enumerate

Current state + evidence (verified):

- Fixtures at `tests/test_enumerate.cpp:64-73`:
  `kDescriptionTextIndicator = "Plots the %R indicator."` (:71),
  `kDescriptionTextFunction = "Returns the next label in the cycle for
  ActivityBar cells"` (:72-73). Commented as "live-proven" from
  `docs/KNOWLEDGE_BASE.md` Sec. 22/23.
- Step 7b (:277-317) queries both strings and asserts clean not-found --
  but nothing asserts the strings are still real description texts in the
  current TS dictionary. A TradeStation dictionary update silently turns
  the regression probe into a meaningless query (the check would pass
  vacuously).

Why it matters: these are the only regression guards for the 2026-11-01
description false-positive root cause (name-index-only lookup). Silent
degradation of the fixture defeats their purpose.

Fix direction: runtime-assert the fixture precondition (e.g. verify the
description string still appears in a record description field via the
existing record-walk helpers before using it as a probe; fail with a
distinct code when the precondition breaks) OR build synthetic fixture
records carrying known description texts. Keep the -NoTs contract: this is
a RequiresTs suite (skipped under `-NoTs` by design --
`docs/WORKING_MEMORY.md`) and its fixtures have a live lifecycle -- the
suite creates and deletes a live probe Strategy during its run
(`tests/test_enumerate.cpp:112-123` (pre-clean), `:126-135`, cleanup
`:319-327`), so live items
appear and vanish in the TS library mid-run.

Definition of done: a fixture-precondition failure is loud and
distinct (not a confusing "found" failure); the historical FP strings keep
their not-found assertion; live run green in the TS window.

### RC-09 -- vacuous pytest in the red state

Current state + evidence (verified at `tests/test_tsdev_client.py:
2898-2904`): `test_cli_list_rejects_extra_flags` wraps
`_main(["list","strategy","--case-insensitive"])` in a bare
`pytest.raises(SystemExit)`. argparse raises SystemExit(2) both for the
unrecognized flag AND for an unknown subcommand (invalid choice) -- so the
test passes even if the `list` subcommand is deleted from the parser.

Why it matters: it is the only parser-level guard that `list` rejects
flags; today it cannot fail for the reason it exists.

Fix direction: assert `excinfo.value.code == 2` AND the specific argparse
error ("unrecognized arguments: --case-insensitive" via `capsys`) -- or
pair it with a positive parse assertion (`_main(["list","strategy"])`
reaches the client stub) that fails when `list` is absent.

Definition of done: removing the `list` subparser turns the test red
(recorded red proof); suite green after restore.

### RC-10 -- serve unknown_type message parity gap

Current state + evidence (verified):

- Serve `list` parse: `src/tsdev_cli.cpp:7396-7404` emits
  `unknown type '<type>'` (single quotes, NO hint). The generic serve path
  (:7428-7436) uses the same text for other ops.
- Direct `cmdList`: `src/tsdev_cli.cpp:4066-4073` emits
  `unknown type "<type>" (expected strategy/function/indicator/
  tradingapp/showme)` (double quotes + hint; same shape at the other
  direct commands, e.g. :4724).
- Exit codes and `error.code` parity already hold (serve check 24d).

Why it matters: the two surfaces present different messages for the same
input; users and the pinned tests see whichever surface they use.

Fix direction: extract one message builder (hint text + quoting) shared by
the direct commands and `parseServeRequest`; then pin per RC-07.

Definition of done: the serve frame message text equals the direct-path
message for the same input (pinned by tests on both surfaces).

### RC-11 -- pytest harness access violation (one-off)

Current state + evidence:

- One Windows access violation (fatal exception) in the mock-harness
  worker thread when the full suite ran concurrently with a C++ build;
  not reproducible in isolation (clean 466-pass re-run). Recorded at
  `docs/WORKING_MEMORY.md` anomaly (1).
- Harness location verified: `class _FakeCliProc`
  (`tests/test_tsdev_client.py:6472-6793`), worker thread named
  "fake-tsdev-cli" started at :6506-6507, in-memory pipe cores
  (`_FakePipeCore`/`_FakePipeReader`/`_FakePipeWriter`, :6182-6385).
  The crash itself is NOT independently re-verifiable from the tree (no
  preserved artifact beyond the WM note).

Why it matters: an intermittent fatal exception in the largest Python
suite erodes trust in every green run and can mask real failures.

Fix direction: stress-reproduce first (full suite x10-20 while a parallel
MSBuild `/t:Rebuild` runs -- the original trigger). Suspects: daemon
thread teardown racing interpreter/pipe close under I/O load. If a repro
appears: root-cause and fix (e.g. join the harness thread on close, no
daemon). If a bounded campaign (documented N runs) yields zero crashes:
document it as accepted-flake WITH a narrow, documented retry guard (re-run
the single test once on a fatal-exit signature only) -- never a blanket
pass. No hacks. Any retry-guard approach must be adjudicated against the
AGENTS.md no-masking rule at TVS review before adoption: a retry-on-fatal
can mask a real defect if the stress campaign was not actually run first.

Definition of done: either deterministic repro + fix + N clean stress
runs, or a preserved stress-campaign record (commands, N, results) plus
the documented retry rule and its no-masking adjudication record; both end
with the suite green.

### RC-12 -- interpreter-shutdown zipfile.__del__ OSError (one-off)

Current state + evidence: one occurrence of `zipfile.__del__` raising
OSError [Errno 9] Bad file descriptor at pytest shutdown (exit 0
regardless); did not recur in 3 later runs. NOT verifiable from the tree:
no `zipfile`/`ZipFile` usage exists in any repo .py file (grep clean) and
no preserved log contains the traceback (tmp\pytest_*.log grepped for
"Bad file descriptor" -- clean). The frame owner is therefore a
stdlib/pytest-internal user; the originating test is unknown.

Why it matters: harmless today (exit 0), but it is unattributed noise at
shutdown; if it ever lands mid-run it will look like data corruption.

Fix direction: on the next occurrence capture and preserve the FULL
traceback (redirect pytest stderr to a tracked artifact). Meanwhile audit
test utilities for file/stream handles left open across interpreter
shutdown and close them explicitly (context managers / atexit). If the
frame proves stdlib-internal only: document and accept.

Definition of done: a preserved traceback naming the owning frame and the
owning test, or a documented "observed once, stdlib-internal, accepted"
note with a re-occurrence trigger.

### RC-13 -- HGLOBAL leak on the _LoadMasterRecsEX SEH path (scope re-verified)

Current state + evidence (re-verified post-rewrite -- the PM's suspicion
was right):

- The SEH helper `callLoadMasterRecsEx`/`...Impl`
  (`src/tsdev_lib.cpp:838-858`) returns false from the `__except` (:845-
  846) without touching `*outBuf` -- but the callee may have populated
  `*outBuf` (an `HGLOBAL*`) before raising.
- Call-site census NOW: exactly ONE call site remains --
  `TsDevLib::enumerateReverseDependents`, `src/tsdev_lib.cpp:2859-2863`.
  On the SEH path it `continue`s (:2863) WITHOUT freeing `buf`; the
  non-SEH paths free correctly (:2870, :2876, :3018). The old
  enumerateRecords path was rewritten name-index-only (2026-11-02) and
  `listItems` moved to the WHServer COM name index, which removed the
  other two call sites of the 2026-11-01 review.
- Same-shape wrappers audited: `callGetMasterRecByType` (call sites :2042,
  :2589) and `callOrd28` (:2697, :2895) write into CALLER-owned stack
  buffers -- no ownership leak. `callLoadTextFileEx` (:806-816; call sites
  :2060, :2086) takes the HGLOBAL as a RETURN value -- a raise inside the
  callee loses a callee-owned allocation the wrapper cannot reach (note
  only, not fixable here). `callFindExistPos`-style wrappers take int*
  outs -- not applicable.
- Named per the 2026-11-02 review fold (the "unlock failure" spec-wording
  gap vs the lock-failure handling): `GlobalUnlock` return values are
  IGNORED at all three HGLOBAL sites -- `src/tsdev_lib.cpp:2072`
  (decryptHMem), `:2099` (get-source hMem) and `:3017` (this function's
  `buf`, immediately before the `GlobalFree` at :3018). `GlobalLock`
  results ARE checked at the same sites (:2068-2070, :2095-2097). Per
  Win32 semantics `GlobalUnlock` returns FALSE with `GetLastError() ==
  NO_ERROR` when the lock count merely drops to zero (benign), so the
  ignored return hides exactly the real unlock errors and unexpected
  residual lock counts.

Why it matters: a raised _LoadMasterRecsEX on a degraded server is a real
path (the SEH wrappers exist for exactly that); the leaked HGLOBAL is
warehouse-sized.

Fix direction: free `buf` in the :2863 SEH branch (or
`GlobalFree(*outBuf)` in the Impl's `__except` when non-null, documenting
the transfer-of-ownership rule), matching the :2870/:2876 pattern. Handle
or observe the `GlobalUnlock` result per Win32 semantics at all three
sites (distinguish the benign FALSE + NO_ERROR from a real error), or
explicitly document the accepted unlock pattern at each site; the :3017
site shares `buf`'s lifecycle with the leak fix above.

Definition of done: a unit test drives `callLoadMasterRecsEx` with a stub
fn that writes `*outBuf` then raises (same-TU include or test-hook seam,
per the established conventions) and proves the buffer is freed (tracking
allocator / CRT debug heap); red before, green after. Plus evidence that
the `GlobalUnlock` results are handled/observed at :2072, :2099 and :3017
(benign FALSE + NO_ERROR distinguished from real errors) or explicit
documented acceptance at all three sites.

### RC-14 -- pre-existing MSVC warnings at HEAD

Current state + evidence (sources verified present):

- C4476 family (format string): the giant usage literal in
  `printUsage`'s single `std::fprintf(stderr, ...)` call
  (`src/tsdev_cli.cpp:438-...`) contains unescaped `%LOCALAPPDATA%\
  tsdev_cli\cache-backup` at :592 -- bare `%` sequences inside a printf
  format string.
- C4996 (strncpy deprecation): `src/dispatch_watchdog.cpp:198`
  (`std::strncpy(g_marker.site, ...)`).
- C4244 (wchar_t -> char, reported inside xutility):
  `tests/test_path_discovery.cpp:199` (`std::string regRoot(regRootW.begin(),
  regRootW.end())`).
- The fresh Release Win32 rebuild capture performed while writing this
  handoff is recorded in Appendix C (warning set confirmation).
- Reproduction status (fresh FULL rebuilds in BOTH configs -- Release and
  Debug, Appendix C): the complete warning set is the ONE C4244, which
  reproduces in both. The C4476 and C4996 listed at
  `docs/WORKING_MEMORY.md:27` do NOT reproduce at the standard warning
  level even though the offending code shapes are still present -- they
  presumably surfaced under a different warning level/config. Treat the WM
  list as the historical claim and Appendix C as the measured baseline;
  fix all three root causes anyway (the shapes are wrong regardless of
  whether the default level flags them).

Why it matters: warning debt hides real warnings; the project rule forbids
suppression.

Fix direction (root causes, no /wd flags): escape `%%` in the usage text
or print it via a non-formatting call; replace strncpy with an explicitly
sized copy (memcpy of a known field size, or StringCchCopyA with an
explanatory comment); narrow wchar_t via an explicit conversion
(WideCharToMultiByte or a cast loop) instead of iterator-range
construction.

Definition of done: clean Release AND Debug Win32 rebuilds (the Appendix C
command) report zero warnings with no warning-suppression flags added
anywhere; behavior unchanged (usage text renders byte-identical after the
literal `%` -> `%%` fix; dispatch marker site text unchanged).

### RC-15 -- non-ASCII characters in code comments

Current state + evidence (measured 2026-11-02, byte scan of every
*.cpp/*.h/*.py/*.ps1/*.psm1 under src/ + tests/, skipping __pycache__):

- 7,551 non-ASCII bytes total (UTF-8 sequences: em-dashes and similar).
  src/ = 4,084 bytes; tests/ = 3,467 bytes. Concentration confirmed:
  src/tsdev_cli.cpp alone = 1,414 bytes, next: src/tsdev_lib.cpp 492,
  src/tsdev_automation/client.py 415, tests/test_tsdev_client.py 358.
  Keep the historical "~421 em-dashes" figure SEPARATE from these
  measurements: it was an all-of-src em-dash character estimate (not a
  byte count; roughly 1.3 KB at 3 bytes/char) -- do not equate it with
  the 1,414 bytes measured for src/tsdev_cli.cpp.
- Magnitude is LARGER than the historical estimate covered: the tests/
  tree (~3.5 KB) was outside it. The docs/CLI-output tree is ASCII-clean
  (unchanged).

Why it matters: hard project rule -- editors and terminal codepages mangle
these bytes (observed mojibake). Code comments are the remaining non-ASCII
surface.

Fix direction: mechanical sweep src/ + tests/ comments to pure ASCII
("--" for em-dash, plain quotes/apostrophes); do NOT touch byte values
inside string literals that are part of pinned output contracts without
checking the pinning tests (verify each replacement against the tests that
pin message text). Two tool scripts from the earlier sweeps exist in tmp\
(find_nonascii.py, find_emit_nonascii.py) but tmp\ is volatile -- re-create
the scanner as a tracked check if the team wants enforcement.

Definition of done: the byte scan reports 0 non-ASCII bytes for src/ +
tests/ (or an explicitly justified exception list), full unit/pytest suite
green (pinned messages unchanged), and the mandatory E2E still passes.

### RC-16 -- check_ts_state.ps1 "ABSENT (ANOMALY)" false alarm

Current state + evidence (verified): the script lives ONLY at
`tmp\check_ts_state.ps1` (gitignored; NOT in tests/). Line 13 prints
"WHServer: ABSENT (ANOMALY)" whenever no WHServer process exists. Idle
self-reap is NORMAL: transient servers self-reap <17 s idle
(`docs/HANDOFF_WHServer_VAS_recovery.md:45`), so post-run snapshots
false-alarm.

Why it matters: the script is used for E2E popup/state audits; a standing
false alarm trains reviewers to ignore its output.

Fix direction: make the expectation explicit (e.g. `-ExpectRunning`
switch or a label argument convention: ABSENT is an anomaly only when a
server is expected; otherwise print "WHServer: ABSENT (idle self-reap,
normal)"). Move the script into a tracked location (tests/ or tools/) --
this also resolves its RC-21 volatility.

Definition of done: a post-run snapshot prints a non-alarm absence line;
an in-session snapshot with a missing server still alarms; script is
tracked in git.

### RC-17 -- deferred boundary marker text

Current state + evidence (verified):

- `kRecycledMarkerMessage = "WHServer recycled; resubmit remaining items"`
  (`src/tsdev_cli.cpp:5335-5336`) is selected purely by OUTCOME
  (`src/tsdev_cli.cpp:6391-6395`). A granted DEFER (reason
  "recycle_deferred_own_session", `src/tsdev_cli.cpp:5678`) is outcome
  Recycled -- so the not-run markers claim "WHServer recycled" although at
  that boundary NO recycle happened (only the bounded close ran; the
  engine runs in the resumed process). `reason` is the truthful
  discriminator.
- The serve response path already interpolates reason
  (`src/tsdev_cli.cpp:7908-7914`).
- Coupling: the Python client re-words these markers
  (`src/tsdev_automation/client.py:317-341`, :373-379) and tests pin the
  exact text (`tests/test_recycle_resume.cpp:391`,
  `tests/test_tsdev_client.py:4840`).

Why it matters: the marker text is the user-visible explanation of why
items never ran; "recycled" vs "deferred" changes what the user should do.

Fix direction: key the marker text on `rec.reason`
("WHServer recycle deferred; resubmit remaining items" for
recycle_deferred_own_session; keep the resumable-tail suffix the client
matches on), and update the client mirrors + pinning tests in the same
TVS. Note the pin-swap must keep `client.py`'s marker-matching suffix
intact or update it atomically.

Definition of done: resume E2E (desktop-closed window for the live leg)
shows the deferred wording in the marker tail; client re-wording still
fires; all pinning tests updated and green.

### RC-18 -- close-hang terminateSelf(19) loses the partial results array

Current state + evidence (verified):

- Known-good emission exists for the close-RETURNS-false paths: the batch
  recycle exit emits + flushes the results array before any
  TerminateProcess-self (`src/tsdev_cli.cpp:6391-6411`, kill at :6483-
  6488; forward decl rationale :5558-5562) and the batch-tail closeFailed
  path does the same (:6803-6816).
- The LOSS path is the close HANG: `boundedCloseWarehouse`'s bound-fire
  runs `closeBoundAbortFire` (`src/whserver_recycle.cpp:129-146`) --
  stderr line + `TerminateProcess(GetCurrentProcess(), 19)` DIRECTLY on
  the monitor thread, bypassing `recycleExit` entirely (comment :132-133:
  the emitLine channel is a CLI-main-thread lambda, unsafe cross-thread).
  The in-memory partial results array never reaches stdout; only the
  stderr "RECYCLE ABORT: closeWarehouse exceeded ..." line and the
  already-streamed per-item stderr progress survive.
- Documented as a known loss at `docs/WORKING_MEMORY.md:26` (citing the
  recycle design's Section 5 close-stage family; the shipped close-stage
  contract is `docs/HANDOFF_WHServer_VAS_recovery.md:495-501`).
- Contrast: the analogous dispatch-watchdog loss IS documented for users
  (`docs/BUNDLE_README.md:71` paragraph "A watchdog abort emits no JSON
  frame ..."); the close-bound loss is not.

Why it matters: exit-18 boundaries are the resume contract (results array
+ marker tail + manifest); losing the array on the close-hang exit-19
variant loses the only durable record of the completed items.

Fix direction (pick one, in preference order): (1) pre-stage the boundary
results array BEFORE starting the close stage -- the boundary index is
known when the defer begins; emit + flush per the pinned Section 4.1 order
(results array -> manifest -> event -> summary -> fflush -> exit) and let
the close outcome only affect the event/exit code; (2) make the bound-fire
handler carry a pre-built, thread-safe snapshot (the monitor could write
the array via a pre-registered buffer + plain write(2)-style flush); or
(3) formally accept the loss with a loud marker: pin the RECYCLE ABORT
stderr line by test AND add the loss to the `docs/BUNDLE_README.md:71`
abort-loss paragraph. Constraints: never `delete lib`/ExitProcess into a
possibly-open warehouse (the TerminateProcess-self rule stands), and the
client resume parsing keys on the event name + manifest member -- keep
them stable.

Definition of done: a scripted close-hang test (the seams exist:
`onCloseBoundExceeded` injection at `src/tsdev_cli.cpp:5655-5659`, tests
in `tests/test_recycle_resume.cpp` family) shows the partial results array
on stdout with the attempted head + marker tail; or, for option (3), the
documented-acceptance paragraph + pinned stderr line exist and a review
signs off on the accepted loss.

### RC-19 -- serve never-attached site exits 18 after a successful engine run

Current state + evidence (verified):

- The serve VAS guard's never-attached site runs the engine IN-PROCESS
  (`src/tsdev_cli.cpp:7884-7887`, `runWhserverRecycleCli(..., false)`) --
  and on success still returns an exit-18 refusal-shaped response frame
  ("WHServer recycled (reason); resubmit on the respawned broker",
  :7908-7914), which makes the Python client respawn the broker and
  resubmit (the 3-broker choreography).
- The batch pre-flight site does the opposite and CONTINUES in-process
  after a successful never-attached recycle ("item 0's lazy attach is a
  FIRST activation on the fresh server", `src/tsdev_cli.cpp:6508-6518`).
- Current status is by design per `docs/WORKING_MEMORY.md:28`.

Why it matters: three broker lifecycles for one successful request is
churn (elevation + respawn + resubmit) where the batch path proves a
safe in-process continuation exists. But exit 18 is the pinned client
contract (`docs/HANDOFF_WHServer_VAS_recovery.md:522-536`), so changing it
is a contract change, not a bug fix.

Fix direction: DECIDE ONE: (a) serve mirrors the batch continuation (the
request has not dispatched yet; the engine already reset the server --
dispatch it in-process, frame exit 0) with client/test pins updated
atomically; or (b) formally document exit 18 at the never-attached serve
site as the response-frame contract (BUNDLE_README serve section + this
handoff's successor note), with a test pinning the behavior so it cannot
drift silently.

Definition of done: either a serve test pins the in-process continuation
(frame exit 0 after a successful never-attached engine run) end to end, or
the documented-contract paragraph exists and a test pins exit 18.

### RC-20 -- exit-code documentation gap

Current state + evidence (verified):

- `README.md:161` lists `0/11/8/5/14/15/18/19` and points to
  `docs/BUNDLE_README.md` "for the full table". Codes `1` (usage),
  `2` (unknown_type), `4` (init failure), `10` (thread failure) -- and the
  `12/13/25-28` family -- are absent from the summary.
- `docs/BUNDLE_README.md:112-119` contains ONLY the update table
  (25-28) with a `0/1/11` preamble. No consolidated table exists anywhere.

Why it matters: three documents promise "the full table" and none
delivers; exit codes are a client contract (the Python exceptions map by
them, `docs/BUNDLE_README.md:180`).

Fix direction: build ONE table (SSOT discipline: one location, everything
else references it). Enumerate the codes from the code, not from memory
(grep the `kExit*` constants and `return` sites in `src/tsdev_cli.cpp`),
include the meanings already pinned in `docs/UPDATE_DESIGN.md` Appendix A
and `docs/HANDOFF_update.md`. Put it in `docs/BUNDLE_README.md` (the
bundle-facing doc) and reduce README.md:161 + the UPDATE_DESIGN references
to pointers.

Definition of done: exactly one full exit-code table exists; every
"see the full table" pointer resolves to it; every code the CLI can exit
appears in it (verified by a grep-driven cross-check recorded in the TVS).

### RC-21 -- evidence durability + tmp\ hygiene

Current state + evidence (verified):

- `tmp\` is gitignored (`.gitignore:14:/tmp/`; `git check-ignore` confirms).
- Load-bearing evidence cited from tmp\: `docs/KNOWLEDGE_BASE.md` Sec
  22-25 (e.g. `tmp\probe_walk_post_insert_0/1.txt`,
  `tmp\probe_walk_post_delete_0/1.txt` at KB:544; `tmp\frida_enum_final.js`
  / `tmp\frida_enum_trace.js` at KB:550), the name-index RE SSOT
  `tmp\enum_mechanism_report.md` and `tmp\exists_fix_corpus\`
  (`docs/WORKING_MEMORY.md:6-7`). All verified PRESENT in tmp\ at writing
  time (`enum_mechanism_report.md`, `exists_fix_corpus\`,
  `tvs1_bulk_dump_out.txt`/`out2.txt`, `verify_names_results.txt`,
  `probe_walk_post_*.txt`, `probe_lifecycle_results.txt`). KB:544 warns
  `probe_lifecycle_results.txt` contains a known summary-line parsing bug
  -- the WALK TRANSCRIPTS are the proof; preserve them preferentially.
- Scratch cleanup candidates verified present: `pre_rewrite_backup.bundle`
  + `pre_rewrite_mirror\`, `e2e_list_gate\`, `gate_final\`, dozens of
  `frida_*.js`, `wh_enum*` probes, `tvs1_bulk_dump*`, plus a stray
  `tmp\TSDev.exe` copy.

Why it matters: one `git clean`, clone, or tmp sweep away, every KB
Section 22-25 claim becomes unverifiable -- and those sections back the
name-index correctness contract.

Fix direction: decide per citation class: (a) archive load-bearing
evidence into a tracked location (`docs/evidence/` for transcripts/reports,
`tests/fixtures/` for binary baselines) and update the citations; or (b)
explicitly mark the citations volatile in the KB (a standing note listing
which tmp\ artifacts are load-bearing). Independently: delete/archive the
scratch candidates above (user confirmation before deleting
`pre_rewrite_backup.bundle` -- it is a deliberate backup).

Definition of done: every KB Sec 22-25 / WORKING_MEMORY citation either
resolves to a tracked path or is marked volatile in-place; the tmp\
scratch list is emptied or explicitly retained with a reason.

### RC-22 -- unpushed commits + CI confirmation (ops)

Current state + evidence (verified):

- `git rev-list --count origin/main..main` = 2:
  9bb1842 "list: add list <type> op (CLI/Python/broker) on WHServer COM
  names" and 0abe7c0 "wm: list feature complete; AGENTS ops + current
  state". `main..origin/main` = 0 (nothing to pull). NOTE: `git status
  -sb` prints bare "## main" -- no upstream tracking is configured; push
  with an explicit refspec (`git push origin main`).
- CI wiring verified: `.github/workflows/ci.yml:41-42` runs
  `tests/run_regression.ps1 -NoTs`, whose unit inventory is the SSOT
  `tests/run_unit_tests.ps1` `$TestDefinitions` -- including
  `test_list_items` (:82), `test_list_cmd` (:86), `test_name_index_outcome`
  (:98), all `RequiresTs = $false`, so the CI -NoTs path picks them up.

Why it matters: the whole `list` feature lives only on this machine until
pushed; CI green on the new targets is unconfirmed.

Fix direction: push, then watch the CI run on 9bb1842/0abe7c0: Win32
build + `-NoTs` regression + mocked pytest must be green and the three new
test names must appear in the job log. If CI is red, that is new
information -- fix before anything else.

Definition of done: `origin/main` contains both commits; the CI run for
each is green with the new test targets visible in the log.

### RC-23 -- LICENSE decision (owner; no code)

Current state + evidence (verified): no LICENSE file exists anywhere in
the tree (root check + full-tree grep clean). Without one, the published
code is all-rights-reserved by default. Decision stalled since 2026-10-29.

Why it matters: the repo is (about to be) public for the update feed
(`docs/HANDOFF_update.md` Preconditions); the license is the public
contract.

Fix direction: owner picks a license (or explicitly records the
all-rights-reserved decision); if a license is chosen, add the LICENSE
file and reference it from README.md. No engineering work beyond that.

Definition of done: a LICENSE file exists OR a recorded owner decision
says none will.

### RC-24 -- update feature deviations vs spec

Current state + evidence (verified):

- `--pbar` download progress bar: NO pbar/progress code exists in
  `src/update_feed.cpp` / `src/update_apply.cpp` (grep clean) while
  `docs/UPDATE_DESIGN.md:229` specifies "With `--pbar`, a determinate
  pinned bar (Content-Length based) replaces ...". The implementation
  prints plain lines ("Downloading %s (%s) ...\n"
  `src/tsdev_cli.cpp:9282`; "Installing %s ...\n" :9323).
- Synthesized download-fail message: "the update download failed (the
  update service could not deliver the file). Nothing was changed. Try
  again; if it repeats, reinstall manually."
  (`src/tsdev_cli.cpp:9306-9309`) has NO row in the spec's exact-message
  table (`docs/UPDATE_DESIGN.md:485-507`) -- invented outside the spec.

Why it matters: `docs/UPDATE_DESIGN.md` is the pinned UX contract
(exact grammar/output/JSON/exit codes); the implementation and the spec
now disagree in two places.

Fix direction: reconcile in ONE direction per deviation: implement the
pinned bar (spec 2/9 per the spec's own section refs) OR amend the spec to
the shipped plain-line behavior; add the download-fail row to the spec's
message table (or replace the synthesized text with a spec-conformant
row). Keep `tests/test_update_cmd.cpp` decision-table pins in sync.

Definition of done: spec and implementation agree on both points;
`test_update_cmd` pins the download-fail message; a `--pbar` update run
matches whatever the spec then says.

### RC-25 -- stale local branch exp/remote-control

Current state + evidence (verified): local branch `exp/remote-control`,
head da74dc1, contains 28 commits NOT on main
(`git rev-list --count main..exp/remote-control` = 28) -- the superseded
`tsdevctl` UI-automation approach (TSDev.exe injection; commit subjects:
"TVS #8: implement tsdevctl list (ORELUI20.dll in-process enumeration)"
79a2eed, "TVS #7 research deliverables" ec8556a, etc.). No remote carries
it (the remote side was deleted; retained deliberately as a backup during
the 2026-10-29 history rewrite). Cosmetic note: several commit subjects on
this branch carry stray UTF-8 BOM characters (edb0988, 36c619f, fe86d25,
71111e9, 0e6b630, 52f2815) -- history-only, not worth rewriting.

Why it matters: an unadvertised 28-commit branch is exactly the thing a
later `git branch -D` sweep destroys unnoticed -- or worse, resurrects by
accident.

Fix direction: decide delete or formal archive. If archiving: create a
tag first (e.g. `archive/tsdevctl-approach-2026-10` at da74dc1) and push
the tag to origin so the backup survives branch deletion; then delete the
branch. If deleting: record the head sha (da74dc1) in the commit message.

Definition of done: the branch is either gone (with a tag or recorded sha)
or explicitly documented as retained; `git branch` shows only intentional
refs.

### RC-26 -- stale local branch user_bug_batch_ops_hang_1 (new; from sweep)

Current state + evidence (verified): local branch
`user_bug_batch_ops_hang_1`, head cbfc5d3, is FULLY merged into main
(`git rev-list --count main..user_bug_batch_ops_hang_1` = 0;
`git merge-base --is-ancestor` true). It is a stale pointer with zero
unique commits.

Why it matters: same housekeeping class as RC-25; left unrecorded it is
invisible stray state.

Fix direction: re-run the two verification commands at delete time (they
must still report 0 / ancestor-true), then `git branch -d` (the SAFE
delete -- it refuses if anything appears). No archive needed.

Definition of done: `git branch` no longer lists it; the safety commands'
output is recorded in the TVS notes.

### RC-27 -- from_json silently coerces a malformed non-list `names` frame to `[]`

Current state + evidence (verified):

- `TsDevResult.from_json`, `src/tsdev_automation/results.py:128`:
  `names=[str(n) for n in obj.get("names")] if isinstance(obj.get("names"), list) else []`.
  An ABSENT `names` field defaults to `[]` (fine -- non-list ops omit it),
  but a PRESENT non-list `names` (string, number, dict) is silently
  coerced to `[]` as well: a contract-violating CLI frame becomes a
  successful empty result.
- The masking behavior is currently PINNED by a test:
  `tests/test_tsdev_client.py:630-636`
  (`test_result_names_non_list_stays_empty`, docstring "A malformed
  non-list `names` field yields `[]` (no raise)"). The neighboring tests
  cover only the benign shapes: absent -> `[]` (:611-616) and element
  `str(...)` coercion (:618-628).

Why it matters: `list` results flow through `from_json`
(`TsDevClient.list_items`, `src/tsdev_automation/client.py:1714`); a
malformed frame from the CLI/serve broker is a contract violation that
must fail loudly, not surface as "0 items". A caller cannot distinguish
an empty library from a corrupt frame -- a silent plausible-but-wrong
result of the kind the fail-loud contracts elsewhere in this inventory
exist to prevent.

Fix direction: fail loudly on a PRESENT-but-non-list `names`: raise a
typed error via the client/exceptions layer or return an explicit
malformed-frame result (the absent-field -> [] default and `str()`
element coercion may stay). TDD: flip `test_result_names_non_list_stays_empty` to the
fail-loud contract first (red), then change `from_json`; keep the absent
and element-coercion tests green. No masking.

Definition of done: a unit test drives `from_json` with a non-list
`names` frame and asserts the loud failure (red before, green after);
absent `names` still yields `[]` and list elements are still coerced with
`str(...)` (tests/test_tsdev_client.py:611-628 stay green).

### RC-28 -- CliResult default exitCode=0 can lie about success on a dead worker (judged unreachable)

Current state + evidence (verified):

- `struct CliResult` (`src/tsdev_cli.cpp:687-...`) defaults `bool success =
  false; int exitCode = 0;` (:692-693).
- Every single-op command runs its flow on a worker thread whose params
  carry a default-constructed `CliResult` marked "populated by run*":
  `ListParams::result` (`src/tsdev_cli.cpp:4026`, filled by `runList` at
  :4041-4047/:4053) is emitted unchanged after `WaitForSingleObject`
  (:4108-4109); identical shape in cmdExists (`src/tsdev_cli.cpp:3838`,
  emit :3933-3934) and the other cmd*/run* pairs. If a worker thread died
  WITHOUT filling `params.result`, `emitOutput` would emit
  `success=false, exit_code=0` -- a success exit code on a failed frame.
- Adjudication (2026-11-01 review): judged unreachable in practice --
  every worker installs per-thread fatal containment
  (`installThreadFatalContainment`, e.g. :4033) and re-asserts the process
  UEF (:4051), so a fatal on the worker terminates the process instead of
  letting the thread vanish silently. The identical exposure exists in
  cmdExists/cmdList (sites above).

Why it matters: exit 0 is the success contract on every surface (direct,
--json, serve); a `success=false, exit_code=0` frame would make scripts
and `TsDevResult.exit_code` treat a failure as success. Latent only while
the containment stays as strong as it is today.

Fix direction: root-cause only -- either (a) initialize `exitCode` to a
failure code (the existing thread-failure code 10 is the natural fit; see
the RC-20 exit-code documentation gap) so an unfilled result can never
carry exit 0, or (b) detect a never-filled result after the join (e.g. a
"filled" flag set on every worker exit path) and emit an explicit "worker
died" error frame. If the team keeps the default, record the adjudication
at TVS review instead of leaving it implicit. No masking.

Definition of done: either a seam/unit test proving an unfilled
`CliResult` can never be emitted as exit 0 (red before, green after), or
a recorded TVS-review adjudication citing the 2026-11-01 judgment and the
containment mechanisms, accepting the exposure as documented-only; the
cmdExists/cmdList instances follow the same decision.

### RC-29 -- elevated-shell broker bypass gotcha (documented design; verification trap)

Current state + evidence (verified):

- `TsDevClient._serve_active` (`src/tsdev_automation/client.py:2312-2322`)
  returns `use_serve AND NOT elevated`: with `use_serve=True` in an
  ALREADY-elevated shell, single-item ops BYPASS the named-pipe broker and
  run the direct path -- documented design (zero overhead, no extra UAC;
  the docstring at :2313-2321 says so). Consulted at 9 routing sites
  (:1475, :1495, :1535, :1688, :1731, :1775, :2488, :2680, :2953).
- This directly caused a verification misread in the 2026-11-02 final
  gate: broker evidence had to be gathered by forcing the non-elevated
  routing branch (`docs/WORKING_MEMORY.md`).

Why it matters: a team testing the broker contract (persistence across
ops, frame parity) from an elevated shell runs the DIRECT path and can
misread it as broker behavior -- or conclude the broker is unused. The
bypass is silent (the results look identical either way).

Fix direction: documentation/verification-procedure only -- this is by
design, NOT a defect; do not "fix" it into always-broker. State the rule
wherever broker verification is specified: exercise the broker by running
the client UNELEVATED with `use_serve=True`, or force the non-elevated
routing branch in tests (stub `_is_elevated` / `_serve_active`), and
confirm which path ran via `broker_process_handle()`
(`src/tsdev_automation/client.py:2324`) -- non-None means the broker ran.

Definition of done: the verification instructions state the gotcha and
the correct technique (run the client unelevated or force the non-elevated
routing branch; confirm via `broker_process_handle()`), so a
broker-contract run cannot silently test the direct path; referenced from
the live-leg reminders (Section 5).

## 5. Ops / owner items (non-code)

- RC-22: push `main` (explicit refspec -- no upstream configured), confirm
  CI green on both commits with the three new test targets in the log.
- RC-23: LICENSE decision (owner).
- RC-21: approve/perform the tmp\ scratch deletion (esp.
  `pre_rewrite_backup.bundle` -- deliberate backup; confirm before
  deleting) and pick the evidence archival policy (archive vs mark
  volatile).
- RC-25/RC-26: approve branch disposition (tag-then-delete vs retain).
- RC-29: before any broker-contract verification, read the RC-29 gotcha
  (an elevated shell bypasses the named-pipe broker by documented design)
  and use its stated verification technique.
- Reminder for whoever runs live legs: desktop must be CLOSED for recycle
  testing; the machine runs TS offline-mode; UAC prompts must be disabled
  before any auto-elevation test and restored after.

## 6. Appendix -- verification sweep performed while writing (2026-11-02)

All commands ran in `C:\Users\freek\projects\tsdev_automation` at HEAD
0abe7c0.

### A. TODO/FIXME/HACK/XXX/WORKAROUND markers

`grep` over src/ tests/ tools/ and root *.ps1/*.py for
`TODO|FIXME|HACK\b|XXX|WORKAROUND`: exactly one hit --
`src/vss_snapshot.cpp:508` (`"{XXXXXXXX-...}"` describing a GUID shape in
a comment) = false positive. No new RC from this sweep.

### B. Git stray state

- `git stash list`: EMPTY.
- `git branch -a`: `main` (current), `exp/remote-control` (RC-25),
  `user_bug_batch_ops_hang_1` (RC-26), remote `origin/main` only.
- `git status -sb`: "## main" (no upstream tracking configured).
- `git rev-list`: origin/main..main = 2 (9bb1842, 0abe7c0 -> RC-22);
  main..exp/remote-control = 28 (-> RC-25); main..user_bug_batch_ops_hang_1
  = 0 (-> RC-26).

### C. Build capture (RC-14 warning-set confirmation)

- Fresh FULL rebuild in the CI configuration:
  `cmake --build build --config Release -- /t:Rebuild /nologo
  /verbosity:minimal` (VS 18 BuildTools, Win32) -- EXIT=0; exactly ONE
  warning: `xutility(5094): warning C4244: '=': conversion from 'wchar_t'
  to 'char', possible loss of data` in `test_path_discovery` (the RC-14
  site `tests/test_path_discovery.cpp:199`). No C4476, no C4996.
- Fresh FULL Debug rebuild (same command with `--config Debug`) -- EXIT=0;
  the SAME single C4244 warning and nothing else. (An earlier Debug
  attempt that ran CONCURRENTLY with the re-run failed at LINK with
  LNK1104 "cannot open file 'Debug\tsdev_lib.lib'" -- a two-rebuilds-one-
  tree race, not a code defect; its compile stage showed the same single
  C4244. The clean sequential run supersedes it.)
- Conclusion: in BOTH standard configs the complete warning set is the one
  C4244. The C4476 (usage-text format string) and C4996 (strncpy) listed
  at `docs/WORKING_MEMORY.md:27` do NOT reproduce at the standard warning
  level even though the code shapes are present (`src/tsdev_cli.cpp:592`
  bare `%` in the usage format string; `src/dispatch_watchdog.cpp:198`
  strncpy) -- they presumably surfaced under a different warning
  level/toolset invocation. RC-14 scope: fix all three root causes
  regardless, and make "zero warnings" hold for both configs (RC-14
  definition of done).
- Historical captures in tmp\ (`build_clean.log`,
  `build_clean_first.log`, `build_d_full.log`) also show ONLY the C4244
  -- no preserved log ever recorded the C4476/C4996.

### D. Non-ASCII magnitude (RC-15)

Byte scan (>127) of every *.cpp/*.h/*.py/*.ps1/*.psm1 under src/ + tests/
(excluding __pycache__): 7,551 non-ASCII bytes -- src/ 4,084 (top:
tsdev_cli.cpp 1,414), tests/ 3,467. The historical "~421
em-dashes" figure is kept SEPARATE from these measurements: it was an
all-of-src em-dash character estimate (not a byte count; roughly 1.3 KB
at 3 bytes/char). Caveat: whole-file
counts include any non-ASCII string literals and
possible UTF-8 BOMs; the RC-15 fix must distinguish comments from pinned
literals.

### E. tmp\ gitignore status (RC-21)

`git check-ignore -v tmp` -> `.gitignore:14:/tmp/`. Confirmed gitignored;
all tmp\ citations are volatile (see RC-21).

### F. Per-RC location verification

| RC | Status |
|---|---|
| RC-01 | verified present (src/whserver_names.cpp:309, :285-288; src/whserver_names.h:97-100; src/whserver_recycle.cpp:724, :558-560, :865-866; rule src/com_apartment_guard.cpp:26-28) |
| RC-02 | verified present (src/whserver_names.cpp:186-202; except path :198-199); recycle Invokes verified NOT SEH-wrapped (:801-807, :833-839) |
| RC-03 | verified present (src/tsdev_lib.cpp:2274-2277 silent, :2284 warning, :2288-2291 silent catch-all) |
| RC-04 | drifted-to src/tsdev_cli.cpp:3972 (was :3959); tie runs verified N<=4 at tests/test_list_cmd.cpp:210-261, 289-301 |
| RC-05 | drifted-to tests/test_list_cmd.cpp:175-203 (was :181-201); production text src/tsdev_cli.cpp:4005-4006 (was :3992-3993) |
| RC-06 | verified present (tests/test_serve_dispatch.cpp:1326-1351, weak assert :1344-1347; gate texts src/tsdev_cli.cpp:7303, :7325) |
| RC-07 | verified present (tests/test_serve_dispatch.cpp:1305-1306) |
| RC-08 | verified present (tests/test_enumerate.cpp:64-73, :277-317) |
| RC-09 | drifted-to tests/test_tsdev_client.py:2898-2904 (was ~:2855-2861) |
| RC-10 | verified present (src/tsdev_cli.cpp:7396-7404 vs :4066-4073) |
| RC-11 | harness verified at tests/test_tsdev_client.py:6472-6793 (thread :6506-6507); the one-off crash is NOT re-verifiable (docs/WORKING_MEMORY.md anomaly 1 is the only record) |
| RC-12 | could NOT verify: no zipfile usage in repo code, no preserved traceback in tmp\pytest_*.log -- remains a reported one-off (entry kept, fix = capture on recurrence) |
| RC-13 | RE-VERIFIED + scope corrected: exactly ONE call site remains (src/tsdev_lib.cpp:2859-2863); leak path confirmed present |
| RC-14 | sources verified (src/dispatch_watchdog.cpp:198, tests/test_path_discovery.cpp:199, src/tsdev_cli.cpp:592 in the printUsage literal); warning set confirmed per Appendix C |
| RC-15 | measured: 7,551 non-ASCII bytes (see Appendix D) |
| RC-16 | verified present (tmp\check_ts_state.ps1:13 -- script lives ONLY in tmp\) |
| RC-17 | verified present (src/tsdev_cli.cpp:5335-5336, :6391-6395, reason :5678) |
| RC-18 | verified present (src/whserver_recycle.cpp:136-146 bypass kill; emission exists only for the return-false paths src/tsdev_cli.cpp:6408-6411, :6803-6816) |
| RC-19 | verified present (src/tsdev_cli.cpp:7884-7887, :7908-7914; contrast :6508-6518) |
| RC-20 | verified present (README.md:161; docs/BUNDLE_README.md:112-119 update-only) |
| RC-21 | verified (gitignore .gitignore:14; all cited tmp\ artifacts confirmed present) |
| RC-22 | verified (2 unpushed commits; ci.yml:41-42 -> run_unit_tests.ps1 inventory :82/:86/:98) |
| RC-23 | verified (no LICENSE file in tree) |
| RC-24 | verified (docs/UPDATE_DESIGN.md:229 vs no update-side pbar code; synthesized message src/tsdev_cli.cpp:9306-9309 absent from spec table :485-507) |
| RC-25 | verified (exp/remote-control head da74dc1, 28 unmerged commits) |
| RC-26 | verified (user_bug_batch_ops_hang_1 head cbfc5d3, 0 unmerged commits) |
| RC-27 | verified present (src/tsdev_automation/results.py:128; the masking behavior is pinned at tests/test_tsdev_client.py:630-636) |
| RC-28 | verified present (src/tsdev_cli.cpp:692-693 struct defaults; unfilled-result emission :4108-4109, :3933-3934; containment :4033, :4051); judged unreachable per the 2026-11-01 review |
| RC-29 | verified present (src/tsdev_automation/client.py:2312-2322 `_serve_active`; 9 routing sites :1475..:2953); documented design, not a defect |

No RC turned out already fixed; RC-13's scope was materially narrowed by
the 2026-11-02 rewrite (entry updated accordingly).
