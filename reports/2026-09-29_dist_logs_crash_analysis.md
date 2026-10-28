# Analysis -- User-machine `tsdev_cli_diag` logs (`dist_logs/`): batch-operation hangs and TS-side crashes

**Date:** 2026-09-29 - **Status:** evidence-complete at the process level; WHServer-side state still open - **Method:** forensic analysis of 21 harvested diag logs + one 1.44 GB full-memory minidump (run 39964) via cdb; cross-run WHServer pid correlation; code/tag mapping of the `TSDEV_DIAG` instrumentation. Confidence labels: **[PROVEN]** (log/dump/code evidence), **[INFERRED]** (best-supported), **[OPEN]** (needs more data).

---

## 1. Executive summary

**The failures are the RCA-predicted stall flavor, now proven on user hardware.** All three failed runs show the identical chain (reports/2026-09-02 RCA Section 1):

```
our batch worker  ->  warehouse cmd dispatch (posts cmd, waits evDone INFINITE)
  ->  orlib20 worker thread  ->  synchronous COM call into WHServer.exe
  ->  reply never arrives  ->  evDone never signaled  ->  eternal 0%-CPU wait
```

Run 39964's minidump captures the complete chain verbatim: batch worker tid 43460 in `WaitForSingleObject(evDone, inf)` at `orlib20+0x1CF5EC` (the `FUN_101cf5b0` dispatch wait); in-flight warehouse cmd **0x35** (the `IProgram` record-fetch behind `_LoadMasterRecsEX`); orlib20 worker tid 47936 inside the cmd-0x35 handler at `orlib20+0x1D2F67` = return address of `CALL [IProgram_vtable+0x24]`, parked in `combase!CCliModalLoop::BlockFn`/`ClassicSTAThreadWaitForCall` -> `NdrClientCall2` -- an outbound RPC to WHServer.exe (pid 48560) that never returned. WHServer was alive (`whserver.sample present=1` every 5 s throughout) but not answering.

**User-visible "crashes" are the TS-side fallout.** User confirms the observed symptom is `TSCrashReport.exe` dialogs for **TSDev, TS Desktop, or both**, and that TSDev/desktop is **usually open** during batch runs. WHServer.exe is a singleton out-of-proc warehouse server shared by every attached process; when it wedges or dies, every client (our CLI AND the open desktop) loses its synchronous COM calls -- producing our hang and the TS-side hangs/crashes/crash reports. The truncated-log failures are consistent with either external kill of our frozen CLI or the worker-thread RPC exception crashing our process; both leave the same truncated signature (the VEH crash path writes no diag record).

**Root cause of WHServer's non-response is [OPEN]** -- it is not determinable from client-side artifacts; WHServer.exe is a separate process absent from our dump. Leading hypothesis (below) is cross-process COM contention/deadlock between our dispatch flood and the concurrently open desktop. Closing it requires a WHServer-side dump captured at hang time (Section 5.1) or the TS-side crash artifacts (Section 5.2).

> **RESOLVED 2026-09-30 -- see "Postscript 2026-09-30" below.** The TS-side crash artifacts (Section 5.2) were harvested and analyzed: WHServer threw an unhandled `std::bad_alloc` at 2047 MB used / 10 MB free of its 2 GB non-LAA VAS at 08:00:36 -- the run-45356 non-response is proven server death-by-VAS-exhaustion, not STA deadlock.

## 2. Census of all 21 logs

| Log (pid) | Date/time | Command | Outcome | WHServer pid |
|---|---|---|---|---|
| 29760 | 09-03 06:00 | upsert x17 | clean exit=0, 35.6s | 9012 |
| 21952 | 09-03 11:30 | upsert x95 | clean exit=0, 123.4s | 9012 |
| 30000 | 09-05 19:36 | upsert x42 | clean exit=0, 69.2s | 9012 |
| 21112 | 09-10 15:49 | `--help` | no-op (ends at run.start) | -- |
| 36496, 6048 | 09-11 06:40 | (no args) | no-op | -- |
| 15460 | 09-11 06:40 | insert x465 | clean exit=0, 674.0s (537MB log) | 27636 |
| 8876 | 09-17 06:52 | `--pbar batch-insert-dir ...` | ended at run.start -- early exit before batch.start (arg/elevation path, not a hang) | -- |
| 4064 | 09-18 02:59 | (no args) | no-op | -- |
| 6036 | 09-18 08:01 | upsert x103 | clean exit=0, 124.6s | 9928 |
| 1092 | 09-19 15:10 | upsert x405 | clean exit=0, 407.4s (357MB) | 9928 |
| 26388 | 09-20 15:12 | upsert x4 | clean exit=0, 21.3s | 19532 |
| 10936 | 09-20 16:39 | upsert x10 | clean exit=0, 25.9s | 19532 |
| 31724 | 09-23 10:13 | upsert x640 | clean exit=0, 821.8s (728MB) | 19532 |
| 38492 | 09-24 03:20 | upsert x85 `run1sb\HISTORY` | clean exit=0, 136.0s (110MB) | 19532 |
| 39388 | 09-24 03:25 | upsert x85 `run1sb\SFB\eval` | clean exit=0, 153.4s (113MB) | 19532 |
| **45356** | **09-24 08:00** | upsert x9 `SFB\eval` (modify) | **HANG -> truncated @65.8s** (killed/died before 60s watchdog) | 19532 |
| 10660 | 09-24 08:04:39 | delete x9 | clean exit=0, 1.26s -- all 9 deleted | -- (too fast) |
| **39964** | **09-24 08:04:58** | upsert x9 `SFB\eval` (create) | **HANG -> watchdog abort exit=13 @84.5s** + 1.44GB minidump | **48560** (fresh) |
| 3104 | 09-24 10:08:09 | delete x9 | clean exit=2, 2.47s (deleted orphan 11882; 8 absent -> per-item exit=5) | -- (too fast) |
| **5528** | **09-24 10:08:25** | upsert x9 `SFB\eval` (create) | **HANG -> truncated @55.5s** (killed/died before watchdog) | **10436** (fresh, post-reboot) |

Deletes are short and never hang (no verify/traverse path). The machine rebooted between ~08:06 and 10:08 (QPC epoch resets; the 10:08 run is ~2h uptime).

## 3. The three failures -- detail

All three are `batch-upsert-dir strategy` on the same eval directory, all wedged **after `SaveModule_New` succeeded**, during phases that issue warehouse worker commands bound for WHServer:

1. **45356 (08:00)** -- item 1 (`S-220M-B10-R1sb-11632-6_SFB`, modify) completed a 26.4s traversal; item 2 (`S-220M-B10-R1sb-2780-5_SFB`, progId 11798, modify): `SaveModule_New ok=1` then `phase.link.enter cat=0 progId=11798 when=pre-verify` @27.97s -- no leave. Hang is inside `linkModule()` (Ord28->Ord69; unlogged inner calls). Truncated at 65.8s (37.8s into stall) -- external kill or silent crash before the watchdog's 60s.
2. **39964 (08:04)** -- item 1 (insert, new progId 11882): verify ok -> post-verify link ok -> `dispatch.cmd0x0D` ok -> `phase.traverse.enter` @1.82s -> reverse-enum scanned cat 0 (2080 recs), cat 1 (83), cat 2 (416), cat 3 (5) -- then `dispatch.LoadMasterRecsEX.enter cat=4` @21.94s never returned. Watchdog fired @82s (60.0s idle): stacks + `watchdog.warehouse` (cmd=53 in flight, evDone not-signaled, mutex held) + minidump -> `TerminateProcess(13)` @84.46s.
3. **5528 (10:08)** -- identical replay after reboot: item 1 (insert, new progId 11883): verify ok -> traverse -> cat=4 `LoadMasterRecsEX` returned count=414 @37.49s -> ~18 Linker/Ord70 pairs -> `dispatch.Linker.enter cat=4 progId=518` @37.86s never returned. Truncated @55.5s (17.7s into stall).

Not a poisoned record: progId 518 linked fine in runs 38492, 39388, and 45356 itself; the stall lands at a different call each time. `ptr=00000000` Linker results are normal (8,670 occurrences in clean run 38492).

## 4. Conditions established

| Condition | Evidence | Status |
|---|---|---|
| Stall (not crash) flavor on 09-24 | WHServer `present=1` throughout all 3 hangs; dump shows pending RPC wait | [PROVEN] |
| Blocked site varies run to run | pre-verify link vs LMRX vs Linker -- different calls, same phase family | [PROVEN] -- timing-dependent wedge |
| Item count not causal | 9-item batches hang; 640-item batch clean; each item triggers a full-library scan regardless | [PROVEN] -- matches user report |
| Fresh WHServer does not prevent it | new pid 48560 hung 21.9s into the run; 10436 hung on a fresh boot (spawn times are not logged -- first sample at ~5s already shows the new pid) | [PROVEN] -- kills the "wedged leftover server" theory (RCA Section 5 candidate 1) as the sole cause |
| Poisoned warehouse residue | delete batch ran clean at 08:04:41, ~4 min after 45356's stall, same boot (WHServer pid unrecorded -- run ended before the first 5s sample; same-boot QPC epoch + lazy singleton lifetime make "probably still 19532" reasonable) | [INFERRED] -- wedge is per-request-timing, not persistent corruption |
| WHServer shared across days and clients | pid 19532 served runs from 09-20->09-24 08:00 incl. the 640-item clean run | [PROVEN] -- lazy-lifetime singleton (E6) |
| Concurrent desktop/TSDev use | user: "usually open"; all 3 failures at 08:00/10:08 (workday); the cleanest big runs at 03:2x (overnight, likely idle desktop) | [INFERRED] -- contention-active-desktop correlation is consistent but not directly logged |
| WHServer-side wedge mechanism | server alive, not answering; in-proc evidence shows no AV/EDR, no injected DLLs | [OPEN] -> **RESOLVED for the 08:00 run by postscript**: WHServer pid 19532 threw unhandled `std::bad_alloc` at 08:00:36 inside a COM blob-getter (selector 0x5DC), then was frozen-but-alive by TSCrashReport -- explaining both `present=1` and non-response. The 08:04/10:08 wedge-only failures remain [OPEN] (postscript) |

**Leading mechanism hypothesis [INFERRED]** *(superseded for the 08:00 run -- see postscript; the actual mechanism was VAS exhaustion, not STA deadlock, though the desktop's contribution to cumulative VAS growth remains plausible)*: the per-item post-verify `cmd 0x0D` dispatch "fires WHServer COM events that cause orchart.exe to recalculate" (code comment, `tsdev_lib.cpp:2150`). With the desktop open, every verified item injects recalc work into WHServer->desktop traffic. If WHServer's request handling serializes on an STA that is blocked delivering an event to a busy/modal desktop STA (or deadlocked on a reentrant call back into WHServer from that delivery), every subsequent client call -- ours and the desktop's -- stalls permanently. This explains: why it only fails when the desktop is up and active; why the hang lands at a random later call (the wedge is already armed when item N's notification churns); and why the desktop crashes too.

An alternative [OPEN] *(for the 08:00 run, superseded -- proven `std::bad_alloc` under VAS exhaustion; see postscript)*: WHServer is merely *busy* on a heavyweight recalc/record job triggered by our verify on the user's large library (2080 cat-0 + 414 cat-4 records) rather than deadlocked; >60s server-side work produces the same signature. The WHServer dump discriminates (blocked-in-send vs busy-execute stack).

## 5. Recommended next steps

1. **Extend the diag build -- capture the other side (highest value, closes the root cause).** On watchdog fire, in addition to the current self-dump: enumerate and minidump **WHServer.exe plus any running TSDev.exe/orchart.exe** (we run elevated; 32-bit self can dump 32-bit WHServer). Also log at run start: WHServer pid + uptime, and presence of TSDev/orchart/tscharts processes. One WHServer stack answers "deadlocked vs busy vs dead" -- the last unknown. Small, contained addition to `diag_watchdog.cpp`'s fire path.
2. **Collect TS-side crash artifacts from the user (zero code).** After the next incident, harvest `C:\Program Files (x86)\TradeStation 9.5\Program\TS_*.tsc` and `TSErrorLog.txt` -- the `.tsc` filename encodes which process crashed (`TS_0_TSDev_*` vs `TS_*_orchart_*` naming observed on the dev machine) and contains the faulting stack, telling us whether the desktop crash is the RPC-failure flavor. Also useful: whether TSDev/desktop was open *and what it was doing* at each failed run.
3. **Fix candidates, ranked** (pending Section 5.1 confirmation):
   a. **Attack the brute-force reverse-enum's exposure surface** -- it issues O(entire-library x items x recursion) WHServer-bound Linker+LMRX calls (~2900 candidates per item on the user's library). Two sub-options, in dependency order:
      - *Verify feasibility of TSDev's native `cmd 0x3B` (IProgram::Callers)* -- `enumerateCallers` already exists (`tsdev_lib.cpp:2669`) and would collapse the scan to one server roundtrip. **Caveat (corrected):** KB Section 9 observed 0x3B return an *empty* SafeArray not only in our CLI but **via the TSDev UI path itself in a WHServer-connected session** -- the caller/dependency table is simply not populated by the insert/compile path. So 0x3B-as-replacement is currently *contradicted by evidence* unless a session state can be found where the dep DB is populated (verify dispatches cmd 0x0F -> the dep DB writer -- gated on `wh+0xAE != 0`; whether that populates the table on the user machine is testable). Treat as a bounded investigation, not an assumed fix.
      - *Skip/bound the scan for strategy roots* -- a cat-0 root's reverse-enum searches for callers of a strategy, which cannot exist (nothing calls a strategy). If TSDev parity tolerates skipping the traversal for cat-0 roots (or capping it), the dominant exposure surface disappears for the failing workload specifically. Requires a parity check of what TSDev FUN_004D91E0's cat-0 traversal actually accomplishes.
   b. **Coalesce per-item notifications** (`cmd 0x0D` post-verify + `0x40A` per item) to once per batch end -- removes the per-item recalc storm into a live desktop, the most likely contention trigger. Deliberate, documented deviation from single-op parity.
   c. **Production containment**: bounded wait on dispatch completion with loud, diagnosed abort (distinct exit code; name the blocked call + WHServer pid) -- converts the eternal silent hang into a clean failure. Implementation note: the INFINITE wait lives *inside* orlib20 (`FUN_101cf5b0` -> `WaitForSingleObject(evDone, inf)`, `orlib20+0x1CF5EC`), so this requires wrapping the dispatch at our layer (e.g. a dedicated dispatch thread + bounded join + abandon-and-terminate on timeout), not changing a parameter. Once a dispatch times out the session is protocol-poisoned; terminate rather than continue.
   d. **Interim operational guidance**: run batches with TSDev/desktop closed (or at least idle) until the fix lands; if a run stalls, let the 60s watchdog complete instead of killing -- it produces the dump we need.
4. **Reproduction path (dev machine):** launch `TSDev.exe` locally (it is installed; unlike orchart it may run standalone) so it attaches to WHServer, then run a large upsert batch against it; watch for the wedge and attach cdb to WHServer.exe at hang time. Never tried -- all dev experiments ran without a concurrent TS client, which is exactly the field difference.

## 6. Ruled out

Leftover-wedged WHServer as sole cause (fresh servers hang); poisoned record (same progIds link fine in other runs); AV/EDR injection in our process (clean module list in dump); stderr backpressure (file sink); item count; stale WHServer residue after the 08:00 hang (a delete batch ran clean ~4 min later on the same boot, WHServer pid unrecorded but likely still 19532 [INFERRED]); orlib20 version drift (user's orlib20 = 9.50.1.3344, matches signatures).

## 7. Evidence artifacts

- `tmp\dump_analysis_39964.md` (+ `dump_stacks.txt`, `dump_details.txt`, `dump_lm.txt`, `dump_runaway.txt`, `dump_handles.txt`) -- cdb analysis of the 1.44GB minidump.
- Log evidence quoted in-line above; tag->code map lives in `src/diag_log.*`, `src/diag_watchdog.*`, and the `TSDEV_DIAG_LOG` sites in `tsdev_lib.cpp`/`tsdev_cli.cpp`.

---

## Postscript 2026-09-30: WHServer-side root cause proven

The Section 5.2 artifacts were harvested from the user machine (`D:\protege_strategy_dev\tsdev_automation_dist_diag\TS_Crash_files`) and fully analyzed in `tmp\whserver_crash_analysis\report.md`. This postscript records the resolved facts; it supersedes the [OPEN]/[INFERRED] items named below without rewriting the dated client-side record above.

### Proven facts (all [PROVEN] unless tagged)

- **WHServer.exe pid 19532 threw an unhandled `std::bad_alloc` at 09-24 08:00:36 -- during run 45356.** `_ThrowInfo` at `WHServer+0x3C41C` decodes via RTTI to `.?AVbad_alloc@std@@` (+ "bad allocation" what-string); the CRT path is `operator new` (`0x42D150`) -> `malloc` NULL -> `_callnewh` -> `__scrt_throw_std_bad_alloc` (`0x42DB87`) -> `_CxxThrowException`.
- **VAS exhaustion at the throw:** report header records `2047 MBytes user address space / 10 MBytes user address space free`. WHServer.exe is 32-bit **non-LARGEADDRESSAWARE** -> hard 2 GB user-VAS ceiling. `malloc` returned NULL because nothing committable remained.
- **The failing allocation was inside an incoming COM handler:** STA dispatch (`user32 DispatchMessage` -> `combase`/`rpcrt4`/`oleaut32`) -> selector `0x5DC` -> `WHServer!0x415D90` -- a binary-blob getter that materializes an item's compiled/encrypted body into `SAFEARRAY(VT_UI1)` (`new[blob_count]` + memcpy in `WHServer!0x40CC80`; the same call was about to allocate a second equal-size block for the SafeArray data).
- **The hang mechanism is proven end-to-end:** TSCrash.dll's unhandled-exception filter spawned TSCrashReport.exe and blocked `WaitForSingleObject(child, INFINITE)` -> WHServer stayed frozen-but-alive (`present=1` for days) -> our client's pending ORPC never returned -> `CCliModalLoop` infinite wait. This is exactly the run-39964 chain (tid 47936) -- WHServer non-response was the frozen corpse, not a live wedge.
- **Client echoes:** the `.28`/`.33` bundles contain minidumps of two TSELAppHost.exe processes (pids `0x48AC`, `0x2AB0`), each mid-throw of `_com_error(0x80010105 RPC_E_SERVERFAULT)` on an orlib20 worker thread -- pending calls into WHServer receiving the marshaled server fault.
- **TSDev.exe crashed 6 s after WHServer** (breakpoint `0x80000003` in the same cmd-0x35 dispatch family -- the `.28` bundle aggregates WHServer+TSDev+TSELAppHost report blocks). The user-observed "TSDev/desktop crashes" are this cascade.
- **`CAL_2026-09-24.Log` shows hundreds of `[storage cache] code #cf-20001 Index checksum does not match` errors at 11:14** -- desktop cache corruption, consistent with the day's session destabilization.
- **[INFERRED -- not attributable]** which client owned the in-flight selector-0x5DC call (ours vs TSELAppHost); both were connected. Also [INFERRED] what filled the 2 GB -- no WHServer dump exists; VAS state is proven only by the self-reported header.

### Open questions from the original report -- resolution status

| Original item | Status |
|---|---|
| Section 1 "Root cause of WHServer's non-response is [OPEN]" | **RESOLVED for 08:00** -- `std::bad_alloc` at 2047/10 MB VAS, frozen by TSCrashReport. Not STA deadlock, not busy-work. |
| Section 4 "WHServer-side wedge mechanism [OPEN] -- needs WHServer dump" | **RESOLVED for 08:00** via crash-bundle evidence (no live dump needed). |
| Section 4 "Concurrent desktop/TSDev use [INFERRED]" | Corroborated: report header lists `orchart.exe`, `TSDev.exe`, and our `tsdev_cli_diag.exe` all attached at 08:00:36. Desktop's contribution to VAS growth remains [INFERRED]. |
| Section 5.1 "dump WHServer at hang time" | Made moot for 08:00; still the discriminating tool for wedge-only failures. |
| 08:04 (run 39964, fresh pid 48560) and 10:08 (run 5528, fresh pid 10436, post-reboot) hangs | **REMAIN [OPEN].** No crash bundles exist for those runs -- both are wedge-only (server alive, ORPC pending). [INFERRED]: plausibly the same cumulative-VAS mechanism manifesting as pre-fatal slowness/wedge rather than a crash -- a server approaching its 2 GB ceiling slows (new-handler retries, fragmented commits) before `malloc` returns NULL. Not proven; 48560 was fresh (cumulative-VAS within 22 s is implausible unless a shared cache/compaction job inflated it), so an alternative mechanism cannot be excluded. |
| Which client's workload exhausted the VAS | **[INFERRED/OPEN]** -- cumulative across all attached clients (ours, TSDev, TSELAppHost, orchart); pid 19532 had served batches since 09-20 plus live desktop traffic. |

### Fix-implication changes

The Section 5.3 candidates stand, with reprioritization: prevention now means bounding WHServer VAS growth (smaller batches, WHServer restart between large batches, monitoring private/commit bytes) plus the production containment of Section 5.3c (bounded dispatch wait -> loud abort). The "deadlock inside STA delivery" theory is retired for the 08:00 failure; coalescing notifications (Section 5.3b) remains useful as VAS-pressure reduction rather than deadlock avoidance.
