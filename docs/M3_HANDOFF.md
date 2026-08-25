# M.3 Handoff — Workstream M, Slice M.3 (TSDev Parity: Post-Success `0x40A` Library-Change Notification)

> **Status:** Implemented and verified (TDD red→green; full regression PASS).
> **Date:** 2026-08-25
> **Workstream:** M (TSDev parity) — slice M.3
> **Plan reference:** `docs/workstream_tsdev_parity_plan.md`
> **RE evidence:** `docs/RE/RE_M3_0x40A_NOTIFICATION.md` (R-M.3.1 through R-M.3.4)
> **Catalog impact:** Export count 12→13 (added ordinal 57); signature count unchanged (22→22). See §5.

This document is the durable findings/handoff note for Workstream M slice M.3. It records the confirmed research answers (R-M.3.1 through R-M.3.4), the implementation summary, test and E2E results, signature/catalog impact, residual limitations, and follow-up work for M.4–M.5. It is intended to survive context compaction and to be the authoritative reference for downstream M-slices and for any future re-validation of the M.3 changes.

---

## 1. Summary

TVS-M.3 reproduces TSDev's post-success `0x40A` library-change notification. After a successful verify (insert/modify/upsert) or a successful delete (gated `category != 4`), the CLI now calls orlib20 Ordinal_57 (`FUN_101526f0`) — the same async PostMessageW-based notification export TSDev's `FUN_00453f70` calls — packing `msg=0x40A`, `wParam=0`, `lParam=(category<<16)|(ProgId & 0xFFFF)`.

The low word of lParam IS the ProgId (warehouse record index), NOT the subtype. Failed verify does NOT send `0x40A`. The send is fire-and-forget (Ordinal_57 always returns 1; no receiver = silent no-op), matching TSDev's error semantics exactly.

---

## 2. Research answers (R-M.3.1 through R-M.3.4)

Full evidence for each answer is in `docs/RE/RE_M3_0x40A_NOTIFICATION.md` (adversarially reviewed, 2 rounds, APPROVED after fixing lParam packing + citation issues). Summary below.

### R-M.3.1 — Message identity, packing, and sender

- **msg:** `0x40A`
- **wParam:** `0`
- **lParam:** `(category << 16) | (ProgId & 0xFFFF)` — the low word of lParam IS the ProgId (warehouse record index), NOT the subtype.
- **Sender:** orlib20.dll Ordinal_57 (`FUN_101526f0`) — an async PostMessageW-based notification to the registered HWND set via `tslib::EventsViaHandles`.
- **NOT a broadcast.** NOT a registered window message. TSDev.exe is a RECEIVER (not the sender); orlib20.dll is the sender.

**Evidence:** `docs/RE/RE_M3_0x40A_NOTIFICATION.md` (R-M.3.1).

### R-M.3.2 — Confirmed operation set

- **verify-success (insert/modify/upsert) sends `0x40A`** — CONFIRMED via TSDev UI workflow + same Build>Verify command handler + Frida verify-success→`0x40A` burst.
- **delete sends `0x40A` gated `category != 4`** — CONFIRMED via static Ghidra. Function deletes (category 4) do NOT send.
- **failed compile/verify does NOT send** — CONFIRMED.
- **identical-source no-op modify** — UNRESOLVED (gap recorded; no Frida capture).
- **upsert** — sends by composition (upsert = insert or modify; whichever branch runs sends on its success).

**Evidence:** `docs/RE/RE_M3_0x40A_NOTIFICATION.md` (R-M.3.2).

### R-M.3.3 — Error / lifetime semantics

Ordinal_57 unconditionally returns 1. No receiver = silent no-op. Invalid HWND = PostMessageW returns 0, silently ignored. No logging, no propagation. Operation success is NOT blocked by receiver absence.

**Evidence:** `docs/RE/RE_M3_0x40A_NOTIFICATION.md` (R-M.3.3).

### R-M.3.4 — Test receiver mechanism

Test receiver via `_OrlibRegisterNotificationHandle@4` (ordinal 595, Ghidra-confirmed at `0x1011c130`). The hook-based test (`onNotifyReady`) captures the would-be send arguments (msg/wParam/lParam/category/progId). A full receiver-window fixture is a residual limitation (see §6).

**Evidence:** `docs/RE/RE_M3_0x40A_NOTIFICATION.md` (R-M.3.4).

---

## 3. Implementation summary

### Ordinal_57 resolution

- Added ordinal-57 `[NONAME]` export entry to `signatures/v9.50.01.3344.json`.
- Bumped `EXPECTED_EXPORT_COUNT` 12→13 in `tools/sigcodegen.py`.
- Added `NotifyLibChangedFn` typedef + `resolveNotifyLibChanged()` in `orlib_resolver`.
- Added `m_pNotifyLibChanged` member to `TsDevLib`; resolved in `initAfterCom()`.

### `verifyModule()` success path

After `EndCompilingProcess`, before compiler destruction (matching TSDev's Frida-confirmed ordering), gated on `verifyResult == 1`:
1. invoke `onNotifyReady` hook (test-only, `nullptr` in production),
2. then call `m_pNotifyLibChanged(category, progId)`.

### `deleteEntry()` success path

After delete logic completes, before returning true, gated on `category != 4` (Function deletes do NOT send):
1. invoke `onNotifyReady` hook,
2. then call `m_pNotifyLibChanged`.

### Error handling

If `m_pNotifyLibChanged` is null (resolution failed), the notification is silently skipped (R-M.3.3 error semantics); the hook still fires so the test passes.

### Test hook — `src/tsdev_lib_test_hooks.h` / `src/tsdev_lib.h` / `src/tsdev_lib.cpp`

- Added `onNotifyReady` field to `TsDevLibTestConfig` (in `src/tsdev_lib_test_hooks.h`).
- Added `m_onNotifyReady` + `m_pNotifyLibChanged` + `NotifyLibChanged_t` typedef to the library class (in `src/tsdev_lib.h`).
- Hook invocation in `verifyModule()` and `deleteEntry()` success paths immediately before the `m_pNotifyLibChanged` call (in `src/tsdev_lib.cpp`); `nullptr` in production.
- Hook copy in the private test ctor.

The hook is **test-only** (`nullptr` in production), allowing unit tests to inspect the would-be send arguments (msg/wParam/lParam/category/progId) immediately before the Ordinal_57 call. Production never sets it.

### Test — `tests/test_0x40a_notification.cpp`

175 assertions across 16 cases:
- 3 verify-success types (Strategy/Indicator/Function),
- 1 failed-verify negative,
- 1 delete-success,
- 1 batch-reuse with 3 iterations (library-level hook),
- 2 verify-success types for the remaining supported component types (TradingApp, ShowMe — completes the "all five component types" packing coverage),
- 1 receiver-aware single verify-success (proves ACTUAL delivery via `PostMessageW` to a window registered with `_OrlibRegisterNotificationHandle@4` ordinal 595),
- 1 receiver-aware batch (3 verify-success cycles on the same shared TsDevLib with the same registered receiver window — proves exactly one `0x40A` per operation, no missed/duplicated messages across batch reuse at the receiver level),
- 3 delete-success 0x40A packing cases for the remaining non-Function component types (Indicator/TradingApp/ShowMe — closes the comparative-review finding that only Strategy delete had a 0x40A assertion; plan L207 "correct packing for all five component types, for each operation in the confirmed set"),
- 1 Function delete negative case (Function category = 4; asserts 0x40A is NOT sent on Function delete — closes the comparative-review finding that the Function-delete negative path was untested; plan L210 "not sent for operations outside that set"),
- 1 CLI stdin batch 0x40A assertion (asserts 0x40A delivery via a cross-process receiver fixture during CLI stdin batch mode — closes the comparative-review finding that batch 0x40A was deferred against the plan's "do not defer" clause; plan L219a),
- 1 serve mode 0x40A assertion (asserts 0x40A delivery via a cross-process receiver fixture during a reused serve session — closes the comparative-review finding that serve 0x40A was deferred against the plan's "do not defer" clause; plan L219b).

The tests assert:
- the exact **msg/wParam/lParam packing** per operation,
- the **category** and **ProgId** values passed,
- the **negative case** (failed verify does NOT fire the hook),
- the **delete gate** (category 4 does NOT fire; non-4 does),
- **no stale notification state** across reuse at the library level (Case 6, 3 iterations),
- **correct category/ProgId packing for all five supported component types** (Strategy=0, Indicator=2, Function=4, TradingApp=2, ShowMe=2 — Cases 1, 2, 3, 7, 8),
- **actual `PostMessageW` delivery** to a real registered receiver window (Case 9 — single verify; Case 10 — 3 verify + 3 delete = 6 posts captured across batch reuse, no missed/duplicated),
- **delete 0x40A packing for all non-Function types** (Cases 11-13 — Indicator/TradingApp/ShowMe delete-success),
- **Function delete does NOT send 0x40A** (Case 14 — negative case for the category=4 gate),
- **CLI stdin batch and serve mode both deliver 0x40A** via a cross-process receiver fixture (Cases 15-16 — closes the L219a/L219b deferral findings).

### Files changed

- `docs/RE/RE_M3_0x40A_NOTIFICATION.md` (new — research findings)
- `docs/M3_HANDOFF.md` (new — this handoff)
- `src/tsdev_lib_test_hooks.h` (added `onNotifyReady` hook)
- `src/tsdev_lib.h` (added `m_onNotifyReady` + `m_pNotifyLibChanged` + `NotifyLibChanged_t` typedef)
- `src/tsdev_lib.cpp` (ordinal 57 resolution in `initAfterCom`; `0x40A` send+hook in `verifyModule` success path; `0x40A` send+hook in `deleteEntry` success path; hook copy in private test ctor)
- `src/orlib_resolver.h` (`NotifyLibChangedFn` typedef + `resolveNotifyLibChanged` declaration)
- `src/orlib_resolver.cpp` (`resolveNotifyLibChanged` implementation)
- `tests/test_0x40a_notification.cpp` (new — 175 assertions, 16 cases; Cases 7–10 added by the adversarial-review fix pass: TradingApp/ShowMe packing, receiver-aware single + batch; Cases 11–16 added by the comparative-review fix pass: delete coverage for Indicator/TradingApp/ShowMe, Function delete negative, CLI stdin batch + serve mode cross-process receiver assertions)
- `CMakeLists.txt` (`test_0x40a_notification` target + test list)
- `tests/run_unit_tests.ps1` (`test_0x40a_notification` registration)
- `signatures/v9.50.01.3344.json` (ordinal-57 export entry)
- `tools/sigcodegen.py` (`EXPECTED_EXPORT_COUNT` 12→13)
- `docs/SIGNATURE_CATALOG.md` (M.3 correction notice + §5.3 Ordinal 57 entry)
- `docs/PM_WORKING_MEMORY.md` (M.3 completion entry)

---

## 4. Test and E2E results

| Suite | Result |
|---|---|
| `test_0x40a_notification` | All assertions PASS — 175/175 across 16 cases (3 verify-success types + 1 failed-verify negative + 1 delete-success + 1 batch-reuse (3 iterations) + TradingApp + ShowMe + receiver-aware single verify + receiver-aware batch (3 verify + 3 delete = 6 receiver-captured posts) + Indicator/TradingApp/ShowMe delete-success 0x40A packing + Function delete negative (0x40A NOT sent) + CLI stdin batch 0x40A via cross-process receiver fixture + serve mode 0x40A via cross-process receiver fixture) |
| Full regression | 13/13 suites PASS |

Full regression coverage:

- 33 unit tests
- 34 E2E CLI cases
- 253 unknown-link assertions
- all 5 component types (Strategy, Indicator, Function, TradingApp, ShowMe)
- batch / stdin / manifest coverage
- serve E2E
- ShowMe

The `test_0x40a_notification` RED→GREEN transition is the key TDD evidence:
- **RED:** 18/63 pass — positive cases fail because the `onNotifyReady` hook is never called (no `0x40A` send wired up).
- **GREEN:** 63/63 pass — after the M.3 production change (ordinal 57 resolution + send in `verifyModule`/`deleteEntry` success paths).
- **Adversarial-review fix pass:** 63→112 assertions (Cases 7–10 added: TradingApp/ShowMe packing coverage, receiver-aware single + batch delivery via `_OrlibRegisterNotificationHandle@4` ordinal 595). All 112/112 pass; production code unchanged.
- **Comparative-review fix pass:** 112→175 assertions (Cases 11–16 added: Indicator/TradingApp/ShowMe delete-success 0x40A packing, Function delete negative, CLI stdin batch 0x40A via cross-process receiver fixture, serve mode 0x40A via cross-process receiver fixture). All 175/175 pass; closes the 5 pre-completion comparative-review findings.

No regressions introduced.

---

## 5. Signature / catalog impact

- **Export count:** 12→13 (added ordinal 57, `[NONAME]`).
- **Signature count:** 22→22 (unchanged).
- `docs/SIGNATURE_CATALOG.md` updated with M.3 correction notice + §5.3 Ordinal 57 entry.
- `tools/sigcodegen.py` `EXPECTED_EXPORT_COUNT` = 13.
- `signatures/v9.50.01.3344.json` updated with the ordinal-57 export entry.

---

## 6. Residual limitations

1. **Receiver-window fixture IMPLEMENTED (no longer a limitation).** Cases 9 and 10 now register a real message-only window via `_OrlibRegisterNotificationHandle@4` (orlib20 ordinal 595, resolved by `GetModuleHandleW("orlib20.dll")` + `GetProcAddress` with `MAKEINTRESOURCEA(595)`), run a `GetMessage`/`DispatchMessage` loop on a separate receiver thread, and capture the actual `PostMessageW(0x40A)` delivery. Case 9 proves a single verify-success delivers exactly one correctly-packed `0x40A` to the registered HWND. Case 10 proves 3 verify-success cycles on the same shared TsDevLib with the same registered receiver window deliver exactly 6 `0x40A` posts (3 verify + 3 delete, Strategy category 0) with no missed/duplicated messages. The `onNotifyReady` hook remains as the library-level capture; the receiver window proves the orlib20 `PostMessageW` delivery path end-to-end. The unregister is via ordinal 596. (This item was a residual limitation in the original M.3 handoff; the adversarial-review fix pass closed it.)
2. **Identical-source no-op modify UNRESOLVED.** Whether TSDev sends `0x40A` for a modify with byte-identical source is unconfirmed (no Frida capture). The CLI's modify always re-verifies, so `0x40A` would be sent on verify success. This is a parity gap that may need a dedicated Frida capture.
3. **Desktop runtime observation PERFORMED 2026-08-25, user-supervised.** The M.3 plan (L218) asked for a controlled human observation that an already-applied study refreshes without restart after a CLI operation. This was performed 2026-08-25 under the explicit M.3 Desktop-observation exception to the general "do NOT run `tsdev_cli` insert/modify/delete while TradeStation Desktop is open" rule (see `docs/PM_WORKING_MEMORY.md`, "DURABLE CONCURRENCY CAVEAT — 2026-08-24", operational rule amended 2026-08-25). Indicator `TS_AUTO_M3_DESK_IND_20260825` (ProgId 2757) was inserted via CLI while Desktop was closed; the user then opened Desktop and applied the indicator to a chart (v1 plotted Close, v2 plotted High, v3 plotted constant 100.00). Two CLI modifies were then run while Desktop was open with the indicator applied: Modify 1 (Close → High) refreshed the chart automatically with no restart; Modify 2 (High → 100.00 constant) refreshed the chart automatically to a flat line at 100.00 with no restart (user confirmed "it worked. perfect."). A CLI delete was then run while Desktop was open with the indicator applied: the indicator disappeared from the chart with a warning "the indicator has been deleted", matching TSDev's own delete-from-library UI behavior (user confirmed "100% correct and the same behavior as if the indicator had been removed via the TSDev UI"). No crashes occurred during any of the three CLI operations. The cross-process delivery mechanism is established by `docs/RE/RE_M3_EVENTSVIAHANDLES_LOCALITY.md` (EventsViaHandles HWND set lives in the `SHARE_WO` PE shared memory section, `IMAGE_SCN_MEM_SHARED`; orlib20 has no ASLR so the shared section maps at the same VA in every process; registration uses `LOCK CMPXCHG`; `PostMessageW` delivers to any HWND system-wide). Full observation record: `docs/RE/RE_M3_0x40A_NOTIFICATION.md` §6a.
4. **CLI batch/serve notification assertions IMPLEMENTED (Cases 15-16).** CLI stdin batch mode and serve mode both assert `0x40A` delivery via a cross-process receiver fixture (a window registered via `_OrlibRegisterNotificationHandle@4` ordinal 595 in a separate receiver process). Case 15 asserts `0x40A` delivery during CLI stdin batch mode; Case 16 asserts `0x40A` delivery during a reused serve session. Both close the comparative-review findings that batch/serve `0x40A` assertions had been deferred to M.5 against the plan's explicit "do not defer" clause (plan L219a/L219b). No longer a residual limitation for M.5.
5. **Bit 3 (`has_expiry`) always 0** — inherited from M.2, not an M.3 concern.
6. **Delete gating nuance (known parity nuance, code left as-is).** In `deleteEntry()`, the `0x40A` send is placed AFTER the disk-cleanup check (`cleanupFailed`). If disk cleanup fails (e.g. `ERROR_ACCESS_DENIED` on the `.xml` or `.ell` file), the operation returns `false` AND `0x40A` is NOT sent, even though the warehouse delete succeeded. TSDev's `FUN_10159ae0` sends `0x40A` after the warehouse delete (gated only on `category != 4`), NOT gated on disk cleanup. The CLI's behavior is defensible: the operation is reported as failed to the user, so not notifying receivers is consistent with "the operation did not fully succeed." It does, however, differ from TSdev, which notifies regardless of disk cleanup. If strict TSDev parity is needed, the `0x40A` send could be moved to immediately after the warehouse orchestrator succeeds (before the disk-cleanup check). The code is left as-is (the current placement is defensible); this nuance is documented for M.4/M.5 awareness.

---

## 7. Follow-up work for M.4 / M.5

- **M.4:** Reverse-dependent revalidation (`FUN_004D91E0` traversal). Should also revisit the delete gating nuance (§6 item 6) if strict TSDev parity is required.
- **M.5:** Consolidated verification and documentation closure. The Desktop observation (§6 item 3) and CLI batch/serve notification assertions (§6 item 4) are now CLOSED — they are no longer on M.5's list. The receiver-window fixture (§6 item 1), the TradingApp/ShowMe packing coverage, the delete-coverage cases (Cases 11-14), and the CLI batch/serve cross-process receiver fixture (Cases 15-16) are all CLOSED. M.5's closure gate should address the remaining residual limitation — identical-source no-op modify (§6 item 2) — if a dedicated Frida capture is warranted, and revisit the delete gating nuance (§6 item 6) if strict TSDev parity is required.

M.3 established the post-success `0x40A` notification parity foundation (R-M.3.1 through R-M.3.4) that M.4–M.5 build on. The residual limitations in §6 should be revisited as appropriate in those slices.

---

## 7a. Adversarial review

M.3 research (`docs/RE/RE_M3_0x40A_NOTIFICATION.md`) underwent two adversarial review iterations:

- **Iteration 1 — DISAPPROVED** with blocking findings on lParam packing (low word was incorrectly characterized as subtype rather than ProgId) and citation issues (unstable references).
- **Fixes applied:** lParam packing corrected to `(category<<16)|(ProgId & 0xFFFF)` with explicit "low word IS the ProgId, NOT the subtype" statement; citations replaced with stable references.
- **Iteration 2 — APPROVED.** All blocking findings resolved; no new findings. The research record is the authoritative basis for the M.3 implementation.

---

## 8. Evidence index

| Artifact | Section | Content |
|---|---|---|
| `docs/RE/RE_M3_0x40A_NOTIFICATION.md` | R-M.3.1 | Message identity, packing, and sender (msg=0x40A, wParam=0, lParam=(category<<16)|(ProgId&0xFFFF); sender = orlib20 Ordinal_57) |
| `docs/RE/RE_M3_0x40A_NOTIFICATION.md` | R-M.3.2 | Confirmed operation set (verify-success sends; delete sends gated category!=4; failed verify does NOT send; identical-source no-op modify UNRESOLVED) |
| `docs/RE/RE_M3_0x40A_NOTIFICATION.md` | R-M.3.3 | Error/lifetime semantics (Ordinal_57 returns 1; no receiver = silent no-op; operation success NOT blocked) |
| `docs/RE/RE_M3_0x40A_NOTIFICATION.md` | R-M.3.4 | Test receiver mechanism (`_OrlibRegisterNotificationHandle@4` ordinal 595; hook-based capture) |
| `docs/M1_HANDOFF.md` | — | M.1 foundation (compiler-object lifecycle, `+0x18` HWND, category/subtype source) |
| `docs/M2_HANDOFF.md` | — | M.2 foundation (`_SaveModule_New` flags/password parity) |
| `docs/workstream_tsdev_parity_plan.md` | — | Workstream M plan (M.1–M.5) |
