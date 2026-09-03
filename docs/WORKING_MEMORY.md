# Working Memory — TSDev Automation

Live, real-time state for the PM. Keep minimal; drop stale/superseded facts.
Do not restate what AGENTS.md or the code documents (machine conventions,
paths, and process rules live in AGENTS.md).

## Current state

- Branch `exp/remote-control`. Requirements SSOT: AGENTS.md (R1–R15).
  TSDev.exe facts SSOT: docs/KNOWLEDGE_BASE.md.
- **TVS #1 DONE (commit ad1f93a): `tsdevctl open`** — lifecycle + pipe channel.
- **TVS #2 DONE (commit b94e63f): `tsdevctl exists`** — phantom-discriminator check.
- **TVS #3 DONE (commit cadbccd): `tsdevctl getsrc`** — open-read-close, source emission.
- **TVS #4 DONE (commit 35976b4): `tsdevctl insert`** — the first write operation.
- **TVS #5 DONE (this commit): `tsdevctl modify`** — replace source + reverify.
  - Implementation: proactive exists check (exit 1 NotFound if absent) →
    pipe-open the REAL item → WM_SETTEXT new source → show frame if hidden
    (REQUIRED for verify) → WM_COMMAND 0x296a (verify, frame stays VISIBLE) →
    two-phase "*" poll → failure classification (dialog safety net + Output
    error-list summary) → on compile failure: close_doc_discard + exit 12
    with "[item 'X' was saved with uncompiled new source]" (R10) → on success:
    close_doc_window → post-verify SOURCE CONFIRMATION (open_read_and_close +
    rtrim_ws compare; exit 10 if source not updated) → hide frame.
  - Exit codes: 0 ok, 1 not-found, 12 compile-failed, 10 source-not-updated.
  - Adversarially reviewed + APPROVED (1 cycle; one LOW stale-comment fix
    applied: args.cpp --output comment updated to "non-getsrc verbs").
  - E2E (live, elevated, real TSDev, targeted run): insert Strategy exit 0
    (13.2s) → modify with valid source exit 0 (8.1s) → getsrc confirms
    "MODIFIED_MARKER_XYZ" present → modify nonexistent exit 1 not_found
    (0.2s) → modify garbage exit 12 compile_error (6.7s). No residual
    dialogs / crash reports / stray processes.
  - Known E2E issue (pre-existing, not modify-specific): the full
    run_e2e.ps1 wedges TSDev after ~20 rapid pipe operations (the
    exists/getsrc sections), causing the modify section to time out. This is
    the KB-documented pipe-wedge condition (rapid back-to-back probing as a
    possible trigger). The targeted modify-only run (with 2s pauses between
    commands) passes cleanly. Fix: add inter-command pauses to run_e2e.ps1
    or split the E2E into separate script sections.
- Test items created in the TSDev library during E2E (no delete verb yet —
  manual cleanup or the Delete TVS): Tvs4* variants (from TVS #4),
  E2eMod5_20260903T175216, E2eMod5b_20260903T175342 (from TVS #5). The
  library items remain until Delete exists.
- Ghidra: project `tsdev`, program `/TSDev.exe` open (18478 funcs), MCP bridge
  connected (restart via the headless analyzer if stale). Frida 17.17.0
  available for live tracing.
- **Open UX consideration (user, 2026-09-03, not yet a numbered requirement):**
  "optimally, users can keep working while the tool runs — no popups; a single
  startup flicker is acceptable, then quiet." Assessment: achievable with
  ~100–150 lines. Design: replace SW_SHOW/SW_HIDE with off-screen positioning
  (SetWindowPos −32000,−32000 + restore; keeps WS_VISIBLE so IsWindowVisible
  stays true) + SW_SHOWNA to avoid focus stealing; optionally a WinEvent hook
  (EVENT_OBJECT_SHOW, out-of-context — no injection needed) to move the create
  dialog off-screen the instant it appears (flicker-free). TWO LIVE TESTS GATE
  the design: (1) does the create-dialog name propagation survive an off-screen
  frame (WS_VISIBLE set)? (2) does the dialog's OK still propagate if the
  dialog itself is moved off-screen/hidden before IDOK? If either fails,
  injection (now sanctioned by the AGENTS.md rule) can suppress the dialog from
  inside TSDev. Residual artifact even then: a sub-second focus steal while
  TSDev's modal dialog exists. The verify phase is already quiet (errors go to
  the error list, no MessageBox, when the frame is visible).
- **Recommended next TVS: `tsdevctl delete`** — it enables the insert-failure
  cleanup + the E2E gate's item cleanup (the test-item list above grows every
  TVS). Upsert after that (it is insert-or-modify, both proven). List last
  (it requires the item-enumeration mechanism, which has no external trigger
  yet — KB line 45: the Open dialog's enumeration is inside ORELUI20.dll, out
  of scope; a new enumeration mechanism is needed, possibly via injection).
