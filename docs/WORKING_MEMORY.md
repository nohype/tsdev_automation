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
- **Quiet-UX + foundation fixes DONE (commit 27f6412).**
- **TVS #6 DONE (this commit): `tsdevctl delete`** — destructor-path library deletion.
  - **Implementation:** DLL injection into TSDev.exe. Inline hook on
    `FUN_004944b0` (document open). On target match, sets up the three
    conditions the CELDocument destructor requires to call `FUN_0044c0f0`
    (library deletion): (1) calls `vtable[0x1F4]` (loader) to set
    `doc+0x174=1`, (2) calls `vtable[0x1B4]` (clear-saved) to make `IsSaved`
    return false, (3) sets `doc+0x17C=1` (modified). Then calls
    `vtable[0x8c]` (OnCloseDocument) — the destructor handles library deletion
    during close, after editor views are destroyed. This satisfies the
    "documents must be closed before deletion" requirement.
  - **TradingApp fix:** `CELApplicationDoc`'s vtable[0x1B4] is an empty stub
    (`FUN_00413900`). For TradingApp (type `0x1402`), the hook calls
    `FUN_0044f3d0` (common clear-saved helper) directly. See
    `docs/RE_DELETE_TRADINGAPP.md`.
  - **Re-injection fix:** PE export RVA parsing for `ReArmHook` address
    resolution. The previous local-DLL-load approach was unreliable under ASLR
    (DLL rebases differently in CLI vs TSDev). The PE parser reads the export
    RVA from the DLL file on disk; `remote_base + RVA` is always correct.
  - **Restored session documents:** TSDev restores 30+ previous-session
    documents on launch. These cause the delete hook's `OnCloseDocument` to
    crash with `0xc000041d` (STATUS_FATAL_USER_CALLBACK_ERROR in mfc140u.dll),
    bypassing `__try/__except`. The E2E script closes all restored documents
    via WM_CLOSE before operations. The delete implementation itself does NOT
    manage restored documents — callers that launch TSDev should be aware.
  - **Research:** exhaustive RE investigation found no higher-level deletion
    API. The UI delete command (`0x80e8`) does NOT delete Function items. No
    pipe command, exported function, or name-string-accepting delete function
    exists. A "pipe-open + modify + activate + close-without-saving" approach
    is NOT viable (two of three destructor conditions have no high-level
    trigger). See `docs/RE_DELETE_UI_FLOW.md`, `docs/RE_DELETE_VIEW_ACTIVATION.md`.
  - **Adversarial reviews:** 2 review cycles. First review (implementation):
    DISAPPROVE → fixed dead code, re-entry guard, error-path document cleanup.
    Second review (view-activation research): APPROVED with vtable[0x6c]
    correction applied to `RE_DELETE_UI_FLOW.md`.
  - **E2E (live, elevated, real TSDev):** all 5 R13 types passed
    (Function, Strategy, Indicator, TradingApp, ShowMe). Each: insert →
    exists → delete → verify gone. Nonexistent delete → exit 1. No crash
    dialogs. TSDev restarted (graceful WM_CLOSE) between types to prevent
    pipe-wedge. ShowMe does NOT crash when restored documents are closed first.
  - **KB updated:** delete section in `docs/KNOWLEDGE_BASE.md` updated to
    reflect the destructor-path mechanism, TradingApp fix, and new
    version-specific addresses.
- Test items from earlier E2E runs may remain in the library (leftover from
  failed runs before the restored-docs fix). These are test artifacts with
  `E2eDel_*` / `E2eModify_*` prefixes. They do not interfere with new
  operations as long as restored documents are closed before delete.
- Ghidra: project `tsdev`, program `/TSDev.exe` open (18478 funcs), MCP bridge
  connected (restart via the headless analyzer if stale). Frida 17.17.0
  available for RE/research only — NOT for production (users don't have RE tools).

## Next TVS for the next team (PM-set, 2026-09-04)

**Recommended: `tsdevctl upsert`** (insert-or-modify, both proven). Then `list`
(requires the item-enumeration mechanism, which has no external trigger yet —
KB: the Open dialog's enumeration is inside ORELUI20.dll, out of scope; a new
enumeration mechanism is needed, possibly via injection).
