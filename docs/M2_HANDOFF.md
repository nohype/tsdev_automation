# M.2 Handoff — Workstream M, Slice M.2 (TSDev Parity: Save-Side `_SaveModule_New` Flags & Password Buffer)

> **Status:** Implemented and verified (TDD red→green; full regression PASS).
> **Date:** 2026-08-24
> **Workstream:** M (TSDev parity) — slice M.2
> **Plan reference:** `docs/workstream_tsdev_parity_plan.md`
> **RE evidence:** `docs/RE/RE_M2_SAVEMODULE_NEW.md` (R-M.2.1 through R-M.2.5)
> **Catalog impact:** None (see §5)

This document is the durable findings/handoff note for Workstream M slice M.2. It records the confirmed research answers (R-M.2.1 through R-M.2.5), the implementation summary, test and E2E results, signature/catalog impact, residual limitations, and follow-up work for M.3–M.5. It is intended to survive context compaction and to be the authoritative reference for downstream M-slices and for any future re-validation of the M.2 changes.

---

## 1. Summary

TVS-M.2 reproduces TSDev's `_SaveModule_New@20` flags and password buffer for save-side internal encryption parity. The CLI previously passed NULL for both arg3 (flags) and arg5 (password). It now passes:

- **arg3 (flags):** a non-NULL pointer to a flags word computed exactly as TSDev's `FUN_00452290` computes it: `(type_word & 0x7) | (has_expiry << 3) | (is_verified << 4) | (is_protected << 5) | (is_protected_plus << 6)`, with TSDev's always-on protection bits (5 and 6) forced on using TSDev's internal default password.
- **arg5 (password):** a non-NULL pointer to a 64-byte buffer that is zeroed first, then filled with TSDev's internal default password `"!p$Kn@aSus"` (the 10-byte constant at `0x00567b40` in TSDev.exe).

This is internal encryption parity only. The password is **not** a user-supplied secret and is never exposed to the user; it is an internal zeroed-then-filled buffer mirroring TSDev's behavior. The constant salt `0xe1ca1015` observed across items confirms the fixed internal password.

---

## 2. Research answers (R-M.2.1 through R-M.2.5)

Full evidence for each answer is in `docs/RE/RE_M2_SAVEMODULE_NEW.md`. Summary below.

### R-M.2.1 — Flags word (arg3) construction

The flags word (arg3) is computed by TSDev's `FUN_00452290` as:

```
flags = (type_word & 0x7)
      | (has_expiry      << 3)
      | (is_verified     << 4)
      | (is_protected    << 5)
      | (is_protected_plus << 6)
```

TSDev **always** forces protection (bits 5 and 6) using its internal default password `"!p$Kn@aSus"`. Therefore:

- **New items (insert, pre-verify):** `flags = (category & 0x7) | 0x60` (bits 5+6 set; bit 4 = is_verified is 0 because the item has not yet been verified).
- **Existing verified items (modify):** `flags = (category & 0x7) | 0x70` (bits 5+6 set; bit 4 = is_verified is 1 because the item was previously verified).

Per type:

| Type | Insert (pre-verify) | Modify (verified) |
|---|---|---|
| Strategy    | `0x60` | `0x70` |
| Indicator   | `0x62` | `0x72` |
| Function    | `0x64` | `0x74` |
| ShowMe      | `0x62` | `0x72` |
| TradingApp  | `0x62` | `0x72` |

**Evidence:** `docs/RE/RE_M2_SAVEMODULE_NEW.md` (R-M.2.1).

### R-M.2.2 — CELProperties is NOT a save-path dependency

CELProperties is **not** an orlib20.dll export. It is a TSDev.exe import from a non-orlib20 DLL. The save path does **not** use CELProperties — it only needs the editor HWND (arg4), which the CLI already has (`m_editorHwnd`, established by M.1 / R-M.1.4). CELProperties is only used in the verify path (compiler `+0x18`), already handled by M.1.

**Conclusion:** CELProperties is **not a blocker** for M.2.

**Evidence:** `docs/RE/RE_M2_SAVEMODULE_NEW.md` (R-M.2.2).

### R-M.2.3 — Save-side encryption trigger and password buffer

Encryption is triggered by a **non-NULL password pointer** (arg5) in `FUN_101a08b0`: `if (param_5 != NULL) → encrypt`. The password buffer is 64 bytes, zeroed first, then `"!p$Kn@aSus"` (10 bytes at `0x00567b40` in TSDev.exe) copied in. This is TSDev's **internal default**, not a user secret. The constant salt `0xe1ca1015` across items confirms the fixed password.

**Evidence:** `docs/RE/RE_M2_SAVEMODULE_NEW.md` (R-M.2.3).

### R-M.2.4 — Pre-verify save only; no post-verify save

Only a **pre-verify save** is performed. No post-verify save is needed. The CLI's single `saveSource()` before `verifyModule()` matches TSDev's flow.

**Evidence:** `docs/RE/RE_M2_SAVEMODULE_NEW.md` (R-M.2.4).

### R-M.2.5 — No user-facing password

Confirmed. No user-facing password parameter is needed or appropriate. The password is an internal zeroed-then-filled buffer mirroring TSDev, never a user-supplied secret. This is consistent with the project rule that the CLI emulates TSDev.exe and does not invent user-facing controls that TSDev does not expose.

**Evidence:** `docs/RE/RE_M2_SAVEMODULE_NEW.md` (R-M.2.5).

---

## 3. Implementation summary

### Production change — `src/tsdev_lib.cpp` `saveSource()`

Changed from passing `NULL`/`NULL` for arg3/arg5 to passing non-NULL values:

- **arg3 (flags):** `&saveFlags` where
  - `saveFlags = (getCategory(type) & 0x7) | 0x60` for **insert** (pre-verify), and
  - `saveFlags = (getCategory(type) & 0x7) | 0x70` for **modify** (bit 4 = `is_verified` set, because the existing item was previously verified).
- **arg5 (password):** `savePasswordBuf` — a 64-byte buffer, zeroed first, then `"!p$Kn@aSus"` copied in (mirrors TSDev's `FUN_00452290` buffer construction).

### Test hook — `src/tsdev_lib_test_hooks.h` / `src/tsdev_lib.h` / `src/tsdev_lib.cpp`

- Added `onSaveModuleNewReady` field to `TsDevLibTestConfig` (in `src/tsdev_lib_test_hooks.h`).
- Added `m_onSaveModuleNewReady` member to the library class (in `src/tsdev_lib.h`).
- Hook invocation in `saveSource()` immediately before the `_SaveModule_New` call (in `src/tsdev_lib.cpp`); `nullptr` in production.

The hook is **test-only** (`nullptr` in production), allowing unit tests to inspect the flags word and password buffer immediately before the `_SaveModule_New` call. Production never sets it.

### Test — `tests/test_save_module.cpp`

30+ assertions across all 5 component types (Strategy, Indicator, Function, ShowMe, TradingApp) plus a modify case and a batch mode. The tests assert:

- the exact **flags value** per type and per insert/modify path (per the table in R-M.2.1),
- the **password buffer content** (zeroed tail beyond the 10-byte password, correct password bytes),
- `saveSource()` success,
- `verifyModule()` success,
- `getSource()` round-trip (encrypted save → decrypt on read returns the original source).

### Files changed

- `src/tsdev_lib.cpp`
- `src/tsdev_lib.h`
- `src/tsdev_lib_test_hooks.h`
- `tests/test_save_module.cpp`

No signature, catalog, resolver, or sigcodegen changes (see §5).

---

## 4. Test and E2E results

| Suite | Result |
|---|---|
| `test_save_module` | All assertions PASS (30+ assertions, 5 component types + modify + batch) |
| Full regression | 13/13 suites PASS |

Full regression coverage:

- 33 unit tests
- 34 E2E CLI cases
- 253 unknown-link assertions
- all 5 component types (Strategy, Indicator, Function, TradingApp, ShowMe)
- batch / stdin / manifest coverage
- serve E2E
- ShowMe

The `test_save_module` RED→GREEN transition is the key TDD evidence: with the pre-M.2 `NULL`/`NULL` arguments for arg3/arg5, the suite produced **15 failures** on flags/password assertions; after the M.2 production change (non-NULL `&saveFlags` and non-NULL `savePasswordBuf`), the suite passes fully.

---

## 5. Signature / catalog impact

**None.** M.2 changes the **arguments** passed to `_SaveModule_New`, not export resolution. The `_SaveModule_New` export is already resolved by name (ordinal 634, RVA `0x00159A70`) — no new signatures, no new exports, no new patch sites, no sigcodegen changes. `docs/SIGNATURE_CATALOG.md` is unchanged by M.2.

---

## 6. Residual limitations

- **Bit 3 (`has_expiry`) is always 0 in the CLI.** TSDev sets bit 3 if `CELProperties::GetDate() != 0`. The CLI does not support expiration dates, so bit 3 is always 0. This is acceptable and documented.
- **CELProperties is not accessible from the standalone CLI.** This is documented as a non-blocker for the save path (R-M.2.2): the save path only needs the editor HWND (arg4), which the CLI already has. CELProperties is only used in the verify path, already handled by M.1.
- **TSDev.exe caller `FUN_00452290` was identified** and its flags/password construction fully analyzed. No remaining unknowns in the save-side flags/password construction.

---

## 7. Follow-up work for M.3 / M.4 / M.5

- **M.3:** Post-success `0x40A` library-change notification.
- **M.4:** Reverse-dependent revalidation.
- **M.5:** Consolidated verification and documentation closure.

M.2 established the save-side flags/password parity foundation (R-M.2.1 through R-M.2.5) that M.3–M.5 build on. The residual limitations in §6 (bit 3 always 0; CELProperties not accessible) should be revisited as appropriate in those slices.

---

## 7a. Adversarial review

M.2 underwent two adversarial review iterations:

- **Iteration 1 — DISAPPROVED** with 3 blocking findings: (1) modify-operation flags parity gap (bit 4 `is_verified` never set for modify path); (2) operation-mode coverage requirement not met (no batch/serve mode flag assertions); (3) no M.2 handoff note. Plus 4 minor notes: regression log not captured, substring matching instead of byte-for-byte, only 3 of 5 component types tested, PM_WORKING_MEMORY not updated.
- **Fixes applied:** Added `isVerified` parameter to `saveSource` (bit 4 set for modify path); added modify test case (Case 4, asserts flags=0x70); added batch-reuse test case (Case 7, 3 iterations with per-iteration flag assertions); added ShowMe (Case 5) and TradingApp (Case 6) cases; added length-tolerance check to round-trip assertions; created `docs/M2_HANDOFF.md`; captured regression log to `tmp/m2_regression_20260825.log`; updated `docs/PM_WORKING_MEMORY.md` with M.2 completion entry.
- **Iteration 2 — APPROVED.** All 3 blocking findings resolved, all 4 minor notes addressed, all M.2 acceptance criteria met, 13/13 regression suites pass, no new issues introduced. 3 non-blocking minor notes remain (documented above in §6 and in the review record).

---

## 8. Evidence index

| Artifact | Section | Content |
|---|---|---|
| `docs/RE/RE_M2_SAVEMODULE_NEW.md` | R-M.2.1 | Flags word (arg3) construction in `FUN_00452290` |
| `docs/RE/RE_M2_SAVEMODULE_NEW.md` | R-M.2.2 | CELProperties is not a save-path dependency |
| `docs/RE/RE_M2_SAVEMODULE_NEW.md` | R-M.2.3 | Save-side encryption trigger and password buffer |
| `docs/RE/RE_M2_SAVEMODULE_NEW.md` | R-M.2.4 | Pre-verify save only; no post-verify save |
| `docs/RE/RE_M2_SAVEMODULE_NEW.md` | R-M.2.5 | No user-facing password |
| `docs/M1_HANDOFF.md` | — | M.1 foundation (compiler-object lifecycle, `+0x18` HWND, category/subtype source) that M.2 builds on |
| `docs/workstream_tsdev_parity_plan.md` | — | Workstream M plan (M.1–M.5) |
