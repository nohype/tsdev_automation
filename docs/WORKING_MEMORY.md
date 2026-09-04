# Working Memory — TSDev Automation

Live, real-time state for the PM. Keep minimal; drop stale/superseded facts.
Do not restate what AGENTS.md or the code documents (machine conventions,
paths, and process rules live in AGENTS.md).

## Current state

- Branch `exp/remote-control`. Requirements SSOT: AGENTS.md (R1–R15).
  TSDev.exe + loaded-module facts SSOT: docs/KNOWLEDGE_BASE.md.
- **TVSs #1–#6 DONE** (commits ad1f93a, b94e63f, cadbccd, 35976b4, 4d96592,
  27f6412, 67ac757): `open`, `exists`, `getsrc`, `insert`, `modify`,
  `delete`. TVS #6 delete is TEMPORARY (7 binary-internal coupling points:
  3 vtable offsets, 2 struct offsets, 2 function addresses) — replacement
  via ORELUI20.dll `DeleteItem` is the recorded next candidate (unblocked
  by TVS #7; see docs/RE_DELETE_OPEN_DIALOG.md for prior analysis).
- **TVS #7 (research) DONE (2026-09-04).** ORELUI20.dll enumeration +
  delete characterized and Frida-verified; deliverable docs/RE_ORELUI20.md;
  facts in the KB ("ORELUI20.dll + ORLIB20.dll ordinals"). Per-operation
  assessment: delete YES, list YES, exists YES; getsrc/insert/modify/upsert
  NO. Do NOT re-run its A–E subtasks.
- **TVS #8 DONE (2026-09-04): `tsdevctl list` via ORELUI20.dll in-process
  enumeration.** Commits 410cdd1 (implementation) + 321ff8d (approved fix
  round). New `src/enum_hook/` injected DLL (Ordinal_17 ctor / Ordinal_1291
  LoadList / Ordinal_166 dtor + version-brittle base+0x44820 and base+0x8fd04,
  SHA-256-guarded ORELUI20.dll), CLI `run_list` (all-items / `--type` filter,
  human + JSON per the contract in the implemented code), per-verb arg
  validation in args.cpp (only shared-code change), 49 new unit checks.
  Review: round 1 APPROVE (0 blocking / 5 non-blocking) → fix round
  (layout_error_flag; run_list wait-loop wraparound clamp; E2E check
  tightened) → round 2 APPROVE (0 blocking / 4 non-blocking). Acceptance
  gate PASS: docs/TVS8_E2E_GATE.md — 143/143 unit checks; run_e2e 33 ok /
  0 FAIL; 5-type insert/modify/delete regression 35/35 (all R13 types,
  zero dialogs, cleanup verified, count back to 2507 baseline).
- Ghidra: project `tsdev`, programs `/TSDev.exe` and `/ORELUI20.dll` open,
  MCP bridge connected (restart via the headless analyzer if stale). Frida
  17.17.0 available for RE/research only — NOT for production (users don't
  have RE tools).

## Known issues / deferred candidates (recorded facts; the next PM session scopes the next minimal TVS)

Per the TVS #8 scope's deferred order: delete-reimpl via `DeleteItem` →
exists-robustification (reuses enumeration) → upsert (pure composition).

Production bugs (KB "Known issues" section is the SSOT for details):
- `insert --type Strategy` timeout — state-dependent; NOT reproduced in the
  TVS #8 gate (3/3 clean inserts, 7.3–9.2 s).
- `exists` timeout — Function/Indicator (TVS #7) + ShowMe observed in the
  TVS #8 gate (exit 6 at 60.5 s, advisory probe; R10 contract honored).
- `delete --type Strategy` crash — state-dependent (Strategy doc open in QE);
  did not reproduce in TVS #7/#8 gates.

TVS #8 review carryovers (non-blocking, recorded for future TVSs):
- run_delete's wait loop still has the timeout wraparound run_list was fixed
  for (plus a residual theoretical race in run_list's two-now() form);
  clean both in one future cross-cutting TVS.
- inject.cpp's delete-specific error string ("ReArmHook failed") surfaces on
  list setup failures — cosmetic; fix when inject.cpp is next touched.
- Rebuild/upgrade hazard: replacing enum_hook.dll/delete_hook.dll on disk
  while a tool-launched TSDev still holds the old module loaded →
  get_export_rva (on-disk file) + LoadLibraryW (stale module) → wrong-address
  remote call → crash (observed 2026-09-04, correctly handled per the dialog
  rule). Not covered by the TSDev.exe/ORELUI20.dll SHA-256 guards. Hardening
  candidate: resolve the export from the remote loaded module instead of the
  on-disk file. Same hazard has existed for delete_hook.dll since TVS #6.

## Handoff

This session (PM, 2026-09-04): implemented TVS #8 per the approved scope
(delegate → adversarial review → fix round → re-review APPROVE → acceptance
gate PASS → docs). Stopped after TVS #8 per the iteration rule. The next
team: determine and scope the next minimal TVS (candidates above; do not
re-plan TVS #7; do not expand TVS #8). If the Strategy insert timeout
reproduces in a future gate: STOP and escalate to the PM — do not expand
scope to fix it (recorded SSOT bug).
