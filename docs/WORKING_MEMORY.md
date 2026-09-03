# Working Memory — TSDev Automation

Live, real-time state for the PM. Keep minimal; drop stale/superseded facts.
Do not restate what AGENTS.md or the code documents (machine conventions,
paths, and process rules live in AGENTS.md).

## Current state

- Branch `exp/remote-control`. Branch direction is fixed: drive TSDev.exe
  itself (see AGENTS.md). The old approach (direct orlib/library interfacing
  without TSDev.exe) is discarded; most of the existing codebase, tests, and
  CI will be discarded/replaced by implementing teams — do not trust or build
  on existing code.
- Requirements interview complete (2026-09-03). AGENTS.md is the
  requirements SSOT (R1–R15); do not duplicate it here.
- Docs cleanup (2026-09-03): repo docs purged of old-approach and
  other-binary content (three docs deleted; see git history). Surviving doc
  set: AGENTS.md, README.md (minimal stub), docs/KNOWLEDGE_BASE.md
  (rewritten: TSDev.exe-only facts), docs/WORKING_MEMORY.md. Uncommitted at
  the time of writing.
- Next: implementing teams shape and execute TVSs against the AGENTS.md
  requirements.

## Still-valid machine facts

- UAC ConsentPromptBehaviorAdmin=5 (registry) → an elevated launch from an
  unelevated context costs ONE interactive UAC prompt. Elevation state must
  be checked per session (`whoami /groups` → High Mandatory Level =
  elevated); TS 9.5 processes require elevation (R8).
- `tmp/`, `dist/`, `build/` are gitignored scratch. `tmp/` still holds
  old-approach experiment artifacts on disk — not part of the repo, not
  current truth.
