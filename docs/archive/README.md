# Archived Documentation

These documents are historical planning/research/design materials for completed workstreams. They are preserved for reference but are NOT active operational documents. Counts and implementation details within may be stale — the codebase and `signatures/*.json` are the source of truth.

## Completed workstream plans & handoffs
- **HANDOFF_A_COMPILATION_ERRORS.md** — Workstream A (compilation errors) planning document. COMPLETE.
- **HANDOFF_B_ROBUSTNESS.md** — Workstream B (robustness) planning document. COMPLETE.
- **HANDOFF_C_TRADINGAPP.md** — Workstream C (TradingApp) planning document. COMPLETE.
- **HANDOFF_D_PYTHON_INTEGRATION.md** — Workstream D (Python integration) handoff. COMPLETE. Superseded by `docs/INTEGRATION_APPROACH_EVALUATION.md` and `src/tsdev_automation/`.
- **WORKSTREAM_F_PLAN.md** — Workstream F (new operations & inline batch) plan. COMPLETE (all 6 TVSs F.1–F.6).
- **WORKSTREAM_F5_PLAN.md** — TVS-F.5 detailed subtask decomposition. COMPLETE.
- **WORKSTREAM_F5_PLAN_REVIEW.md** — Adversarial review of F5 plan (DISAPPROVED → fixed).
- **WORKSTREAM_F5_PLAN_REREVIEW.md** — Re-review of F5 plan (APPROVED).

## Completed design documents
- **ARCHITECTURE_REDESIGN.md** — Workstream E architecture design (signature JSON migration, build-time codegen). IMPLEMENTED. The code (`tools/sigcodegen.py`, `signatures/*.json`) and `docs/SIGNATURE_CATALOG.md` are now the source of truth.
- **ORLIB_RESOLVER_DESIGN.md** — OrlibResolver design rationale. IMPLEMENTED. The code (`src/orlib_resolver.h`/`.cpp`) is now the source of truth.
- **SIGNATURE_INFRASTRUCTURE_REPORT.md** — Research report surveying professional signature management patterns. Recommendations fully implemented in Workstream E.

For current project state, see `AGENTS.md`. For build instructions, see `docs/BUILD.md`.
