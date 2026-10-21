# Working Memory

## Session objective (2026-10-21) — stabilize + resume plan
- PM-orchestrated. User authorized FULL live-TS plan testing ON THIS MACHINE ("must replicate users' real-life conditions"; no test machine exists). The 2026-10-19 freeze is lifted by explicit authorization, but the zero-destabilization mandate stands: sequence live work read-only → benign ops → destructive paths last, check for TS error/crash dialogs after every TS-touching action.
- Mechanical audit of previous team's uncommitted changeset: **"broke the codebase" FALSIFIED** — clean Win32 Release build (CI recipe) = 0 errors, all targets; 28/28 -NoTs unit tests PASS. Changeset = the §7.4 gated-recycle implementation (census, recycle engine, `--recycle-whserver`, resume manifests, exits 18/19), all opt-in gated, heavily unit-tested. Junk: 0-byte `nul` file (bash redirect artifact), stale `tsdev_cli_stale_locked.exe` in build dir.
- Adversarial safety review: not less stable than HEAD for non-opted-in users; **4 blocking findings** for the recycle path: (1) NO WHServer binary-identity gate — fixed-VA `m_nLock` read + raw vtable calls on any `WHServer.exe` → post-update relayout can yield silent false-zero census → ForceShutDown on client-held server (the exact destabilization class); (2) `findWhServerPids` treats mid-walk enumeration failure as authoritative-empty; `findCrashReportParentPids` silently collapses snapshot failure → zombie check can false-pass; (3) recycle `closeWarehouse` bounded only by the 300s dispatch watchdog — a modal (non-dispatch) hang is unbounded; (4) `--batch-control-timeout-secs 0` = documented unbounded-wait escape hatch (non-default, accepted).
- Fix plan in flight: F1 binary-identity gate (census+recycle), F2 enum fail-closed (whserver_health), F3 deathPoll ok-flag + bounded closeWarehouse, then rebuild → review → live E2E → census/recycle live verification → P4 gaps.
- Fix status (all landed, build green, 29/29 -NoTs + sigtool 12/12 + pytest 409 pass; independent adversarial review APPROVED):
  F1 DONE — `whServerImageIsKnown`/`whServerPidImageIsKnown` (size 295368 + tds 0x5C016EF8 + checksum 0x00055BF2, SSOT `whserver_census.h:62-85`) gates RPM in censusPoll + decideCensus (Indeterminate err=577) AND `attachAndForceShutDown` (:502-508) regardless of opt-in.
  F2 DONE — `findWhServerPids` fail-closed (whserver_health.cpp:13-34); `findCrashReportParentPids` bool-overload fail-closed (dispatch_watchdog.cpp:269-292); `RecycleDeathPoll.crashSnapshotOk` (recycle.cpp:328 inconclusive→deadStreak=0).
  F3 DONE — bounded closeWarehouse: close stays on engine thread (per-(thread,path) binding → helper-thread close would no-op false-pass); monitor thread fires stderr+TerminateProcess(19) at 30 s (`closeBoundSecs`); test seam `onCloseBoundExceeded`.
- Live E2E gate 2026-10-21: **PASS 11/11** — insert/exists/get-source/modify/upsert-both-arms/delete/not-found on live warehouse; WHServer PID 6864 stable; zero dialogs/crash reports; disk artifacts cleaned. Reusable checker: `tmp\check_ts_state.ps1`.
- Reviewer non-blocking nits (queue for P4 cleanup): mixed-Pass over-refusal checks dead pids' images (fail-closed, rare); `closeBoundSecs<=0` unguarded (negative wraps DWORD → refuse guard wanted); watchdog still uses collapsing findWhServerPids (dispatch_watchdog.cpp:133, pre-existing); verify-death default reason label imprecise.
- Live desktop running (offline, cache-corrupted): WHServer 6864 + ORPlat 6240 + TSScanner 8668 + ORDllHst 3888 + ORCAL 11620 + TsRTAServer 11408 + ORCLPrxy 10180 + AgentForms 14168.
- **Full regression 2026-10-21: 15/15 suites green live** — all E2E suites pass (e2e 34/34, unknown-link 253/253, batch-stdin 8/8, upsert 9/9, batch-upsert 5/5, directory-batch 9/9, ShowMe 9/9, verbose 7/7, pbar 10/10, serve 16/16); serve needed ConsentPromptBehaviorAdmin=0 temporarily (auto-elevation path; restored to 5, verified). Zero dialogs/crashes; WHServer 6864 never respawned; zero warehouse leftovers.
- Wave-1 fixes DONE: (b) zombie corpse pre-checks before close+attach (`zombie_present_close`/`zombie_present_attach`/`zombie_unverifiable_*`); (f-i) attach image-gate now only checks last-poll-listed pids; (f-ii) `closeBoundSecs<=0` → `close_bound_invalid` refuse; (f-iv) precise reasons (`census_unverifiable`/`death_unverifiable` vs `census_lock`/`forceshutdown_inert`); (f-iii) watchdog uses fail-closed `findWhServerPids` + `accountWhServerSample` (inconclusive resets streak).
- New nit queued: `pollIntervalSecs<0` same unsigned-wrap class in sleepMs conversions (whserver_recycle.cpp).
- **LIVE FINDING 2026-10-21 (proven, instrumented): the recycle engine is structurally self-refusing inside an attached process.** Our process holds `m_nLock=+7` for its ENTIRE lifetime (process-lifetime CControl/factory activation refs; `closeWarehouse` does NOT release them — only process exit drops lock→0, control-verified). Between-items arm: 57 samples lock=7 through the whole census → deterministic `census_lock` exit 19; ForceShutDown unreachable. Same applies to the serve mid-session arm. Prior desktop `census_lock` refuse was likely confounded by the same +7 (desktop held 6 + ours 7 = 13?; measured 6 constant pre-close).
- **TVS-R1 fix direction (chosen): engine may only run at NEVER-ATTACHED sites** (`ownSessionExists=false` → own contribution 0). Attached sites (batch between-items, serve mid-session) must exit-18 immediately + write resume manifest → the RESUMED process / fresh broker runs the engine at its never-attached guard site → census can pass → real recycle. This is also the only shape consistent with the proven 0x4E in-process-reactivation hazard.
- Live evidence: refuse path fully proven (desktop lock=6 → `census_lock`, exit 19, both CLI + serve-broker surfaces; conflict flags refuse exit 1; §6.3 warning fires). Commit-arm live test pending TVS-R1. Fresh-server free VAS ≈1095 MB. SCM respawn on every activation; idle server self-reaps ~6 s at lock==0.
- Wave-2 fixes DONE + review APPROVED: C1 `sessionMayBeOpen`/`terminateSelfNoDetach` — every uncertain-close exit path (batch/serve tails, insert/delete/upsert workers, emitOutput catch) now self-terminates instead of ExitProcess-into-open-session (kills the 'WHouse in memory' modal/deadlock class); C2 `whserver_zombie` pre-dispatch refuse at both guard sites + `whserver_enum_failed`/`crashreport_enum_failed` warn-once latches + `poll_interval_invalid`; D fatal containment — extended VEH fatal codes + set_terminate(20) + SIGABRT(21) + invalid-param(22) + purecall(23) + process UEF(24) w/ 1s re-arm sentry (install AFTER `new TsDevLib`), all proven via real child crashes, zero TSCrashReport.
- **LIVE FINDING 2 (proven, instrumented): TerminateProcess ORPHANS the +7.** Graceful teardown (bounded closeWarehouse + delete lib + ExitProcess) releases refs promptly (lock→0 at exit, self-reap ~6s); TerminateProcess leaves lock=7 ≥5.6min with zero clients → resumed census sees corpse's lock → `census_lock` deadlock. Explains the KB's "24.5min linger" class.
- **TVS-R2 landed + LIVE-VERIFIED END-TO-END:** defer now does `boundedCloseWarehouse` (extracted helper, whserver_recycle.h:294) on the owning thread → graceful ExitProcess(18) (close-fail → terminateSelf(19) refuse). Live proof: full chain exit18→resume→ForceShutDown(−7 lock signature seen on 9720/17064)→SCM respawn→exit0 on CLI AND Python auto-resubmit; zero orphans/dialogs/crashes. Orphan test-artifact pid 5824 killed manually (desktop closed).
- **Review DISAPPROVED R2 → blocking fix in flight:** `client.py:2319` exit-18 respawn reap = `_wait_and_close_process(5000)` → `_wait_for_exit` → `TerminateProcess` at 5s — races the broker's graceful teardown under R2; mid-teardown kill re-orphans +7 → census_lock deadlock. Fix: exit-18 reap must be WAIT-ONLY bounded ≥ watchdog window (~360s), never kill; other reap call sites keep 5s+kill. Subagent f047020e on it (client.py + test_tsdev_client.py only).
- Remaining: re-review the client fix → optional live serve-arm verify → commits (user approved: ONE feature commit + `.devin/` separate) → AGENTS/handoff doc updates (recycle engine model changed: engine only at never-attached sites; defer=bounded-close+graceful-exit).
- **Live refuse-path verification 2026-10-21: PROVEN.** Forced guard trip (`--whserver-free-mb 4000 --recycle-whserver`): batch → `census_lock` refuse exit 19 + resume manifests; serve → exit-19 frame → `TsDevError(whserver_recycle_refused)`, broker clean exit. Conflicts refuse at parse (exit 1). WHServer 6864 untouched throughout. Real ForceShutDown arm NOT exercised — unreachable while desktop holds lock=6 (fail-safe direction by design; arm is unit-tested + mechanism proven 2026-10-19).
- Serve-path nuance: `_serve_active()` = `use_serve AND NOT _is_elevated` — an ELEVATED Python client bypasses the broker (direct subprocess); single ops carry no VAS guard (guard = batch/serve-mutations only).
- Pre-existing HEAD nits (not from this work): C4476 format-string in usage text (tsdev_cli.cpp:343); strncpy deprecation (dispatch_watchdog.cpp:198).

## Session objective (2026-10-19)
Execute `docs/HANDOFF_WHServer_VAS_recovery.md` plan one step at a time, starting with P1 (deferred-commit drain). PM-orchestrated.

## P1 outcome (2026-10-19) — FlushCache falsified as drain lever (executed conditions)
- `tmp\P1_FLUSH_DRAIN_FINDINGS.md` (R1): fresh server commits inserts/modifies in-session (~0.35-0.40s/rec); no backlog at flush time; deferral not reproduced under R1 conditions. Adversarially DISAPPROVED → corrected in place.
- `tmp\P1R2_BACKLOG_FINDINGS.md` (R2a): deferral REPRODUCED (a3f verbatim `run_wh_big.py` replay → 15-rec burst ~85-99s dirty-age). Contrast observed: single-op creates commit early on first pulse (~4-26s, POST-exit — not inside op window); modifies + batch-ops gate at ~60s+ (a3f bounds ~73-92s; best-fit rule: first pulse after YOUNGEST dirty record ~60s drains backlog aggregate-style). Confounds open: wave order, warmth (~13-16min server), cumulative state. DISAPPROVED → corrected in place.
- `tmp\P1R2B_FINDINGS.md` (R2b): wh_flush at +15s queued ~50s server-side (executed only at cascade tail); at +75s backlog pre-drained 0.4s before flush. Backlog-conditioned ~2300calls/s enum scan loop observed (driver undiscriminated: client vs server-internal). DISAPPROVED → corrected in place.
- `tmp\P1R3_FINDINGS.md` (R3): env degraded (idle-exit instability, scan churn); cell A flush never dispatched; cell C reconfirmed deletes synchronous in-session (RemoveProgram, 40KB items); cell B zero-drain inconclusive.
- Instrument: `tmp\wh_flush.exe` zero-arg Invoke(0x18) ≡ native vtable+0x80 (impl takes only ctx; unconditional flush). New: `tmp\wh_hold.exe` passive session-pin.
- Deletes: synchronous in-session RemoveProgram in both tested rounds (720B + 40KB items); contradicts BIG40K "deferred deletes" (inference from never-fired) — scoped.
- P1 remaining lever = bounded post-batch wait holding session open (~75-90s: youngest-rec ~60s + pulse boundary) so our pulses drain our backlog in our guarded session. Untested as product shape; UX cost = +~90s batch tail.
- Reviews complete; all three findings docs DISAPPROVED then corrected in place per enumerated findings.

## P2 census screen (2026-10-19) — m_nLock signal PROVEN readable
- Ghidra (WHServer.exe, no ASLR → fixed VAs): `_Module` @0x43f8a0; **`m_nLock` DWORD @0x43f8a8** = CControl-singleton refs + live CComObject count + LockServer pins; `m_bActivity` @0x43f8d8; `m_hEventShutdown` @0x43f8d4; MonitorShutdown @0x4246d0 exits after ~5s lock==0. `ForceShutDown` impl @0x41ccf0→0x424490 hard-zeroes m_nLock + signals → ~5s exit + class revoke → SCM respawn = **recycle without taskkill** (clean when lock truly 0; attached callers still die on next call otherwise).
- Lock writers mapped: CWHouse ctor +1 @0x40a6b0, global CControl AddRef +1, LockServer, transient enum/object +1s, Release tails Lock→delete→Unlock.
- No IExternalConnection/ROT/exports; single registered class = CLSID_Control {7CEEA90C}; per-license-key CWHouse map is heap-only (not statically reachable); DumpProcessAttached/SetProcessId = literal `return 0` stubs reading no table.
- Live probe (`tmp\P2_CENSUS_LOCK_PROBE.md`, `tmp\wh_lock.ps1` sampler): wh_hold session = **+9 steady**; paused batch-delete session = **+6 steady**; `exists` op transient ramp 0→422→0; graceful release → 0 in <0.5s; **self-reap ~6s after lock==0** (3× observed; earlier ~24.5min linger NOT reproduced). Baseline-foreign floor NEVER observed — WHServer simply absent during idle desktop; discrimination vs real foreign holder unproven. Own-delta subtraction viable only at quiescence.
- Gate shape: `m_nLock==0` sustained ≥~3s (max observed benign transient zero-gap 2.06s; <5s monitor window) OR server-absent/exits≤~7s = fail-closed "no client-held objects". TOCTOU: factory-proxy-only clients contribute 0 but can become callers instantly — inherent race; abrupt-death ORPC rundown lag untested (conservative direction only).
- **Desktop watch (25min idle evening desktop, PROVEN): WHServer ABSENT entire window — zero spawns.** Foreign floor = none; census gate CAN open under idle desktop ("process absent" passes). Scope: idle evening; heavy interactive use untested. §2.7 inertness prediction not borne out.
- Model correction (review-verified): drains do NOT require a held session — require a LIVE SERVER INSTANCE (cell A drained post-release; n=1). Pulse/gating mechanism unresolved — falsified candidates: per-record ~60-75s, youngest ~60s aggregate (cell C drained w/ youngest ~49s), cadence varies ~50×; observed pattern = drains run in op-free windows; "drains defer while op in flight" = untested alternative.
- **P4 robustness signal (from R3 review):** tsdev_cli.exe itself crashed 4× during experiments (TSCrashReport) — teardown crash while server alive + orphan 40min control-wait→crash-on-exit. Own-CLI crash modes belong in P4 gap list.

## INCIDENT (2026-10-19/20) — experiments destabilized live TS platform
- User report: TS Desktop crashed on chart use after our session; crash corrupted TS cache → no data, cannot re-download (single-login account needed for live trading elsewhere); the terminal hosting our agent fleet crashed 2× (only during our work); machine rebooted, desktop restarted (running, no data).
- Plausible mechanisms: sustained COM activation/session churn + Frida attach/detach (~135 hooks, 130MB+ traces); WHServer deaths adjacent to commit cascades (cell A: died 0.095s after last cascade step; cell B: died ~2.7s post-detach) risking torn warehouse state; 4× tsdev_cli crashes + orphan 40min process; memory/IO pressure from giant traces.
- Post-reboot state verified: zero leftover experiment processes; desktop procs up; WHServer 10964 freshly spawned by restarted desktop.
- **MANDATE: no further live-TS-touching experiments/probes on this machine without explicit user authorization — the platform has demonstrated fragility; previously-"safe" individual actions are NOT safe in aggregate.** All remaining plan steps that require live verification (E2E gate, census validation, recycle testing) must run on a non-live/test machine.

## Session objective (2026-10-18) — completed
Research whether `whserver_vas_free` aborts can be recovered WITHOUT restarting TS Desktop. Delivered report + handoff. No implementation.

## Verified facts (evidence-backed)
- Guard measures WHServer.exe free VAS via VirtualQueryEx MEM_FREE walk < 0x80000000 (non-LAA 32-bit, 2GB). Defaults: total free < 256MB or largest contiguous < 64MB → abort, exit 14 = whserver_vas_abort. close_failed = end-of-batch `_CloseWarehouseForCurrentThreadAndPath@4` threw SEH.
- Guard: preflight + between batch items; serve checks before each mutating request. Read-only ops unguarded.
- WHServer.exe = out-of-proc COM server spawned ON DEMAND by DCOM SCM (-Embedding, parent svchost -k DcomLaunch — verified live PID 2956). Lazy singleton shared by all orlib20 clients. Lingers ~24.5min idle. Fresh PIDs after deaths → SCM respawn works for NEW activations.
- WHServer.exe on disk: Program\WHServer.exe, 295,368 B, x86 PE32, non-LAA (0x0103). WHServer does NOT load orlib20; uses orgcom20.dll+orshr20.dll.
- LIVE CENSUS: 11 procs hold orlib20+ORELUI20+ORELP20: ORPlat(13668), ORDllHst(4016), TSScanner(14768), orchart ×8. Loaded module != COM-attached (lazy activation). TSDev.exe not running. Pre-existing ORDllHst "Script Error" dialog (unrelated).
- Per-op VAS deltas: single exists/insert/delete → 0 MB MEM_FREE delta; ~30MB one-time first-contact reservation per fresh server. VAS exhaustion is CUMULATIVE across many ops + all clients.
- Client-death evidence (reports re-read): EVERY client attached at WHServer death is lost — no documented survivor/reconnect. Death is ON-CALL (next COM call), not immediate — TSDev survived ~6s until next dispatch. Idle never-calling clients' fate = open → blast radius needs live test.
- New client process always gets fresh server after old fully gone (proven ×4+). Zombie (TSCrashReport-frozen) server gets reactivated → must ensure full death before reactivation.
- No reconnect/purge API in orlib20 docs. Per-item closeWarehouse UNSAFE. COM proxies die permanently (no auto-reconnect).
- User fact: when our tool aborts on whserver_vas_free, TSDev.exe still inserts/modifies/deletes fine against the SAME server → abort is protective tripwire; server still functional at ~251MB free. Sharpens prevention question (does TSDev's call pattern grow VAS less? — Frida-trace compare = follow-up).

## Ghidra WHServer findings (PROVEN unless noted)
- WinMain: -Embedding → CoRegisterClassObject (REGCLS_MULTIPLEUSE) → msg loop → revoke+exit. MonitorShutdown thread: 5s idle while m_nLock==0 → WM_QUIT → clean exit. 24.5min linger is held-lock driven, not a coded timer.
- **`IWHouse::ForceShutDown` (dispid 0x2b)** → forces m_nLock=0, signals event → clean exit. NO client notification — attached clients still die on next call.
- **`IWHouse::ReleaseRemainingPrograms` (dispid 0x30)** → walks per-type std::map item caches, Release()es every cached record (records hold IUnknown + blob at +0x1c/+0x20). THE in-place cache-release API.
- **`IWHouse::FlushCache` (0x18)** → flush/commit all 8 per-type storage streams. Also ClearDependencyLists(0x20), ClearVerifyFlagByType(0x1f), BeginImport/EndImport(0x23/0x24).
- Memory: CRT malloc/free only — no HeapCreate/VirtualAlloc imports. VAS reclaim on free depends on process-heap decommit (plausible for large blocks; VERIFY live).
- COM IDs: CLSID_WHouse {7CEEA907}, IID_IWHouse {7CEEA903}, LIBID {7CEEA906}; typelib embedded in .rsrc. IDispatch::Invoke delegates to ITypeInfo::Invoke → late binding likely works.
- TSWAREHOUSEMUTEX = likely "WHouse in memory" token. SetProcessId/DumpProcessAttached = STUBS, no PID table.
- orlib20.dll (4.9MB) AND ORELUI20.dll (1.2MB) both exist on disk; our code loads orlib20.dll.

## orlib20 findings (Ghidra, PROVEN)
- Activation: CELWHouse worker → CoGetClassObject(Control {7CEEA90C}, custom factory IID {7CEEACBC}) → factory vtbl+0x20(token BSTR) → IControl::WHouseEx → IWHouse at wh+0xAE + events advise. Once per (thread,path) for process life.
- ZERO reconnect code (no server-death HRESULT checks anywhere). Dead server = permanently broken CELWHouse until close.
- closeWarehouse (_CloseWarehouseForCurrentThreadAndPath@4) → CELWHouse teardown → calls IWHouse::ReleaseRemainingPrograms on server + releases all proxies. Next warehouse call builds FRESH CELWHouse → CoGetClassObject → SCM-respawns WHServer if dead. **close+next-op = full reconnect mechanism (existing code path).**
- BUT: in-process reuse after close is documented UNSAFE (freed iface → later calls null-deref in cmd 0x4E) → reconnect effectively means NEW PROCESS (batch end / broker restart).
- IWHouse vtable offsets (direct call): FlushCache 0x80, ClearDependencyLists 0xA4, ForceShutDown 0xD0, ReleaseRemainingPrograms 0xE4. orlib20 reachability: FlushCache via VerifyModule_New/InstallVerifyAllEx/InstallTransfer; ClearDependencyLists via Ordinal_49; ForceShutDown = dead arm; ReleaseRemainingPrograms only at teardown.
- IsWHRegistered export (ord 554) = activation probe.

## Live COM-call results (PROVEN)
- Direct activation replicated bit-identically (Frida-verified); FlushCache/ClearDependencyLists/ClearVerifyFlagByType/ReleaseRemainingPrograms all S_OK live, server healthy.
- CLSID_WHouse NOT registered directly (REGDB_E_CLASSNOTREG) — must use Control factory path or IDispatch.
- **ReleaseRemainingPrograms reclaims ZERO committed VAS even with warm ~17MB cache (142 ops) — freed objects stay in committed heap pages. In-place release CANNOT reset the VAS guard → server restart is the ONLY real VAS reset.**

## Candidate recovery options (final forming)
1. **Controlled recycle: closeWarehouse (release our proxies while server alive) → ForceShutDown via direct COM → process exit/respawn → SCM respawns fresh 2GB server.** Requires our process not to reuse dead session → for batch: abort+exit w/ recycle hint; for serve: broker exit+respawn (client.py manages lifecycle).
   - Safe when only our clients attached.
   - With desktop attached: desktop clients die on next warehouse call (deferred; idle never-calling clients may survive) — BLAST RADIUS TEST RUNNING.
2. taskkill fallback for zombie/wedged server (ForceShutDown unreachable on dead server; must also kill TSCrashReport child).
3. Prevention: reduce per-op server-side footprint (match TSDev call pattern) — follow-up research (Frida trace diff), doesn't help already-exhausted servers.
4. (Rejected) LAA patch; in-place release (no decommit); per-session second WHServer (unverified, warehouse-concurrency risk).

## ForceShutDown live result (PROVEN, experiment aa7b40c6)
- ForceShutDown ×2 → S_OK but **INERT under running desktop**: WHServer PID 2956 stayed alive 7.5+min, still serving new activations; zero client casualties; no respawn. ATL monitor exits only after 5s of ZERO lock activity — desktop clients keep re-locking. Graceful server exit is UNREACHABLE with desktop up.
- Serve broker end-to-end works (spawned via runas, pipe ops fine).

## Decision space (collapsed by evidence)
- Graceful exit: IMPOSSIBLE with desktop clients attached (ForceShutDown idle-gated).
- In-place release: IMPOSSIBLE (no decommit).
- → Only VAS-reset lever with desktop up = HARD KILL + SCM respawn. Blast radius = decisive unknown → taskkill test IN FLIGHT (3b54c53d).
- Without desktop: closeWarehouse → ForceShutDown (or kill) → next-process respawn = clean safe recycle.

## taskkill blast-radius result (PROVEN, experiment 3b54c53d)
- taskkill /F WHServer PID 2956 → gone <1s, no TSCrashReport (clean kill, no exception).
- SCM respawn instant: `tsdev_cli exists` → fresh WHServer ~0.4s later, exit 0. Fresh server VAS = **1899MB free** (vs 1003MB before). Broker-warmed server settles ~1105MB free (orlib20 eager-commit ~1GB at attach).
- **ZERO casualties in 7min watch**: all 15 TS procs (ORPlat, 8 orchart, TSScanner, ORDllHst, ORCAL, TsRTAServer, ORCLPrxy, AgentForms) alive, Responding, no dialogs.
- "Clients die on next call" NOT disproven — no desktop client called the warehouse during window; field evidence shows attached callers DO die on next call. Risk is event-driven (latent), not immediate.
- Transient servers self-reap <17s idle; broker-activated server lingered 50s+ after broker death (lock rundown lag) — consistent with lock-driven idle exit.

## New user fact 2 (2026-10-18)
- TSDev UI automations insert 500-1000+ strategies/session rapidly, no VAS problems → **"volume" hypothesis dead; our per-op server footprint is anomalously high vs TSDev's.** Call-mix diff (Frida trace TSDev vs our op) = highest-value prevention research. Also a mandate signal: our op sequence likely diverges from TSDev's.

## New user fact 3 (2026-10-18) — CRITICAL
- The taskkill experiment caused a REAL desktop malfunction: TS Desktop did not render for ~2 minutes, then recovered with issues that seem resolved. Timing = our taskkill window. → **Hard-kill WHServer under running desktop = ~2min UI stall (ORPC-blocked until timeout/retry) + latent next-call crash risk = UNACCEPTABLE for live trading.** Process-liveness "zero casualties" undercounted functional blast radius.
- No further desktop-affecting experiments authorized/safe.

## REVISED recovery assessment
- Hard-kill + respawn: SAFE only when no foreign orlib20 clients (esp. desktop) attached — census-gated auto-recycle viable for headless/night batches.
- With desktop up: recycle = warn/refuse or user-confirmed (documented ~2min freeze + deferred app-crash risk).
- → **Root-cause fix now dominant**: match TSDev's per-op server footprint (user fact 2) so the wall is never hit. Frida call-diff = next research.
- Detection hardening: census must detect ACTIVE attachment, not just loaded module (taskkill test: 15 procs had orlib20 loaded but were idle-detached; desktop still froze → ORPlat/orchart DID call during window or had in-flight call).

## Status (Tier-1 root-cause work, in progress)
- Per-item VAS slope (PROVEN, 8c98b55e): insert/get-source/upsert/delete ≈ 0 MB/item on 14-byte items (±1.25MB noise over 100 ops). Fresh server = 1900MB free baseline. → our 322-item batch CANNOT itself fill a fresh server; field aborts hit already-poisoned servers (~1.7GB prior multi-client fill). Tiny items can't resolve <0.05MB/item.
- TSDev surface (PROVEN, e4b99478): NO remote insert API. TSDev insert/verify export path = CreateEntryEX → SaveModule_New → StartCompilingProcess → VerifyModule_New → EndCompilingProcess (+WM_ELCREATEDOCUMENT→FUN_004417d0 create/save, WM_USER+0xA→FUN_00451800 verify). /qva = silent verify-all. TSDevPipe opens/creates docs (no source). UI automation or user's own automation needed for TSDev-side live trace.
## WHServer-side Frida trace (PROVEN, d7d2396a) — reshapes root cause
- Our mutations fire ZERO server-side mutation calls (no CreateProgram/RemoveProgram/WriteRecord/BlobPut) — records live in SHARE_WO shared mem + client-written .ell/.xml; WHServer does enumeration + locks + events + DEFERRED catalog persistence (~4.5s later: CreateProgram+WriteRecord+FlushCache+ReleaseRemainingPrograms, freed ~15.4MB observed).
- exists = ~150 server enum calls (get_ProgramById + IProgram getter per record, whole-category) + ~5.6MB transient alloc churn. insert ~40 calls/+1.45MB. get-source ~26/+0.92MB. delete ~18/+0.65MB. Net live-bytes after commit cascade ≈ ~1MB.
- Server SELF-manages cache (own ReleaseRemainingPrograms/FlushCache on commit cycle) — exhaustion is fragmentation + volume over days, not retained-by-cache alone.
- Desktop idle produced zero calls during windows.
- Artifacts: tmp\wh_impl_trace.js, run_wh_trace.py, trace_cli_insert*.log, WH_TRACE_FINDINGS.md.

## Static export diff (PROVEN, c4c6f94e): our ops vs TSDev path (CreateEntryEX→SaveModule_New→StartCompile→VerifyModule_New→EndCompile)
- Our extras per item: reverse-dep traversal (_LoadMasterRecsEX ×8 categories + linker ordinals + per-dependent VerifyModule_New for cats 1/4; cat-0 already skipped), modify get-source readback (_GetMasterRecByType+_LoadTextFileEX), per-item CloseWarehouse flush, exists whole-category LoadMasterRecsEX×2.
- KB:371: TSDev UI save ~468 RPC events vs our ~141 (stale measure) → per-op we may be LIGHTER; real divergence = TEMPORAL DENSITY (hundreds ops/min vs human pacing) + real-size blobs.
- Revised root-cause read: aborts hit already-poisoned servers (cumulative multi-client aging over days); per-op anomaly hypothesis WEAK — trace-diff vs TSDev still worth confirming but recovery-by-recycle + scheduling are the likely real levers.

## Realistic-size op trace (PROVEN, c845a7b8)
- get-source fires NO server BlobGet — source reads are client-side shared-mem. 2×-blob alloc only inside commit cascades (blob serialization for WriteRecord).
- Genuine per-op server VAS cost ≈ NET ZERO — live-bytes floor ~1KB between sessions; everything drains at teardown. Nothing persists ucrtbase-visibly.
- Commit lag: single inserts commit in-session ~0.7s; batch/upsert dirty records flush ~0.6s/record in a burst inside the NEXT live session, gated ~60-70s dirty-age. No commit with zero clients.
- "Mov Avg 1 Line"(1KB) get-source = 563KB/1785 calls > 40KB probe → record structure, not size, drives churn.
- Frida instrumentation itself drove freeMB 1067→231 (send() queue) — artifact; server self-healed post-detach.
- Field 2GB exhaustion = cumulative multi-client aging + heap fragmentation over days + big-library transient spikes; NOT per-op retained cost.

## TIER-1 VERDICT (review-corrected ×2, then superseded by 2026-10-19 experiments)
- Deferred commits verified real but the model was substantially revised on 2026-10-19 — see "P1 outcome" + "P2 census screen" sections above for current state. Original phrasing kept for provenance: BATCH/upsert mutations leave dirty backlog → commit burst gated ~60-70s dirty-age landing wherever pulses fire (guard-invisible).
- Comparative claim downgraded: "no evidence of anomaly in our ABSOLUTE per-op retained cost (≈0 between sessions; ~+0.085MB/item committed ratchet bookend); TSDev-side per-op server cost UNMEASURED." KB:371 dropped (stale/wrong metric/op).
- Fill mechanism ("cumulative multi-client aging + fragmentation") = INFERRED (KB §15 unattributed; no multi-day measurement).
- ~~Census gate = UNSOLVED~~ → SOLVED mechanically (2026-10-19): m_nLock DWORD @fixed VA 0x43f8a8 via ReadProcessMemory; gate = lock==0 sustained ≥~3s OR process-absent; §2.7 inertness prediction not borne out (25min idle-desktop watch: WHServer never spawned). Residual unknowns: foreign floor under heavy interactive desktop; abrupt-death rundown lag; factory-proxy TOCTOU.
- Churn attribution: batch path already uses cheap findExistPos BST (tsdev_cli.cpp:2465); the ~150-call whole-category enum lives in standalone `exists`/find-dependents, unguarded (read-only) — not the batch worker.
- Shipped mitigations (TODAY): --whserver-free-mb / --whserver-largest-free-mb / --no-whserver-vas-guard can loosen guard for continued ops at ~250MB (residual risk = commit bursts + other clients' allocs).
- Desktop-restart caveat: WHServer linger behavior varies (24.5min held-lock rundown observed 2026-10-18; ~6s self-reap after lock==0 observed 2026-10-19 ×3) → verify a NEW WHServer PID before assuming a restart reset it.
- ~~Candidate lever: FlushCache in-session drain~~ → FALSIFIED 2026-10-19 (autonomous age-gated pulses drain; flush dispatch queued/uncontrollable). Remaining in-session lever = bounded post-batch wait (~75-90s) holding our session open.
