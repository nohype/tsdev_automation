# Working memory

## Current state (2026-11-03) -- ALL 29 RCs implemented+reviewed+committed; live gate running

- HEAD 6aa4a5b. Map: 01 f78abb0, 02 7863dba, 03 22041d5, 04+05 e5f6392, 06+07+10 a2734a0, 08 254dad8, 09 0dc105c, 11 6aa4a5b (record; 10 runs 0 crashes, accepted flake), 12 8dab6f7, 13 c2c206c, 14 2239541 (0 warnings both configs), 15 24c3be1 (7,604->0 bytes, KEEP 2 runtime-identical escapes), 16 01e9621, 17+18 aebe517, 19 9e514e4, 20 cd3aa2a, 21 4f682fb, 22 pushed via 069da6a (CI 41454791554 green), 23 research 6a29397 (LICENSE decision OWNER-DEFERRED), 24 3781670, 25+26 tag archive/tsdevctl-approach-2026-10 pushed + branches deleted, 27 890818f, 28 0f5567e, 29 a328a76.
- Owner decisions (2026-11-02): RC-21 archive to tracked paths (done); RC-18 pre-stage results array opt 1 (done); RC-19 document exit-18 contract (done); RC-25/26 tag+delete (done); RC-23 license: research done (GPL-3.0-only + commercial dual recommended), final choice OWNER-DEFERRED.
- REMAINING: consolidated live gate G1-G3 + production insert/modify/delete E2E (background) -> push all + CI watch -> closeout. Unpushed: everything after 069da6a.

- Feature DELIVERED and live-verified end to end: `tsdev_cli list <type>` + `TsDevClient.list_items(type)` + broker route. Contract: human = one stored name per line (zero items -> no output = success); `--json` names array always on success; sorted by name (case-insensitive ASCII, stable ties) applied ONCE in doList (renderers/Python passthrough); no dedup; names verbatim case-preserved; exit codes follow `exists` (0/1/2/4/10/11; 4 = fail-loud, never a false empty list).
- Name source: WHServer IWHouse COM enumeration (`ProgramsCountByType` + `NextId` + `ProgramById` walk + `IProgram::Name`), pure GetIDsOfNames, whType 0/1/2/1282/5122, KB Sec 17 activation chain, one STA thread. Only non-typelib contract: factory IID {7CEEACBC} + vtbl slot 8. New module src/whserver_names.{h,cpp} (IWhNameSource seam + pure collectTypeNames + WhComNameSource). SSOT for the RE: docs\evidence\enum_mechanism_report.md; KB Sec 22-25 records all discovered facts.
- Companion root-cause fix landed and verified (user-approved scope): `exists`/`dependents` Phase-1 description false-positive ELIMINATED -- enumerateRecords is name-index-only with a fail-loud lookupOk tri-state (incl. the contradictory-BST-answer hole closed); live-proven: both historical FP strings now exists=0, 1229/1229 name parity byte-identical to pre-fix baseline, case semantics preserved (docs\evidence\exists_fix_corpus\ RED+GREEN transcripts).
- ALL reviews APPROVED (C++ core, Python, CLI wiring, COM name source, KB (after 9-finding fix round), sorting slice, exists fix, serve op, docs, both final fix slices). Final live gate PASS (2026-11-02): production insert/modify/get-source/delete E2E exit 0 + byte-identical round-trip; all 5 types sorted/verbatim/counts 761/84/314/61/9; error paths 1/1/1/2/2 incl. empty-string -> exit 2 parity; FP regression clean; probe lifecycle +1/-1 cross-process visible; Python direct + broker identical results; -NoTs 36/55/0/19-skip; pytest 466 passed; zero popups at 15 checkpoints; cleanup proven.
- KNOWN ANOMALIES (honest, for next team): (1) pytest run under concurrent C++ build once crashed with an access violation in the fake-tsdev-cli harness thread (test_tsdev_client.py mock harness) -- not reproducible in isolation, clean 466-pass re-run; likely flaky CPython/threading under load. (2) TsDevClient(use_serve=True) in an ALREADY-elevated shell bypasses the broker by documented design (_serve_active = use_serve AND not elevated); broker evidence gathered via the forced non-elevated routing branch (real pipe/runas underneath). VERIFICATION RULE: to exercise the broker, run the client UNELEVATED with use_serve=True, or force the branch in tests (stub _is_elevated/_serve_active); confirm via broker_process_handle() (client.py:2325) -- non-None = broker ran, None = direct path. Identical results alone prove nothing (both paths return the same shapes).
- Post-cleanup (HEAD f799284): all 29 RCs implemented+reviewed+committed per the line-3 map; live gate PASS; CI green. docs/HANDOFF_residuals.md is retained as archaeology (fix directions + evidence reasoning preserved below its top banner, not actionable work). RC-14 correction vs older claims: only C4244 (tests/test_path_discovery.cpp:199) reproduces at default warning level in both Release+Debug; the historical C4476/C4996 shapes exist in code (src/tsdev_cli.cpp:592 bare %, dispatch_watchdog.cpp:198 strncpy) but were NOT reproduced -- the old WM warning list was an unreproduced claim.

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

## Known non-blocking residuals (post-cleanup standing notes)

- RC-11: one-off harness AV closed as accepted flake (10-run campaign 0 crashes, record docs/RC11_stress_campaign.md); fix sketch + retry-guard proposed only, not implemented. Re-open if a fatal-exit signature recurs.
- RC-12: one-off zipfile shutdown noise closed as stdlib-internal accepted (record docs/RC12_*.txt). Re-open on recurrence with preserved traceback.
- RC-17+18 residual: serve attached-site defer still has no pre-stage (no stdout array at risk there); batch close-hang now pre-stages array+manifest (BUNDLE_README:69).
- RC-19: serve never-attached exit-18 is the pinned contract (Group 27), not a defect.
- RC-23: LICENSE choice OWNER-DEFERRED (advisory docs/LICENSE_RESEARCH.md); tree ships unlicensed = all-rights-reserved until owner picks.
- RC-29: elevated-shell broker bypass is by design; verify unelevated or check broker_process_handle() (client.py:2325).
- test_enumerate (RequiresTs) creates/deletes its own fixtures when run live; skipped under -NoTs by design.