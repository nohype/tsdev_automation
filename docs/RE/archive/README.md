# Archived RE Process Artifacts (TVS-F.5)

These documents are intermediate process artifacts from the TVS-F.5 reverse-engineering investigation (case-insensitive `exists` with verbatim stored name retrieval). They document approaches that were evaluated, reviewed, and ultimately **not adopted** — the final implementation uses a two-phase `enumerateRecords` approach (Phase 1: `_LoadMasterRecsEX@16` cached `_stricmp` search; Phase 2: `FUN_101d05e0` BST search + `FUN_101c36d0` verbatim name getter from warehouse node).

These files are preserved for historical context only. The durable RE findings that inform the production implementation are in `docs/RE/` (the parent directory).

## Files
- **RE_F5_approach_evaluation.md** — Evaluated Approach F (cmd 0x4E cache refresh) vs Hybrid (`_GetMasterRecByType@16`). Neither adopted.
- **RE_F5_cache_fix_findings.md** — Cache-staleness fix investigation. Approach B rejected (incorrect subtype mapping).
- **RE_F5_cache_fix_review.md** — Adversarial review of cache_fix_findings (DISAPPROVE).
- **RE_F5_cache_refresh_feasibility.md** — Direct cmd 0x4E cache refresh feasibility. Disproven by later findings.
- **RE_F5_cache_refresh_feasibility_review.md** — Review of cache_refresh_feasibility (APPROVE with conditions).
- **RE_F5_cli_review.md** — Review of earlier F.5 CLI implementation (source-parsing approach, now superseded).
- **RE_F5_doc_review.md** — Documentation consistency check at a point in time. Superseded.
- **RE_F5_enumeration_review.md** — Original review of enumeration_findings (DISAPPROVE, errors fixed).
- **RE_F5_frida_trace_analysis.md** — Frida trace of cache-reset caller flow. Approach not adopted.
- **RE_F5_tsdev_cache_refresh_trace.md** — Live Frida trace of TSDev cache-refresh sequence. Approach not adopted.
- **RE_F5_tsdev_cache_refresh_trace_review.md** — Review of tsdev_cache_refresh_trace (APPROVE with findings).
