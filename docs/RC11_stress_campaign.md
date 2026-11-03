# RC-11 stress campaign record (tracked artifact)

One-off Windows access violation in the fake-tsdev-cli mock-harness thread
(tests/test_tsdev_client.py `_FakeCliProc`, thread "fake-tsdev-cli") seen
once when the full suite ran concurrent with a C++ build; clean 466-pass
re-run in isolation. Goal: stress-reproduce first, then root-cause or
accept as flake. NO production code changed in this slice (reads + test
runs + new log artifacts only).

## Commands

Phase 1 (loaded; script `tmp/rc11_stress.ps1`, launched 15:00:09):

  powershell -ExecutionPolicy Bypass -File tmp\rc11_stress.ps1 -N 12

  Per run: `python -X faulthandler -m pytest tests/test_tsdev_client.py -q
  -p no:cacheprovider -o faulthandler_timeout=180`
  Load: background job looping `MSBuild.exe
  build\tsdev_automation.slnx /t:Rebuild /m /v:m /nologo`
  (full-solution Rebuild, VS 18 BuildTools MSBuild).

Phase 2 (PM narrowed plan; script `tmp/rc11_phase2.ps1`, armed 16:32:43):
wait for `run_02 exit=`, stop phase-1 script + job hosts + load MSBuild +
orphan pytest, quarantine partial loaded run_03, then 8 UNLOADED runs
(`run_03`..`run_10`) with the same pytest command and no parallel build.

PM narrowing (documented in-session): 12-loaded variant rejected as
infeasible (measured >90 min/loaded run => 18+ h). Final plan: 2 loaded +
8 unloaded = 10 counted runs. Owner CPU complaint arrived after the load
phase had already ended (phase 2 stopped load 18:26:25, residual
msbuild=0); the single-project softening order was recorded as moot --
both loaded runs completed under identical full-solution load, no mixed
intensity to label.

## Results (N = 10 counted; 1 quarantined orphan not counted)

| run | condition | exit | status | elapsed | note |
|-----|-----------|------|--------|---------|------|
| 01 | LOADED full-sln Rebuild | 1 | FAIL | 6530 s | 482 pass; 2 live-TS E2E fail (strategy AlreadyExistsError leftover fixture PyAutoTest_Strat; function ArtifactMissingError .ell not found). No crash signature. |
| 02 | LOADED full-sln Rebuild | 0 | PASS | 5763 s | full green |
| 03 | loaded orphan (KILLED, not counted) | - | - | - | `run_03.loaded-orphan.log`; killed by phase 2 at load stop |
| 03 | UNLOADED | 0 | PASS | 1749 s | full green |
| 04 | UNLOADED | 0 | PASS | 2214 s | full green |
| 05 | UNLOADED | 1 | FAIL | 2190 s | 482 pass; 2 live-TS E2E fail (function ArtifactMissingError; indicator NotFoundError PyAutoTest_Ind). No crash signature. |
| 06 | UNLOADED | 0 | PASS | 1648 s | full green |
| 07 | UNLOADED | 0 | PASS | 1630 s | full green |
| 08 | UNLOADED | 0 | PASS | 1652 s | full green |
| 09 | UNLOADED | 0 | PASS | 1644 s | full green |
| 10 | UNLOADED | 0 | PASS | 1642 s | full green, 484 passed |

Crash count: 0 in 10 counted runs. No `Windows fatal exception`,
`access violation`, or `0xC0000005` text in any log; no exit
-1073741819; no `.CRASH.log` artifact produced. faulthandler_timeout=180
dumps: none fired.
Suite green at end: YES (`run_10`: 484 passed).

The two FAILs are the same signature: live-TradeStation `TestE2E`
lifecycle tests touching the real warehouse (leftover-fixture /
timing artifacts), present with and without load, unrelated to the
mock-harness thread. Not the RC-11 AV.

## Conclusion: NO-REPRO -> accepted flake

10 runs (2 under full-solution Rebuild pressure, 8 clean) produced zero
access violations. Statically, no deterministic repro path exists in the
tree: every `_FakeCliProc` child thread is provably bounded (10 s stall
caps on `_child_readline`/`_push`/`_kill`) and 22/23 harness installs
assert end-of-call reap (`_assert_proc_clean`); pure-Python threads
cannot raise C0000005 mid-run. Remaining suspect, unprovable from the
tree: a CPython daemon-thread-at-interpreter-shutdown teardown race
(`fake-tsdev-cli` + production drain threads are `daemon=True`, joined
only with timeouts) stretched by CPU starvation from the parallel
Rebuild. No fix is proposed for implementation in this slice beyond the
hardening sketch below; the fixer is a separate TVS.

## Fix proposal (PROPOSED ONLY, not implemented)

Make mock-harness teardown synchronous: give `_FakeCliProc` an explicit
`close()` that closes pipes, signals abort, and JOINS the child thread
without timeout (fail loudly on timeout instead of leaking), call it
from `__exit__` and from a session/fixture finalizer covering all
spawned procs, and drop `daemon=True` once the join is guaranteed
(non-daemon threads block interpreter exit instead of dying inside it).
Mirror on the production side: verify drain-thread joins in
`_run_batch_controlled` (normal path joins with timeout=10 but does not
assert death; timeout path joins with timeout=5 and proceeds regardless).

## Retry-guard proposal (PROPOSED ONLY, not implemented) + no-masking adjudication

Rule: on a Windows fatal-exit signature ONLY (process exit
-1073741819 / `Windows fatal exception` on stderr -- i.e. NO test
verdict was produced), re-run the single affected test file once in a
fresh unloaded process and take that result; any second crash, any
assertion failure, error, or hang fails loudly. Full crash stderr is
preserved as an artifact. Never a blanket pass; counter logged.

No-masking case (AGENTS.md): a fatal AV yields no pass/fail verdict, so
there is nothing to convert -- the guard cannot turn a real defect into
a pass. It triggers exclusively on interpreter death (no verdict),
retries exactly once minus the suspected load trigger, and any
recurrence or ordinary failure still fails. Assertion failures, errors,
and hangs never qualify. It distinguishes "no verdict (process died)"
from "verdict: fail" without suppressing either. Adjudication of this
case belongs to TVS review before any implementation.
