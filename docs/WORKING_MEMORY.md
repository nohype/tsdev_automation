# Working memory -- WHServer VAS recovery + continuation-policy work

## Current state (2026-10-28, post tri-state recycle-authorization wave)

- **Tri-state recycle authorization LANDED (unit-verified, NOT live-verified).** `tsdev::RecycleMode { Off, RescueOnly, Full }` (src/whserver_health.h); default = RescueOnly (recycle reserved for the abort-floor rescue; band ALWAYS continues); explicit `--recycle-whserver`/`TSDEV_RECYCLE_WHSERVER` = Full (today's full behavior incl. conflicts + 5-line warning); explicit `--no-recycle-whserver`/`TSDEV_NO_RECYCLE_WHSERVER` = Off (today's unauthorized behavior). Precedence pinned in test_recycle_whserver_flag check 3b (both flags -> exit 1; flag beats env; env-vs-env -> negative wins; env "0" neutralizes own arm only). Conflicts scoped to Full; RescueOnly + --batch-progress-control SUPPRESSES the rescue (run behaves as Off) + one-line NOTE. Python ctor kwarg tri-state (None default forwards nothing; False forwards --no-recycle-whserver on batch cmd lines AND broker launch). Design/details: HANDOFF_WHServer_VAS_recovery.md Section 8.4.
- **Serve E2E default-rescue phase: checked, NOT added.** Mechanically possible with run_serve_e2e.ps1's UAC-disabled setup (respawn auto-elevates; client respawn+resubmit is live-verified) but recycles the real WHServer up to 3x and needs the desktop-closed window (else census_lock refuse arm); add as an opt-in phase only in an authorized desktop-closed window.
- **Field incident FIXED and live-verified.** The 2026-10-28 field report (batch aborts at free=246MB while Desktop+TSDev held the server; TSDev UI kept working) is resolved by the two-threshold continuation policy:
  - `vasGuardAction` (src/whserver_health.h): trigger 256/64 = recycle attempt (Full mode only now); abort floor 64/32 = hard stop (Off) / rescue (RescueOnly+Full); above floor -> Continue except Full; below floor -> fail-closed as before for Off. Flags `--whserver-abort-free-mb`/`--whserver-abort-largest-free-mb` + env mirrors; inverted trigger/floor rejected at parse (vasThresholdsInverted).
  - Foreign-client latch: m_nLock sampled BEFORE own COM activation (batch tsdev_cli.cpp ~5617, serve ~7574) -> foreign clients present -> skip recycle attempts, continue.
  - census_lock refuse above floor -> continue (covers desktop-attaches-mid-run); below floor -> abort 19. Other refuse reasons stay fail-closed.
  - New event `whserver_vas_continue` (cause: recycle_not_authorized | foreign_clients | census_lock), once per run/broker; client.py tolerates unknown events (verified).
- **Wave-1 measurement (basis):** per-item marginal VAS ~0.07MB; session working set ~920MB (library-size dependent) released ~7s after close; plateau is library-dependent; death zone ~10MB. Static 256MB threshold confused plateau with death.
- **Output hygiene + logging landed:** ASCII-only emitted output (pbar `#`/`-`, em-dashes -> `--`), slim 5-line recycle warning (+ client.py sync), census_lock plain-English detail, "to continue the remaining items, run: ..." hint, `--log-file <path>`/`TSDEV_LOG_FILE` full-debug file sink (inside tsdev_log_message, independent of pbar sink swap, broker-forwarded). All .md docs pure ASCII.
- **Verification (tri-state wave):** full build 0 errors; test_whserver_health / test_recycle_whserver_flag / test_recycle_resume all pass; pytest -m "not e2e" 420 passed; -NoTs regression green. Live (TS-touching) verification of the RescueOnly default rescue NOT run this session (task constraint: unit + -NoTs only) -- the below-floor rescue arm and the serve exit-18 boundary on the default mode still need a desktop-closed live window.
- Reviews: M1 continuation policy APPROVED; --log-file APPROVED; output-hygiene DISAPPROVED->fixed (missed ps1 lines + new-comment non-ASCII) then verified.
- Elevation note: PM shell + subagents run at whatever IL the terminal has; live TS work needs High IL (user re-elevated 2026-10-28). Elevated job-runner pattern in tmp\elev_runner.ps1 if needed.
- Known residuals: source-code COMMENTS still contain non-ASCII (~421 em-dashes in tsdev_cli.cpp etc.) -- out of the docs/CLI rule scope, optional future sweep; check_ts_state.ps1 "WHServer ABSENT (ANOMALY)" label false-alarms post-run (idle self-reap is normal).
- abort_reason truthfulness FIXED (2026-10-28): vasAbortReason (src/whserver_health.h) names the metric that crossed the FIRING threshold pair -- abort floor below the floor, restart trigger for an authorized above-floor boundary; both crossed -> deeper relative breach (value/threshold ratio, integer cross-product), equal ratios -> largest block. Token format unchanged; client.py/tests parse no reason text (verified).

## Field incident + measurement (2026-10-28)

- Field report: `batch-upsert-dir --recycle-whserver` on 252 strategies with Desktop+TSDev RUNNING: item-47 defer(18) -> resumed engine `census_lock` refuse(19) -> abort loop. Server sat ~246MB free; TSDev UI ran 250+ inserts on the same server fine -> tool unusable though never crashing.
- **Wave-1 live measurement DONE** (desktop closed, `--no-whserver-vas-guard`, 300+50 items): WHServer NEVER neared death. Per-item marginal cost ~0.07MB; dominant consumer = per-session working set ~920MB (scales w/ library size), fully released ~7s after session close. Plateau ~950-985MB free during work; second run on warm server fine; zero crashes/dialogs; SCM respawns fresh ~1.9GB server per dispatch. Death zone ~10MB (field bad_alloc) unreachable by inserts.
- **Verdict:** static 256MB threshold confuses big-library plateau with approaching death. New policy (approved): recycle trigger stays ~256MB; new ABORT FLOOR ~64MB free / ~32MB largest; above floor -> continue (TSDev behavior); census_lock under foreign clients -> continue not abort; early m_nLock pre-activation sample detects foreign clients to skip pointless defer dance.
- New AGENTS.md rules: ASCII-only in docs+CLI output; console output user-facing/minimal, debug -> log files.
- Elevation NOTE: subagent shells AND current PM shell run Medium IL -- direct tsdev_cli can't launch (original elevated shell lost when terminal crashed). Workaround: elevated job-runner `tmp\elev_runner.ps1` (one user UAC consent) watches `tmp\elev_jobs\submit\*.ps1`; children inherit elevation. STOP file kills it. Runner stopped after measurement.
- In flight: A1 (49167e0e) output hygiene. DONE: A2 docs ASCII sweep (all .md pure ASCII, incl. cp1252 fixes in KB). QUEUED: log-file mechanism then continuation-policy impl (sequential, same files).
- Design map: guard = vasGuardCheck tsdev_cli.cpp:5550-5651 + serveVasGuardRefuse 6994-7165; census_lock minted at whserver_recycle.cpp:388-410 (continue-fallback seam); foreign-lock pre-attach latch = one RPM before first COM activation; log sink = inside tsdev_log_message (tsdev_lib.cpp:93), --log-file + TSDEV_LOG_FILE, forwarded to broker like TSDEV_WHSERVER_*; NDJSON events + `tsdev_cli:` lines MUST stay on stderr (client.py parses them).

## Final state (2026-10-21)

- **Committed:** `78a32ab` feature commit (30 files) + `6d90c6a` `.devin/` config. Tree clean.
- **Gated recycle (`--recycle-whserver`) live-verified end-to-end**, refuse AND commit arms, on all three surfaces (direct CLI, Python client, elevated serve broker). Zero dialogs, zero crash reports, zero orphaned lock holders across all runs.
- Architecture (see handoff Section 8 + KB Section 21): engine runs ONLY at never-attached sites (batch pre-flight, serve pre-first-request); attached sites defer = bounded owning-thread `closeWarehouse` -> graceful `ExitProcess(18)` + resume manifest (close-fail -> exit-19 `terminateSelf`). Client respawn reap is wait-only `max(360s, TSDEV_DISPATCH_TIMEOUT_SECS+60s)` -- never TerminateProcess.
- Serve arm choreography: 3 brokers per breach (defer-18 -> engine-18 -> complete-0). Batch arm continues in-process after resume.
- ForceShutDown live signature: `m_nLock` 0 -> `0xFFFFFFF9` (-7) -> death. Self-reap: 0 -> READ_FAIL, ~6 s.
- Full verification record: clean Win32 build 0 errors; `-NoTs` all-pass; pytest 415; live E2E 11/11; full regression 15/15 (serve needed UAC `ConsentPromptBehaviorAdmin=0`, restored to 5); recycle live tests on CLI + Python + serve all PASS.
- Reviews: all waves adversarially APPROVED (hardening F1-F3, wave-2 C1/C2/D/Fix-A/Fix-B, TVS-R1/R2, client.py respawn-reap fix).

## Operational facts for future sessions

- Live recycle tests require **desktop closed** -- foreign clients hold `m_nLock` -> engine correctly refuses `census_lock`.
- Desktop closed is currently also forced by the corrupted-cache incident: TS runs offline-mode only (login in use elsewhere).
- `m_nLock` probe: `powershell -ExecutionPolicy Bypass -File tmp\wh_lock.ps1` (fixed VA `0x43f8a8`, identity-gated).
- Fresh-server free VAS ~ 1.9 GB cold / ~1.1 GB warmed; guard threshold `--whserver-free-mb` compares against that.
- Orphaned lock holder cleanup: only safe with desktop closed + zero foreign clients; `taskkill /PID` on the orphaned WHServer is proven clean (self-reap impossible since lock never reaches 0).
- UAC-elevation tests: temporarily set `ConsentPromptBehaviorAdmin=0`, restore to 5, verify.
- Bash shell mangles `tasklist/taskkill` flags and `$` vars -- use PowerShell with single-quoted strings. `> nul` in bash creates a literal `NUL` file -- avoid.

## Known non-blocking residuals

- Exit-18 defer boundary loses the partial results array only on the close-hang `terminateSelf(19)` path (documented Section 5 convention).
- Pre-existing HEAD warnings: C4476 format-string (tsdev_cli.cpp usage text), strncpy deprecation (dispatch_watchdog.cpp), C4244 (test_path_discovery/xutility).
- Serve never-attached site exits 18 even after a *successful* engine run (3-broker choreography) -- by design, response frame must reflect engine outcome.
- Deferred marker text reuses "WHServer recycled; resubmit remaining items" phrasing; `reason` field is the truthful discriminator.
