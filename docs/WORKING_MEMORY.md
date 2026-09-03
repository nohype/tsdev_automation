# Working Memory — TSDev Automation

Live, real-time state for the PM. Keep minimal; drop stale/superseded facts.
Do not restate what AGENTS.md or the code documents (machine conventions,
paths, and process rules live in AGENTS.md).

## Current state

- Branch `exp/remote-control`. Requirements SSOT: AGENTS.md (R1–R15).
  TSDev.exe facts SSOT: docs/KNOWLEDGE_BASE.md (2026-09-03: complete
  doc-template map incl. ShowMe=1282/TradingApp=5122 — R13 fully covered;
  exists discriminator; raw-pipe surface complete, no List/Exists/query
  command; Open dialog not externally reachable; title/session-restore
  behaviors).
- **TVS #1 DONE (commit ad1f93a): `tsdevctl open`** — lifecycle + pipe channel,
  adversarially reviewed and approved.
- **TVS #2 IMPLEMENTED (2026-09-03, this session), fix round after DISAPPROVAL:**
  `tsdevctl exists --type <Strategy|Function|Indicator|ShowMe|TradingApp>
  --name <n>`. Mechanism: pipe-open → poll the editor (`WM_GETTEXT`, sized via
  `WM_GETTEXTLENGTH`) until the phantom error page appears (⇒ not-exists) or
  the text is stable across two 500 ms reads (⇒ exists) — a single read is
  NOT decidable (verified live: a fresh phantom's editor transiently holds a
  single space; rendering lags under session-restore flood). Closes only docs
  it opened; re-scans for late-executed opens on failed waits. Exit 0=exists,
  1=NotFound (successful answer), 10=Unexpected, 2..9 unchanged. All five R13
  types supported. Review round 1: DISAPPROVED (5 findings) — all fixed:
  render-race poll (CRITICAL), timeout-path late-open re-scan (HIGH), E2E
  settle fix committed (MEDIUM), E2E dead type-number data removed + session
  hygiene documented (MEDIUM), verbatim-name/ProgId boundary accepted by PM
  (LOW; the channel cannot provide it — documented in usage_text() and the
  KB "Exists output boundary" entry; the round-2 review caught that the
  original claim of documentation was false, now actually documented).
  Verified: unit tests green; E2E green ×5 consecutive runs after the fix
  (previously reproduced failures: 2/6/8/8/6 false positives per run).
  - PROCESS DEVIATION (documented): the `subagent_general` delegation channel
    broke mid-session (4 consecutive empty/failed completions: da60d4c9,
    43a23faa, 73874f38, ed75b36d; only the explore profile worked). The PM
    implemented TVS #2 directly as error recovery; spec-discovery artifacts
    from the first (partial) implementer were reused (tmp\phantom_template.txt).
  - Research reports (scratch): tmp/report_live_exp_20260903.md (live probes,
    APPROVED by independent re-run), tmp/report_static_re_20260903.md (complete
    pipe surface + doc-template map; bridge healthy on port 8089).
- Next TVS (recommended): **`getsrc` (Get source)** — same mechanism as exists
  (pipe-open → WM_GETTEXT → emit raw source → close), `--output <file>` per
  R2; decrypt behavior must be checked against TSDev's own handling. Then
  Insert/Modify (editor text set + the UI verify path, KB-documented). List
  has NO external trigger (KB negative evidence) — needs a scoped decision
  (defer or new research approach).
