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
- **TVS #2 DONE (2026-09-03): `tsdevctl exists` — APPROVED after 3 review
  rounds** (commits e3bbc48 → 607f5e7 → 7cec93b → b94e63f). Final shape:
  `exists --type <Strategy|Function|Indicator|ShowMe|TradingApp> --name <n>`
  [--json --timeout --nohide --ts-path]; pipe-open → decidable editor poll
  (phantom error page ⇒ not-exists; text stable across two 500 ms reads ⇒
  exists; single reads are NOT decidable — transient single-space state,
  verified live); closes only docs it opened; late-open re-scan on failed
  waits (dialog/crash surfaced, SendMessageTimeoutW(SMTO_ABORTIFHUNG) bounds
  cross-process reads; 64 MB length cap). Exit 0=exists, 1=NotFound
  (successful answer), 10=Unexpected, 2..9 unchanged. Requested-name/ProgId
  boundary PM-accepted and documented (usage_text + KB). Verified: unit
  green; E2E green ×5 consecutive (reviewer, round 3) + further greens
  after the LOW-fix commits; zero dialogs; no residue; no stray processes.
  Review carry-overs for the NEXT TVS brief: (i) comment wording in
  doc_read.cpp slightly overstates the deadline bound (a wedged scan
  overshoots by ≤ one bounded pass); (ii) if a late-executed command raises
  a dialog AND opens a doc, the ErrorDialog return preempts the doc
  close/report (unreachable today — no pipe-reachable error dialog is known);
  (iii) collect_doc skips unreadable windows under a wedged UI thread
  (acceptable degradation, docs_before captured while healthy).
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
