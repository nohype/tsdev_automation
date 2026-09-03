# Working Memory — TSDev Automation

Live, real-time state for the PM. Keep minimal; drop stale/superseded facts.
Do not restate what AGENTS.md or the code documents (machine conventions,
paths, and process rules live in AGENTS.md).

## Current state

- Branch `exp/remote-control`. Requirements SSOT: AGENTS.md (R1–R15).
  TSDev.exe facts SSOT: docs/KNOWLEDGE_BASE.md.
- **TVS #1 DONE (commit ad1f93a): `tsdevctl open`** — lifecycle + pipe channel.
- **TVS #2 DONE (commit b94e63f): `tsdevctl exists`** — phantom-discriminator check.
- **TVS #3 DONE (commit cadbccd): `tsdevctl getsrc`** — open-read-close, source emission.
- **TVS #4 DONE (this commit): `tsdevctl insert`** — the first write operation.
  - Implementation: proactive exists check (exit 11 on collision) → pipe-open
    phantom (MDI command target) → show frame if hidden (REQUIRED for the New
    dialog's OK) → WM_COMMAND <NewCmdId> to the phantom's MDI child → TS_IDD_*
    create dialog → WM_SETTEXT name + IDOK → new real doc → WM_SETTEXT source →
    MDI-activate → verify (WM_COMMAND 0x296a, frame stays VISIBLE) → poll →
    failure classification (dialog safety net + Output error-list summary
    "N error(s)" via cross-process 32-bit LVITEM read) → close docs
    (close_doc_discard answers No on the failure path) → post-verify exists check.
  - Exit codes: 0 ok, 11 already-exists, 12 compile-failed (message includes
    "[item 'X' was created and saved uncompiled]" — the verify path saves
    before compiling, so failed items remain in the library, R10-reported).
  - Adversarially reviewed + APPROVED (2 cycles; the R6 pre-existing-window
    fix, verify-timeout discard close, and the name-set result check applied).
  - E2E (live, elevated, real TSDev): Strategy insert exit 0 (13.3s) + exists +
    getsrc exact; duplicate → 11; garbage source → 12 in 6.4s with the error
    text; Function insert exit 0 (8.0s); Function mismatched-name → 12. No
    residual dialogs / crash reports / stray processes.
  - Reviewer notes (non-blocking, for the next team): extend run_unit_tests.ps1
    to build the tsdevctl target (main.cpp /W4 /WX coverage); E2E for
    Indicator/ShowMe/TradingApp inserts; the error-list staleness probe;
    the usage text could mention the name charset restriction.
- Test items created in the TSDev library during E2E (no delete verb yet —
  manual cleanup or the Delete TVS): Tvs4S190626, Tvs4Rv191516, Tvs4F190725,
  Tvs4Bad190656, Tvs4Bad190814, Tvs4RvBad, Tvs4FinalOk, Tvs4FinalBad,
  Tvs4OkStrat, Tvs4InsertFlow, Tvs4HiddenInsert, Tvs4CopyStrat, Tvs4E2EStrat,
  Tvs4FinalStrat, Tvs4CopyStrat, Tvs4Bad190656, Tvs4Bad190814, Tvs4Fn190725,
  Tvs4F190725, Tvs4S190626 and earlier Tvs4* variants. The phantom docs
  (Style_Phantom_*, Tvs4* phantoms) were closed via tmp/cleanup_docs.py before
  the final TSDev restart; the library items remain until Delete exists.
- Ghidra: project `tsdev`, program `/TSDev.exe` open (18478 funcs), MCP bridge
  connected. Frida 17.17.0 available for live tracing.
- **Recommended next TVS: `tsdevctl modify`** — it reuses insert's proven
  machinery (pipe-open a REAL item → WM_SETTEXT new source → verify → poll →
  error-list classification) without the New-dialog complexity. Delete after
  that (it enables the insert-failure cleanup + the E2E gate's item cleanup).
