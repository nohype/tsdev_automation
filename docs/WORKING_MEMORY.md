# Working memory — WHServer VAS recovery (COMPLETE, committed)

## Final state (2026-10-21)

- **Committed:** `78a32ab` feature commit (30 files) + `6d90c6a` `.devin/` config. Tree clean.
- **Gated recycle (`--recycle-whserver`) live-verified end-to-end**, refuse AND commit arms, on all three surfaces (direct CLI, Python client, elevated serve broker). Zero dialogs, zero crash reports, zero orphaned lock holders across all runs.
- Architecture (see handoff §8 + KB §21): engine runs ONLY at never-attached sites (batch pre-flight, serve pre-first-request); attached sites defer = bounded owning-thread `closeWarehouse` → graceful `ExitProcess(18)` + resume manifest (close-fail → exit-19 `terminateSelf`). Client respawn reap is wait-only `max(360s, TSDEV_DISPATCH_TIMEOUT_SECS+60s)` — never TerminateProcess.
- Serve arm choreography: 3 brokers per breach (defer-18 → engine-18 → complete-0). Batch arm continues in-process after resume.
- ForceShutDown live signature: `m_nLock` 0 → `0xFFFFFFF9` (−7) → death. Self-reap: 0 → READ_FAIL, ~6 s.
- Full verification record: clean Win32 build 0 errors; `-NoTs` all-pass; pytest 415; live E2E 11/11; full regression 15/15 (serve needed UAC `ConsentPromptBehaviorAdmin=0`, restored to 5); recycle live tests on CLI + Python + serve all PASS.
- Reviews: all waves adversarially APPROVED (hardening F1-F3, wave-2 C1/C2/D/Fix-A/Fix-B, TVS-R1/R2, client.py respawn-reap fix).

## Operational facts for future sessions

- Live recycle tests require **desktop closed** — foreign clients hold `m_nLock` → engine correctly refuses `census_lock`.
- Desktop closed is currently also forced by the corrupted-cache incident: TS runs offline-mode only (login in use elsewhere).
- `m_nLock` probe: `powershell -ExecutionPolicy Bypass -File tmp\wh_lock.ps1` (fixed VA `0x43f8a8`, identity-gated).
- Fresh-server free VAS ≈ 1.9 GB cold / ~1.1 GB warmed; guard threshold `--whserver-free-mb` compares against that.
- Orphaned lock holder cleanup: only safe with desktop closed + zero foreign clients; `taskkill /PID` on the orphaned WHServer is proven clean (self-reap impossible since lock never reaches 0).
- UAC-elevation tests: temporarily set `ConsentPromptBehaviorAdmin=0`, restore to 5, verify.
- Bash shell mangles `tasklist/taskkill` flags and `$` vars — use PowerShell with single-quoted strings. `> nul` in bash creates a literal `NUL` file — avoid.

## Known non-blocking residuals

- Exit-18 defer boundary loses the partial results array only on the close-hang `terminateSelf(19)` path (documented §5 convention).
- Pre-existing HEAD warnings: C4476 format-string (tsdev_cli.cpp usage text), strncpy deprecation (dispatch_watchdog.cpp), C4244 (test_path_discovery/xutility).
- Serve never-attached site exits 18 even after a *successful* engine run (3-broker choreography) — by design, response frame must reflect engine outcome.
- Deferred marker text reuses "WHServer recycled; resubmit remaining items" phrasing; `reason` field is the truthful discriminator.
