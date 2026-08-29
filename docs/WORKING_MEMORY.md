# Working Memory — TSDev Automation

Live, real-time state for the PM. Keep minimal; drop stale/superseded facts.
Do not restate what the code itself documents.

## Current objective

**TS Desktop interaction acceptance.** The six operations (Insert/Delete/Modify/
Exists/Get-source/Upsert) are implemented in C++ CLI + Python `TsDevClient` with
batch (manifest + stdin) support. The user has clarified that production
acceptance REQUIRES proper interaction with a running TS Desktop, matching
TSDev.exe:
- (a) all operations work WHILE TS Desktop is open
- (b) inserted items immediately available, usable, and FUNCTION when applied
- (c) modifications immediately picked up and applied to items already on
  charts, or already opened within TS Desktop as a tradingapp
- (d) deletions handled like TSDev: TS Desktop detects applied items, removes
  them, shows a "deleted" warning

The `dependents` workstream (M) is paused; it is beyond this acceptance target.

## Current state (not derivable from code)

- TradeStation 9.5 installed: `C:\Program Files (x86)\TradeStation 9.5\Program\
  TSDev.exe` and `orlib20.dll` (v9.50.01.3344). TS Desktop process is
  `orchart.exe` (imports `_OrlibRegisterNotificationHandle@4`).
- `tsdev_cli.exe` built (32-bit Win32 Release) at `build\Release\tsdev_cli.exe`
  and `dist\tsdev_cli.exe`. NOTE: `cmake --build` exit code is non-zero due to a
  POST_BUILD packaging step missing `docs/BUNDLE_README.md`; the exe itself
  builds and runs fine. Repo-content issue, not toolchain.
- TS Desktop cross-process notification mechanism: see KB §19 (SSOT). Summary:
  the CLI's per-process `NotifyLibChanged` 0x40A reaches no windows and is NOT
  the cross-process path; the real mechanism is shared warehouse memory
  (SHARE_WO) + WHServer.exe COM `_IWHouseEvents`, already exercised by orlib20's
  COM vtable calls during CLI operations.

## TVS just completed (2026-08-29): Fix Function category 4→1 (read-only → editable)

`ComponentType::Function` was `4` (the "Function (Read Only)" warehouse category
for built-in/system functions). TSDev.exe uses category 1 for user-created
editable functions (`CELFunctionDoc` @ `0x00457eb0` sets cat=1; `CELBIFunctionDoc`
@ `0x004140c0` sets cat=4). orlib20 `GetFileTypeName1` @ `0x10157dc0`: cat 1 →
"Function", cat 4 → "Function (Read Only)". Categories 1 and 4 are separate
warehouse search trees (`FUN_101d43a0` @ `0x101d43a0`).

**Fix:** `ComponentType::Function` changed from 4 to 1 in `src/tsdev_lib.h`.
Comments/tests updated. `category != 4` delete gate left as-is (correct: after
fix, cat-1 function deletes DO send 0x40A, matching orlib20's `FUN_10159ae0`).
E2E gate passed. Unit tests pass (17/17 non-TS, test_save_module 81/81,
test_0x40a_notification 183/183). Adversarially reviewed and APPROVED. KB §20
updated with confirmed root cause. User confirmed function is now editable in
TSDev.exe.

**Probe items currently live (category 1):**
- Function `TsDeskFnProbe_20260829` — ProgId 84, `elmodule_1_84.ell`
- Indicator `TsDeskFnProbeCaller_20260829` — ProgId 3008, `elmodule_2_3008.ell`
- Caller indicator is applied to a chart in TS Desktop (user-confirmed).

## TVS just completed (2026-08-29): Runtime Frida confirmation of recalc signal

Run A (TSDev UI edit+save) and Run B (CLI modify) traced with Frida attached to
TSDev.exe / WHServer.exe / all orchart.exe. Results in KB §22 (SSOT). Summary:

- TSDev.exe posts 19 `PostMessageW(0x40A, wParam=0, lParam=(cat<<16|progId))`
  inside Ordinal_57. HWND ownership verified via `GetWindowThreadProcessId`:
  16 orchart HWNDs, 2 TSELAppHost, 1 TSScanner, ZERO TSDev.exe. The 0x40A is
  cross-process to other TradeStation processes, not to TSDev's own views.
- CLI calls Ordinal_57 but its orlib20 subscriber list is empty (per-process
  `.data` section) → 0 0x40A posts → no immediate recalc.
- TSDev also runs the post-verify block (ORD71 ~87 calls, ORD68, worker cmd
  0x0D) + ORD25/ORD56/ORD17, generating ~327 extra WHServer COM events absent
  from the CLI run. These may be an additional/alternative trigger.
- KB §19 "per-process only" refined: the subscriber LIST is per-process, but
  the 0x40A messages go cross-process. How orchart/TSELAppHost/TSScanner HWNDs
  get registered in TSDev's orlib20 subscriber list is an OPEN question.
- Adversarial review of the analysis DISAPPROVED on first pass (factual errors
  in event counts, unverified HWND ownership, ignored alternative hypothesis).
  All findings corrected in KB §22; re-review APPROVED.

## Next step (one TVS ahead)

**Implementation: cross-process 0x40A notification for CLI function-modify.**
Two candidate approaches (KB §22.6):
- **Approach A (preferred):** discover TradeStation process HWNDs that subscribe
  to library-change notifications and `PostMessageW(0x40A, 0, (cat<<16|progId))`
  directly. Open question: how does TSDev populate its subscriber list? Needs
  a focused RE TVS (likely `_OrlibRegisterNotificationHandle@4` or a shared-
  memory HWND registry).
- **Approach B (fallback):** replicate the full post-verify block (ORD10 +
  FUN_004D7140 → ORD71 + worker cmd 0x0D + ORD68). More complex; requires
  worker-thread infrastructure that may not exist in the CLI process.

A discriminating test (block 0x40A but keep the post-verify block, or vice
versa) is still needed to determine whether 0x40A alone, the extra WHServer COM
events alone, or both jointly, are the actual trigger.

**Before implementation:** re-run adversarial review on the corrected KB §22.
Then run a focused RE TVS on `_OrlibRegisterNotificationHandle@4` to understand
the subscriber-list registration mechanism (needed for Approach A).

## Key references

- Function-modify recalc signal (static findings, SSOT): `docs/KNOWLEDGE_BASE.md`
  §21 — candidate calls, TSDev.exe `FUN_004D91E0` category-gated post-verify
  block, worker cmd 0x0D, ordinals 10/68/71, recommended Frida trace (§21.10).
- Read-only function root cause (CONFIRMED): KB §20.
- CLI modify path: `src/tsdev_lib.cpp` `verifyModule` (post-success
  NotifyLibChanged block) and `modifyEntry`; `traverseReverseDependents` (local
  re-verify, NOT equivalent to TSDev's worker-cmd-0x0D path).
- orlib20 ordinal resolvers: `src/orlib_resolver.cpp` (CLI resolves
  28/57/66/67/69/70/85; does NOT resolve 10/17/25/56/68/71).
- Cross-process notification mechanism (SSOT): KB §19. Dependents/callers: §18.
- Frida trace scripts: `tmp/frida_recalc_trace.js`,
  `tmp/frida_recalc_trace_runner.ps1`, `tmp/frida_recalc_trace_driver.py`,
  `tmp/frida_recalc_trace_README.md`.
- Probe sources: `tmp/probe_modify_function_v1.el` (=`1`),
  `tmp/probe_modify_function_v2.el` (=`2`),
  `tmp/probe_modify_function_caller.el`.
