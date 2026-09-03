# Working Memory — TSDev Automation

Live, real-time state for the PM. Keep minimal; drop stale/superseded facts.
Do not restate what AGENTS.md or the code documents (machine conventions,
paths, and process rules live in AGENTS.md).

## Current state

- Branch `exp/remote-control`. Branch direction is fixed: drive TSDev.exe
  itself (see AGENTS.md). The old approach (direct orlib/library interfacing
  without TSDev.exe) is discarded.
- Requirements interview complete (2026-09-03). AGENTS.md is the
  requirements SSOT (R1–R15); do not duplicate it here.
- Cleanup complete (2026-09-03): the old codebase is archived under
  `archive/` (reference-only; see `archive/README.md`). orlib20-specific
  content (signatures, sigcodegen, diag probes, DLL fixture) was dropped —
  recoverable from git history (commits before the archive restructure).
  Top level now holds only: AGENTS.md, README.md, .gitignore,
  .python-version, docs/, archive/.
- Next: implementing teams shape and execute TVSs against the AGENTS.md
  requirements.

## Still-valid machine facts

- UAC ConsentPromptBehaviorAdmin=5 (registry) → an elevated launch from an
  unelevated context costs ONE interactive UAC prompt. Elevation state must
  be checked per session (`whoami /groups` → High Mandatory Level =
  elevated); TS 9.5 processes require elevation (R8).
- Gitignored scratch on disk is old-approach residue, not repo content:
  `build/`, `build_probe/`, `dist/`, `dist_diag/`, `artifacts/`, `tmp/`,
  `.venv/`, `.pytest_cache/`. `ghidra_project/` (TSDev.exe RE workspace)
  is current and in scope for this branch.
