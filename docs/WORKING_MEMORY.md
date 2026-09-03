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
- **TVS #2 IMPLEMENTED (2026-09-03, this session), pending adversarial review:**
  `tsdevctl exists --type <Strategy|Function|Indicator|ShowMe|TradingApp>
  --name <n>`. Mechanism: pipe-open → read editor (`WM_GETTEXT`, sized via
  `WM_GETTEXTLENGTH`) → TSDev's own phantom error page ⇒ not-exists; closes
  only docs it opened (R6); case-insensitive title match (TSDev keeps the
  original spelling for already-open docs). Exit 0=exists, 1=NotFound
  (successful answer), 10=Unexpected, 2..9 unchanged. All five R13 types
  supported (ShowMe/TradingApp numbers came from the completed doc-template
  map, live-verified). Verified: unit tests green; E2E green (open real,
  exists real/wrong-case/phantom ×5 types, residue checks, clean shutdown,
  zero dialogs).
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
