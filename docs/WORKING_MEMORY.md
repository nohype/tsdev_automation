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
- **TVS #6 DONE (commit 67ac757): `tsdevctl delete`** — destructor-path library
  deletion. **TEMPORARY — to be replaced.**
  - **Current implementation:** DLL injection into TSDev.exe. Inline hook on
    `FUN_004944b0` (document open). On target match, artificially sets up the
    three conditions the CELDocument destructor requires to call `FUN_0044c0f0`
    (library deletion), then calls `OnCloseDocument`. This satisfies the
    "documents must be closed before deletion" requirement but operates at a
    low abstraction level (7 binary-internal coupling points: 3 vtable offsets,
    2 struct offsets, 2 function addresses).
  - **Why it's temporary:** the real UI delete path is in `ORELUI20.dll` (the
    Open dialog's Delete button — works for all types including Functions,
    deletes at the library-record level, no document-state manipulation needed).
    This path was unreachable under the old constraint (d) which prohibited
    touching orlib DLLs. Constraint (d) has now been relaxed to allow
    calling/hooking functions in any module loaded within the TSDev.exe
    process. The delete should be reimplemented to use the `ORELUI20.dll` path.
    See `docs/RE_DELETE_OPEN_DIALOG.md`.
  - **E2E passed** for all 5 R13 types with the current destructor-path
    approach. The replacement must pass the same E2E.
- **Constraint (d) relaxed (this commit):** the process boundary is now the
  safety boundary. Code injected into the TSDev.exe process may call and hook
  functions in any module loaded into that process (`ORELUI20.dll`,
  `ORLIB20.dll`, `mfc140u.dll`, etc.). Separate processes remain off-limits.
  This unblocks higher-level paths for delete and list.
- Test items from earlier E2E runs may remain in the library (leftover from
  failed runs before the restored-docs fix). These are test artifacts with
  `E2eDel_*` / `E2eModify_*` prefixes. They do not interfere with new
  operations as long as restored documents are closed before delete.
- Ghidra: project `tsdev`, program `/TSDev.exe` open (18478 funcs), MCP bridge
  connected (restart via the headless analyzer if stale). Frida 17.17.0
  available for RE/research only — NOT for production (users don't have RE tools).

## Next TVS for the next team (PM-set, 2026-09-04)

**Priority 1: Reimplement `tsdevctl delete` via the `ORELUI20.dll` path.**
The current destructor-path approach is temporary. The Open dialog's Delete
button (`CELFileOpenDialog` in `ORELUI20.dll`) deletes at the library-record
level and works for all types including Functions — no document-state
manipulation needed. Research needed: find the `ORELUI20.dll` function that
performs the delete (likely calls `ORLIB20.dll`'s `_SetSigDeleted` directly),
determine its calling convention and parameters, and call it from injected
code. This replaces 7 binary-internal coupling points with what should be a
single function call. See `docs/RE_DELETE_OPEN_DIALOG.md` for the TSDev.exe
side of the analysis (the `ORELUI20.dll` side is new RE work).

**Priority 2: `tsdevctl list` via `ORELUI20.dll` enumeration.**
The Open dialog enumerates all items internally in `ORELUI20.dll`. Now that
we can call into loaded modules, the enumeration mechanism is reachable.
Research needed: find the enumeration function in `ORELUI20.dll`, determine
how to iterate all items (or items per type), and extract verbatim stored
names and ProgIds. This also enables verbatim-name and ProgId output in
`exists`. See `docs/RE_DELETE_OPEN_DIALOG.md` §6 for the TSDev.exe-side
analysis of what `CELFileOpenDialog` exposes.

**Priority 3: `tsdevctl upsert`** (insert-or-modify, both proven — no new
mechanism needed).
