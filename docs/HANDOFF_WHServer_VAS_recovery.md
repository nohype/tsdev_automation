# Hand-off -- WHServer VAS-exhaustion recovery (2026-10-18)

Operational hand-off for continuing the `whserver_vas_free` recovery work.
Every experiment-derived claim is marked **(PROVEN)** / **(INFERRED)** /
**(UNTESTED)**. Primary ledger: `docs\WORKING_MEMORY.md`; durable facts:
`docs\KNOWLEDGE_BASE.md` Section 15/Section 17/Section 18; experiment evidence:
`reports\2026-10-18_whserver_recovery_research.md`.

## 1. Problem statement

Users running batches hit:

```
BATCH COMPLETE ABORTED ... abort_reason=whserver_vas_free=NNNMB ... exit_code=14
```

`WHServer.exe` -- the shared out-of-proc warehouse server -- is 32-bit
non-LARGEADDRESSAWARE with a hard 2 GB user-VAS ceiling (PROVEN). When its
free address space drops below the guard thresholds the batch aborts loudly
before the next item. The only remedy users have today is **restarting TS
Desktop**, which interrupts live trading / long optimization runs. Goal:
recovery and/or prevention **without** a desktop restart.

Two facts that shape everything:

- **The tool never crashes -- it only aborts.** The guard works correctly;
  the abort is a protective tripwire. The server is still functional at
  ~251 MB free (user-observed: TSDev.exe inserts fine against the same
  server right after our abort) (PROVEN, user-reported).
- **The server arrives at batches already exhausted.** Field aborts hit
  servers carrying ~1.7 GB of prior multi-client fill accumulated over
  days; our own per-op VAS cost is ~ 0 (PROVEN -- see Section 2.5). The fill
  mechanism itself is cumulative aging + fragmentation, unattributed
  (INFERRED).

## 2. Key proven facts

### 2.1 Server model

| Fact | Status |
|---|---|
| WHServer.exe = out-of-proc COM singleton, spawned ON DEMAND by DCOM SCM (`-Embedding`, parent `svchost -k DcomLaunch`), shared by ALL orlib20 clients. | PROVEN |
| 32-bit, non-LAA (`CHARACTERISTICS 0x0103`), 2 GB user-VAS ceiling; on disk `Program\WHServer.exe`, 295,368 B; loads `orgcom20.dll`+`orshr20.dll`, NOT orlib20. | PROVEN |
| WinMain: `CoRegisterClassObject(REGCLS_MULTIPLEUSE)` -> msg loop; MonitorShutdown thread exits after 5 s of `m_nLock==0` -- idle-exit is lock-driven, not a timer. | PROVEN (Ghidra) |
| Lingers ~24.5 min after last client (held-lock rundown); transient servers self-reap <17 s idle; broker-activated server lingered 50 s+ after broker death. | PROVEN (live) |
| After FULL death, next activation SCM-respawns a fresh server: ~0.4 s to serving; fresh server = **1899 MB free**; broker-warmed settles ~1105 MB (orlib20 eager-commits ~1 GB at attach). New client processes provably get fresh servers (x4+). | PROVEN |
| A TSCrashReport-FROZEN zombie still enumerates and gets reactivated -- must ensure full death before reactivation. | PROVEN |
| Field crash signature: `std::bad_alloc` at 2047 MB used / 10 MB free, thrown inside an incoming COM blob-read handler (selector `0x5DC` -> `0x415D90` -> `new[blob_count]`+memcpy at `0x40CC80`, needs ~2x blob bytes). | PROVEN (KB Section 15) |

### 2.2 Guard mechanics (shipped, `src\whserver_health.*`, `src\tsdev_cli.cpp`)

| Fact | Status / Ref |
|---|---|
| `measureVasFree` = `VirtualQueryEx` MEM_FREE walk < `0x80000000` via `PROCESS_QUERY_INFORMATION`; sums total free + largest contiguous region. | PROVEN -- `whserver_health.cpp:30-71` |
| Defaults: total free < **256 MB** OR largest block < **64 MB** -> breach (strictly-below semantics; equal = pass). | CODE-VERIFIED -- `tsdev_cli.cpp:4303-4307`, `vasBreach` `whserver_health.h:70` |
| Checked pre-flight + between batch items (`runBatch` `tsdev_cli.cpp:4495-4556, 4644-4647`); serve checks before each MUTATING request only (`serveVasGuardRefuse` `tsdev_cli.cpp:5557-5620`). Read-only ops unguarded -- caveat: once VAS is ~exhausted ANY server-side allocation can fail at a random later call (KB Section 15); `exists` churns ~5.6 MB server-side (Section 2.5), so an unguarded read on an exhausted server can provoke the crash cascade itself. | PROVEN |
| Breach -> abort event + remaining items become exit-12 not-run markers + **exit 14** (`whserver_vas_abort`); `abort_reason=whserver_vas_free=NNNMB` or `whserver_vas_largest_free=NNNMB`. Serve returns exit-14 refusal, `error.code "whserver_vas_abort"`. | PROVEN |
| WHServer not running (lazy singleton) = pass; transient measure failure warns once, does NOT abort. | PROVEN |
| `close_failed` in `BATCH COMPLETE` = end-of-batch `closeWarehouse()` (`_CloseWarehouseForCurrentThreadAndPath@4`) threw SEH; all attempted successes rewritten to exit 6. | PROVEN -- `tsdev_cli.cpp:4666-4685` |
| Shipped flags: `--whserver-free-mb`, `--whserver-largest-free-mb`, `--no-whserver-vas-guard`; env mirrors `TSDEV_WHSERVER_FREE_MB` / `TSDEV_WHSERVER_LARGEST_FREE_MB` / `TSDEV_NO_WHSERVER_VAS_GUARD` (any value but "0"); explicit flags win. Python client forwards env -> explicit flags to the runas broker (runas does not inherit env). Residual-risk caveat: loosening permits continued ops at ~250 MB free -- residual risk = commit bursts + other clients' allocs (WORKING_MEMORY shipped-mitigations line); `--no-whserver-vas-guard` removes the ONLY protection against the bad_alloc -> frozen-zombie -> desktop-client-death cascade (KB Section 15). | CODE-VERIFIED -- `tsdev_cli.cpp:337-348, 6228-6255` |
| Dispatch watchdog: `--dispatch-timeout-secs` / `TSDEV_DISPATCH_TIMEOUT_SECS`, default 300, 0 disables, exit 15 (`whserver_dispatch_watchdog_abort`). | CODE-VERIFIED -- `tsdev_cli.cpp:355-359, 6267-6279` |

### 2.3 In-place maintenance surface -- dead ends and foundations

Release/exit are dead ends; `FlushCache` and `closeWarehouse` are not --
they are the P1/P2 foundations.

| Fact | Status |
|---|---|
| `ReleaseRemainingPrograms` (dispid `0x30`) releases every cached record but reclaimed **+0.00 MB** committed VAS even with ~17 MB warm cache (142 ops): WHServer uses CRT malloc/free only (no `HeapCreate`/`VirtualAlloc` imports); freed objects stay in committed heap pages. **In-place release is a dead end.** | PROVEN -- `tmp\wh_call_run_warm*.txt` |
| `ForceShutDown` (dispid `0x2B`) returns S_OK but is **INERT under running desktop**: PID stayed alive >=7.5 min still serving new activations -- attached clients keep re-locking so the 5 s zero-lock window never occurs. Graceful exit is unreachable while any client calls. | PROVEN -- experiment aa7b40c6, `tmp\exp_watch.log` |
| `FlushCache` (0x18), `ClearDependencyLists` (0x20), `ClearVerifyFlagByType` (0x1F, VT_I2 type arg) all callable, all S_OK on live server. | PROVEN -- `tmp\wh_call_run*.txt` |
| No reconnect/purge API in orlib20; ZERO reconnect code -- dead server = permanently broken `CELWHouse` until close. COM proxies die permanently. | PROVEN (Ghidra) |
| `closeWarehouse` + next-op = the full reconnect path (fresh `CELWHouse` -> `CoGetClassObject` -> SCM respawn). BUT in-process reuse after close is unsafe (null-deref in cmd `0x4E`) -> reconnect effectively means a NEW PROCESS or broker restart. **Open hypothesis: a fresh worker THREAD may re-activate in-process** -- activation is per-(thread,path) (UNTESTED). | PROVEN / UNTESTED |

*Lock caveat (live 2026-10-21, KB Section 21): `closeWarehouse` does NOT release the process's +7 `m_nLock` refs -- only process exit does; a TerminateProcess'd client orphans them >=5.6 min. The shipped recycle relies on this: attached sites close then exit GRACEFULLY (Section 8).*

### 2.4 Kill + SCM respawn -- the only real VAS reset

| Fact | Status |
|---|---|
| `taskkill /F WHServer` -> dead <1 s, no TSCrashReport child (clean kill); fresh server ~0.4 s later, 1899 MB free (+~900 MB reclaimed). | PROVEN -- experiment 3b54c53d |
| Zero of 15 watched TS client processes died in a 7 min watch -- **BUT** the user observed a real **~2 min TS Desktop UI stall** matching the kill window (ORPC-blocked clients). Process-liveness "zero casualties" undercounts functional blast radius. | PROVEN |
| Attached callers die on their NEXT COM call after server death (`RPC_E_SERVERFAULT 0x80010105`; TSDev.exe crashed 6 s post-death in field evidence) -- latent, event-driven risk; not reproduced in the watch (no client called in-window). | PROVEN (field) / UNTESTED (repro) |
| Idle never-calling clients' fate = open question. | UNTESTED |

### 2.5 Mutation/commit model (WHServer-side Frida traces)

| Fact | Status |
|---|---|
| Mutations fire ZERO server-side mutation calls: records live in `SHARE_WO` shared mem + client-written `.ell`/`.xml`; WHServer only sees the deferred catalog `WriteRecord`. `BlobPut`/`ReadRecord`/`RemoveProgram` never fired. | PROVEN -- `tmp\WH_TRACE_FINDINGS.md`, `tmp\BIG40K_TRACE_FINDINGS.md` |
| **Single insert** -> in-session commit ~0.7 s: `CreateProgram -> BlobGet -> WriteRecord(own ProgId) -> FlushCache -> BlobGet -> ReleaseRemainingPrograms` (5/5 in the 40 KB run). **Counter-data-point:** in the earlier small-item (~13-byte source) run the single insert did NOT commit in-session -- it deferred like batch. Mechanism unknown -- possibly insert's own verify/close path flushes, or a per-session vs batch-transaction distinction; cache/dirty state may differ (run-2 server was warm). | PROVEN both observations; mechanism INFERRED -- `tmp\BIG40K_TRACE_FINDINGS.md:152-156` |
| **Batch-insert / upsert / delete** -> dirty backlog, ZERO commits in own session; the burst lands in the NEXT live session (observed ~72 s after batch end); ~0.6 s/record under tracing, `BlobGet`x2/record at the **proven crash site** (0x5DC blob getter). **Guard-invisible, lands in foreign sessions.** *Deferred-delete claim superseded -> Section 7: deletes commit synchronously in-session via RemoveProgram (tested conditions).* | PROVEN (delete portion superseded -> Section 7) |
| Commit-burst gating mechanism = "~60-70 s dirty-age threshold + active-session requirement"; alternative (untested): "fixed periodic flush gated to sessions". Either way: deferred commits do NOT fire on a timer with no client. *Superseded -> Section 7: per-record ~60-75s and youngest~60s models both falsified; session-held requirement falsified (cell A drained post-release); gating mechanism UNRESOLVED.* | INFERRED -- `tmp\BIG40K_TRACE_FINDINGS.md:145-151` (superseded -> Section 7) |
| Server self-manages cache (own `ReleaseRemainingPrograms`/`FlushCache` on commit cycle). Per-op net retained ~ 0 -- ucrtbase live-bytes floor ~1-2 KB between sessions; ~+0.085 MB/item committed ratchet bookend. | PROVEN |
| Per-item VAS slope ~ 0 MB/item (+/-1.25 MB noise over 100 ops); ~30 MB one-time first-contact reservation per fresh server. Our 322-item batch CANNOT fill a fresh server. | PROVEN -- experiment 8c98b55e |
| `exists` = ~150 whole-category server enum calls + ~5.6 MB transient churn; get-source reads are client-side shared-mem (NO server `BlobGet`). **Safety caveat:** exists is unguarded (read-only), yet it churns ~5.6 MB server-side -- once VAS is ~exhausted ANY server-side allocation can fail at a random later call (KB Section 15), so an unguarded exists on an exhausted server can itself trip the bad_alloc -> frozen-zombie -> client-death cascade. P1's exists settle-probes must be guarded/monitored. | PROVEN |
| Field 2 GB exhaustion = cumulative multi-client aging + heap fragmentation over days + big-library transient spikes -- NOT per-op retained cost. | INFERRED (unattributed; no multi-day measurement) |
| User fact: TSDev UI automations insert 500-1000+ strategies/session with no VAS problems; "volume" hypothesis dead. TSDev-side per-op server cost UNMEASURED -- Frida-trace a real TSDev insert = open offer from user. | PROVEN (user) / UNTESTED |

### 2.6 COM IDs + vtable offsets (direct calls -- copy verbatim)

From `docs\WORKING_MEMORY.md` + KB Section 17 (all PROVEN live):

```
CLSID_WHouse   {7CEEA907-D59E-11D3-8394-00C04F7BDF10}  NOT directly registered (REGDB_E_CLASSNOTREG)
CLSID_Control  {7CEEA90C-D59E-11D3-8394-00C04F7BDF10}  CoCreateInstance = REGDB_E_CLASSNOTLICENSED; use factory path
factory IID    {7CEEACBC-D59E-11D3-8394-00C04F7BDF10}  (NOT {7CEEAEBC} -> E_NOINTERFACE)
IID_IControl   {7CEEA90A}   IID_IWHouse {7CEEA903}   LIBID typelib {7CEEA906} (embedded .rsrc, IWHouse cFuncs=58)

Activation chain (orlib20's own, replicated bit-identically):
  CoGetClassObject(CLSID_Control, CLSCTX_ALL, IID {7CEEACBC}) -> factory
  factory vtbl+0x20: HRESULT fn(this, u4 a0=0, u4 a1=0, const IID* riid,
      BSTR name, void** outObj, BSTR* outName)
      riid=&IID_IControl; name = "<268435478>,<ror(((GetTickCount>>12)&0xFFFF|0x160000)^0x1256abef,6)>"
      (first field = constant 0x10000016; returns IControl + out-name "268435479,.")
  IControl::WHouseEx (dispid 0x01, DISPATCH_PROPERTYGET, (BSTR DirName, I4 Create)->VT_DISPATCH) -> IWHouse
      (equivalent: QI(IID_IWHouse) on the returned Control object)
  Activation is once per (thread,path) for process life; proxies are STA-bound
      (cross-apartment QI -> RPC_E_WRONG_THREAD). orlib20 worker slots:
      wh+0xAA = IControl, wh+0xAE = IWHouse, wh+0xC6 = CELWHouseEvents sink.

IWHouse:   member            dispid   vtable offset
           FlushCache          0x18      0x80
           ClearDependencyLists 0x20     0xA4
           ClearVerifyFlagByType 0x1F     --        (VT_I2 = warehouse category)
           ForceShutDown       0x2B      0xD0
           ReleaseRemainingPrograms 0x30  0xE4

orlib20 reachability: FlushCache via VerifyModule_New/InstallVerifyAllEx/
InstallTransfer; ClearDependencyLists via Ordinal_49; ForceShutDown = dead arm;
ReleaseRemainingPrograms only at closeWarehouse teardown.
Activation probe: orlib20 export IsWHRegistered (ord 554).
```

### 2.7 Census gate -- UNSOLVED (superseded -> Section 7: solved mechanically via `m_nLock`)

- 11 of 13 TS-name-matched processes held orlib20/OREL modules
  (ORPlat, ORDllHst, TSScanner, 8xorchart) yet were idle-detached;
  **module-loaded != COM-attached** (lazy activation). The desktop still
  froze during the kill window -> clients did call in-window. (PROVEN)
- `IWHouse::SetProcessId`/`DumpProcessAttached` are STUBS -- **no
  server-side PID table** exists. `TSWAREHOUSEMUTEX` = likely "WHouse in
  memory" token (INFERRED). (PROVEN: Ghidra)
- A fail-closed "no foreign clients" census is likely inert on field
  machines where ~11 module-holders always run. Active-attachment
  detection is the open problem (UNTESTED).
- Candidate attack vectors (none proven; screening them IS the first
  census TVS): (i) **server-side lock-activity observable** -- attached
  clients keep re-locking `m_nLock` so the 5 s zero-lock window never
  occurs (Section 2.3, PROVEN); probing whether a zero-lock window ever opens
  (or whether `ForceShutDown` takes effect) approximates "actively
  attached"; (ii) **RPC/ALPC endpoint enumeration** of WHServer's ORPC
  bindings; (iii) **OXID/object state** on the server's exported COM
  objects; (iv) **Ghidra connection-tracking entry points** -- the
  server's lock counter and connection plumbing must expose something
  readable.

### 2.8 Desktop-restart caveat

WHServer lingers ~24.5 min after the last client (lock rundown), so a
desktop restart may NOT reset the server promptly -- users relaunching
fast reattach to the same exhausted instance. Verify a **new WHServer
PID** before assuming the restart worked. (PROVEN)

## 3. Plan (recommended execution order)

*Ordering superseded -> Section 7 (P1 FlushCache drain falsified; P2/P3(c) merged
into one recycle-engine artifact; revised next steps in Section 7).*

### P1 -- Deferred-commit drain (FIRST TVS CANDIDATE) -- superseded -> Section 7 (FlushCache drain falsified)

Test whether `IWHouse::FlushCache` (dispid `0x18` / vtable `0x80`, proven
callable via the direct activation chain) drains the dirty backlog
**in-session** post-batch. If insufficient: chunked commits or a bounded
wait-for-idle after batch close. Coverage caveat: the deferral vector
may not be batch-only -- in the earlier small-item trace a SINGLE insert
deferred too (Section 2.5), so drain efficacy must be verified for single ops,
not just batch-end. Probe safety: the exists settle-probes used to
observe the drain are unguarded read-only ops -- on an exhausted server
their ~5.6 MB churn can itself trip the cascade (Section 2.5); guard/monitor
them.

**REASON:** our batch/upsert mutations leave a dirty backlog that
commits ~0.6 s/record in a burst inside the NEXT live session -- the
proven `BlobGet`x2/record crash site -- landing in *other clients'*
sessions, invisible to our guard. Draining in-session removes our
poisoning vector AND reduces our contribution to the cumulative aging
that later trips our own guard. This is the smallest end-to-end
verifiable slice: call FlushCache via the Section 2.6 chain (or via orlib20's
`VerifyModule_New`/`InstallVerifyAllEx` reachability) at batch end, then
Frida-trace to confirm WriteRecord fires in-session instead of deferring.

### P2 -- Gated auto-recycle

On guard trip AND a fail-closed census showing no foreign TS clients:

1. `closeWarehouse` with a bounded wait (if it hangs, the server is
   wedged -> kill unwedges it);
2. kill WHServer; capture TSCrashReport evidence BEFORE killing zombie
   children (a frozen zombie gets reactivated -- ensure full death);
3. wait-for-full-death;
4. resume via fresh activation -- new process or broker respawn. Open
   hypothesis: a fresh worker THREAD may re-activate in-process since
   activation is per-(thread,path) (UNTESTED).

**REASON:** the only real VAS reset (+~900 MB measured). Proven safe
ONLY with zero foreign clients. Needs: batch-resume plumbing, and
UAC-free broker self-respawn -- the current broker spawn (per-PID pipe +
deleted handshake + UAC'd `start()`) produces a UAC prompt per recycle
cycle. Blocked by the unsolved census gate (Section 2.7) -- do not ship the gate
on module-load alone.

### P3 -- With desktop attached: honest options only

Keep the abort, with better diagnostics: list running TS clients, and
offer the user: (a) close desktop -> auto-recycle, (b) defer the batch to
a headless window, or (c) explicit opt-in `--recycle-whserver` force
flag accepting the documented ~2 min desktop UI stall + latent crash
risk **in the user's desktop applications** -- attached callers die on
their next warehouse call (field evidence: TSDev.exe crashed 6 s
post-server-death; Section 2.4). The victim is the user's platform, not this
tool -- required for informed consent.

**Dependency mapping:** (a) requires P2's recycle engine -- unbuilt and
census-blocked. (b) scheduling only -- no engine dependency. (c) needs
the recycle engine (shared with P2, unbuilt -- kill -> capture
TSCrashReport evidence / ensure full death -> respawn -> batch resume)
but NOT the census gate -- the census blocker does not block P3(c).
Do not implement `--recycle-whserver` as "continue past the guard".

**REASON:** no silent auto path is defensible -- hard-kill under a
running desktop provably stalls a live trading platform (PROVEN, user
fact 3). The user must own that trade-off.

### P4 -- Robustness gaps to close regardless of plan choice

- (a) **Serve path passes the guard when WHServer is gone/zombie** -> the
  next dispatch hangs into a dead session. The shipped dispatch watchdog
  (default 300 s, exit 15) bounds the hang -- the gap is the missing
  PRE-dispatch health check (existence/liveness), not just VAS.
- (b) **Guard can trip on a frozen corpse** where `closeWarehouse` would
  hang -- corpse check before close.
- (c) **Exiting with an open session** risks blocking on the "WHouse in
  memory" detach dialog -- under the broker the dialog is `SW_HIDE` =
  invisible deadlock. Recycle paths must close-or-terminate cleanly.
- (d) **Capture TSCrashReport evidence before killing zombie children**
  -- otherwise the zombie gets reactivated and the crash data is lost.

### Optional follow-ups (lower priority)

- Live-trace ONE real TSDev insert via the user's own UI automation
  (offer already extended) -> direct per-op comparison vs our op's
  server-side signature.
- VAS-trend/decay-rate guard signal instead of a static threshold.
- Long-horizon VAS logging on a real user machine for fill attribution.

## 4. Constraints & cautions

- **Implementation scope:** resolved -- gated recycle shipped behind
  `--recycle-whserver` and live-verified 2026-10-21 (Section 8).
- **LIVE-VERIFICATION FREEZE ON THIS MACHINE (Section 7 incident).** The
  2026-10-19 experiments destabilized the live trading platform (desktop
  crash -> unrecoverable cache corruption; terminal crashed 2x; machine
  rebooted). NO further live verification (E2E gate, census-on-desktop,
  recycle testing) on this machine without a non-live/test machine or
  EXPLICIT user authorization. Earlier note: our taskkill already caused
  a real ~2 min malfunction; recycle testing must run only when the user
  confirms the desktop is closed. **Update 2026-10-21:** the user
  authorized a desktop-closed window; the gated recycle was live-verified
  end-to-end (Section 8). Live recycle tests STILL require the desktop closed --
  foreign clients hold `m_nLock` (KB Section 21); the freeze otherwise stands
  for new live experiments.
- Direct `tsdev_cli` requires an elevated shell (`isElevated()` -> exit
  11). For UAC auto-elevation tests, temporarily disable UAC prompts
  before running unelevated (see AGENTS.md).
- Check for TS error/crash dialogs and `TSCrashReport.exe` after EVERY
  TS-touching action (standing rule); record popup text + corrective
  action.
- **Frida instrumentation itself inflates WHServer VAS** -- the buffered
  `send()` queue for ~644 k trace lines drove a false 1067->231 MB drop;
  the server self-healed post-detach. Never treat instrumented VAS
  numbers as server truth; measure deltas, detach, re-measure. (PROVEN)
- PowerShell 5.1 quirks: no `[ulong]` -- use `[uint64]`;
  `[UIntPtr]::new([uint64])`; `0x80000000` parses negative -- use decimal
  2147483648.
- PM rules apply (AGENTS.md): small atomic subtasks, `subagent_general`
  default, adversarial review of substantive work, keep
  `docs\WORKING_MEMORY.md` as the live ledger.
- Never parse source/comments for metadata; stored names/ProgIds come
  from orlib20 records only.

## 5. Evidence & artifact index

### Core references

| Path | Content |
|---|---|
| `docs\WORKING_MEMORY.md` | Complete session ledger -- primary source; all experiment IDs + corrected Tier-1 verdict |
| `docs\KNOWLEDGE_BASE.md` Section 15/Section 17/Section 18 | Crash mechanism, COM activation chain, lifecycle/recovery facts (KB is ANSI -- read with `-Encoding Default`) |
| `reports\2026-10-18_whserver_recovery_research.md` | Experiment evidence table (claims a-g) + artifact list |
| `tmp\WH_TRACE_FINDINGS.md` | Server-side impl-call trace, tiny (~13-byte source) items: deferred-commit discovery |
| `tmp\BIG40K_TRACE_FINDINGS.md` | Server-side trace, 40 KB items: in-session vs deferred commit, instrumentation-VAS caveat |
| `src\whserver_health.cpp` / `.h` | VAS measurement + `vasBreach` |
| `src\tsdev_cli.cpp` | Guard wiring: comment block `:135-154`, usage `:337-348`, abort/render `:4249-4322`, batch gate `:4495-4556,4640-4647`, close rewrite `:4660-4685`, serve gate `:5557-5620`, flags `:6003-6140`, env `:6228-6280` |

### Experiment tooling (`tmp\`)

| Artifact | Content |
|---|---|
| `wh_call.cpp`/`.exe`, `wh_call_run.txt`, `wh_call_run_warm.txt`, `wh_call_run_warm2.txt` | Standalone COM client + 3 live runs (cold/warm/warm2); ReleaseRemainingPrograms +0.00 MB proof |
| `wh_shutdown.cpp`/`.exe`, `build_shdn.bat` | ForceShutDown probe (inert under desktop) |
| `wh_com_trace.js`, `run_trace.py`, `wh_com_trace.log`/`.out`, `run_stdout.txt` | Frida capture of orlib20's real activation chain |
| `wh_impl_trace.js`, `run_wh_trace.py`, `run_wh_big.py`, `hold_whserver.ps1` | 135-hook impl-call tracer + drivers + paused-batch keep-alive harness |
| `trace_cli_insert.log`, `trace_cli_insert2.log`, `trace_analysis.txt`, `dump_op_seq.py`, `analyze_trace.py` | small-item (~13-byte source) trace runs + analysis |
| `trace_big40k2.log` (644 k lines), `big40k2_analysis.txt`, `analyze_big.py`, `probe_boundary.ps1`, `gen_big_probes.py`, `TSDEVBIG00-29*.el`, `trace_big40k_run1_failed_compile.log` | Realistic-size trace + analysis + VAS sampler + probe generator |
| `vas_probe_report.txt`, `vas_probe_run.ps1`, `whs_vas.ps1`, `whs_step3.ps1`, `census_detail.ps1`, `drift_check.ps1` | Census (247 procs) + per-op VAS delta harnesses |
| `exp_snapshot.ps1`, `exp_pre.txt`, `exp_post.txt`, `exp_watch.ps1`, `exp_watch.log` | ForceShutDown experiment snapshots + 287 s watch |
| `exp_watch_kill.ps1` | taskkill blast-radius watch script (**output NOT persisted** -- figures live in WORKING_MEMORY) |
| `exp_serve.py`, `serve_ready.flag`, `serve_go.flag`, `pipe_probe.py`, `exp_probe.py` | Serve-broker end-to-end + post-kill pipe probe (outputs not persisted) |
| `gen_warm.ps1`, `gen_warm_del.ps1`, `warm_insert_manifest.txt`, `warm_delete_manifest.txt`, `warm_getsrc.ps1`, `warm_real.ps1`, `TSDEVWARM00-59.el`, `real_items*.txt`, `find_real_*.ps1`, `measure_3x.ps1`, `bin_snapshot_before_warm.txt`, `bin_snapshot_after_warm.txt` | Cache warm-up tooling (~17 MB warm cache) |
| `gen_slope.ps1`, `slope_run.ps1`, `slope_run_log.txt`, `slope_ins*/ups*/del*`, `slope_progids.txt`, `TSDEVSLOPE00-99.el` | Per-item VAS slope experiment (8c98b55e): ~0 MB/item |
| `orlib20_exports.txt`, `tlbdump.cpp`/`.exe` | Export listing + typelib dump tooling |
| `TSDEVVASPROBE.el`, `probe2.el` | Probe EasyLanguage sources |
| `tmp\whserver_crash_analysis\report.md` | **Referenced by KB Section 15 but absent in this tree** -- crash analysis lives in `reports\2026-09-29_dist_logs_crash_analysis.md` |

## 6. Open questions

1. **Active-attachment detection** -- RESOLVED -> Section 7.2 (m_nLock). How to
   prove no foreign client is
   COM-attached (not just module-loaded) given no server-side PID table?
   Census gate unsolved; fail-closed likely inert on machines where ~11
   holders always run. Candidate vectors listed in Section 2.7 (lock-activity
   observable, RPC/ALPC endpoints, OXID state, Ghidra connection
   plumbing); screening them is the first census TVS.
2. **FlushCache drain efficacy** -- RESOLVED -> Section 7.1 (falsified). Does
   `IWHouse::FlushCache` actually
   drain the dirty backlog in-session (WriteRecord fires now, not
   deferred)? First TVS experiment. Mechanism uncertainty (Section 2.5):
   under the alternative "fixed periodic flush gated to sessions" model
   a bounded wait may NOT drain -- a null result must not be misread as
   "FlushCache ineffective"; and drain must be verified for single ops
   too (earlier small-item run: single insert deferred).
3. **Idle-client survival of server death** -- does a never-calling
   attached client survive a WHServer kill? Blast radius is
   event-driven; untested.
4. **Multi-day fill attribution** -- which client/workload fills 2 GB
   over days? Needs long-horizon VAS logging on a real user machine.
5. **Fresh-thread in-process re-activation** -- can a NEW worker thread
   re-activate the warehouse in-process after `closeWarehouse`
   (activation is per-(thread,path)), avoiding the process/broker
   restart? Untested.
6. **Real-size field behavior vs probes** -- do production-size items and
   pacing change the commit/slope picture? TSDev-side per-op server cost
   still unmeasured (Frida trace of a real TSDev insert = pending user
   offer).

## 7. Execution results (2026-10-19/20)

Supersedes the stale parts of Section 2.5/Section 2.7/Section 3 (marked inline). Detail lives
in the cited artifacts -- all findings docs below were adversarially
APPROVED: `tmp\P1_FLUSH_DRAIN_FINDINGS.md`,
`tmp\P1R2_BACKLOG_FINDINGS.md`, `tmp\P1R2B_FINDINGS.md`,
`tmp\P1R3_FINDINGS.md`, `tmp\P2_CENSUS_LOCK_PROBE.md`,
`tmp\P2_DESKTOP_WATCH.md`.

### 7.1 P1 outcome -- FlushCache drain FALSIFIED

- `IWHouse::FlushCache` cannot serve as an in-session drain lever:
  dispatch is synchronous only when the server worker is idle (ar2:
  ~2 ms); queued ~50 s when mid-churn (arm G); unobserved on dying
  servers. When a backlog exists the worker is busy; when idle nothing
  is pending. (PROVEN)
- Drain is AUTONOMOUS on housekeeping pulses while the server instance
  is alive -- held session NOT required (cell A drained post-release;
  n=1). (PROVEN, single observation)
- Deferred-commit model REVISED: single-op creates commit early
  (~4-26 s post-exit pulse or in-op); modifies + batch-ops gate at
  ~60 s+ dirty-age. Gating mechanism UNRESOLVED -- falsified: per-record
  ~60-75 s, youngest~60 s aggregate (cell C drained w/ youngest ~49 s),
  cadence varies ~6x (19.7-116.1 s standalone gaps). Pattern observed:
  drains run in op-free windows; "drains defer while op in flight" =
  untested alternative. Deletes commit synchronously in-session
  (RemoveProgram) -- contradicts Section 2.5's deferred-delete claim (superseded,
  scoped to tested conditions). (PROVEN/INFERRED per findings docs)
- Cross-run variance UNEXPLAINED: same CLI binary commits in-session
  some days, defers others (server-state dependent; warmth correlates
  ~13-16 min uptime but not isolated). (INFERRED)
- Remaining P1 lever = bounded post-batch wait holding session open
  (~75-90 s) so our backlog drains inside our guarded session -- UX cost,
  untested as product shape.
- Instrument validated: `tmp\wh_flush.exe` zero-arg Invoke(0x18) ==
  native vtable+0x80 (impl FUN_0041ccd0 takes only ctx, RET 0x4,
  unconditional flush). New tools: `tmp\wh_hold.exe`/`wh_hold2.exe`
  (passive session pins), `tmp\wh_lock.ps1` (m_nLock sampler). (PROVEN)

### 7.2 P2 census gate -- SOLVED mechanically (Section 2.7 superseded)

- `_Module` @0x43f8a0; `m_nLock` DWORD @ fixed VA **0x43f8a8** (no
  ASLR) = count of live client-held COM objects; `m_bActivity`
  @0x43f8d8; `m_hEventShutdown` @0x43f8d4; MonitorShutdown @0x4246d0
  exits ~5 s after lock==0. ForceShutDown impl @0x41ccf0->0x424490
  hard-zeroes lock + signals -> ~5 s exit + class revoke -> SCM respawn =
  **recycle WITHOUT taskkill** (clean only when lock truly 0; attached
  callers still die on next call). (PROVEN)
- Measured deltas: passive session +9, batch session +6, per-op
  transient ramps (peak 422 during exists), graceful release ->0 in
  <0.5 s, self-reap ~6 s after sustained 0. (PROVEN)
- **Gate: `m_nLock==0` sustained >=~3 s** (max benign transient zero-gap
  observed 2.057 s, bounded <=2.49 s; <5 s monitor window) **OR
  process-absent/exits <=~7 s.** Own-session subtraction viable only at
  quiescence (known deltas 9/6). TOCTOU: class-factory-proxy-only
  clients contribute 0 but can become callers (fail-open residual);
  abrupt-death rundown lag UNTESTED (only makes gate more conservative).
- Desktop floor: 25-min idle-evening watch = WHServer absent entire
  window -> gate CAN open under idle desktop; our-spawned +
  desktop-sustained case UNTESTED.
- Discovered scan-loop: backlog-correlated ~2000-3000 impl-calls/s
  enumeration churn (get_ProgramById walks + ProgramsCountByType + QI
  churn); also appears without backlog -- conditioning + driver
  undiscriminated; stops at drain. (INFERRED)
- No PID table exists (SetProcessId/DumpProcessAttached literal
  `return 0` stubs); no IExternalConnection/ROT/exports; single
  registered class CLSID_Control; ORPC endpoints not enumerable via
  RpcView/RpcMgmt. (PROVEN)

### 7.3 INCIDENT -- freeze on live-TS work on this machine

- The 2026-10-19 experiments destabilized the live trading platform:
  TS Desktop crashed on chart use -> cache corruption -> no data
  (unrecoverable on this account's single-login constraint); hosting
  terminal crashed 2x during agent work; machine rebooted. Mechanisms:
  COM churn + Frida attach/detach + WHServer deaths adjacent to commit
  cascades + giant trace volumes + 4x tsdev_cli crashes.
- **All further live verification (E2E gate, census-on-desktop, recycle
  testing) requires a non-live/test machine or explicit user
  authorization.** *2026-10-21: authorization granted for a
  desktop-closed window; gated recycle live-verified end-to-end -> Section 8.
  Live recycle testing still requires desktop-closed -- foreign clients
  hold `m_nLock` (KB Section 21).*

### 7.4 Revised next steps (replacing Section 3 ordering)

- **P3(c) `--recycle-whserver` opt-in flag + P2 engine are the same
  artifact:** recycle engine = quiesce-own-session -> census
  (m_nLock >=~3 s-0 or process-absent) -> closeWarehouse bounded-wait ->
  ForceShutDown (NOT taskkill) -> verify full death -> SCM respawn ->
  resume in NEW process. Census gate required only for the AUTO variant;
  explicit user opt-in accepts documented residual risk (TOCTOU,
  idle-attached callers die on next call). *Engine shape superseded ->
  Section 8: an in-process census at an attached (trip) site can NEVER pass --
  the process's own +7 `m_nLock` contribution survives `closeWarehouse`
  (KB Section 21). Shipped: attached sites DEFER via bounded close + graceful
  exit 18; the engine runs only at never-attached sites.*
- **P4 gaps unchanged + NEW:** tsdev_cli.exe itself crashed 4x during
  experiments (teardown crash while server alive; orphan 40 min
  control-wait->crash-on-exit) -- own-CLI crash modes join the gap list.
- **P1 fallback** (bounded post-batch wait) = optional UX-costly
  hygiene; marginal vs the abort problem (guards trip on pre-exhausted
  servers regardless of our backlog hygiene).
- **Open questions retired/remaining:** FlushCache drain -> RESOLVED
  (falsified); active-attachment detection -> RESOLVED (m_nLock);
  idle-client survival of server death -> still untested; multi-day fill
  attribution -> still open; fresh-thread in-process re-activation ->
  still open; real-size field behavior -> open.

## 8. Shipped gated auto-recycle (2026-10-21) -- supersedes Section 7.4 engine shape

Implemented in `src\whserver_recycle.{h,cpp}` + `src\whserver_census.h`,
wired in `src\tsdev_cli.cpp`; Python respawn/resubmit in
`src\tsdev_automation\client.py`. Authorization = `--recycle-whserver` /
`TSDEV_RECYCLE_WHSERVER` (batch-* and serve only; conflicts refuse exit
1) -- it never bypasses the census gate. *2026-10-28: the binary
authorization became the tri-state RecycleMode -- Section 8.4
supersedes the authorization paragraphs below; the engine/census/defer
machinery and exit codes are unchanged.* **Live-verified end-to-end
2026-10-21 on this machine in a user-authorized desktop-closed window:**
refuse arm (`census_lock`, exit 19) and commit arm (exit-18 defer ->
resume -> ForceShutDown -> SCM respawn -> exit 0) on both the direct CLI
and Python auto-resubmit surfaces; zero orphans/dialogs/crashes.

### 8.1 Site split -- the engine runs ONLY at never-attached sites

- **Why:** an attached process contributes +7 to `m_nLock` for its whole
  lifetime; `closeWarehouse` cannot release it and only GRACEFUL process
  exit does -- a TerminateProcess'd client orphans the +7 >=5.6 min
  (KB Section 21, live-proven 2026-10-21). An in-process census at an attached
  site therefore can never pass (`census_lock`). Site gate:
  `recycleSiteDefers(ownSessionExists)` (`tsdev_cli.cpp:5103`).
- **Attached sites DEFER** (batch between-items after any dispatched op;
  serve mid-session after any warehouse-using request): the boundary
  still exits 18 WITHOUT running the engine
  (`reason=recycle_deferred_own_session`), but FIRST runs the close
  stage itself -- `recycleDeferredResult` -> `boundedCloseWarehouse`
  (zombie pre-check + monitor-bounded `closeWarehouse` on the
  session-owning thread; orlib20 binds CELWHouse per (thread,path)) ->
  `delete lib` -> GRACEFUL `ExitProcess(18)` + resume manifest -- the only
  teardown that releases the +7. A failed/bounded-out close REFUSES the
  defer on the close-stage family: exit 19 + `terminateSelf`
  (TerminateProcess-self, never `delete lib`/ExitProcess into a
  possibly-open warehouse). (`tsdev_cli.cpp:5061-5150`)
- **Never-attached sites run the engine in-process**:
  `runWhserverRecycleCli(ownSessionExists=false)` at the batch
  pre-flight guard (`tsdev_cli.cpp:5789-5802`) and the serve
  pre-first-warehouse-request edge (`serveVasGuardRefuse`,
  `tsdev_cli.cpp:6994-7033`; the attached edge of the same sites takes
  the defer). A recycle at pre-flight continues in-process -- item 0's
  lazy attach is a FIRST activation on the fresh server, not the proven
  0x4E re-activation-after-close hazard.
- **Engine steps** (`runWhserverRecycle`, `whserver_recycle.cpp`):
  quiesce (dispatch-marker check) -> zombie pre-check -> bounded close
  (skipped when `ownSessionExists=false`) -> census (`m_nLock==0`
  sustained >=~3 s OR process-absent) -> per-pid attach + `ForceShutDown`
  (never TerminateProcess on WHServer) -> verify full death -> SCM respawn
  on next activation. Every stage failure = a fail-closed refuse
  (exit 19, `whserver_recycle_refused`); success = exit 18
  (`whserver_recycled`) -- event names are the pinned client contract.
- **ForceShutDown live signature (KB Section 21):** `m_nLock` 0 -> brief
  `0xFFFFFFF9` (-7, forced negation) -> server death; distinguishes a
  real kill from self-reap (0 -> READ_FAIL, no negative phase).

### 8.2 Boundary/resume contract (client-visible)

- Exit 18 = resume boundary: results array + not-run marker tail +
  resume manifest (`tsdev_resume_<pid>_<ticks>.manifest`) whose path
  rides the `whserver_recycled` NDJSON event; exit 19 aborts with the
  refusal standing (`whserver_recycle_refused`). Bound: 3 recycles per
  batch call.
- **Serve truthfulness:** the deferred serve path runs the close stage
  BEFORE writing the response frame so the frame's exit code reflects a
  released session (`tsdev_cli.cpp:7012-7033`).
- **Client-side exit-18 respawn reap is WAIT-ONLY** -- never
  TerminateProcess: an exit-18 broker is mid-graceful-teardown and a
  kill re-orphans the +7 -> resumed `census_lock` -> exit-19 deadlock.
  Bound = `max(360 s, TSDEV_DISPATCH_TIMEOUT_SECS + 60 s)`
  (`client.py:_recycle_respawn_reap_timeout_ms`).

### 8.3 Continuation policy -- trigger vs floor (2026-10-21 field fix)

The 256/64 MB pair is now ONLY the recycle trigger, not a danger level:
field evidence showed a ~435 MB library plateau riding below 256 MB free
for hundreds of inserts (marginal insert cost ~0.07 MB) while a batch
aborted at 246 MB free and a `census_lock` refuse aborted work merely
because another TradeStation client prevented recycling. The observed
death zone remains ~10 MB free.

- **Two threshold pairs in `VasGuardConfig`** (`tsdev_cli.cpp`): trigger
  `freeThresholdMB=256`/`largestThresholdMB=64` (unchanged) vs the new
  hard floor `abortFreeMB=64`/`abortLargestFreeMB=32`
  (`--whserver-abort-free-mb` / `--whserver-abort-largest-free-mb`, env
  `TSDEV_WHSERVER_ABORT_FREE_MB` /
  `TSDEV_WHSERVER_ABORT_LARGEST_FREE_MB`; the Python serve-forwarding
  loop carries them through the elevated broker). Strict `<` semantics
  everywhere -- equality is healthy.
- **`vasGuardAction()`** (`whserver_health.h`) is the single pure
  decision shared by the batch and serve gates: Pass / Continue /
  AttemptRecycle / Abort. Above the floor the Continue arms are:
  mode not Full (RescueOnly default or Off), or the foreign-client
  latch true. Below the floor mode != Off still attempts the recycle
  boundary (RescueOnly rescue + Full); Off still aborts (batch exit 14 /
  serve exit-14 refusal).
- **Foreign-client latch**: `whServerHasForeignClients()`
  (`whserver_census.h`, injected enumerate/image/readLock seams) probes
  `m_nLock` once per probed-build WHServer pid; it is sampled in both
  `runBatch` and `runServe` BEFORE `new TsDevLib`, because our own
  orlib attach contributes +7 and would otherwise masquerade as a
  foreign client. Every inconclusive direction latches false -- the
  fail-closed census engine stays the authority for an attempt.
- **`census_lock` refuse above the floor -> CONTINUE** in-process:
  the refuse itself proved a foreign client holds the server while the
  memory is survivable. Batch: the aborted state is cleared, no
  markers/manifest/exit code. Serve: the request is DISPATCHED (exactly
  one response frame still), no respawn. Below the floor every refuse
  class stays fail-closed, including `census_lock`.
- **`whserver_vas_continue` notice** (once per run): human
  `tsdev_cli: NOTE WHServer memory low ...` line / JSON event with
  `cause` in {`recycle_not_authorized`, `foreign_clients`,
  `census_lock`}. The event name deliberately matches neither pinned
  recycle event name, so client resume parsing is untouched.
- **`abortBelowFloor` context** in `runBatch` records whether the
  boundary breach also dropped below the floor; the zombie pre-check
  arm always sets it (a corpse is a hard stop, never a continue).

### 8.4 Tri-state recycle authorization addendum (2026-10-28)

The binary authorized/unauthorized split became a tri-state
`tsdev::RecycleMode { Off, RescueOnly, Full }` (`whserver_health.h`),
resolved once per process by `resolveRecycleMode()` (`tsdev_cli.cpp`)
into `RecycleConfig.mode`:

- **RescueOnly (DEFAULT):** the recycle is reserved for the abort-floor
  rescue -- a below-floor breach attempts the boundary (the 8.3 arms
  unchanged), a trigger-band breach ALWAYS continues in-process. This
  makes the field fix's fail-closed floor the only place a default run
  can stop for a restart, without asking any band-level opt-in.
- **Full (explicit `--recycle-whserver` / `TSDEV_RECYCLE_WHSERVER`
  presence-not-"0"):** today's full gated-recycle behavior -- band
  breaches attempt too (latch permitting), the 5-line Section 6.3 warning, and
  the exit-1 conflict refusals (guard-off / batch-progress-control /
  non-batch subcommand), which are scoped to this mode only.
- **Off (explicit `--no-recycle-whserver` / `TSDEV_NO_RECYCLE_WHSERVER`
  presence-not-"0"):** the pre-tri-state unauthorized behavior -- band
  continue, below-floor hard abort exit 14. `TSDEV_RECYCLE_WHSERVER="0"`
  keeps its pinned neutralize-env-arm-only semantics: it does NOT opt
  out of the RescueOnly default and never vetoes an explicit flag.
- **Precedence** (pinned in `test_recycle_whserver_flag` check 3b):
  both polarity flags -> usage error exit 1; an explicit flag beats the
  opposite-polarity env var; env-vs-env conflict -> the NEGATIVE arm
  wins (conservative); else the env arms; else RescueOnly.
- **Conflict scoping:** the Section 6.1 refusals apply to Full only. The
  RescueOnly default + `--no-whserver-vas-guard` is legal (guard-off
  disables the whole guard INCLUDING the rescue -- no trigger can fire);
  default + `--batch-progress-control` is legal but the floor rescue is
  SUPPRESSED for that run (a mid-batch exit-18 boundary would strand the
  stdin control channel -- the run behaves as Off) and a one-line NOTE
  names the suppression; default + single-op subcommands are naturally
  inert (the guard only runs for batch-*/serve).
- **Warning scoping:** Full -> the 5-line `kRecycleWhserverWarning`
  (unchanged); RescueOnly -> one line per process/broker for batch-*/serve
  runs (`kRecycleRescueDefaultNote`: "tsdev_cli: NOTE WHServer
  auto-restart near the abort floor is on by default -- disable with
  --no-recycle-whserver or TSDEV_NO_RECYCLE_WHSERVER=1"), replaced by
  `kRecycleControlSuppressedNote` under `--batch-progress-control`; Off ->
  nothing.
- **Python client:** `TsDevClient(recycle_whserver=...)` is tri-state
  (None = follow CLI default/env, forward nothing; True = forward
  `--recycle-whserver`; False = forward `--no-recycle-whserver` on batch
  command lines AND the broker launch -- runas drops env, so the
  negative arm cannot reach the broker otherwise). The client
  `UserWarning` fires only on explicit authorization (kwarg True or the
  env var); the noguard/on_progress ValueErrors key on explicit
  authorization only; a per-call `recycle_whserver=True` next to an
  explicit ctor opt-out is refused client-side (both polarity flags
  would be a CLI usage error).
- **Serve E2E note:** `test_serve_vas_guard_abort` phase 2 (floor ->
  exit-14) now constructs the client with the explicit
  `recycle_whserver=False` opt-out to keep pinning the exit-14 refusal;
  phase 3 (band -> continue) runs on the RescueOnly default. A
  default-rescue phase (floor breach -> exit-18 boundary + broker
  respawn) is mechanically possible with the runner's UAC-disabled
  setup (the respawn auto-elevates; the client respawn+resubmit
  machinery is live-verified) but recycles the real WHServer up to 3x
  and requires the desktop-closed window (foreign clients hold
  `m_nLock` -> `census_lock` refuse arm instead) -- NOT added; add it
  as an opt-in phase only in an authorized desktop-closed window.
