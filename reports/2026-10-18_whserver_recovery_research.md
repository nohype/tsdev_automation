# WHServer VAS-exhaustion recovery research — 2026-10-18

## Context

`whserver_vas_free` aborts (exit code 14) trip when the shared out-of-process
warehouse server `WHServer.exe` (32-bit, non-LARGEADDRESSAWARE, hard 2 GB user-VAS
ceiling) reports < 256 MB total free or < 64 MB largest-contiguous MEM_FREE below
`0x80000000`. Today the only user-facing remedy is restarting TS Desktop. This
session researched whether recovery is possible **without** a desktop restart.

## Method

- Ghidra static analysis of `WHServer.exe` and `orlib20.dll` (activation chain,
  IWHouse maintenance members, idle-exit monitor, memory allocator imports).
- Live COM calls from a standalone probe (`tmp\wh_call.exe`, `tmp\wh_shutdown.exe`)
  replicating orlib20's own activation sequence bit-for-bit; verified against a
  Frida capture of the real orlib20 worker (`tmp\wh_com_trace.log`).
- Server VAS measured remotely via `VirtualQueryEx` MEM_FREE walk below
  0x80000000 (`tmp\whs_vas.ps1`, mirrors `src/whserver_health.cpp:30-71`).
- Controlled experiments: cache warm-up (~17 MB committed), maintenance-method
  invokes, `ForceShutDown` graceful-shutdown attempt, `taskkill /F` blast-radius
  watch — all under a running TS Desktop (ORPlat + 8 orchart + TSScanner +
  ORDllHst + ORCAL + TsRTAServer + ORCLPrxy + AgentForms).
- Dialog/crash surfaces checked after every action (`TSCrashReport.exe`, new
  top-level windows) — per AGENTS.md standing rule.

## Findings

| # | Claim | Status | Evidence |
|---|-------|--------|----------|
| a | Direct COM activation chain replicated: `CoGetClassObject(CLSID_Control {7CEEA90C}, CLSCTX_ALL, IID {7CEEACBC})` → factory vtbl+0x20 token call → `IControl::WHouseEx` (dispid 0x1, PROPERTYGET) → `IWHouse`. `CLSID_WHouse {7CEEA907}` is NOT directly registered (`REGDB_E_CLASSNOTREG` 0x80040154); `CoCreateInstance(CLSID_Control)` = `REGDB_E_CLASSNOTLICENSED` (0x80040112); ROT enumeration returned 0 monikers. | PROVEN | `tmp\wh_com_trace.log` (Frida capture of orlib20's own `FUN_101ec100`/`FUN_101cf170` calls), `tmp\wh_call_run.txt` steps [1][2][5c][5e] |
| b | `IWHouse` maintenance members callable via `IDispatch::Invoke`: `FlushCache` dispid 0x18, `ClearDependencyLists` 0x20, `ClearVerifyFlagByType` 0x1F (VT_I2 type arg), `ReleaseRemainingPrograms` 0x30 — all returned S_OK on the live server (PID 2956). `ForceShutDown` = dispid 0x2B, also invocable (returns S_OK). | PROVEN | `tmp\wh_call_run.txt` / `tmp\wh_call_run_warm.txt` / `tmp\wh_call_run_warm2.txt` section [5e-calls]; dispids from embedded typelib dump (`IWHouse`, cFuncs=58) |
| c | `ReleaseRemainingPrograms` with a warm ~17 MB cache (after ~140+ insert/delete/get-source warm-up ops) reclaimed **+0.00 MB** committed VAS — freed objects stay in committed heap pages (WHServer uses CRT malloc; no HeapCreate/VirtualAlloc imports). In-place release cannot reset the VAS guard. | PROVEN | `tmp\wh_call_run_warm.txt` (baseline 1015.55 MB), `tmp\wh_call_run_warm2.txt` (baseline 999.74 MB after second warm), [VAS delta] lines all `+0.00`; warm manifests `tmp\warm_insert_manifest.txt` (60 items), `tmp\warm_delete_manifest.txt`, `tmp\warm_real.ps1`; bin dir snapshots `tmp\bin_snapshot_before_warm.txt`/`tmp\bin_snapshot_after_warm.txt` |
| d | `ForceShutDown` (×2) under a running desktop returned S_OK but the server stayed alive ≥ 7.5 min still serving new activations. Reason (Ghidra): ATL MonitorShutdown exits only after 5 s of `m_nLock==0`; attached clients keep re-locking. Graceful exit is unreachable while clients call. | PROVEN | `tmp\exp_watch.log` (PID 2956 alive through 20 polls / 287 s), `tmp\exp_pre.txt` (16:53:39) + `tmp\exp_post.txt` (17:01:18) bookend the same PID alive across ~7.6 min; probe source `tmp\wh_shutdown.cpp` |
| e | `taskkill /F WHServer` (PID 2956): dead < 1 s, no `TSCrashReport` child (clean kill). SCM respawned a fresh server ~0.4 s after the next `tsdev_cli exists` activation (exit 0). Fresh server VAS = **1899 MB free** (vs ~1003 MB pre-kill warm); a broker-warmed server settles ~1105 MB free (orlib20 eager-commits ~1 GB at attach). Zero process deaths among all 15 watched TS client processes over a 7 min watch — **BUT** the user observed a ~2 min TS Desktop UI stall matching the kill window (ORPC-blocked clients waiting on the dead server). Hard-kill under a live desktop is functionally disruptive; "zero process casualties" understates the blast radius. | PROVEN | `tmp\exp_watch_kill.ps1` (watch script; **its output log was not persisted** — the kill/respawn/VAS/casualty figures are recorded in `docs\WORKING_MEMORY.md` "taskkill blast-radius result"), `tmp\pipe_probe.py` (post-kill broker probe; output also not persisted). ~2 min UI stall = user-observed field fact |
| f | Census: 11 of 13 TS-name-matched processes had orlib20/OREL modules loaded (ORPlat, ORDllHst, TSScanner, 8×orchart); the blast-radius watch tracked 15 TS client processes. Module-loaded ≠ COM-attached (lazy activation), yet the desktop still froze during the kill window — clients did call the warehouse inside the window (or had calls in flight). A module census alone cannot prove "no active attachments". | PROVEN | `tmp\vas_probe_report.txt` TASK 1 census (247 procs scanned, 13 TS matches, 11 orlib/OREL holders; WHServer itself does NOT load orlib20), `tmp\census_detail.ps1`, `tmp\exp_watch_kill.ps1` `$clientPids` (15) |
| g | Per-op server VAS deltas measured ~0 MB for single exists/insert/verify/delete ops (delta 0.00 MB at 0.01 MB resolution); ~30 MB one-time first-contact reservation observed on a fresh server. Exhaustion is cumulative across many ops and all attached clients. | PROVEN | `tmp\vas_probe_report.txt` TASKS 3/4/6 (every step `deltaFreeMB=0`, VM64 unchanged at 1,082,499,072 B), `docs\WORKING_MEMORY.md` per-op delta ledger |

NOT PROVEN / open:
- Which client workload historically fills the 2 GB remains unattributable
  ([INFERRED] in KB §15).
- "Attached callers die on next COM call after server death" is supported by
  field crash evidence (TSELAppHost `_com_error 0x80010105`, TSDev.exe crash 6 s
  post-death — KB §15) but was NOT reproduced in the 7 min watch (no desktop
  client called during the window). It remains a latent, event-driven risk.
- Whether our per-op server footprint can be reduced to TSDev's level (user
  fact: TSDev UI automations run 500–1000+ inserts/session without VAS issues)
  requires a Frida call-mix diff — not yet done.

## Implications

1. **The only real VAS reset is a server restart** (kill + SCM respawn). In-place
   release does not decommit; graceful shutdown is idle-gated and unreachable
   while any client keeps calling.
2. Hard kill + respawn is **safe only when no foreign orlib20 clients are
   running/attached** (headless/night batches). With the desktop up it causes a
   ~2 min UI stall plus latent next-call crash risk for attached callers —
   unacceptable for live trading.
3. With the desktop running, **no clean in-place recovery exists**. The fix
   direction is root-cause: reduce our per-op server-side footprint to match
   TSDev's call pattern (user evidence: TSDev-driven UI automation does
   500–1000+ inserts per session with no VAS problems, so our sequence diverges
   somewhere). A Frida trace diff of TSDev-insert vs our-insert is the
   highest-value follow-up.
4. Detection hardening: a "clients attached" gate must detect *active*
   attachment, not module load (11 module-holders were idle-detached yet the
   desktop still froze).
5. Related proven mechanics now available for reuse: `closeWarehouse` +
   next-op = full reconnect path (fresh `CELWHouse` → `CoGetClassObject` → SCM
   respawn), though in-process reuse after close is unsafe (null-deref cmd
   0x4E) so reconnect effectively means a new process/broker.

## Artifact index (tmp\)

| Artifact | Content |
|----------|---------|
| `vas_probe_report.txt` | Census + per-op VAS deltas + dialog checks (16:01) |
| `vas_probe_run.ps1`, `whs_vas.ps1`, `census_detail.ps1`, `drift_check.ps1`, `whs_step3.ps1` | VAS probe/census harnesses |
| `wh_com_trace.log`, `wh_com_trace.js`, `wh_com_trace.out`, `run_trace.py`, `run_stdout.txt` | Frida COM capture of orlib20 activation |
| `wh_call.cpp`/`wh_call.exe`, `wh_call_run.txt`, `wh_call_run_warm.txt`, `wh_call_run_warm2.txt` | Standalone COM client + 3 live runs (cold/warm/warm2) |
| `wh_shutdown.cpp`/`wh_shutdown.exe`, `build_shdn.bat` | ForceShutDown probe |
| `exp_snapshot.ps1`, `exp_pre.txt`, `exp_post.txt`, `exp_watch.ps1`, `exp_watch.log` | ForceShutDown experiment snapshots + 287 s watch |
| `exp_watch_kill.ps1` | taskkill blast-radius watch script (**output log not persisted**) |
| `exp_serve.py`, `serve_ready.flag`, `serve_go.flag`, `pipe_probe.py` | serve-broker end-to-end + post-kill pipe probe (outputs not persisted) |
| `gen_warm.ps1`, `warm_insert_manifest.txt`, `warm_delete_manifest.txt`, `warm_getsrc.ps1`, `warm_real.ps1`, `TSDEVWARM00–59.el`, `real_items2.txt`, `find_real_*.ps1`, `measure_3x.ps1` | cache warm-up tooling |
| `bin_snapshot_before_warm.txt`, `bin_snapshot_after_warm.txt` | `MyWork\Bin` file listings around warm-up |
| `orlib20_exports.txt`, `tlbdump.cpp`/`tlbdump.exe` | export/typelib dump tooling |
| `TSDEVVASPROBE.el`, `probe2.el` | probe EasyLanguage sources |

Evidence NOT persisted to disk (exists only in `docs\WORKING_MEMORY.md` prose):
taskkill watch output (kill latency, respawn ~0.4 s, 1899/1105 MB figures, 7 min
zero-death tally), `pipe_probe.py` output, `wh_shutdown.exe` stdout.
