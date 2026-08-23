# RE: TVS-L.8 — Post-Compile Stale-Write Fault A (Read-Only ELL False Success)

**Status:** COMPLETE (documentation / evidence record only)
**Date:** 2026-08-23 (static), 2026-08-23T20:13Z (dynamic Frida capture)
**TVS:** L.8 (durable RE evidence record for the post-compile stale-write fault class)
**Binary analyzed:** `orlib20.dll` (image base `0x10000000`), `ELMDCore.dll`, `ELDebugger.dll`, `ECL.dll`
**Tools:** Ghidra MCP (static decompilation), Frida CLI (dynamic trace of `tsdev_cli.exe`)

> **Workstream L recovery pointer (current truth).** Fault A **detect-and-fail is
> implemented by TVS-L.9** (stale-ELL modify/upsert detect-and-fail production fix), which is
> part of the **L.6–L.15 recovery implemented on branch `workstream-L-recovery`**.
>
> **TVS-L.8 is closed as a DOCUMENTED-PARITY DECISION (2026-08-24) -- NOT as a recovery.** The
> `TSDev.exe`-equivalent save-failure observation has NOW been established (2026-08-24,
> user-supervised Frida trace of `TSDev.exe` PID 23012 -- see Sec. 7.1). It establishes that
> `TSDev.exe` exhibits the SAME silent false-success suppression chain as the CLI: TSDev does
> NOT detect the `.ell` write failure, does NOT roll back / unverify / retry / warn, and
> reports success while leaving the identical inconsistent state (stale ELL + rewritten XML +
> new GUID + updated warehouse source). Because TSDev itself has no such recovery, **no
> invented rollback / unverify / retry is authorized**. The selected L.8 policy is therefore
> **remain BLOCKED -- documented parity (decision)** (see Sec. 7). This is a DECISION, not a
> fake recovery. The L.9 detect-and-fail does **not** invent a recovery; it detects the
> stale-ELL condition and fails the operation rather than reporting false success. **L.9's
> detect-and-fail is in fact STRICTER than TSDev** (TSDev silently reports success; our CLI
> detects and fails). This is acceptable and intentional -- we do not propagate the false
> success that TSDev propagates. Existence checks are necessary but not sufficient.
>
> **L.8 is closed as a DECISION, NOT as a recovery.** Workstream L is CLOSED (2026-08-24,
> explicit user decision) -- L.8's documented-parity DECISION plus L.9/L.12
> detect-and-fail/report plus L.17 automated gates PASS under PS 5.1 plus the Desktop
> restart gate SATISFIED (2026-08-24) constitute the complete closure basis; the user issued
> the explicit close decision on 2026-08-24. Workstream M remains BLOCKED until an explicit
> unblocking decision (Workstream L closure does not automatically unblock M). See
> `docs/PM_WORKING_MEMORY.md`. The evidence body in Sec. 1 -- Sec. 6, Sec. 8 -- Sec. 10 is
> unchanged; only this current-truth pointer banner and Sec. 7 / Sec. 7.1 change.
>
> **Scope of this file.** This is a *durable evidence record* for a distinct fault class
> — **post-compile stale-write Fault A** — that was *not* covered by the prior
> finalization-truncation analysis in
> [`RE_VERIFYMODULE_FINALIZATION_TRUNCATION.md`](./RE_VERIFYMODULE_FINALIZATION_TRUNCATION.md).
> It records static + dynamic evidence, distinguishes Fault A from finalization-truncation,
> and selects the L.8 decision-gate policy. It does **not** authorize any production
> recovery action (see §7).

---

## 1. Executive Summary

**Fault A (post-compile stale-write false success).** When the compile and code-generation
stages of a modify/upsert-existing operation **succeed** (`FUN_10011230` returns `1`,
`FUN_1008e640` returns `1`) but the subsequent `.ell` write fails at the OS level (e.g.
`CreateFileW` on the `.ell` returns `INVALID_HANDLE` because the file is read-only), the
failure is **silently suppressed** by `ELMDCore.dll`:

- `MDWriter::SaveToFile` (`0x10019320`) **forces its return value to `true`** at `0x1001942d`
  **without checking the stream failbit**, so the OS-level write failure is invisible to its
  caller.
- `MDVault::SaveModule` (`0x1001fd80`) **discards `SaveToFile`'s result** and returns its
  local `local_15 = 1` (success).
- `CELBasicCompiler::CompileText` (`0x1001dd80`) then calls `SetGuid`, `SaveModule`, and
  **still proceeds to `AddATVerified` / `UpdateProperties`** when `param_6 == 0`, and
  returns `1`.
- `_VerifyModule_New` (`0x10169630`) returns `1`.

**Net result:** the `.ell` is left **stale** (prior bytes unchanged), the `.xml` is
**rewritten** with a new GUID by `FormatAndSaveXml`, the warehouse source is updated, and
the operation reports **success** (`exit 0`, `success=true`). The new XML GUID has **0
occurrences** in the stale `.ell` → the runtime-linkable artifact and the debugger/source-map
artifact are **inconsistent**, reproducing the unknown-link defect class — but reported as a
success.

**This is a different fault class from finalization-truncation.** Finalization-truncation
(the subject of the prior doc) occurs when `FUN_10011230` returns **false** and
`MDVault::SaveModule` is **skipped**, leaving the prior `.ell` intact *because the write was
never attempted*. Fault A occurs when `FUN_10011230` returns **true**, `SaveModule` **is
called**, the write **is attempted and fails at the OS level**, and the failure is
**suppressed** by `SaveToFile`/`SaveModule`. The two classes share the *symptom* (stale
`.ell`) but have *different causes* and *different detection surfaces*.

**Decision-gate policy (selected — see §7):** **BLOCKED.** No `TSDev.exe`-equivalent
save-failure observation or per-item rollback / unverify / retry API was established as a
basis for authorizing a recovery action. No ELL/XML backup-restore, no invented TSDev
recovery, and no production code are authorized by this finding.

---

## 2. Fault A vs. Finalization-Truncation (distinction)

| Dimension | Finalization-truncation (prior doc) | Fault A (this doc) |
|-----------|--------------------------------------|---------------------|
| `CodeScanner` result | success (`DAT_1023aac8 == 0`) | success (`DAT_1023aac8 == 0`) |
| `FUN_1008e640` (codegen) result | **false** (fails partway, sets `DAT_1023aac8`) | **true** (succeeds) |
| `FUN_10011230` (finalization) return | **false** (`uVar5 = FUN_1008e640` result) | **true** |
| `MDVault::SaveModule` called? | **No** — skipped (`iVar3 != 1` → `goto LAB_1001e2b2`) | **Yes** — called |
| `.ell` write attempted? | No | **Yes** — `CreateFileW` on `.ell` |
| `.ell` write outcome | n/a (never attempted) | **OS-level failure** (`INVALID_HANDLE`); failbit set |
| `.ell` failure propagated? | n/a | **No** — `SaveToFile` forces `true`; `SaveModule` returns `local_15=1` |
| `.xml` written? | Yes (`SymWriter::Save` unconditional) | Yes (`FormatAndSaveXml`) |
| Reported result | `FUN_10011230` returns false → error path | **`_VerifyModule_New` returns 1 → success** |
| Stale `.ell` cause | Write never attempted | Write attempted, failed, suppressed |
| Caught by `.ell`/`.xml` existence checks? | Yes (`.ell` exists, `.xml` may be new — but op reports failure) | **No** — both exist, op reports success |

**Key consequence:** the prior doc's structural guarantee — "the `.ell` is unchanged
because `MDVault::SaveModule` is skipped when `FUN_10011230` returns false" — is **valid for
the finalization-truncation scenario only**. It does **not** cover Fault A, where
`FUN_10011230` returns **true** and `SaveModule` **is** called. The universal form of that
guarantee ("on ANY modify failure") and the claim that "`.ell`/`.xml` existence checks are
sufficient to detect any structural artifact-write failure" are **falsified by Fault A** and
have been scoped accordingly in the prior doc (see §8).

---

## 3. Static Evidence (Ghidra)

All addresses are image-base-relative absolute VAs (`ELMDCore.dll` / `orlib20.dll` image base
`0x10000000`). The three facts below are the static root cause of Fault A.

### 3.1 `MDWriter::SaveToFile` (`ELMDCore.dll` `0x10019320`) forces `true` at `0x1001942d` without failbit check

`SaveToFile` serializes the module to an in-memory `ostrstream`, opens the target `.ell`
path via `std::_Fiopen(path, 0x22, 0x40)` (`ios::out | ios::binary`, truncate-on-open), and
performs `ostream::write`. The decompiled return path **forces `true` at `0x1001942d`
without testing the stream's failbit** (`std::ios_base::failbit` / `badbit`). Consequently,
when `CreateFileW` returns `INVALID_HANDLE` (e.g. read-only target) and the `ostream` enters
a failed state, `SaveToFile` still returns **`true`**. The OS-level write failure is not
propagated to the caller.

### 3.2 `MDVault::SaveModule` (`ELMDCore.dll` `0x1001fd80`) discards `SaveToFile` and returns `local_15 = 1`

`SaveModule` calls `SaveToFile` but **does not branch on its return value**. It returns its
local `local_15`, which is initialized to `1` (success). Because `SaveToFile`'s result is
discarded, `SaveModule` reports success regardless of whether the `.ell` bytes were actually
written. This is the second suppression point: even if `SaveToFile` had returned `false`,
`SaveModule` would still return `1`.

### 3.3 `CELBasicCompiler::CompileText` (`orlib20.dll` `0x1001dd80`) proceeds to `AddATVerified` / `UpdateProperties` when `param_6 == 0`

After `FUN_10011230` returns, `CompileText` calls `ELMeta::ModuleInfo::SetGuid` (IAT
`0x101f9340`) and then `MDVault::SaveModule` (IAT `0x101f9344`). When `param_6 == 0`, it
**still proceeds** to `AddATVerified` and `UpdateProperties` — i.e. it marks the item
verified and updates warehouse properties **without re-checking whether the `.ell` write
actually succeeded**. `CompileText` then returns `1`, and `_VerifyModule_New` returns `1`.

**Static root-cause chain (Fault A):**

```
_VerifyModule_New@20 ............ 0x10169630  (orlib20.dll)
  └─ CompileText ................. 0x1001dd80  (orlib20.dll)
       ├─ CodeScanner ............ 0x1001e8e0  -> retval 0 (success)
       ├─ FUN_10011230 ........... 0x10011230  -> retval 1 (codegen FUN_1008e640 -> 1)
       │    └─ SymWriter::Save ... 0x100120a0  -> writes .xml (FormatAndSaveXml)
       ├─ SetGuid ................ IAT 0x101f9340 -> 1
       ├─ MDVault::SaveModule .... 0x1001fd80  (ELMDCore.dll) -> returns local_15 = 1
       │    └─ MDWriter::SaveToFile 0x10019320 (ELMDCore.dll)
       │         ├─ CreateFileW(.ell, GENERIC_WRITE) -> INVALID_HANDLE  *** OS-level failure ***
       │         └─ return TRUE @ 0x1001942d  *** failbit NOT checked ***
       ├─ [param_6 == 0] AddATVerified / UpdateProperties  *** proceeds despite failed .ell write ***
       └─ return 1
  └─ return 1
```

---

## 4. Dynamic Evidence (Frida CLI trace)

**Run:** `TS_AUTO_L8_MOD_RO_20260823_221307`, strategy, **ProgId 7538**.
**Fault injection:** `attrib +R` on
`C:\Program Files (x86)\TradeStation 9.5\MyWork\Bin\elmodule_0_7538.ell` before the modify.
**Trace target:** `tsdev_cli.exe` (production CLI) only — never `TSDev.exe`. No binary
modification, no production code writes. Raw log:
`tmp/l8_fault_a_trace/frida_trace_l8.log` (transient). Summary:
`tmp/l8_fault_a_trace/l8_summary.json` (transient).

### 4.1 Call sequence and return values (from `frida_trace_l8.log`)

| call# | function | address | retval | note |
|-------|----------|---------|--------|------|
| 276 | `_VerifyModule_New_20` | `0x10169630` | `0x1` | top-level success |
| 277/278 | precheck1 / precheck2 | `0x10166e70` / `0x10166060` | `0x0` | pre-checks pass |
| 279 | `FUN_101087d0` loadSource | `0x101087d0` | `0xebdb760` | source loaded |
| 280 | `CompileText` | `0x1001dd80` | **`0x1`** | compile+finalize success |
| 358 | `CodeScanner` | `0x1001e8e0` | `0x0` | scan success |
| 396 | `FUN_10011230` finalization | `0x10011230` | **`0x1`** | finalization success |
| 397 | `FUN_1008e640` codegen | `0x1008e640` | **`0x1`** | codegen success |
| 398 | `SymWriter::Save` | `0x59f220a0` (ELDebugger) | `0x0` | `.xml` write path entered |
| 399 | `FormatAndSaveXml` | `0x82841b0` (ECL) | **`0x1`** | `.xml` rewritten |
| 400 | `SHCreateStreamOnFileW` `.xml` | shlwapi | `0x0` | flags `0x1011` (`STGM_CREATE\|STGM_WRITE\|STGM_SHARE_DENY_WRITE`) |
| 402 | `IAT_SetGuid` | `0x101f9340` | `0x1` | GUID set |
| 403 | `IAT_SaveModule` | `0x101f9344` | **`0x1`** | SaveModule "success" |
| 404 | `MDWriter::SaveToFile` | `0x80a9320` (ELMDCore) | **`0x1`** | forced true |
| 405 | `CreateFileW` `.ell` | kernel32 | **`INVALID_HANDLE`** | access `0x40000000` = `GENERIC_WRITE`; `.ell` is read-only |

**Critical order (Fault A signature):** `CreateFileW(.ell, GENERIC_WRITE) -> INVALID_HANDLE`
(call 405) is the innermost/deepest call; its OS-level failure is then suppressed outward
through the return-value chain as each enclosing frame reports success — `MDWriter_SaveToFile -> 0x1`
(call 404, the caller of 405), `IAT_SaveModule -> 0x1` (call 403), `CompileText -> 0x1`
(call 280), `_VerifyModule_New_20 -> 0x1` (call 276). (This is outward propagation through
the call stack, not temporal call-number order: 405 occurs last in time but is the deepest
frame.) The OS-level `.ell` write failure is fully suppressed; every layer reports success.

> The `.ell` `CreateFileW` returned `INVALID_HANDLE` with `GetLastError=0` as captured by the
> hook (the read-only attribute denies `GENERIC_WRITE` at open time). The hook's
> `GetLastError` read may reflect a post-call reset; the `INVALID_HANDLE` return itself is
> the authoritative failure signal and is recorded as such.

### 4.2 Artifact state (from `l8_summary.json`)

| Artifact / metric | Pre | Post | Changed |
|-------------------|-----|------|---------|
| `.ell` SHA-256 | `34DC56431E4E6BDD45AA63D60A1B26E0DA2435053998EB44B86B5561DCDA6A6A` | `34DC56431E4E6BDD45AA63D60A1B26E0DA2435053998EB44B86B5561DCDA6A6A` | **False** (stale) |
| `.xml` SHA-256 | `3A3CF74214308C45A5518C17A1EC28AEF42C9238D4AC53CB0132CB3E909B9882` | `54D86987347AE7C5D27727332329A718040BB8C44FF1FA456BA15903F0E5382E` | **True** (rewritten) |
| New XML GUID | — | `{56E03EA7-1B8D-46E2-98F8-C31D4DEB73A5}` | — |
| New GUID occurrences in post `.ell` | — | **0** | GUID absent from stale `.ell` |
| Modify exit code | — | `0` | reported success |
| Modify JSON success | — | `true` | reported success |

### 4.3 Post-restart get-source

After process restart, a fresh CLI `get-source` for the same item returned the **NEW**
(modified) source (`GetSourceMatchesNew = true`, `GetSourceMatchesOld = false`, source length
57). The warehouse source was updated; the `.ell` was not. The false-success state **persists
across restart** and is observable by a fresh process.

### 4.4 Ancillary observation (not part of the success return chain)

After `_VerifyModule_New_20` returned `0x1` (call 276), the Frida log recorded a burst of
`EXCEPTION type=system` events on a **different thread** (`tid=20316`, not the worker
`tid=31212`). These are post-return teardown/cleanup exceptions, **not** part of the Fault A
success-path return chain (which completed with retval `0x1` on the worker thread). They are
noted for completeness and are not used as evidence of the Fault A success suppression, which
is established by the return-value chain in §4.1.

---

## 5. L7 Reproduction (same fault class)

The same Fault A class was reproduced independently by the L7 RED fault-injection suite
(`tests/test_unknown_link_regression.ps1 -IncludeRedFaultInjection`), recorded in
[`RE_L7_RED_FAULT_INJECTION_EVIDENCE.md`](./RE_L7_RED_FAULT_INJECTION_EVIDENCE.md):

- **L7.1** — modify / read-only ELL: `TS_AUTO_L7_MOD_RO_20260823_214728`, ProgId 7530.
  ELL hash unchanged, XML hash changed, new XML GUID `{B24BFA5C-...}` had **0 occurrences**
  in the post-op ELL, `get-source` returned NEW source, CLI exit `0` / `success=true`.
  Desired-state invariant **failed** with `false_verified_success`.
- **L7.2** — upsert-existing / read-only ELL: `TS_AUTO_L7_UPS_RO_20260823_214728`, ProgId
  7531. Same signature: ELL unchanged, XML changed, new GUID occ=0, NEW source, exit `0` /
  `success=true`, desired-state **failed**.

L7 (black-box fault injection) and L.8 (Frida static+dynamic RE) independently establish the
same fault class. L.8 adds the **internal call-sequence and suppression-point root cause**
that L7's black-box observation could not: `SaveToFile` forces `true` at `0x1001942d` without
failbit check, `SaveModule` discards the result and returns `local_15=1`, and `CompileText`
proceeds to `AddATVerified`/`UpdateProperties` when `param_6 == 0`.

---

## 6. What This Finding Is NOT

1. **Not finalization-truncation.** Fault A is not the scenario analyzed in
   `RE_VERIFYMODULE_FINALIZATION_TRUNCATION.md`. There, `FUN_10011230` returns **false** and
   `SaveModule` is **skipped**. Here, `FUN_10011230` returns **true**, `SaveModule` **is**
   called, and the write **fails at the OS level** with the failure suppressed. The prior
   finalization-truncation analysis is **not weakened** by this finding; only its
   *universal* generalization ("on ANY modify failure") and the existence-check-sufficiency
   claim are scoped (see §8).
2. **Not authorization for ELL/XML backup-restore.** No byte-level backup/restore of `.ell`
   or `.xml` is authorized by this finding. Direct artifact restoration remains prohibited
   unless evidence establishes a TSDev-equivalent, cross-store-consistent recovery sequence
   (none established here — see §7).
3. **Not an invented TSDev recovery.** No `TSDev.exe` recovery, reverify, unverify, or
   per-item rollback API is identified or proposed. The decision gate is BLOCKED precisely
   because no such mechanism was established.
4. **Not production code.** This is a documentation/evidence record only. No production
   source, signatures, tests, or build artifacts are modified by TVS-L.8.
5. **Not a runtime-linkability proof.** The GUID-in-ELL binary-search check is used as the
   safest local consistency signal. Actual Desktop runtime linking of a stale-ELL item was
   not tested and must not be tested with non-disposable items.

---

## 7. Decision-Gate Policy (selected)

**Policy: remain BLOCKED -- documented parity (DECISION, 2026-08-24).**

**What changed (2026-08-24).** The `TSDev.exe`-equivalent save-failure observation that was
previously missing has NOW been established by a user-supervised Frida trace of `TSDev.exe`
(PID 23012, 2026-08-24, Verify at 15:10:23Z). The full suppression chain is captured and is
byte-for-byte identical to the CLI trace in Sec. 4.1 (decisive frames in Sec. 7.1 below).
`TSDev.exe` exhibits the SAME silent false-success behavior as the CLI: it does NOT detect the
`.ell` write failure, does NOT roll back / unverify / retry / warn, and reports success while
leaving the identical inconsistent state (stale ELL + rewritten XML + new GUID + updated
warehouse source).

**Selected policy.** Because `TSDev.exe` itself has no recovery for Fault A (it silently
reports success and leaves the inconsistent state in place), **no invented rollback /
unverify / retry is authorized** -- there is no TSDev-equivalent recovery to emulate. The
L.8 decision gate therefore **remains BLOCKED**, but now as a **DOCUMENTED-PARITY DECISION**,
not as a pending-recovery item. This is a DECISION, not a fake recovery. No invented TSDev
recovery sequence is authorized because TSDev itself has no such recovery.

**L.9 detect-and-fail is STRICTER than TSDev.** The L.9 production fix (stale-ELL
modify/upsert detect-and-fail) detects the stale-ELL condition and fails the operation rather
than reporting false success. `TSDev.exe`, by contrast, silently reports success and
propagates the false success. L.9's detect-and-fail is therefore STRICTER than TSDev. This is
acceptable and intentional -- we do not propagate the false success that TSDev propagates.
Emulating TSDev does not require us to inherit TSDev's silent false-success behavior on a
fault class that we can detect.

**Specifically prohibited by this finding (unchanged):**

- Authorizing ELL/XML backup-restore as a recovery mechanism.
- Inventing a TSDev recovery sequence not present in the binary.
- Shipping production code that masks or works around Fault A.
- Weakening the original finalization-truncation analysis in the prior doc.

**L.8 closure status.** L.8 is **closed as a DECISION (documented parity, no recovery)**,
NOT as a recovery. Workstream L is CLOSED (2026-08-24, explicit user decision) -- L.8's
documented-parity DECISION plus L.9/L.12 detect-and-fail/report plus L.17 automated gates
PASS under PS 5.1 plus the Desktop restart gate SATISFIED (2026-08-24) constitute the
complete closure basis; the user issued the explicit close decision on 2026-08-24.
Workstream M remains BLOCKED until an explicit unblocking decision (Workstream L closure
does not automatically unblock M). See `docs/PM_WORKING_MEMORY.md`.

### 7.1 TSDev.exe parity evidence (2026-08-24)

**Experiment.** 2026-08-24, user-supervised. Disposable Strategy
`TS_AUTO_L8_TSDEV_PROBE_01`, ProgId 7933, inserted via CLI. The `.ell` was marked read-only
(`attrib +R`) before TSDev Verify. `TSDev.exe` was spawned via Frida
(`frida -f TSDev.exe -l frida_trace_l8_tsdev.js`), PID 23012. The user opened the strategy in
TSDev, changed `Length(10)` to `Length(20)`, and ran Build > Verify (F3). No F5, no live
orders, no Desktop. TradeStation Desktop was CLOSED throughout.

**TSDev UI result (user-observed).** Verify SUCCEEDED. The diagnostic panel showed
"0 error(s), 0 warning(s)". The Verify button became disabled, indicating the strategy is
considered verified. NO error dialog, NO failure indication, NO rollback/retry observed.

**Frida trace (TSDev.exe, PID 23012, Verify at 15:10:23Z).** The full Fault A suppression
chain was captured and is byte-for-byte identical to the CLI trace in Sec. 4.1. Decisive
frames (call numbers from `tmp/l8_fault_a_trace/frida_trace_l8_tsdev.log`):

| call# | function | address | retval | note |
|-------|----------|---------|--------|------|
| 3188 | `_VerifyModule_New_20` ENTER | `0x10169630` | -- | top-level enter |
| 3192 | `CompileText` ENTER | `0x1001dd80` | -- | compile+finalize enter |
| 3221 | `FUN_1008e640` codegen | `0x1008e640` | `0x1` | codegen success |
| 3223 | `FormatAndSaveXml` | `0x82841b0` (ECL) | `0x1` | `SHCreateStreamOnFileW` on `elmodule_0_7933.xml` flags `0x1011` succeeded; XML rewritten |
| 3226 | `IAT_SetGuid` | `0x101f9340` | `0x1` | GUID set |
| 3229 | `CreateFileW` `.ell` | kernel32 | **`INVALID_HANDLE`** | access `0x40000000` = `GENERIC_WRITE`; `.ell` is read-only -- OS-level write failure |
| 3228 | `MDWriter::SaveToFile` | `0x80a9320` (ELMDCore) | **`0x1`** | forced true; failbit NOT checked (identical to CLI) |
| 3227 | `IAT_SaveModule` | `0x101f9344` | **`0x1`** | result discarded; returns `local_15=1` (identical to CLI) |
| 3192 | `CompileText` EXIT | `0x1001dd80` | **`0x1`** | proceeds to `AddATVerified` despite failed `.ell` write (identical to CLI) |
| 3188 | `_VerifyModule_New_20` EXIT | `0x10169630` | **`0x1`** | success |

**Critical order (Fault A signature, identical to Sec. 4.1):**
`CreateFileW(.ell, GENERIC_WRITE) -> INVALID_HANDLE` (call 3229) is the innermost/deepest
call; its OS-level failure is then suppressed outward through the return-value chain as each
enclosing frame reports success -- `MDWriter_SaveToFile -> 0x1` (call 3228, the caller of
3229), `IAT_SaveModule -> 0x1` (call 3227), `CompileText -> 0x1` (call 3192 EXIT),
`_VerifyModule_New_20 -> 0x1` (call 3188 EXIT). The OS-level `.ell` write failure is fully
suppressed; every layer reports success.

**Artifact state (pre/post):**

| Artifact / metric | Pre | Post | Changed |
|-------------------|-----|------|---------|
| `.ell` SHA-256 | `66559441E176FB1D4A12AF24E119D09B51F050BF428B432326B9456B4C15CB94` | `66559441E176FB1D4A12AF24E119D09B51F050BF428B432326B9456B4C15CB94` | **False** (stale) |
| `.xml` SHA-256 | `EF82A64F47B3779E9364720419229BA36E8639AF0197888BB571076BB26CC6BB` | `9BAF6664186BC3EB6344834556ECFD375E674ABF81E7D2506FAFB3D61EBBABDD` | **True** (rewritten) |
| XML GUID | `A38BF815-9C22-40E1-962E-B4450C71C6DD` | `AA790477-C9A7-4F98-B4B8-125807CCC954` | **True** (new GUID) |
| New GUID occurrences in post `.ell` | -- | **0** | GUID absent from stale `.ell` |
| get-source (post) | -- | NEW source `Length(20)` | warehouse source updated; ELL not |
| ELL attribute | `ReadOnly, Archive` | `ReadOnly, Archive` | unchanged throughout |

**Comparison to CLI trace (Sec. 4.1).** The suppression chain is identical at every frame.
`TSDev.exe` does NOT detect the `.ell` write failure. `TSDev.exe` does NOT roll back,
unverify, retry, or warn. `TSDev.exe` reports success and marks the item verified, leaving
the identical inconsistent state (stale ELL + rewritten XML + new GUID + updated warehouse
source). This is the documented parity that closes L.8 as a DECISION (no recovery).

**Cleanup.** Read-only cleared, disposable item deleted via CLI, ELL and XML confirmed gone
(no orphans). No TS processes remain.

---

## 8. Scoped Corrections to the Prior Finalization-Truncation Doc

This finding falsifies only the **universal** claims in
`RE_VERIFYMODULE_FINALIZATION_TRUNCATION.md` that generalized the finalization-truncation
guarantee to "ANY modify failure" and the claim that `.ell`/`.xml` existence checks are
sufficient to detect any structural artifact-write failure. The scoped edits to that doc:

1. The structural guarantee "the `.ell` is unchanged because `MDVault::SaveModule` is skipped
   when `FUN_10011230` returns false" is **retained and scoped to the finalization-truncation
   scenario** (where `FUN_10011230` returns false). It is no longer stated as a guarantee
   over "ANY modify failure", because Fault A is a distinct class where `FUN_10011230`
   returns true and the write fails at the OS level with the failure suppressed.
2. The claim that "the existing production postconditions (`.ell` and `.xml` existence checks
   in `doInsert`/`doModify`) are sufficient to detect any structural artifact-write failure
   that would compromise runtime linkability" is **removed**. Fault A passes both existence
   checks (stale `.ell` exists, new `.xml` exists) while leaving the library in an
   inconsistent, runtime-unlinkable state and reporting success.
3. The "No backup/restore is needed" conclusion is **qualified**: it remains valid for the
   finalization-truncation scenario specifically, but it is no longer generalized over Fault
   A, for which no recovery mechanism is established (§7).

The original finalization-truncation *analysis* (the `FUN_10011230`-returns-false →
`SaveModule`-skipped → `.ell`-unchained chain, the Q1–Q4 evidence, and the address index) is
**unchanged** and remains correct for its scenario.

---

## 9. Function Address Index (Fault A path)

| Function | Address | DLL | Role in Fault A |
|----------|---------|-----|-----------------|
| `_VerifyModule_New@20` | `0x10169630` | orlib20.dll | Returns `1` (success) |
| `CELBasicCompiler::CompileText` | `0x1001dd80` | orlib20.dll | Calls SetGuid, SaveModule, then AddATVerified/UpdateProperties (`param_6==0`); returns `1` |
| `CodeScanner` | `0x1001e8e0` | orlib20.dll | Returns `0` (scan success) |
| `FUN_10011230` (finalization) | `0x10011230` | orlib20.dll | Returns `1` (codegen succeeded) |
| `FUN_1008e640` (codegen) | `0x1008e640` | orlib20.dll | Returns `1` (codegen success) |
| `SymWriter::Save` | `0x100120a0` | ELDebugger.dll | Writes `.xml` |
| `FormatAndSaveXml` | `0x100a41b0` | ECL.dll | `SHCreateStreamOnFileW(.xml, 0x1011, ...)` — rewrites `.xml` |
| `IAT_SetGuid` | `0x101f9340` | orlib20.dll IAT | `ModuleInfo::SetGuid` — returns `1` |
| `IAT_SaveModule` | `0x101f9344` | orlib20.dll IAT | `MDVault::SaveModule` — returns `local_15=1` |
| `MDVault::SaveModule` | `0x1001fd80` | ELMDCore.dll | Discards `SaveToFile` result; returns `local_15=1` |
| `MDWriter::SaveToFile` | `0x10019320` | ELMDCore.dll | **Forces `true` at `0x1001942d` without failbit check** |
| `CreateFileW` | kernel32 export | kernel32.dll | `.ell` open with `GENERIC_WRITE` → `INVALID_HANDLE` |

---

## 10. Methodology / Reproducibility

- **Static:** Ghidra project `TSDevAnalysis`; decompilation of `ELMDCore.dll`
  `MDWriter::SaveToFile` (`0x10019320`), `MDVault::SaveModule` (`0x1001fd80`), and
  `orlib20.dll` `CELBasicCompiler::CompileText` (`0x1001dd80`). Addresses are consistent with
  the prior truncation doc's address index (same image bases).
- **Dynamic:** Frida CLI (`frida -f tsdev_cli.exe -l frida_trace_l8.js`) tracing the
  production CLI only. Hook script: `tmp/l8_fault_a_trace/frida_trace_l8.js` (transient).
  Hooks: `_VerifyModule_New`, `CompileText`, `CodeScanner`, `FUN_10011230`, `FUN_1008e640`,
  `FUN_101087d0`, `FUN_10156ad0`, prechecks, IAT `SetGuid`/`SaveModule`,
  `MDVault::SaveModule`, `MDWriter::SaveToFile`, `SymWriter::Save`, `FormatAndSaveXml`,
  `CreateFileW`, `SHCreateStreamOnFileW`. None of the hooked functions are signature-scanned
  by the CLI's `OrlibResolver` (so the inline trampolines do not break CLI init).
- **Fault injection:** `attrib +R` on the target `.ell` only (`.xml` left writable) before
  the modify, then Frida-spawned `tsdev_cli modify`. Recovery marker written before
  `attrib +R`; read-only attribute restored and disposable item deleted in cleanup.
- **No live tests run, no staging, no commit** as part of this documentation task. Raw logs
  remain transient under `tmp/l8_fault_a_trace/` (gitignored).
