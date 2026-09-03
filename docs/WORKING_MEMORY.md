# Working Memory — TSDev Automation

Live, real-time state for the PM. Keep minimal; drop stale/superseded facts.
Do not restate what AGENTS.md or the code documents (machine conventions,
paths, and process rules live in AGENTS.md).

## Current state

- Branch `exp/remote-control`. Requirements SSOT: AGENTS.md (R1–R15).
  TSDev.exe facts SSOT: docs/KNOWLEDGE_BASE.md (2026-09-03: complete
  doc-template map incl. ShowMe=1282/TradingApp=5122 — R13 fully covered;
  exists discriminator; WM_GETTEXT trailing-NUL on BCGPEditCtrl; raw-pipe
  surface complete, no List/Exists/query command; Open dialog not externally
  reachable; title/session-restore behaviors).
- **TVS #1 DONE (commit ad1f93a): `tsdevctl open`** — lifecycle + pipe channel,
  adversarially reviewed and approved.
- **TVS #2 DONE (2026-09-03): `tsdevctl exists` — APPROVED after 3 review
  rounds** (commits e3bbc48 → 607f5e7 → 7cec93b → b94e63f). Final shape:
  `exists --type <Strategy|Function|Indicator|ShowMe|TradingApp> --name <n>`
  [--json --timeout --nohide --ts-path]; pipe-open → decidable editor poll
  (phantom error page ⇒ not-exists; text stable across two 500 ms reads ⇒
  exists); closes only docs it opened; late-open re-scan on failed waits.
  Exit 0=exists, 1=NotFound (successful answer), 2..9 unchanged.
  Requested-name/ProgId boundary PM-accepted and documented.
- **TVS #3 DONE (2026-09-03): `tsdevctl getsrc` — APPROVED after review +
  PM fixes** (commits b4da23f → cadbccd). Final shape:
  `getsrc --type <Strategy|Function|Indicator|ShowMe|TradingApp> --name <n>
  [--output <file>] [--json --timeout --nohide --ts-path]`. Reuses the same
  open-read-close mechanism as exists via a shared `open_read_and_close`
  helper (extracted from run_exists; run_exists refactored, behavior
  preserved). Emits the editor text (the item's source) as UTF-8 to stdout
  (raw), to `--output <file>` (UTF-8 no BOM), or as a JSON `source` field.
  `--output` is getsrc-only (usage error with other verbs). Exit 0=found,
  1=NotFound (successful answer), 10=Unexpected (file write failure),
  2..9 unchanged. Decrypt: inherited by construction (getsrc reads what
  TSDev renders; TSDev decrypts when rendering). Explicit verification with
  a known encrypted item pending a fixture.
  - Review: APPROVED by adversarial reviewer (2 LOW findings: dead
    ReadOutcome::failure member, ASCII "->" vs Unicode arrow — both fixed).
  - PM-found defect (reviewer missed): WM_GETTEXT on BCGPEditCtrl includes a
    trailing NUL in the copied count; invisible to exists (substring match)
    but corrupts getsrc verbatim output (JSON/raw/file all had \u0000).
    Fixed in doc_read.cpp (strip trailing NULs); KB updated.
  - Verified: unit green; E2E green (full suite: open + exists + getsrc,
    all 5 R13 types, --output, raw stdout, JSON, phantoms); zero residue;
    no dialogs; no crash reports; TSDev closed cleanly.
- Next TVS (recommended): **`insert`** — the first write operation. Pipe-open
  creates a phantom empty doc for a nonexistent name; set the editor text
  (WM_SETTEXT on BCGPEditCtrl, not clipboard); trigger the UI verify/compile
  + save code path (KB-documented at FUN_00451800). Proactively check exists
  first to avoid name collisions. Then `modify` (open existing, set text,
  re-verify) and `delete` (trigger the UI delete path). `list` has NO
  external trigger (KB negative evidence) — needs a scoped decision (defer
  or new research approach). `upsert` = insert-or-modify, built on both.
