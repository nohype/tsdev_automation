# Working Memory — TSDev Automation

Live, real-time state for the PM. Keep minimal; drop stale/superseded facts.
Do not restate what the code itself documents.

## Current objective

Root-cause the v0.1.0 field bug (batch operations hang indefinitely).
**DONE 2026-09-02:** report approved by adversarial review —
`reports/2026-09-02_v0.1.0_batch_hang_root_cause_analysis.md` (read it first).

## Outcome summary (details in the report — do not duplicate here)

- Structural root cause [PROVEN]: no timeout/containment on the
  tsdev_cli worker → orlib20 dispatch ([wh+0x6A] INFINITE) → WHServer.exe
  COM chain; silent 0%-CPU hang reproduced (E2h, frida stacks
  `orlib20+0x1cf5ec`); abrupt server death → orlib20 worker C++ exception
  (RPC_S_SERVER_UNAVAILABLE) → TSCrashReport dialog (reproduced ×3).
- Natural user-machine trigger [OPEN]: candidates — WHServer wedge/lifetime
  dynamics (lazy lifetime proven, E6), dispatch-volume race (library size),
  quiet worker death, AV interference. §7.1 diagnostic build addresses this
  (done, see below).
- Rare init-line hang [INFERRED]: wedged leftover WHServer after kills.
- **§7.1 diagnostic build: DONE 2026-09-02** (implemented, adversarially
  approved round 2, field-validated, production E2E gate passed, committed).
  Status + evidence pointers live in the report §7.1 "Status" block (SSOT).
  Field-validation artifacts: `tmp\exp\diag_verify\`; review 1:
  `tmp\exp\review1_diag_build.md`; pre-fix incident (ExitProcess detach
  dialog): `tmp\exp\whouse_incident\`.
- Next toward goal (a) closure: ship `tsdev_cli_diag.exe` to affected users
  and collect their diag logs (the natural user-machine trigger is still
  [OPEN]); then goal (b) targeted fix. §7.3 corruption experiments + §7.2+
  containment remain open (see report §7).
- **User-mandated process rule:** after EVERY action affecting any TS-related
  process (incl. our executables/tests), check for error/crash popups
  (list_windows + tasklist TSCrashReport), read text, dismiss, correct course.
  Must be in every subagent brief.
- **Overriding product requirement (user-mandated, report §7 preamble +
  §7.3):** detection/diagnosis is NOT sufficient — the tool must not crash
  the user's TS processes nor risk warehouse corruption. Corruption risk of
  mid-write termination is untested [OPEN]; next teams must run the §7.3
  forced-crash-during-save experiments and implement pre-flight integrity
  checks + partial-batch reporting.
- Investigation-phase experiments ran under `tmp\exp\` (reports, frida
  stacks, dumps, crash artifacts, watcher log); the §7.1 diagnostic build
  itself is committed (see above).

## Process lessons (standing)

- A tool call reported "Canceled due to user interrupt" AFTER the brief had
  been delivered to the subagent — verify delivery before assuming not-received.
- Destructive/long experiments must be polled to completion and reported;
  continuous watcher (`tmp\exp\watcher.ps1` pattern) for TS-touching work.

## Active investigation: user-machine crashes (dist_logs/)

- Analyzing `dist_logs/` — 21 `tsdev_cli_diag` logs + one 1.4 GB MDMP
  minidump harvested from the user's machine (exe runs from
  `C:\Users\user\protege_strategy_dev\tsdev_automation_dist_diag\`).
  All runs = batch ops on strategies (`batch-delete-dir` /
  `batch-upsert-dir` / `batch-insert-dir`).
- Prior RCA: `reports/2026-09-02_v0.1.0_batch_hang_root_cause_analysis.md`
  — PROVEN structural cause: INFINITE waits on orlib20 dispatch →
  WHServer.exe COM; stall=silent hang, server death=worker exception
  (0xE06D7363) + TSCrashReport. Diag build (TSDEV_DIAG) shipped per §7.1.
- Recon so far: run 39964 (upsert, 09-24 08:04) watchdog-fired ~82s,
  `watchdog.warehouse` cmd(+0x04)=53 in flight, evDone not-signaled,
  minidump written, exit 13. Run 5528 (upsert, 09-24 10:08) truncated
  inside `dispatch.Linker.enter cat=4 progId=518 when=reverse-enum`
  (37.86s, no .leave); whserver.sample present=1 pid=10436 windowless
  until log ends 55.5s. Run 3104 (delete, 10:08) finished batch.end
  exit=2 in 2.5s (8/9 items absent).
- cdb.exe available (`Windows Kits\10\Debuggers\x86\cdb.exe`) for dump.
- USER CONFIRMED: crash symptom = TSCrashReport dialog for TSDev and/or
  TS Desktop; TSDev/desktop is USUALLY OPEN during batch runs; some
  dead runs died on their own, some were user-killed (unknown which).
- CENSUS (subagent): 13 clean batch runs, 3 real failures ALL on
  09-24, all inside post-verify reverse-enum/link traversal:
  45356 @08:00 hung in `phase.link.enter pre-verify` (modify,
  progId=11798); 39964 @08:04 hung in `dispatch.LoadMasterRecsEX
  cat=4 when=reverse-enum` during traverse of NEW progId=11882;
  5528 @10:08 hung in `dispatch.Linker cat=4 progId=518
  when=reverse-enum` during traverse of NEW progId=11883. WHServer
  pids: 9012 (09-03..05), 27636 (09-11), 9928 (09-18..19), 19532
  (09-20..24 08:00 incl. clean 728MB run), 48560 (08:04 run),
  10436 (10:08 run) — server restarted between the failed runs;
  39964 hung in 22s on a FRESH WHServer (rules out
  leftover-wedged-server as sole cause).
- DUMP (subagent, full chain proven): batch worker 43460 →
  WFSO(evDone,∞) at orlib20+0x1CF5EC ← submit wrapper +0x1C5B8A
  (arg0=0x35=cmd 53) ← +0x1C3C81; orlib20 WORKER thread 47936 in
  combase!CCliModalLoop::BlockFn → ObjectStublessClient(method 9) →
  NdrClientCall2 — infinite STA wait for WHServer pid 48560 COM reply
  that never came. wh+0x76 workerTid field = SUBMITTER tid (43460),
  not the worker. orlib20 = 9.50.1.3344 confirmed on user machine.
  No AV/EDR DLLs in-proc. WHServer-side state invisible in this dump
  → next diag step: dump WHServer.exe itself on watchdog fire.
- User-visible crash = TS-side: WHServer stall/death shared by our
  proc + open desktop → RPC exception → TSCrashReport on both.
- cmd 0x35 = internal worker cmd (IProgram vtbl+0x24 record fetch)
  behind `_LoadMasterRecsEX` — not dispatched by our code directly.
- `batch.end` is the designed last record (nothing logged in teardown);
  VEH crash path writes NO diag record → truncation can't distinguish
  external kill vs silent fatal exception.
- whserver.sample: presence-only (WHServer always windowless in field),
  does NOT feed watchdog; pid changes only inferable from pids= field.
- CONCLUSION: all 3 failures = proven stall flavor; WHServer-side
  wedge reason still OPEN — needs WHServer dump at hang time.
  Full report: `reports/2026-09-29_dist_logs_crash_analysis.md`
  (adversarially reviewed → APPROVED after KB-§9 correction).
- LEADING FIX CANDIDATES (in report §5): extend diag to dump
  WHServer/TSDev/orchart on watchdog fire; collect user .tsc crash
  artifacts; replace brute-force reverse-enum with cmd 0x3B Callers;
  coalesce per-item 0x0D/0x40A notifications; production bounded-wait
  loud-abort; dev repro = TSDev.exe open + big batch.

## Current state (not derivable from code)

- **Quiet-output TVS done — committed + released as v0.1.3.** New batch
  console contract: human mode = per-item `BATCH SUCCESS/FAILED` completion
  line only (no `BATCH START`; FAILED followed by the item's `humanStderr`
  detail block) + `BATCH COMPLETE`; stdout JSON array ONLY under `--json`;
  `--json` keeps full NDJSON incl. `batch_item_started` (client's
  `_run_batch_controlled` requires the pairing). `--pbar`: bar-only while
  items succeed, `BATCH COMPLETE` only when not clean, single mutation-op
  `SUCCESS:` stdout suppressed (query ops keep answers), lib log lines
  routed above the bar via new `tsdev_log_set_sink` hook. Elevated gate
  36/37 green (1 fail = gate-script cleanup double-delete, items verified
  gone). Adversarially reviewed (2 rounds: code + tests/docs).
- **`--pbar` shipped earlier:** global position-free boolean,
  stderr-only in-place `\r` bar (WriteConsoleW on console / UTF-8 on pipe),
  0->N batch + 0->1 single ops, `— N failed` suffix, `--json`/`serve` =
  usage error. E2E suite 16 (`tests/test_pbar.ps1`, `-SkipPbar`, now
  PB1–PB10) wired into run_regression.ps1.
- **Console-path verification recipe** (for future UI work): PowerShell
  `Add-Type` C# ConPTY harness — CreatePipe pair + CreatePseudoConsole +
  STARTUPINFOEX with `PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE = 0x00020016`
  (attribute NUMBER=22 | THREAD flag 0x00020000; the INPUT flag is
  0x00040000 and does NOT work here); `CreateProcessW` needs
  `CharSet.Unicode` + `lpApplicationName` set; ConPTY holds the output
  pipe open — WaitForSingleObject child exit THEN ClosePseudoConsole,
  then drain. Script was throwaway `tmp\conpty_run.ps1` (deleted).
- The requested batch-progress observability and
  cooperative cancellation work (C++ direct CLI, serve broker, Python
  client) is implemented and verified: the final full regression is green
  and there is no pending acceptance blocker.
- **Both former probe items are GONE from the warehouse** (Function
  `TsDeskFnProbe_20260829`, Indicator `TsDeskFnProbeCaller_20260829`).
  Do not rely on or reuse them; E2E suites create their own disposable
  fixtures.
- TradeStation 9.5 installed (`C:\Program Files (x86)\TradeStation 9.5`);
  CLI works against the warehouse WITHOUT TS Desktop running.
- WHServer.exe spawns on demand per CLI run (COM activation); lifetime is
  lazy (can serve multiple runs and linger — E6). TS Desktop process is
  `orchart.exe` (would not stay running on this machine when launched
  directly, 2026-09-02).
- Probe items from the hang investigation were all deleted (45/45 verified
  absent 2026-09-02); no TS processes left behind.
- **Current shell is NOT elevated** (verified 2026-09-29 via `net
  session`); direct `tsdev_cli` runs from it need `Start-Process -Verb
  RunAs`, and each elevated launch costs ONE interactive UAC prompt
  (ConsentPromptBehaviorAdmin=5); batch elevated steps into one launcher
  script.
- PS 5.1 gotchas hit live: `native.exe 1>file` writes UTF-16LE (BOM FF FE)
  — decode with `ReadAllText` (BOM auto-detect), never explicit UTF-8;
  `Mandatory` string params reject empty strings unless
  `[AllowEmptyString()]`; debug traces embed addresses
  (`refreshCache: ENTRY 0x5E=<addr>`) → never byte-compare stderr across
  runs; `exists` on a MISSING item triggers second-pass `refreshCache`.
- **Ambient PSModulePath is misordered on this machine**: pwsh-7 module
  dirs precede `System32\WindowsPowerShell\v1.0\Modules`, so unguarded
  PS5.1 child launches can resolve Core-only pwsh-7 modules and lose
  cmdlets like `Get-FileHash`.
- Launching the requireAdministrator build of `tsdev_cli.exe` from a
  non-elevated, non-interactive launcher pops an invisible
  secure-desktop UAC prompt and hangs the launcher; do not spawn it
  that way from automation. Non-elevated CLI behavior is exercised via
  the supported `TSDEV_REQUIRE_ADMIN=OFF` asInvoker build (exit 11),
  not a runtime shim.

## Next TVS

None committed. Deferred candidates (require C++ serve schema changes; do
only if explicitly requested): per-request `verbose` in serve requests;
response-schema field exposing broker/CLI log output.

## Key references

- Root-cause report (SSOT for the hang investigation):
  `reports/2026-09-02_v0.1.0_batch_hang_root_cause_analysis.md`.
- Recalc / cross-process notification mechanism (SSOT):
  `docs/RE/RE_OrlibRegisterNotificationHandle.md`.
- Read-only function root cause: KB §11. Function-modify recalc + matrix: KB §13.
- TSDev UI save path vs CLI ordinal diff: KB §12. Dependents (0x3B empty, paused): KB §9.
