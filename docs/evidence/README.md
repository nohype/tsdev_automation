# Evidence for KNOWLEDGE_BASE.md Sec 22-25 (list/enumeration RE + exists fix)

Archived 2026-11-02 (RC-21): these files were the load-bearing live-verification
proof cited by KB Sec 22-25 and WORKING_MEMORY.md, previously living in
gitignored tmp/. They are now tracked here; KB citations point at these paths.

- probe_walk_post_insert_0/1.txt (+ .err) / probe_walk_post_delete_0/1.txt (+ .err):
  COM-enumeration walk transcripts proving +1/-1 probe lifecycle (KB Sec 24).
  The walk transcripts are the proof; probe_lifecycle_results.txt has a known
  summary-line parsing bug (KB Sec 24) and is kept only for context.
- probe_lifecycle_results.txt: same probe cycle driver log (see bug note above).
- enum_run_base.txt (+ .err): full 5-type enumeration baseline
  (761 strategy / 84 function / 314 indicator / 61 showme / 9 tradingapp).
- enum_scan.txt / enum_walk_st4.txt / enum_walk_st18.txt (+ .err): subtype scan
  proving cat-2 surplus rows are PaintBar/ProbabilityMap coverage (KB Sec 22).
- verify_names.ps1 -> verify_names_results.txt: 1229/1229 verbatim name parity.
- tvs1_raw_rec_dump.txt / tvs1_bulk_dump.cpp / tvs1_bulk_dump_out.txt /
  tvs1_bulk_dump_out2.txt / diff_counts2.py: master-record byte dumps proving
  the 0x10A layout has no stored-name field + stale-row churn (KB Sec 22).
- tlbdump_full.txt: WHServer.exe typelib dump (IWHouse/IProgram memids).
- exp_orelui20.txt: ORELUI20 export table (CAnalysisTypeInfo ctor not exported).
- frida_enum_final.js / frida_enum_trace.js: ORELUI20 LoadList/CELTechInfo+0x10
  observation scripts, candidate NOT PURSUED (KB Sec 25).
- enum_mechanism_report.md: RE SSOT for the enumeration mechanism decision.
- exists_fix_corpus/: RED (pre-fix, exists=1 on description text) + GREEN
  (post-fix, exists=0, 1229/1229 parity) re-proof transcripts (KB Sec 23).
