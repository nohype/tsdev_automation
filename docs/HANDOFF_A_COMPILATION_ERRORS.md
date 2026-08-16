# Handoff A — Surface Real Compilation Error Text (Priority A)

**Workstream:** Getting the actual TradeStation EasyLanguage compiler error messages (line numbers, error text) when INSERT or MODIFY compilation fails.
**Priority:** A (highest-ROI follow-up). The acceptance criteria require "fail loudly on compilation errors." Today the CLI *detects* failure (via `VerifyModule_New`'s return value) but does *not* surface the real compiler error text — it synthesizes a generic message. This workstream closes that fidelity gap.
**Date:** 2026-08-15
**Status:** PLANNED — not started. This document is the executable plan for a fresh team.

---

## 1. Current State

### What works today
- **Failure detection works.** `verifyModule()` (`src/tsdev_lib.cpp` ~L404-470) calls `VerifyModule_New(hwnd, whType, progId, 1, compilerObj)` and checks its return value. When it returns `!= 1`, compilation is treated as failed.
- **INSERT, MODIFY, DELETE are all implemented and adversarially APPROVED** at the library level (see AGENTS.md TODO / FEASIBILITY.md §5.3, §5.7, §5.8). Persistence of `.ell` files works standalone via the `FUN_10011230` in-process patch.
- **`GetCompilingErrors` is called** immediately after `VerifyModule_New` inside `callVerifyAndErrors` (`tsdev_lib.cpp` L140-158), and its output is checked.

### What does NOT work
- **`GetCompilingErrors` returns an empty buffer in headless mode** even when compilation genuinely fails. The real compiler error text (line numbers, messages) is NOT surfaced to the CLI user.
- **A synthesized fallback message** is emitted instead. In `verifyModule()` (L452-459):
  ```cpp
  if (seh.errorBuf[0] != '\0') {
      errors = seh.errorBuf;
  } else if (seh.verifyResult != 1) {
      // No explicit error text but VerifyModule_New did not report success.
      errors = "tsdev: VerifyModule_New returned "
               + std::to_string(seh.verifyResult)
               + " with no error text";
  }
  ```
  So on a real compile failure the user sees `"tsdev: VerifyModule_New returned 0 with no error text"` — not the actual EasyLanguage compiler diagnostics.

### Known ABI discrepancy (STRONG RESEARCH LEAD — do NOT assume, verify)
There is a mismatch between the documented signature and the implemented call:
- **FEASIBILITY.md §5.1** documents: `std::string _GetCompilingErrors@4(std::string* hiddenRetPtr)` — i.e. the function **returns `std::string` by value**, and the single 4-byte parameter (`@4`) is the **MSVC hidden return pointer** to a `std::string` object, NOT a `char*` output buffer.
- **The implementation** (`tsdev_lib.cpp` L111, L140-158; `tsdev_lib.h` L148) declares it as `void (__stdcall *GetCompilingErrors_t)(char* buffer)` and calls `errorsFn(out->errorBuf)` where `errorBuf` is a `char[4096]`.

If the documented signature is correct, the current call passes a `char[4096]` where a `std::string*` is expected. The function would construct a `std::string` *object* (internal pointer/size/capacity, ~24-32 bytes, or SSO buffer) into the first bytes of the 4096 buffer, with the actual character data either on the heap or in the SSO small buffer. Reading `errorBuf` as a C-string would then yield garbage/empty — **exactly the observed symptom**. This is the leading hypothesis but MUST be confirmed by research (TVS-A.2), not assumed.

### Other documented facts (from FEASIBILITY.md §5.1 / orlib_param_orders.md §10)
- `GetCompilingErrors` builds a concatenated `"name: msg\n..."` string across all error records in a global error array; if the array is empty, it reads a `VERIFY.ERR` file. An empty buffer = success.
- `StartCompilingProcess` (called before `VerifyModule_New`) resets the error array at globals `[0x1023cea0]` (array) and `[0x1023cea4]` (count); record stride is `0x129` (297 bytes).
- `_GetErrorLevel@4(int idx)` returns 1 if a record's first dword == `0x7775` sentinel (error), 0 = warning/none. Error count = global `[0x1023cea4]`.
- The error-record field layout (stride 297) beyond the `0x7775` sentinel is **unmapped** (FEASIBILITY.md §5.5 item 4).

---

## 2. Goal

Surface the **actual TradeStation EasyLanguage compiler error text** (line numbers + error messages, as the TSDev.exe GUI would show) to the CLI user on INSERT and MODIFY compilation failures. The synthesized `"tsdev: VerifyModule_New returned 0 with no error text"` fallback must be replaced with the real compiler diagnostics (or, only if the real text is genuinely unreachable, a clearly-labeled best-effort message — but the default target is the real text).

**Success looks like:** Inserting/modifying a strategy with a deliberate syntax error (e.g. `Inputs: nothing;` with an undefined token) causes the CLI to print something like `MyStrategy: ')' expected at line 3` rather than the generic synthesized message.

---

## 3. TVS Decomposition

Research TVSs come first and produce **documented, adversarially-reviewed findings**. Implementation TVSs consume those findings. Each TVS is minimal and independently verifiable. Effort estimates are relative (S/M/L).

### TVS-A.1 — Frida trace: how TSDev.exe calls GetCompilingErrors on a real compile failure
- **Goal:** Establish ground truth of the *working* path — exactly how TSDev.exe invokes `GetCompilingErrors` and where the error text materializes, when compiling a known-bad source.
- **Type:** RESEARCH
- **Scope:**
  - DONE: A Frida script (use Frida CLI directly, per AGENTS.md standing notes) that spawns/attaches TSDev.exe, hooks `_GetCompilingErrors@4` (and optionally `VerifyModule_New`, `StartCompilingProcess`, the global error array at `orlib_base+0x23cea0`), and triggers compilation of a deliberately-broken EasyLanguage source via the TSDev GUI.
  - Capture: the actual argument(s) passed to `GetCompilingErrors` (is arg1 a `std::string*` hidden-return ptr or a `char*`?), the return value handling, and the resulting error string content. Also dump the global error array `[0x1023cea0]`/`[0x1023cea4]` before/after to see the records.
  - NOT DONE: No code changes. No standalone-CLI experiments. No conclusions about headless mode yet (that's TVS-A.3).
- **Acceptance criteria:**
  - A documented finding (`tmp/re_artifacts/A1_geterrors_gui_trace.md`) stating the confirmed ABI of `GetCompilingErrors` (hidden `std::string*` return ptr vs `char*` buffer), with Frida log evidence (register/stack dumps at the hook, and the populated error string).
  - The error array globals are confirmed populated in the GUI path, with at least one error record dumped (showing the `0x7775` sentinel and any text).
- **Dependencies:** None.
- **Adversarial review:** YES (authoritative finding — the ABI conclusion gates all downstream TVSs).
- **Effort:** M

### TVS-A.2 — Static analysis: confirm GetCompilingErrors ABI + error array population path
- **Goal:** Independently confirm, via static analysis (Ghidra MCP, per AGENTS.md), the true signature/ABI of `GetCompilingErrors` and the code path that populates the global error array during compilation.
- **Type:** RESEARCH
- **Scope:**
  - DONE: Decompile `_GetCompilingErrors@4` and confirm whether it returns `std::string` by value (hidden return ptr as arg1) or writes to a `char*` buffer. Reconcile with the FEASIBILITY.md §5.1 claim. Document the exact prologue/epilogue and parameter usage.
  - Decompile the compile path (`CompileText` / `tselc.dll` entry) to find where error records are written to the global array `[0x1023cea0]` (stride 297, count `[0x1023cea4]`). Identify the function(s) that append error records.
  - Map the error-record layout (stride 297) at least far enough to locate the error-text field and the line-number field (for later extraction if needed).
  - Confirm the `VERIFY.ERR` file fallback path: when is it read, what format, where on disk.
  - NOT DONE: No code changes. No headless runtime experiments (TVS-A.3).
- **Acceptance criteria:**
  - Documented finding (`tmp/re_artifacts/A2_geterrors_static_abi.md`) with the confirmed ABI (must agree with or correct FEASIBILITY.md §5.1), the decompiled parameter usage, the error-array population function(s) and their call sites, the error-record text/line field offsets, and the `VERIFY.ERR` semantics. Cite Ghidra decompilation addresses.
  - Explicit verdict on whether the current `tsdev_lib.cpp` call (`void(char*)` with a 4096 buffer) is ABI-correct or broken.
- **Dependencies:** None (can run in parallel with TVS-A.1; cross-validate each other).
- **Adversarial review:** YES (authoritative ABI finding).
- **Effort:** M

### TVS-A.3 — Research: is the error array populated in headless mode?
- **Goal:** Determine whether the global error array is actually populated when compiling headlessly (our standalone CLI path), or whether something in the headless environment / our patch sequence prevents it.
- **Type:** RESEARCH
- **Scope:**
  - DONE: Using the standalone CLI/probe (or a Frida-attached run of the standalone probe), compile a known-bad source headlessly and dump the global error array `[0x1023cea0]`/`[0x1023cea4]` immediately after `VerifyModule_New` returns non-1. Determine: is the count >0? Are there records with the `0x7775` sentinel? Is there text?
  - If the array IS populated but `GetCompilingErrors` still returns empty, the bug is the ABI/call (confirms TVS-A.2's likely finding). If the array is NOT populated, investigate whether the `FUN_10011230` persistence patch, the missing message pump, the CLR/SymWriter bypass, or a missing call in our sequence is responsible.
  - Check whether `VERIFY.ERR` is created on disk during a headless failed compile and whether it contains the error text.
  - NOT DONE: No production code changes. (A throwaway probe/Frida script is fine.)
- **Acceptance criteria:**
  - Documented finding (`tmp/re_artifacts/A3_headless_error_array.md`) with a clear root-cause classification: (a) ABI/call bug only (array populated, retrieval wrong), (b) array not populated (identify why), or (c) text lives elsewhere (e.g. `VERIFY.ERR` / a different object). Include memory dumps as evidence.
  - A recommended fix approach for the implementation TVSs to follow.
- **Dependencies:** TVS-A.1, TVS-A.2 (uses their confirmed ABI + array addresses).
- **Adversarial review:** YES (authoritative root-cause finding — determines which implementation TVSs are needed).
- **Effort:** M

### TVS-A.4 — Implement: correct the GetCompilingErrors call ABI (TDD red-green)
- **Goal:** If TVS-A.2/A.3 confirm the ABI is wrong, fix the call so the real error string is retrieved.
- **Type:** IMPLEMENTATION
- **Scope:**
  - DONE: Update the `GetCompilingErrors` typedef and `callVerifyAndErrors` to match the confirmed ABI (likely: pass a `std::string*` hidden return ptr, then read `.c_str()` into the result). Update `tsdev_lib.h` typedef accordingly. Keep SEH protection.
  - TDD: Write a red test first — insert/modify a strategy with a deliberate syntax error and assert the returned error string is non-empty AND contains real compiler text (not the synthesized fallback). Run it red (current behavior). Then implement the fix and run it green.
  - NOT DONE: Do NOT change the synthesized-fallback logic yet (TVS-A.6). Do NOT touch INSERT/MODIFY orchestration. Scope is strictly the error-retrieval call.
- **Acceptance criteria:**
  - Red test committed and failing before the fix; green after.
  - The error string returned by `verifyModule()` on a known-bad source contains real compiler diagnostics (line/message), verified by at least 2 independent test runs.
  - No regression: a known-good source still returns an empty error string and `VerifyModule_New == 1`.
  - Build is clean (32-bit Win32 Release).
- **Dependencies:** TVS-A.2 (confirmed ABI), TVS-A.3 (confirms ABI is the bug, not array population).
- **Adversarial review:** YES.
- **Effort:** S (if ABI-only fix) / M (if std::string return requires careful SEH handling)

### TVS-A.5 — Implement: alternative error extraction (ONLY if TVS-A.3 finds the array is NOT populated headlessly)
- **Goal:** If the error array is genuinely not populated in headless mode, implement the confirmed alternative extraction (e.g. read `VERIFY.ERR`, or read the global array directly at the confirmed offsets, or add the missing call/sequence that TVS-A.3 identifies).
- **Type:** IMPLEMENTATION (CONDITIONAL — execute only if TVS-A.3 classifies the root cause as (b) or (c))
- **Scope:**
  - DONE: Implement whatever extraction TVS-A.3 recommended, behind the existing `verifyModule()` error-retrieval point. TDD red-green with a known-bad source.
  - NOT DONE: Do not change the fallback messaging (TVS-A.6). Do not change INSERT/MODIFY orchestration.
- **Acceptance criteria:**
  - Red test failing before; green after.
  - Real compiler error text surfaced on a known-bad source, 2 independent runs.
  - No regression on known-good source.
  - If the alternative is `VERIFY.ERR`: document its location/format and the read timing (it must be read after `VerifyModule_New` and before `EndCompilingProcess`/cleanup, per TVS-A.3).
- **Dependencies:** TVS-A.3 (root cause + recommended approach). TVS-A.4 (if both apply, A.4 first).
- **Adversarial review:** YES.
- **Effort:** M (depends on approach)

### TVS-A.6 — Implement: wire real error text into CLI output; retire the synthesized fallback
- **Goal:** Ensure the real error text flows from `verifyModule()` through INSERT and MODIFY to the CLI user, and replace the synthesized `"tsdev: VerifyModule_New returned 0 with no error text"` fallback.
- **Type:** IMPLEMENTATION
- **Scope:**
  - DONE: Update `verifyModule()` (L452-459) so that when real error text is available it is returned verbatim. Replace the synthesized fallback: if `VerifyModule_New != 1` AND no real text was retrieved, emit a clearly-labeled best-effort message that explicitly states the real compiler text was unavailable (do NOT silently pass off a synthetic string as the compiler's output). Update `modifyEntry()`/INSERT CLI path to print the error text to the user on failure.
  - TDD: end-to-end test — INSERT a known-bad strategy via the CLI and assert the printed output contains real compiler diagnostics; MODIFY an existing strategy to a known-bad source and assert the same. Negative tests (nonexistent name on MODIFY) still return the not-found message.
  - NOT DONE: No changes to DELETE. No changes to persistence. No new orlib20 calls beyond what TVS-A.4/A.5 introduced.
- **Acceptance criteria:**
  - INSERT with bad source → CLI prints real compiler error text (line + message), exit non-zero, 2 independent runs.
  - MODIFY with bad source → same.
  - INSERT/MODIFY with good source → success, `.ell` persisted, exit 0.
  - MODIFY of nonexistent name → not-found message, `success=false`.
  - The synthesized fallback is gone (or, if retained as last resort, is explicitly labeled as "compiler error text unavailable" and is NOT the common path).
  - No regression in DELETE.
  - Build clean (32-bit Win32 Release).
- **Dependencies:** TVS-A.4 (and TVS-A.5 if applicable).
- **Adversarial review:** YES.
- **Effort:** S

---

## 4. Research Interleaving (summary)

```
TVS-A.1 (RESEARCH: GUI Frida trace of GetCompilingErrors)  ─┐
TVS-A.2 (RESEARCH: static ABI + error-array path)          ─┤── cross-validate
                                                            ▼
TVS-A.3 (RESEARCH: headless error-array population)        ──► root-cause classification
                                                            ▼
TVS-A.4 (IMPL: fix GetCompilingErrors ABI)  ──────────────► (if ABI bug)
TVS-A.5 (IMPL: alternative extraction)      ── CONDITIONAL (if array not populated)
                                                            ▼
TVS-A.6 (IMPL: wire to CLI, retire fallback)
```

Research findings are written to `tmp/re_artifacts/` and adversarially reviewed BEFORE any implementation TVS that depends on them begins. Implementation TVSs do NOT bundle research.

---

## 5. Risk Register

| # | Risk / Unknown | Impact | Mitigation |
|---|----------------|--------|------------|
| R1 | The `GetCompilingErrors` ABI is genuinely `std::string` return-by-value and the current `char*` call is wrong (leading hypothesis). | High — would explain the empty buffer entirely; fix is small but must handle `std::string` under SEH correctly (C2712 constraint). | TVS-A.2 confirms; TVS-A.4 implements with a POD SEH wrapper that receives the `std::string*` and copies `.c_str()` out. |
| R2 | The error array is NOT populated headlessly because the `FUN_10011230` patch (SymWriter bypass) or the missing CLR/message-pump disrupts the error-recording path. | High — would require an alternative extraction (VERIFY.ERR or direct array read). | TVS-A.3 isolates this; TVS-A.5 (conditional) implements the confirmed alternative. |
| R3 | `std::string` returned by `GetCompilingErrors` allocates on the heap; under partial-CLR/SEH this could throw or corrupt. | Medium | SEH-guard the call; copy bytes out immediately; do not let a `std::string` exception escape. |
| R4 | `VERIFY.ERR` may not be written headlessly, or may be written to an unexpected path / require a message pump. | Medium | TVS-A.3 checks for the file's existence and contents during a headless failed compile. |
| R5 | The error-record layout (stride 297) text/line offsets are unmapped, making direct array reads fragile. | Medium | TVS-A.2 maps the minimum needed fields; prefer the `GetCompilingErrors` export over raw array reads when possible. |
| R6 | The fix changes behavior such that a previously-"successful" (empty-buffer) compile now reports warnings as errors. | Low | `GetErrorLevel` distinguishes errors (0x7775) from warnings; ensure only errors (or errors+warnings per TSDev semantics) are surfaced. Confirm against GUI behavior in TVS-A.1. |
| R7 | TSDev.exe GUI automation for TVS-A.1 may be cumbersome (no scripted GUI). | Low | Manual trigger of a compile in the TSDev GUI with Frida attached is acceptable; the hook captures the data regardless of how the compile was initiated. |
| R8 | Read-only TS install constraint: any in-process patches must be memory-only (already the pattern for FUN_10011230). | Low | All patches are in-process VirtualProtect+memcpy on the loaded image; on-disk DLLs untouched. Enforced by existing code pattern. |

---

## 6. Constraints

- **Standalone:** No TSDev.exe dependency in the production CLI. TSDev.exe is used ONLY as a research oracle in TVS-A.1 (Frida trace of the working path).
- **No WHServer killing.** Do not terminate TradeStation processes as a workaround.
- **READ-ONLY TradeStation install.** Never modify on-disk files under `C:\Program Files (x86)\TradeStation 9.5`. Copy DLLs into `tmp/` for static analysis if needed. All runtime patches are in-process memory-only.
- **32-bit.** orlib20.dll is a 32-bit PE; the CLI and all probes are Win32/x86 (MSVC 19.51, CMake `-A Win32`).
- **Adversarial review required** for every substantive result: all three research TVSs (authoritative findings) and all implementation TVSs. Reviewer receives original TVS instructions + the result; checks for complete execution, no scope creep, no drift, no hacks. APPROVE/DISAPPROVE loop until APPROVED.
- **TDD red-green** where applicable (implementation TVSs): commit the failing test first, then the fix.
- **Frida** is used DIRECTLY via the `frida` CLI (never via MCP — the Frida MCP server was removed). Frida 17.17.0 is installed.
- **Ghidra** MCP tools ARE available (server name "ghidra") for static analysis.
- **One subagent → one narrowly scoped subtask.** Each TVS (or sub-step of a TVS) is executed by a single focused subagent.
- **Evidence** goes to `tmp/re_artifacts/`; plan/status updates go to this doc, AGENTS.md TODO, and FEASIBILITY.md as appropriate.

---

## 7. Notes for the Executing Team

- Read this document + `AGENTS.md` + `src/tsdev_lib.cpp` (especially `verifyModule` L404-470 and `callVerifyAndErrors` L140-158) + `tmp/re_artifacts/orlib_param_orders.md` §10/§12 before starting.
- The leading hypothesis (ABI mismatch on `GetCompilingErrors`) is strong but UNVERIFIED. Do not skip TVS-A.2/A.3 — confirm before implementing.
- If TVS-A.3 finds the root cause is the ABI only (array populated, retrieval wrong), TVS-A.5 is SKIPPED and TVS-A.4 → TVS-A.6 is the path.
- If TVS-A.3 finds the array is NOT populated, TVS-A.5 is required and TVS-A.4 may be partially or fully subsumed — let the finding guide.
- Update this document's TVS statuses (PLANNED → IN-PROGRESS → DONE) and AGENTS.md TODO as work progresses.
