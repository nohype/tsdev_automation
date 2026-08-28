# M.4b.1 Handoff — Warehouse-Level Reverse-Dependent Enumeration Primitive

> **Status:** RE-OPENED. cmd 0x0D → cmd 0x3B → SafeArray protocol implemented and confirmed dynamically, but COM server returns zero callers for ALL items (dependency DB empty). Root cause: TSDev.exe dispatches 13 warehouse commands our CLI never sends (0x32, 0x33, 0x34, 0x35, 0x38, 0x3A, 0x4D, 0x4E, 0x4F, 0x53, 0x57, 0x58, 0x65). Next: Ghidra-analyze these commands' vtable methods to identify which populate the dependency DB, then replicate in CLI.
> **Date:** 2026-08-28 (session 3)
> **Workstream:** M (TSDev parity) — slice M.4b.1
> **Plan ref:** `docs/workstream_tsdev_parity_plan.md` §M.4b.1
> **RE evidence:** `docs/RE/RE_M4_REVERSE_DEPENDENT_REVALIDATION.md` §9 (R-M.4a.1–R-M.4a.6, §9.7 SafeArray format, §9.8 gap #5)
> **Catalog impact:** None.

Durable handoff for M.4b.1. Authoritative reference for the cmd sequence, SafeArray parsing, current finding (13 missing commands), action plan, and UI automation tools.

---

## 1. Summary

M.4b.1 implements a read-only warehouse-level reverse-dependent (Callers) enumeration primitive driving the orlib20 warehouse worker-thread protocol (cmd `0x0D` → cmd `0x3B` → SafeArray parse) — the same mechanism TSDev.exe uses.

- **Internal API:** `TsDevLib::enumerateCallers(ComponentType type, const std::string& name)` → `std::vector<CallerInfo>`
- **CLI:** `tsdev_cli [--json] dependents <type> <name>`

Protocol confirmed dynamically: cmd 0x0D sets `warehouse+0x5E` to non-NULL IProgram; cmd 0x3B succeeds (`warehouse+0x4E == 0`) and populates `warehouse+0x26` with a SafeArray parsed per §3.

**Critical finding:** COM server returns zero callers for ALL items (VT_UI1, 4 bytes, all zeros). The dependency DB is empty. See §4.6/§4.7 for root cause.

---

## 2. Confirmed cmd sequence

### cmd 0x0D (ProgramById) — get root program record

1. Resolve root item's ProgId via `findExistPos(name, type)`.
2. Get warehouse singleton via `callGetWarehouse(m_pGetWarehouse, 0, &warehouse)`.
3. Call `refreshCache()` (cmd `0x0F` + cmd `0x4E`).
4. Set `warehouse+0x12 = category` (4-byte int).
5. Set `warehouse+0x16 = ProgId` (4-byte int).
6. Dispatch cmd `0x0D` via `callCmdDispatcher(m_pCmdDispatcher, warehouse, 0x0D)`.
7. Read IProgram COM object pointer from `warehouse+0x5E`.

**Confirmed:** cmd 0x0D via generic dispatcher (`FUN_101cf5b0`) sets `warehouse+0x5E` to non-NULL root record. (An earlier comment about "cmd 0x0D returns null" referred to the COM-interface-get path `FUN_101cfce0` with `type=0xC`, not the generic dispatcher with correct type+ProgId.)

### cmd 0x3B (IProgram::Callers) — get reverse-dependent SafeArray

1. Clear `warehouse+0x26` to NULL.
2. Dispatch cmd `0x3B` via `callCmdDispatcher(m_pCmdDispatcher, warehouse, 0x3B)`.
3. Check `warehouse+0x4E == 0` (status guard).
4. Read SafeArray pointer from `warehouse+0x26`.
5. Clear `warehouse+0x26` to NULL (ownership transfers to `callersCopySafeArray`).
6. Copy via `callersCopySafeArray` (SEH-protected, POD-only).
7. Parse entries per §3.
8. Destroy SafeArray (inside `callersCopySafeArray`).

**Confirmed:** cmd 0x3B succeeds with `warehouse+0x4E == 0`, populates `warehouse+0x26` with a new SafeArray. Descriptor: `cDims=1, fFeatures=0x0080, cbElements=1, cElements=4, lLbound=0`. VARTYPE=VT_UI1. Data: 4 bytes zeros → zero entries.

---

## 3. SafeArray parsing (§9.7)

1-dimensional, variable-length entries packed into one contiguous blob. Total bytes = `elemCount * cbElements`. Parsing starts at byte offset 4.

```
stride = 8 + nameLength + 0x16  (= 0x1e + nameLength)
```

| Offset | Size | Field |
|--------|------|-------|
| `+0x00` | 2 | type/category (unsigned short → int) |
| `+0x02` | 4 | ID (ProgId) |
| `+0x06` | 2 | string length (nameLength) |
| `+0x08` | nameLength | name string (not null-terminated) |
| `+0x08+nameLength` | 0x16 (22) | metadata block |

Defensive: `nameLength < 0` → break; `offset + entryStride > totalBytes` → break. SafeArray destroyed after parsing. Parsing runs on local malloc'd copy (no SEH risk).

---

## 4. Empty-DB finding — root cause investigation

**Finding:** COM server returns zero callers for ALL items — both newly-created AND existing (e.g., Function ProgId=517 "Returns default Down color" with 28 linker callers). SafeArray is VT_UI1, 4 bytes, all zeros.

### 4.1 WHServer.exe is OUT-OF-PROCESS (confirmed fact)

CLSID {7CEEA90C-D59E-11D3-8394-00C04F7BDF10} is registered as **LocalServer32** → `whserver.exe` (separate process). elbardb.dll is the in-process implementation hosted by WHServer.exe. If WHServer.exe is already running (started by TSDev.exe), our CLI connects to the same instance. If not running, a fresh instance with empty DB is started.

### 4.2 ProgramWriter hypothesis — DISPROVED

DISPROVED: DB empty for ALL items including existing ones with 28 linker callers.

### 4.3 Worker thread analysis

See `tmp/worker_thread_decompiled.c` for decompiled FUN_101d2150.

### 4.4 refreshCache never dispatches cmd 0x0F — DISPROVED

DISPROVED by session 3: cmd 0x0F IS dispatched during our verify (see §4.6).

### 4.5 TSDev startup populates DB — DISPROVED

DISPROVED: TSDev.exe fully initialized (25s wait, WHServer.exe spawned), DB still empty.

### 4.6 Session 3 — warehouse+0xAE non-zero, cmd 0x0F dispatched

1. `warehouse+0xAE` is consistently `0x009560F4` (non-NULL) — guard in FUN_101d0950 is satisfied.
2. cmd 0x0F IS dispatched during our verify — Frida trace of CLI's `insert function` confirmed FUN_101d1010 (SafeArray creation, cmd 0x0F proxy) fires multiple times. Full compile path executes: VerifyModule_New → CompileText → ProgramWriter → WriterOrchestration → WriterDispatch → SetNeedsWrite → WriteTokens.

**Conclusion:** Empty DB is NOT caused by warehouse+0xAE being NULL or cmd 0x0F not being dispatched.

### 4.7 Session 3 — CRITICAL: 13 missing commands identified

**Method:** pywinauto-based TSDev UI automation (`src/tsdev_automation/tsdev_ui.py`) drove TSDev.exe's insert+verify flow while Frida hooked FUN_101cf5b0 (cmd dispatcher) to log all cmd IDs.

**TSDev.exe verify dispatches 13 commands our CLI never sends:**

| cmd | vtable call |
|-----|-------------|
| 0x32 | vtable[+0x1c] on `param_1[7].Release` |
| 0x33 | vtable[+0x2c] on `param_1[7].Release` |
| 0x34 | vtable[+0x20] on `param_1[7].Release` |
| 0x35 | vtable[+0x24] on `param_1[7].Release` |
| 0x38 | vtable[+0x18] with 0x5dc, &DAT_101ff098, 0x400, 1, &local_a0, param |
| 0x3A | vtable[+0x40] on `param_1[7].Release` |
| 0x4D | FUN_101d1cb0(param) |
| 0x4E | FUN_101cf710() (our refreshCache dispatches this, but only in refresh path) |
| 0x4F | vtable[+0x8] (Release) on `param_1[7].Release` |
| 0x53 | vtable[+0x98] on `param_1[7].Release` |
| 0x57 | vtable[+0xa8] on `param_1[7].Release` |
| 0x58 | vtable[+0xac] on `param_1[7].Release` |
| 0x65 | vtable[+0x88] on `param_1[7].Release` interface |

**Key:** cmd 0x0D uses `param_1[0xe]` (our CLI dispatches it), but cmd 0x3B uses `param_1[7].Release` — the SAME interface as the 13 missing commands above. Our CLI dispatches cmds 0x0D and 0x3B but NOT the other 13 commands that also use `param_1[7].Release`. This interface is likely IWHouse or a related warehouse interface our CLI never obtains.

Full TSDev verify command sequence: `tmp/M4B1_FINDINGS.md` §F7.

---

## 5. CLI behavior

### `dependents <type> <name>` — human output

```
exists=1
stored_name=<verbatim stored name>
progId=<ProgId>
dependents_count=<N>
dependent name=<name> type=<category> progId=<ProgId>
...
```

### `dependents <type> <name>` — JSON output

```json
{
  "op": "dependents", "type": "<type>", "name": "<name>",
  "success": true, "exit_code": 0,
  "prog_id": <ProgId>, "stored_name": "<verbatim stored name>",
  "dependents": [{"name": "...", "type": <category>, "prog_id": <ProgId>}]
}
```

### Exit codes

| Condition | Exit code |
|-----------|-----------|
| Success (including zero callers) | 0 |
| Root item not found | 5 |
| Unknown type | 2 |
| Wrong argument count | 1 |
| Init failure | 4 |
| CreateThread failure | 10 |

Error paths verified: `dependents function NONEXISTENT` → exit 5; `dependents badtype somename` → exit 2; `--json dependents function NONEXISTENT` → exit 5 with error JSON.

---

## 6. Test commands and outcomes

```
cmake --build build --config Release --target tsdev_cli
cmake --build build --config Release --target test_callers_enumeration
.\build\Release\test_callers_enumeration.exe    # elevated required
```

**Result:** 4/6 pass, 2 fail (zero-callers — expected per empty-DB finding). All existing operations (insert/modify/delete/exists/dependents) work correctly with no regressions (post-TVS E2E gate passed for Strategy + Function).

---

## 7. Adversarial review

Iteration 2: APPROVE (scope-adjusted); RE-OPENED because acceptance criterion (returns correct reverse-dependent set) stands.

---

## 8. Files changed

| File | Change |
|------|--------|
| `src/tsdev_lib.h` | `CallerInfo` struct + `enumerateCallers` + `enumerateCallersByProgId` declarations |
| `src/tsdev_lib.cpp` | `enumerateCallers` + `enumerateCallersByProgId` + `callersCopySafeArray` + TSDEV_DBG logging + `oleauto.h` |
| `src/tsdev_cli.cpp` | `dependents` subcommand (additive) |
| `tests/test_callers_enumeration.cpp` | Manual admin diagnostic test |
| `CMakeLists.txt` | `test_callers_enumeration` target + `OleAut32` linking (additive) |
| `src/tsdev_automation/tsdev_ui.py` | **NEW:** TSDevUI class — pywinauto TSDev.exe GUI automation |
| `src/tsdev_automation/tsdev_ui_cli.py` | **NEW:** `tsdev-ui` CLI entry point |
| `pyproject.toml` | pywinauto + pyperclip deps, `tsdev-ui` script |
| `tmp/frida_trace_verify_v4.js` | Frida trace for our CLI's verify (polling-based, works) |
| `tmp/frida_trace_tsdev_verify.js` | Frida trace for TSDev.exe (hooks FUN_101cf5b0) |
| `tmp/M4B1_FINDINGS.md` | Persistent investigation notes F1-F10, F11-F16 (gitignored) |
| `tmp/worker_thread_decompiled.c` | Decompiled FUN_101d2150 |

No changes to: `traverseReverseDependents`, `callVerify`, `saveSource`, `verifyModule`, `enumerateReverseDependents`, `signatures/*.json`, `SIGNATURE_CATALOG.md`.

---

## 9. Residual gaps

1. **Empty-DB blocker (RE §9.8 gap #5):** COM server returns zero callers for ALL items. Root cause narrowed (session 3): warehouse+0xAE non-zero, cmd 0x0F dispatched, but TSDev dispatches 13 commands our CLI never sends. These commands (and cmd 0x3B) use `param_1[7].Release`; our CLI dispatches cmd 0x0D via `param_1[0xe]` and cmd 0x3B via `param_1[7].Release`, but not the other 13. **Next: Ghidra-analyze the 13 missing commands to identify which populate the dependency DB, then replicate in CLI.**
2. **findExistPos fails for existing items:** Returns -1 for "Returns default Down color" (Function, ProgId=517). Worked around with `enumerateCallersByProgId`; underlying BST search issue may affect other operations.
3. **SafeArray metadata block subfields:** The 0x16-byte metadata block has documented subfields not parsed by M.4b.1. M.4b.2 may need them for contract-change detection.

---

## 10. Next steps

1. **Ghidra-analyze the 13 missing commands (HIGHEST PRIORITY):** cmds 0x32, 0x33, 0x34, 0x35, 0x38, 0x3A, 0x4D, 0x4E, 0x4F, 0x53, 0x57, 0x58, 0x65 all call vtable methods on `param_1[7].Release` (same interface as cmd 0x3B, which our CLI does dispatch; cmd 0x0D uses `param_1[0xe]`). Identify what this interface is (likely IWHouse or related), decompile each vtable method, determine which command(s) populate the dependency DB and what arguments they need.
2. **Identify how TSDev obtains the `param_1[7].Release` interface:** Our CLI dispatches cmd 0x3B through this interface but not the other 13 commands. Determine how TSDev initializes the additional commands on this interface and replicate.
3. **Replicate the missing command sequence in our CLI:** Add DB-populating command(s) to verifyModule or init path via `callCmdDispatcher`.
4. **Re-run focused test** — all 6 assertions must pass.
5. **Re-run adversarial review + post-TVS E2E gate.**
6. **Update SSOT docs with truthful completion status.**

### TSDev UI automation tools

- `src/tsdev_automation/tsdev_ui.py` — `TSDevUI` class (pywinauto)
- `src/tsdev_automation/tsdev_ui_cli.py` — CLI entry point
- `tsdev-ui` console script (`uv run tsdev-ui`)
- Dependencies: pywinauto, pyperclip

```
uv run tsdev-ui is-running
uv run tsdev-ui start
uv run tsdev-ui list <type>
uv run tsdev-ui verify <type> <name>
uv run tsdev-ui open <type> <name>
uv run tsdev-ui close
uv run tsdev-ui source <type> <name>
uv run tsdev-ui insert <type> <name> <file>
uv run tsdev-ui delete <type> <name> [name2 ...]
```

### Frida trace scripts (`tmp/`)

- `frida_trace_verify_v4.js` — traces our CLI's verify (avoids signature-checked functions)
- `frida_trace_tsdev_verify.js` — traces TSDev.exe verify (hooks FUN_101cf5b0)

**Key Frida constraints:**
- FUN_101d0950 and FUN_101cf5b0 are signature-checked by OrlibResolver — do NOT hook when tracing our CLI (hook modifies bytes, fails signature check). Use FUN_101d1010 (SafeArray creation) as proxy.
- TSDev.exe does NOT signature-check — FUN_101cf5b0 can be safely hooked.
- Use `setInterval` polling for orlib20.dll load detection (not `Process.attachModuleObserver` or `Module.findExportByName` — unreliable in Frida 17).

See `tmp/M4B1_FINDINGS.md` for full persistent notes (F1-F10, F11-F16).
See `tmp/worker_thread_decompiled.c` for decompiled FUN_101d2150.
See `docs/workstream_tsdev_parity_plan.md` §M.4b.2 for broader M.4b.2 scope.
