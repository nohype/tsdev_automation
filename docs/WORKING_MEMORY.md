# Working memory

## Current state (2026-11-02) -- residuals cleanup in progress (PM orchestration)

- HEAD c2c206c: RC-02 (EXCEPINFO BSTR SEH free + test_invoke_excepinfo, red 10 green 0) and RC-13 (HGLOBAL SEH free + unlock helper + test_master_recs_seh, red 12 green 0) IMPLEMENTED, independently REVIEWED/APPROVED, COMMITTED as 7863dba + c2c206c. Tree clean. Unpushed: 5 (9bb1842, 0abe7c0, 60429da handoff, 7863dba, c2c206c).
- Owner decisions (2026-11-02): RC-21 archive evidence to tracked paths; RC-18 pre-stage results array (opt 1); RC-19 document exit-18 contract; RC-25/26 tag da74dc1 then delete both; RC-23 license research deferred to very end (copyleft + attribution + no-liability + dual commercial premium -- compare options).
- Next: P1 RC-18 + RC-11, then P2 test-strength/contract batch, then P3. Handoff: docs/HANDOFF_residuals.md.

- Feature DELIVERED and live-verified end to end: `tsdev_cli list <type>` + `TsDevClient.list_items(type)` + broker route. Contract: human = one stored name per line (zero items -> no output = success); `--json` names array always on success; sorted by name (case-insensitive ASCII, stable ties) applied ONCE in doList (renderers/Python passthrough); no dedup; names verbatim case-preserved; exit codes follow `exists` (0/1/2/4/10/11; 4 = fail-loud, never a false empty list).
- Name source: WHServer IWHouse COM enumeration (`ProgramsCountByType` + `NextId` + `ProgramById` walk + `IProgram::Name`), pure GetIDsOfNames, whType 0/1/2/1282/5122, KB Sec 17 activation chain, one STA thread. Only non-typelib contract: factory IID {7CEEACBC} + vtbl slot 8. New module src/whserver_names.{h,cpp} (IWhNameSource seam + pure collectTypeNames + WhComNameSource). SSOT for the RE: tmp\enum_mechanism_report.md; KB Sec 22-25 records all discovered facts.
- Companion root-cause fix landed and verified (user-approved scope): `exists`/`dependents` Phase-1 description false-positive ELIMINATED -- enumerateRecords is name-index-only with a fail-loud lookupOk tri-state (incl. the contradictory-BST-answer hole closed); live-proven: both historical FP strings now exists=0, 1229/1229 name parity byte-identical to pre-fix baseline, case semantics preserved (tmp\exists_fix_corpus\ RED+GREEN transcripts).
- ALL reviews APPROVED (C++ core, Python, CLI wiring, COM name source, KB (after 9-finding fix round), sorting slice, exists fix, serve op, docs, both final fix slices). Final live gate PASS (2026-11-02): production insert/modify/get-source/delete E2E exit 0 + byte-identical round-trip; all 5 types sorted/verbatim/counts 761/84/314/61/9; error paths 1/1/1/2/2 incl. empty-string -> exit 2 parity; FP regression clean; probe lifecycle +1/-1 cross-process visible; Python direct + broker identical results; -NoTs 36/55/0/19-skip; pytest 466 passed; zero popups at 15 checkpoints; cleanup proven.
- KNOWN ANOMALIES (honest, for next team): (1) pytest run under concurrent C++ build once crashed with an access violation in the fake-tsdev-cli harness thread (test_tsdev_client.py mock harness) -- not reproducible in isolation, clean 466-pass re-run; likely flaky CPython/threading under load. (2) TsDevClient(use_serve=True) in an ALREADY-elevated shell bypasses the broker by documented design (_serve_active = use_serve AND not elevated); broker evidence gathered via the forced non-elevated routing branch (real pipe/runas underneath).
- Committed as two commits (see git log): `list:` feature code+tests+docs, `wm:` AGENTS/WORKING_MEMORY. **Handoff for the next team: docs/HANDOFF_residuals.md -- ALL known residual issues (29 RCs: 4xP1, 11xP2, 13xP3, 1 owner item; pre-existing + this session's leftovers), review-APPROVED 2026-11-02.** RC-14 correction vs older claims: only C4244 (tests/test_path_discovery.cpp:199) reproduces at default warning level in both Release+Debug; the historical C4476/C4996 shapes exist in code (src/tsdev_cli.cpp:592 bare %, dispatch_watchdog.cpp:198 strncpy) but were NOT reproduced -- the old WM warning list was an unreproduced claim.

## Operational facts

- PM + subagent shells High IL (elevated); direct tsdev_cli works without tmp\elev_runner.ps1.
- Live recycle tests require desktop CLOSED (foreign clients hold `m_nLock`). Machine runs TS offline-mode.
- `m_nLock` probe: `powershell -ExecutionPolicy Bypass -File tmp\wh_lock.ps1` (fixed VA `0x43f8a8`, identity-gated).
- Fresh-server free VAS ~1.9 GB cold / ~1.1 GB warmed. Orphaned lock holder cleanup: desktop closed + zero foreign clients only; `taskkill /PID` proven clean.
- UAC-elevation tests: temporarily set `ConsentPromptBehaviorAdmin=0`, restore to 5, verify.
- Bash mangles `tasklist/taskkill`/`$` -- PowerShell single-quoted strings. `> nul` in bash creates literal `NUL` file -- avoid. PowerShell 5.1 drops empty CLI args -- use `cmd /c` for `list ""`-style calls.
- Do NOT frida-spawn tsdev_cli (TSCrashReport risk); frida-attach or in-process tmp\ diagnostics only.
- Live `list strategy` walk ~4.2 s (function 0.2 s), synchronous RPC. `cmake` is not on PATH in subagent shells -- use the VS 18 BuildTools full path.
- Popup-audit regex false positive: explorer windows titled with the TradeStation path -- exclude `explorer`.

## Known non-blocking residuals

- Exit-18 defer boundary loses partial results array only on close-hang `terminateSelf(19)` path (HANDOFF_WHServer_VAS_recovery.md Section 5).
- Pre-existing HEAD warnings: C4476 (tsdev_cli.cpp usage text), strncpy deprecation (dispatch_watchdog.cpp), C4244 (test_path_discovery/xutility).
- Serve never-attached site exits 18 even after successful engine run (3-broker choreography) -- by design.
- Source-code COMMENTS still contain non-ASCII (~421 em-dashes) -- outside docs/CLI-output rule scope.
- check_ts_state.ps1 "WHServer ABSENT (ANOMALY)" label false-alarms post-run.
- Review MINORs (approved, unaddressed by choice): whserver_names.cpp CoInitializeEx non-balancing (pattern debt, 2 sites); EXCEPINFO BSTR leak on SEH Invoke path (edge-only); test_serve_dispatch 24f asserts only key-substring (gate-specific message unpinned) and 24d message text unpinned; test_enumerate 7b fixtures depend on live dictionary description strings; test_list_cmd tie-runs are N<=4 (std::stable_sort not discriminated from std::sort at >16).
- Theoretical HGLOBAL leak on _LoadMasterRecsEX SEH path (pre-existing 3-site pattern).
- test_enumerate (RequiresTs) creates/deletes its own fixtures when run live; skipped under -NoTs by design.