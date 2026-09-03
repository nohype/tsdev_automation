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
- **TVS #5 DONE (commit 4d96592): `tsdevctl modify`** — replace source + reverify.
- **Quiet-UX + foundation fixes DONE (this commit):**
  - **Quiet-UX:** eliminated visual flicker and focus stealing for insert and
    modify when the user is working in another application. Design:
    - `SW_SHOWNA` (show without activating) instead of `SW_SHOW` — no focus steal.
    - Frame moved off-screen (`SetWindowPos(-32000, -32000)`) while keeping
      `WS_VISIBLE` (required for the New dialog's name propagation and verify
      "*" removal — KB).
    - `FrameGuard`: a background thread that polls the frame position every 10ms
      and immediately moves it back off-screen when TSDev's New dialog creation
      repositions it on-screen. Eliminates the brief on-screen flicker.
    - `save_foreground` / `restore_foreground`: saves the user's foreground
      window before any TSDev interaction and restores it at exit via
      `AttachThreadInput` + `SetForegroundWindow`. Safety net for any focus steal.
    - `restore_frame_quiet`: hides the frame BEFORE restoring its position
      (eliminates a brief on-screen flash and a race with FrameGuard).
    - `--nohide` opt-in: disables all quiet-UX (SW_SHOW, on-screen, no focus
      save/restore) for users who want to watch.
  - **CRLF normalization:** `normalize_eols` strips all `\r` characters before
    source comparison, so `\r\n` (Windows) and `\n` (TSDev editor) compare equal.
    Without this, modify's post-verify source confirmation falsely reported
    "source_not_updated" for Windows-line-ending source files.
  - **E2E pipe-wedge fix:** `run_e2e.ps1` now restarts TSDev before the
    insert/modify and modify-garbage sections (the KB-documented pipe-wedge
    condition occurs after ~6–16 pipe writes). Inter-command pauses (2s) added.
    Doc-title normalization fixed to strip `*` anywhere in the title (not just
    trailing), since the `*` marker can appear before the `:N` MDI child suffix.
  - **Unit test build fix:** `run_unit_tests.ps1` now builds the `tsdevctl`
    target alongside the unit test target, so `main.cpp` is compiled under
    `/W4 /WX`.
  - **Usage text:** `--name` option now documents the charset restriction
    (no commas or double quotes; TSDev's pipe encoding does not support them).
  - **KB updated:** added quiet-UX facts (WS_VISIBLE off-screen is sufficient,
    SW_SHOWNA doesn't steal focus, New dialog repositions the frame, FrameGuard
    + focus restore as the solution).
  - Adversarially reviewed: 1 DISAPPROVE cycle (FrameGuard race with
    `restore_frame_quiet` on failure paths + KB not updated). Both required
    fixes applied. Review findings 4–6 (IsWindow checks, FrameGuard hang
    hardening, happens-before comment) also applied. Re-tested: E2E passed.
  - E2E (live, elevated, real TSDev, notepad foreground):
    - Insert: exit 0, notepad stays foreground, frame hidden after.
    - Modify: exit 0, notepad stays foreground.
    - getsrc: confirms source changed (contains marker).
    - Modify nonexistent: exit 1.
    - Modify garbage: exit 12.
    - No crash reports, no residual dialogs, no stray processes.
  - Full `run_e2e.ps1` passed (0 failures) with the pipe-wedge restart fix.
- Test items created in the TSDev library during E2E (no delete verb yet —
  manual cleanup or the Delete TVS): Tvs4* variants, E2eMod5_*,
  E2eModify_* (from TVS #4/#5 and quiet-UX E2E). The library items remain
  until Delete exists.
- Ghidra: project `tsdev`, program `/TSDev.exe` open (18478 funcs), MCP bridge
  connected (restart via the headless analyzer if stale). Frida 17.17.0
  available for live tracing.
- **Recommended next TVS: `tsdevctl delete`** — it enables the insert-failure
  cleanup + the E2E gate's item cleanup (the test-item list above grows every
  TVS). Upsert after that (it is insert-or-modify, both proven). List last
  (it requires the item-enumeration mechanism, which has no external trigger
  yet — KB line 45: the Open dialog's enumeration is inside ORELUI20.dll, out
  of scope; a new enumeration mechanism is needed, possibly via injection).
