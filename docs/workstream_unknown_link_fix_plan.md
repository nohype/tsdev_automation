# Workstream L — Runtime-Link Root-Cause Fix

**Status:** NOT YET CLOSED — Workstream L recovery L.6–L.15 is implemented on branch `workstream-L-recovery`; L.1–L.3 remain COMPLETE; L.4/L.5 completion was FALSIFIED by persistence and deletion fault injection and is superseded by L.6–L.15 (L.8 BLOCKED — no invented TSDev recovery; L.9 detect-and-fail stale-ELL modify/upsert; L.12 detect-and-report orphan-ELL delete; existence checks are necessary but not sufficient; the `FUN_10011230` persistence bypass is removed). No production handoff is authorized until L.17 independent final verification passes (see `docs/PM_WORKING_MEMORY.md`). Workstream M remains BLOCKED until L.17.
**Review status:** Independent review loops APPROVED; final hands-on adversarial PM review APPROVED on 2026-08-23 after correcting RED-suite gating, cross-references, behavioral test design, rollback policy, runtime-scope wording, and causal E2E timing. (Plan-level review approval is distinct from the L.4/L.5 completion-claim falsification, which reopened the workstream after the fact.)

> **Workstream L is NOT YET CLOSED — see docs/PM_WORKING_MEMORY.md. Recovery L.6–L.15 is implemented on branch `workstream-L-recovery`; L.4/L.5 completion was FALSIFIED and is superseded by L.6–L.15. No new Desktop runtime re-gate is claimed without an L.17 result.**

**Priority:** P0 / blocker. The CLI currently produces runtime-unlinkable
strategies that fail in TradeStation Desktop with "Unknown link error
occurred." This invalidates the core product until fixed.
**Profile:** `universal` (planning/documentation only for this task; no code
changes, no new reverse engineering).
**Source of truth:**
- `docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md` (approved research plan, adversarial
  review Round 2 APPROVED; durable promotion of the former
  `tmp/unknown_link_root_cause_and_plan.md`)
- `docs/RE/RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md` (durable static evidence
  appendix; durable promotion of the former
  `tmp/unknown_link_ghidra_evidence.md`)
- `docs/PM_WORKING_MEMORY.md` top section (in-flight root-cause state)
- `docs/workstream_serve_plan.md` (plan style precedent)
- `AGENTS.md` hard rules and mandatory E2E process

> **Durable-evidence note:** The two `tmp/` research artifacts above have been
> promoted to tracked durable storage under `docs/RE/`
> (`RE_UNKNOWN_LINK_ROOT_CAUSE.md` and `RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md`),
> satisfying Workstream M's durable-evidence prerequisite gate
> (`docs/workstream_tsdev_parity_plan.md` §"Durable-evidence prerequisite").
> This promotion does not weaken Workstream M's hard dependency on Workstream L
> being approved and merged; it only satisfies the documentation-closure
> evidence gate. Raw experiment artifacts under `tmp/` (Frida traces, probe
> stdout, ELL/XML/GUID analysis files) remain transient/reproducible evidence
> and are cited as such from the durable documents.

---

## 1. Problem statement, root cause, goals, non-goals, dependencies

### 1.1 Problem statement
Strategies inserted/modified/upserted by the production `tsdev_cli` appear
**VERIFIED in TSDev** but fail at runtime in TradeStation Desktop with
"Unknown link error occurred." A full Desktop restart does not repair a broken
CLI Strategy, so the failure is durable, not a live-cache artifact. The same
common compile/finalization path is used by the other supported component
types, so they are in the verification scope, but equivalent runtime failure
has not been claimed without type-specific evidence. The user workaround
(open the Strategy in TSDev, make a harmless edit, re-Verify) re-runs the
original finalization path and rewrites the ELL/XML pair, after which Desktop
links correctly. This is unacceptable for an automated tool.

### 1.2 Confirmed root cause
The CLI patches `orlib20!FUN_10011230` (RVA `0x11230`, preferred VA
`0x10011230`) to `B8 01 00 00 00 C2 0C 00` (`MOV EAX,1; RET 0x0C`), bypassing
the original function while forcing success. Durable Ghidra evidence proves the
original function's ordered work includes a call to `FUN_1008E640`
(conservatively characterized as compiler/link-finalization behavior, NOT as
the writer of any identified RNAM section), `SymInfo::AdjustOffsets`, and
`SymWriter::Save`. The patch skips all of that and forces `EAX=1`, so
`CompileText` proceeds to `ModuleInfo::SetGuid` / `MDVault::SaveModule` and
marks the item verified despite an incomplete finalization.

A controlled dynamic A/B proves the combined fix:
- **A (current patched):** CLI insert produces only `elmodule_0_<ProgId>.ell`;
  Desktop cannot link it.
- **B (STA COM + restored original function):** a temporary probe initialized
  STA COM on the actual CLI worker, restored the original `FUN_10011230` bytes
  in memory, inserted ProgId 6689, and produced a paired ELL/XML with matching
  embedded version GUID. The user confirmed the strategy ran in Desktop
  **without** opening or re-verifying it in TSDev.

The patch existed because the unpatched `SymWriter::Save` path uses ECL's
MSXML COM wrapper, and the CLI's 64 MiB worker threads never initialize COM.
TSDev's UI/main initialization thread statically calls `CoInitialize(NULL)`
before `_OrlibInit(NULL,1)` on the same synchronous path; the CLI omits this
prerequisite, so the unpatched standalone path raised native C++ exception
`0xE06D7363`. The old "partially initialized CLR" explanation was unproven and
wrong; the real prerequisite is STA COM on the orlib-owning thread.

The replacement is also ABI-invalid: the original callee ends in plain `RET`
(`0x10011454`) and the caller executes `ADD ESP,0x0C` (`0x1001E0C1`), so the
original ABI is fastcall-like register passing with **caller cleanup** for the
12 stack bytes. The patch's `RET 0x0C` changes that cleanup contract. This is a
static ABI hazard; no claim is made that an observed historical crash was
caused by it.

### 1.3 Goals
1. Make every CLI/Python-produced component **runtime-linkable in TradeStation
   Desktop without a TSDev edit/re-verify**, across all entry paths and all
   five supported component types.
2. Establish STA COM on every thread that owns/uses `TsDevLib`, before any
   orlib load/init, with exact `S_OK`/`S_FALSE`/`RPC_E_CHANGED_MODE`/other
   HRESULT semantics and same-thread `CoUninitialize` balancing.
3. Remove the `FUN_10011230` persistence patch completely so the original
   finalization/symbol path runs; fail loudly on compile/link/persistence
   failure.
4. Add a RED regression/acceptance harness that proves the current bypass
   behavior and distinguishes XML evidence from real runtime linkability.
5. Coordinate signature/resolver/test/doc cleanup so the codebase reflects
   24 signatures / 11 patch sites with no stale references.
6. Harden persistence validation and partial-artifact cleanup/rollback.
7. Verify via the full project E2E matrix plus real Desktop runtime acceptance.

### 1.4 Non-goals (explicitly separate follow-up work — Section L of the
research plan, `docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md`)
- Compiler object setup/factory parity (category `+0x08`, subtype `+0x0C`,
  current ProgId `+0x10`, notification HWND `+0x18`, clear `+0/+4`; ordinals
  66/67). Current `+0x10=0xA02` hardcode is inaccurate UI emulation but not
  causal for the current direct `_VerifyModule_New` call.
- `SaveModule` flags / `CELProperties` metadata parity. Password is not the
  link fix.
- Post-success event `0x40A` live refresh for running clients. Cannot replace
  durable linking.
- Reverse-dependent reverify traversal. Not required to make the root Strategy
  linkable.
- Isolating the exact persistent byte-level mismatch inside the otherwise
  structurally similar failing/working ELL/warehouse state. This is a
  **non-blocker** and must NOT trigger more research before L.1 unless
  implementation evidence contradicts the approved causal boundary
  (see §9.3).

### 1.5 Dependencies and evidence
- **Approved research plan:** `docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md`
  (adversarial review Round 2 APPROVED; durable promotion of the former
  `tmp/unknown_link_root_cause_and_plan.md`).
- **Durable static evidence:** `docs/RE/RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md`
  (control flow, ABI, runtime loader, COM ordering, UI call chain, explicit
  limits; durable promotion of the former
  `tmp/unknown_link_ghidra_evidence.md`).
- **Dynamic A/B proof (transient/reproducible raw experiment artifacts under
  `tmp/`):** `tmp/com_unpatched_probe/com_unpatched_probe.cpp`
  (L47-L74 COM guard, L533-L571 worker sequence), `tmp/com_nopatch_probe.stdout.txt`,
  `tmp/com_nopatch_guid_analysis.json`, `tmp/com_nopatch_6689.ell`,
  `tmp/com_nopatch_6689.xml`.
- **Current production source/signature evidence:** `src/tsdev_lib.cpp`
  (L41-L63 patch bytes, L771-L844 constructor applies patch before OrlibInit,
  L854-L900 destructor restores, L1170-L1234 verify flow, L1611-L1639 patch
  application), `src/tsdev_lib.h` (L65-L71, L198-L225 patch surface),
  `signatures/v9.50.01.3344.json` (L156-L175 patch signature),
  `tools/sigcodegen.py` (L35-L40 exact counts), `src/orlib_resolver.h` /
  `src/orlib_resolver.cpp` (persistence resolver API + comments),
  `src/tsdev_cli.cpp` (L630-L665 single worker, L1456-L1535 batch worker,
  L2137-L2156 / L2244-L2248 serve worker).
- **Process rules:** `AGENTS.md` hard rules (one subagent → one narrow
  subtask; every substantive result adversarially reviewed; mandatory real
  CLI E2E after every TVS; TDD red-green).
- **Test spec:** `docs/E2E_TEST_SPEC.md` (all 5 component types, type-specific
  semantic errors, cleanup verification).

---

## 2. TVS breakdown — L.1 through L.5

The workstream is decomposed into exactly five Thin Vertical Slices. Each TVS
follows TDD (red → green) where applicable, ends with mandatory real CLI E2E,
and is adversarially reviewed before being declared complete. The future PM
must split each TVS into atomic edit/build/test/review/e2e subagents
(see §8); no single subagent performs multi-stage or multi-concern work.

### TVS-L.1 — RED regression/acceptance harness proving current bypass behavior
and artifact-completion failure

**Objective:** Add a focused desired-state E2E harness that proves today's
production CLI does not complete the original finalization path, while
explicitly distinguishing **XML evidence** (a completion marker / debugger
source-map artifact) from **real runtime linkability** (what Desktop actually
links). Capture the harness failing once against the pre-fix production binary
as the TDD RED baseline without leaving the standard regression suite red.

**Explicit scope:**
- Test only. No production code changes.
- Insert a unique, minimal Strategy through the **production CLI**.
- Before insert: assert neither target ELL nor XML exists for the assigned
  test entry (use a unique name; discover ProgId from JSON output).
- Desired-state assertions after reported success:
  - ELL and XML both exist and are fresh for this operation. Current code is
    expected to fail here because the bypass skips `SymWriter::Save`.
  - Parse XML as `<symbols file="elmodule_0_<ProgId>" version="{GUID}">` and
    assert that GUID's 16 Windows mixed-endian layout bytes occur exactly once
    in the ELL (do NOT use "last 16 bytes"; see
    `tmp/com_nopatch_guid_analysis.json`).
  - Source round-trips unchanged via `get-source`.
  - Cleanup removes the warehouse entry and any artifacts even after an
    assertion failure.
- Record one explicit nonzero run against the current production binary as the
  RED baseline. The harness remains **opt-in and excluded from
  `tests/run_regression.ps1`** while it is intentionally red. L.3 registers it
  in the standard suite only after the production path makes it green.
- Document that XML/GUID assertions prove the original path ran but do not
  prove Desktop linkability. The existing durable user/runtime evidence is the
  L.1 runtime-failure baseline; a controlled Desktop check may be recorded if
  available, but the test must not fake runtime success/failure with a proxy
  assertion.

**Files/systems likely touched:**
- `tests/test_unknown_link_regression.ps1` (new) — desired-state opt-in E2E
  harness.
- `docs/E2E_TEST_SPEC.md` — document how to run the opt-in RED baseline and why
  it is not yet part of the always-green regression runner.
- `tests/run_regression.ps1` is **not changed in L.1**; registration belongs to
  L.3 after the harness is green.
- No production source changes.

**TDD RED/GREEN sequence:**
- **RED (this TVS):** invoke the opt-in harness directly against current
  production code and capture its nonzero desired-state failure at the missing
  XML/finalization marker.
- **Mainline remains GREEN:** run all existing registered tests and mandatory
  real-CLI E2E; they must pass. No deliberately failing test is registered or
  committed into an always-run suite.
- **GREEN for this harness:** delivered by L.3 after L.2 establishes STA COM
  and L.3 removes the bypass.

**Acceptance criteria:**
- The desired-state harness exists and its pre-fix run is durably recorded as
  RED with the exact documented bypass signature.
- It is opt-in, cleans up after itself, and is not registered in the standard
  regression suite while red.
- The standard unit/regression and mandatory real-CLI E2E suites remain green.
- The test encodes Windows mixed-endian GUID layout and searches the complete
  ELL (no trailing-GUID assumption).
- Test names/output/documentation state that XML evidence is not runtime
  acceptance and do not substitute a fake runtime proxy.

**Tests:**
- Direct opt-in run of `tests/test_unknown_link_regression.ps1` — expected
  nonzero only for the recorded pre-fix RED baseline.
- Existing registered regression suites — expected PASS.

**Mandatory real CLI E2E after TVS:** Run the existing registered project E2E
suite against the real production CLI and require PASS, as mandated by
`AGENTS.md`. Separately invoke the opt-in desired-state harness and record its
expected nonzero RED result. L.1 is not complete if the registered suite is
left failing.

**Documentation updates:**
- `docs/E2E_TEST_SPEC.md` — opt-in unknown-link RED procedure and
  XML-vs-runtime distinction.
- `docs/PM_WORKING_MEMORY.md` — record both the RED baseline and the passing
  registered E2E result.

**Risks/decision gates:**
- **Decision gate:** if the desired-state harness unexpectedly passes on
  current code, STOP and re-examine the causal boundary before proceeding.
- **Risk:** GUID byte-layout bug. The corrected raw analysis at
  `tmp/com_nopatch_guid_analysis.json` must be encoded as Windows mixed-endian
  GUID layout with a complete-ELL search.
- **Risk:** a failed assertion bypasses cleanup. Use guaranteed cleanup/finally
  semantics and verify the disposable warehouse item and artifacts are gone.

**Handoff state:** Opt-in RED baseline recorded; all registered suites and
mandatory real-CLI E2E remain green. L.2 may begin.

---

### TVS-L.2 — Thread-correct STA COM lifecycle across single, batch, serve;
hard error semantics

**Objective:** Add a small non-copyable RAII STA COM guard owned by
`TsDevLib`, constructed as the **first effective action/member** of
`TsDevLib` before `SetDllDirectoryW`, `LoadLibraryW`/`OrlibResolver`, support
DLL loads, patch resolution, or `_OrlibInit`. Cover all three ownership modes
(single, batch, serve) with one initialization per owning worker lifetime and
exact HRESULT semantics.

**Explicit scope:**
- Add the COM guard (preferably an incomplete private implementation member so
  COM mechanics do not leak into the public API).
- Call `CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)`.
- Treat `S_OK` and `S_FALSE` as success. Each successful call must receive
  exactly one `CoUninitialize`, including `S_FALSE`.
- Treat `RPC_E_CHANGED_MODE` as a hard initialization failure with a specific
  diagnostic: the owning thread was already initialized incompatibly (normally
  MTA). Treat every other failed HRESULT as a hard initialization failure and
  include the hexadecimal HRESULT.
- Keep the guard alive until after hidden windows are destroyed and
  `OrlibExit` has completed. Member RAII must unwind/balance COM if any later
  constructor step throws.
- Assert/document the existing contract: construct, use, and destroy a
  `TsDevLib` on one thread. Never initialize COM on the main thread for work
  later performed on another thread.
- Link `Ole32` explicitly if not already transitively linked.
- Do NOT remove the persistence patch in this TVS (that is L.3). L.2 only adds
  the COM prerequisite so the unpatched path becomes safe to run.

**Exact mode behavior (entry paths):**
- **Single insert/modify/upsert:** the per-command worker enters STA through
  `TsDevLib` construction, performs the operation, destroys `TsDevLib` /
  calls `OrlibExit`, then leaves STA. Current worker locations:
  `src/tsdev_cli.cpp` insert L643-L665, modify L758-L779, upsert L1052-L1073.
- **Batch (manifest/stdin/directory):** one STA initialization at `runBatch`
  thread/session start, one `TsDevLib` across all items, one
  `CoUninitialize` after final close/`OrlibExit`; NOT per item.
  `src/tsdev_cli.cpp` L1465-L1535.
- **Serve (broker reuse):** one STA initialization on `runServe` before the
  long-lived `TsDevLib`, retained across clients/requests, released only after
  final warehouse close and `OrlibExit`. Do NOT initialize per request.
  `src/tsdev_cli.cpp` L2137-L2156, L2244-L2248.
- **Other library-backed commands (exists/get-source/delete):** they inherit
  the same correct prerequisite. This avoids an inconsistent library contract
  even where no compile occurs.

**Files/systems likely touched:**
- `src/tsdev_lib.h` — add private COM guard member (incomplete type) +
  forward declaration; document the same-thread contract.
- `src/tsdev_lib.cpp` — implement the guard; construct it first in the
  `TsDevLib` constructor; balance in destructor; explicit `Ole32` link.
- `CMakeLists.txt` — link `Ole32` if needed; add the COM-guard test target and
  include it in the normal regression build dependency.
- `tests/test_com_apartment_guard.cpp` (new) — unit tests for the guard via an
  injectable seam/function wrapper (NOT process-global apartment manipulation
  in a shared test runner).
- `tests/run_unit_tests.ps1` — register the new test; the master regression
  runner then exercises it through the existing unit-test suite, with no new
  standalone sub-suite.

**TDD RED/GREEN sequence:**
- **RED:** unit tests for the injectable/narrow COM apartment guard: `S_OK`,
  `S_FALSE`, `RPC_E_CHANGED_MODE`, generic failed HRESULT, exactly-once
  balancing, and constructor-unwind balancing. Keep production calls
  unsuppressed. Fails to compile (guard doesn't exist).
- **GREEN:** implement the guard; place it before all orlib loading/init in
  `TsDevLib`. Unit tests pass.

**Acceptance criteria:**
- `S_OK` and `S_FALSE` both succeed and each receive exactly one
  `CoUninitialize` on the same thread.
- `RPC_E_CHANGED_MODE` and any other failed HRESULT produce a hard init
  failure with HRESULT detail; no orlib load/init, no window creation, no
  entry creation, no ELL/XML mutation occurs on failure.
- Constructor unwind (failure after `S_OK`/`S_FALSE`) balances COM exactly
  once on the same thread; forced failure tests are mandatory.
- COM is initialized exactly once per owning worker lifetime across single,
  batch, and serve modes.

**Tests:**
- `tests/test_com_apartment_guard.cpp` — `S_OK`, `S_FALSE`,
  `RPC_E_CHANGED_MODE`, generic failure, exactly-once balancing,
  constructor-unwind balancing.
- Existing init-failure tests must still pass (COM failure arrives as existing
  `init_failure` / exit 4 semantics with HRESULT detail).

**Mandatory real CLI E2E after TVS:** Run the project E2E suite
(`docs/E2E_TEST_SPEC.md`) from an elevated session and require PASS. All
single/batch/serve paths must remain behaviorally unchanged (the COM guard is
additive and the patch is still present). Confirm no COM imbalance,
handle/window leak, or crash reporter across repeated sequential instances and
a batch/serve stress run. The L.1 desired-state harness remains opt-in,
expected RED, and excluded from the registered suite until L.3; its expected
failure does not excuse any registered-suite failure.

**Documentation updates:**
- `src/tsdev_lib.h` — same-thread contract documentation.
- `docs/BUILD.md`, `AGENTS.md`, and other live current-count summaries — note
  explicit `Ole32` linkage if applicable and update C++ executable/case totals
  from the actual registered test definitions; do not guess counts.
- `docs/PM_WORKING_MEMORY.md` — record L.2 completion and verified counts.

**Risks/decision gates:**
- **Decision gate:** if any existing test breaks because the guard changes
  init ordering, STOP and resolve before L.3. The guard must be strictly
  additive relative to current behavior (the patch still suppresses the COM-
  dependent path in this TVS).
- **Risk:** COM guard unwind correctness. Mandatory forced-failure tests.
- **Risk:** thread-affinity violation if the guard is accidentally
  constructed on the main thread. The guard must live on the same 64 MiB
  worker that owns `TsDevLib`.

**Handoff state:** COM prerequisite in place across all entry paths. L.3 may
begin (it removes the patch now that the unpatched path is safe to run).

---

### TVS-L.3 — Remove persistence bypass/signature/resolver/test/doc
assumptions and run real finalization path

**Objective:** Remove the `FUN_10011230` persistence patch completely so the
original finalization/symbol path runs, and coordinate the full
signature/resolver/test/doc cleanup so the codebase reflects 24 signatures /
11 patch sites with no stale references.

**Explicit scope:**
- Remove, rather than disable behind a flag:
  - patch byte constants and validation helpers;
  - constructor call to `applyPersistencePatch()`;
  - cached persistence RVA/original-byte members and destructor restoration
    branch;
  - resolver convenience API specific to that patch;
  - signature/catalog/generated/test entries for
    `FUN_10011230_persistence_patch`.
- Keep the unrelated, still-required warehouse thread-check patches. Update
  patch bookkeeping so it represents only those sites. Do NOT retain a
  fallback that returns success after link/symbol failure.
- Run the real finalization path: `FUN_10011230` → `FUN_1008E640` →
  `SymInfo::AdjustOffsets` → `SymWriter::Save`, then `ModuleInfo::SetGuid` /
  `MDVault::SaveModule`.
- Coordinate signature/resolver/test/doc cleanup per §5 (Cleanup inventory).

**Files/systems likely touched (see §5 for the full cleanup inventory):**
- `src/tsdev_lib.h` / `src/tsdev_lib.cpp` — remove the complete persistence-
  patch surface; preserve/rename generic `m_patchesApplied` bookkeeping for
  the 11 unrelated thread-check patches.
- `src/orlib_resolver.h` / `src/orlib_resolver.cpp` — remove
  `resolvePersistencePatch()` and all persistence-specific comments/references.
- `signatures/v9.50.01.3344.json` — delete the
  `FUN_10011230_persistence_patch` entry.
- `tools/sigcodegen.py` — change `EXPECTED_SIGNATURE_COUNT` from 25 to 24;
  regenerate `signatures.gen.h` through the normal build.
- `tests/test_orlib_resolver.cpp` — update hard-coded `kSignatures[8]`
  references; prefer name lookup over positional indices; remove the
  validate/apply persistence case.
- `tests/test_persistence_validation.cpp` — repurpose into a behavior-based
  finalization regression (see §5 item 6); do not require resolving the removed
  internal patch target by hardcoded RVA or a duplicate test-only signature.
- `tests/run_regression.ps1` — register the L.1 desired-state harness only after
  it passes with the production binary.
- `tests/test_sigtool.ps1` — update 25/12 expectations to 24/11; fix stale
  "20/20" comments.
- `docs/SIGNATURE_CATALOG.md` — rewrite totals/table/section 4.9/scan results;
  correct stale `__fastcall`/callee-cleanup statements to match the appendix
  evidence (caller `ADD ESP,0x0C`, original plain `RET`, patched `RET 0x0C`).
- `docs/FEASIBILITY.md` — correct stale `__fastcall`/callee-cleanup
  statements.
- `docs/PROJECT_HISTORY.md` — prepend a conspicuous dated correction (preserve
  history; do not silently rewrite).
- `CMakeLists.txt` / `tests/run_unit_tests.ps1` — update registrations if the
  persistence validation test is renamed.

**TDD RED/GREEN sequence:**
- **RED (from L.1):** the opt-in unknown-link desired-state harness has a
  recorded nonzero baseline with the documented bypass signature.
- **GREEN:** remove the patch, run the real path, and coordinate cleanup. The
  harness passes: ELL and XML both exist, XML parses as
  `<symbols file="elmodule_0_<ProgId>" version="{GUID}">`, the GUID's 16
  Windows mixed-endian bytes occur exactly once in the ELL, source round-trips
  unchanged, and cleanup removes the entry and both artifacts. No
  process-local restoration trick.
- Only after that green run, register the harness in `tests/run_regression.ps1`
  and prove the complete registered suite remains green.

**Acceptance criteria:**
- The L.1 desired-state harness turns GREEN with the real production binary
  (no in-memory restoration) and is then registered in the standard suite.
- New exact counts: **24 signatures, 11 patch sites, 3 data globals, 10
  exports** (unless another independently justified signature is added; do
  NOT add a dummy to preserve a count).
- Every remaining signature is unique; every remaining patch byte validates;
  version detection tiers (manifest identity, PE timestamp/file size, SHA-256)
  are unchanged — they are properties of the unmodified on-disk DLL, not
  signature array length.
- No stale `__fastcall`/callee-cleanup statements remain in
  `docs/SIGNATURE_CATALOG.md` or `docs/FEASIBILITY.md`.
- No hardcoded RVA or duplicate private signature is introduced merely to
  inspect the removed patch target.
- One Strategy inserted by the final L.3 production CLI executes in Desktop
  without TSDev edit/reverify.
- No production code path returns success after link/symbol failure.

**Tests:**
- L.1 regression harness (now GREEN).
- Full resolver/signature/unit/regression suites.
- `tests/test_sigtool.ps1` (24/11 expectations).
- Version detection Tier 1/2/3 mismatch tests after index cleanup.

**Mandatory real CLI E2E after TVS:** Run the full registered project E2E
suite (`docs/E2E_TEST_SPEC.md`) — all 5 component types and all subcommands —
and require PASS. The newly registered L.1 harness must be GREEN. As the first
TVS containing the complete causal fix, L.3 also requires one controlled
Strategy smoke using the final production CLI binary: insert without opening
or re-verifying in TSDev and confirm Desktop executes it. This may be a
recorded human check where Desktop automation is unavailable under project
policy. L.5 later repeats this as the comprehensive release gate, including
restart and the wider mode/type matrix.

**Documentation updates:** Complete §5 cleanup inventory. After registering the
new green harness, derive and update current sub-suite/case/executable counts in
`AGENTS.md`, `README.md`, `docs/BUILD.md`, and `docs/E2E_TEST_SPEC.md`; preserve
dated historical counts as historical.

**Risks/decision gates:**
- **Decision gate:** if the real finalization path raises `0xE06D7363` or any
  other exception despite STA COM being in place (L.2), STOP and re-examine
  before proceeding. The dynamic A/B proves the combined fix; if production
  diverges, that is new implementation evidence that may justify revisiting
  the causal boundary.
- **Risk:** signature count cascade. Prefer name lookup over positional
  indices in resolver tests so future catalog edits do not cascade silently.
- **Risk:** stale doc references. The cleanup inventory in §5 is exhaustive
  for **operational/current** docs and source (`src/`, `tests/`,
  `tools/`, `signatures/`, and the live `docs/` files enumerated in §5).
  References under `docs/archive/` and `docs/RE/archive/` are intentionally
  **retained as frozen historical artifacts** per `AGENTS.md` (Historical
  reference only; counts and details within may be stale) and are deliberately
  excluded from this inventory — they must NOT be silently rewritten; if a
  correction is warranted it is applied as a conspicuous dated banner per
  §5 item 10, not by editing the archived content. Every operational inventory
  item must be addressed in the same TVS.

**Handoff state:** Real finalization path runs end-to-end; cleanup complete.
L.4 may begin.

---

### TVS-L.4 — Persistence validation and partial-artifact cleanup/rollback
hardening

**Objective:** Harden persistence validation and partial-artifact
cleanup/rollback so a production operation never silently reports success when
the expected ELL/XML persistence markers are absent or stale, and so a failed
insert/modify does not leave orphaned or corrupt artifacts.

**Explicit scope:**
- **COM failure occurs before mutation:** on `RPC_E_CHANGED_MODE` or another
  failed HRESULT, do not load/init orlib, create windows, create entries, or
  touch ELL/XML. Return existing init failure semantics with HRESULT detail.
  Never continue in MTA and never fall back to the patch.
- **Compile/link failure is failure:** preserve compiler diagnostics; if the
  original linker/symbol path returns failure or raises an SEH/C++ exception,
  return non-success. Do not force return 1, suppress exceptions, downgrade
  warnings/errors, or claim verification.
- **Failed insert/upsert-new:** retain the existing newly-created-entry
  cleanup intent (`src/tsdev_lib.cpp` L1236-L1258), but extend its tests to
  require that both target `.ell` and `.xml` are absent afterward. Cleanup
  errors must be reported alongside — not replace — the primary compile/link
  diagnostic. The current empty catch is not sufficient for production
  observability.
- **Failed modify/upsert-existing:** never delete the existing warehouse
  component. Before changing rollback behavior, characterize the unpatched
  TSDev/orlib sequence under syntax failure and a fault injected after artifact
  creation begins. The default implementation requirement is to reproduce
  TSDev's warehouse/API behavior, not invent direct file manipulation. If the
  prior known-good artifact state remains intact, assert that invariant. If it
  can be replaced or truncated, first identify the TSDev/orlib transaction,
  recovery, or reverify sequence and determine whether warehouse GUID/property
  state changes. Direct byte backup/restore is not authorized merely because it
  is convenient: it may desynchronize warehouse and ELL state and violates the
  emulate-TSDev rule unless evidence proves TSDev uses an equivalent sequence
  or no proper API exists and cross-store consistency is demonstrated. If no
  safe confirmed rollback exists, failed modify/upsert-existing remains a
  blocker rather than shipping a file-level workaround. Source should remain
  the user's newly saved, unverified source if that is TSDev's behavior; do not
  invent source rollback by parsing source or recreating metadata.
- **Absent/stale pair on success path:** a production operation must never
  silently report success when the expected ELL/XML persistence markers are
  absent or stale. The immediate guarantee comes from using the original
  return path; E2E must additionally reject an absent/stale XML or
  disagreement between its version GUID and the ELL. If fault injection proves
  the original function can return success with absent/stale files, promote
  these checks into production postconditions using version-scoped parsing;
  otherwise keep binary-format parsing in tests to minimize runtime coupling.
- **Close/flush failure:** report it if an API provides a detectable failure.
  Do not delete known-good modify artifacts merely because final close is
  uncertain. Preserve enough path/ProgId detail for recovery.
- **Cleanup ordering:** on normal worker exit: finish operation → close
  warehouse where mode requires → destroy `TsDevLib` (`OrlibExit`, windows) →
  `CoUninitialize` on the same thread. Constructor unwind must release only
  resources successfully acquired.
- **No suppressions/hacks:** do not catch and ignore COM/link failures,
  restore the bypass conditionally, add compensating stack manipulation for
  the removed patch, or parse mutable source text for identity/cleanup. Names,
  types, and ProgIds remain warehouse/API-derived.

**Files/systems likely touched:**
- `src/tsdev_lib.cpp` — extend failed-insert cleanup; add close/flush failure
  reporting; ensure cleanup ordering.
- `src/tsdev_lib.h` — any new postcondition hooks (kept private).
- `tests/test_persistence_validation.cpp` — extend to require both `.ell` and
  `.xml` absent after failed insert; add post-failure artifact/warehouse
  assertions.
- `tests/test_unknown_link_regression.ps1` — extend to modify and upsert-
  existing/new paths; assert per-operation artifact freshness and GUID
  agreement; add stale-pair-on-modify and induced-compile/link-failure cases.
- New fault-injection test if practical (characterize modify rollback
  behavior).

**TDD RED/GREEN sequence:**
- **RED:** extended cleanup/rollback tests fail on current behavior (e.g.,
  failed insert leaves an orphan, or stale-pair-on-modify is not detected).
- **GREEN:** implement hardening; tests pass.

**Acceptance criteria:**
- Failed insert/upsert-new leaves no `.ell` and no `.xml` for the target;
  cleanup errors are reported alongside the primary diagnostic.
- Failed modify/upsert-existing never deletes the existing component; rollback
  behavior is characterized and either invariant-asserted or
  backup/restore-protected.
- Absent/stale XML or GUID disagreement on a success path is rejected (in
  production if fault injection proves it can happen, otherwise in E2E).
- COM failure before mutation touches no orlib state.
- Cleanup ordering is correct across single/batch/serve.

**Tests:**
- Extended `tests/test_persistence_validation.cpp`.
- Extended `tests/test_unknown_link_regression.ps1` (modify/upsert/stale/fault
  cases).
- Compile-error tests with post-failure artifact/warehouse assertions.

**Mandatory real CLI E2E after TVS:** Run the project E2E suite including the
extended regression harness. Confirm: no orphan from failed insert; no
deletion of existing component on failed modify; subsequent operation
succeeds after an earlier per-item compile error (batch/serve resilience).

**Documentation updates:**
- `docs/E2E_TEST_SPEC.md` — extend the unknown-link regression section with
  cleanup/rollback cases.
- `docs/PM_WORKING_MEMORY.md` — record L.4 completion (PM updates; this
  planning task does not touch it).

**Risks/decision gates:**
- **Decision gate (modify rollback):** if fault injection shows orlib can
  truncate/replace a prior known-good ELL/XML on failed modify, a confirmed
  TSDev/orlib recovery sequence and cross-store consistency proof become
  blockers for that path. Do not fall back to artifact-only restoration unless
  evidence establishes it as TSDev-equivalent and safe. This must be resolved
  before production hardening ships.
- **Risk:** over-coupling production code to ELL binary format. Keep binary
  parsing in tests unless fault injection proves it is needed in production.
- **Risk:** source rollback by parsing source text — explicitly forbidden
  (AGENTS.md critical rule on mutable item internals).

**Handoff state:** Persistence validation and cleanup/rollback hardened. L.5
may begin.

> **FALSIFICATION NOTE (2026-08-23):** The "Persistence validation and
> cleanup/rollback hardened" handoff-state claim above is FALSIFIED for the
> L.4 completion claim. Independent fault injection showed the production CLI
> returns `exit 0` / `success=true` while leaving the library inconsistent:
> Fault A (stale-ELL modify, audit `TS_AUTO_AUDIT_STALE_20260823_154324`,
> ProgId 7333) left the ELL SHA-256 unchanged while the XML changed, and Fault
> B (orphan-ELL delete, audit `TS_AUTO_AUDIT_DELETE_20260823_160907`,
> ProgId 7456) left the ELL present after a "successful" delete. Both
> persistence and deletion fault injection jointly falsify the L.4 completion
> claim. L.4 is FALSIFIED and superseded by the L.6–L.15 recovery
> (implemented on branch `workstream-L-recovery`; production fix owned by L.9
> for Fault A — detect-and-fail stale-ELL modify/upsert — and L.12 for Fault B
> — detect-and-report orphan-ELL delete; test-oracle repair by L.6; complete
> evidence re-capture by L.7). The L.8 decision-gate for Fault A recovery is
> BLOCKED — see `docs/RE/RE_L8_STALE_WRITE_FAULT_A.md` §7 (no
> `TSDev.exe`-equivalent save-failure observation established; no invented
> TSDev recovery; L.9 cannot invent a recovery sequence not present in the
> binary). Existence checks are necessary but not sufficient. See
> `docs/PM_WORKING_MEMORY.md`. The
> handoff-state line is retained above for provenance only and must NOT be read
> as a current completion statement.

---

### TVS-L.5 — Full verification matrix + real production CLI/Desktop runtime
acceptance across required operations/modes/types

**Objective:** Run the full verification matrix and the real Desktop runtime
acceptance gate. **Runtime acceptance cannot be replaced by XML existence.**
A strategy inserted by the production CLI must execute in TradeStation Desktop
without a TSDev edit/re-verify. All five component types receive appropriate
artifact/semantic tests, and executable/visual types are exercised in Desktop.

**Explicit scope:**
- Run the full verification matrix (see §7).
- **Strategy Desktop execution (causal release gate):** insert a unique
  Strategy via the production CLI; **execute it in Desktop without opening or
  re-verifying it in TSDev**; confirm a successful order/strategy calculation.
  Merely adding it to a chart is insufficient if execution can be observed
  directly. Then **restart Desktop** and confirm the strategy still executes
  (durable, not live-cache).
- **All five component types** (Strategy, Indicator, Function, TradingApp,
  ShowMe) receive appropriate artifact/semantic tests:
  - Successful valid compile; correct expanded filename type; fresh ELL/XML;
    XML GUID embedded once in ELL; source round-trip.
  - Type-specific semantic error cases per `docs/E2E_TEST_SPEC.md`
    (e.g., Plot in a Strategy, Buy in an Indicator).
- **Desktop runtime smoke for executable/visual types feasible in Desktop:**
  - Strategy: actually executes (causal gate).
  - Indicator: plots.
  - ShowMe: plots.
  - TradingApp: launches.
  - Function: exercised through a verified caller where practical.
- The release report must state which Desktop checks were automated versus
  manually observed. The confirmed 6689 probe run is proof of direction, NOT
  a substitute for testing the final production binary.

**Files/systems likely touched:**
- `tests/test_unknown_link_regression.ps1` — extend to all 5 types and the
  Desktop execution gate (manual observation recorded in test output where
  automation is not feasible).
- `tests/run_regression.ps1` — register the full matrix.
- `docs/E2E_TEST_SPEC.md` — finalize the unknown-link regression section with
  the full matrix and Desktop runtime acceptance procedure.
- Existing durable completion records (`docs/PROJECT_HISTORY.md`,
  `docs/PM_WORKING_MEMORY.md`, and `docs/E2E_TEST_SPEC.md`) — record automated
  versus manually observed Desktop checks. Do not create a standalone report
  unless the future PM identifies a durable need not served by these files.

**Verification progression:**
- **Pre-gate:** L.3 has passed one controlled Strategy Desktop smoke and L.4
  has passed artifact/rollback checks. Those narrow results do not satisfy the
  release matrix.
- **Release GREEN:** repeat Desktop execution with the final post-L.4
  production binary, before and after restart, and pass every required matrix
  cell. Any failure reopens the owning implementation TVS; L.5 does not patch
  failures in-place.

**Acceptance criteria:**
- Every cell of the verification matrix passes.
- A CLI-inserted Strategy executes in Desktop without TSDev edit/re-verify,
  before and after a Desktop restart.
- All 5 component types pass artifact + semantic tests.
- Executable/visual types pass their Desktop smoke where feasible.
- The release report documents automated vs. manual observations.

**Tests:**
- Full verification matrix (§7).
- Desktop runtime acceptance (Strategy causal gate + type smokes).

**Mandatory real CLI E2E after TVS:** This TVS **is** the mandatory real CLI
E2E, extended to Desktop runtime. The full `docs/E2E_TEST_SPEC.md` suite plus
the unknown-link regression plus Desktop execution must all PASS before the
workstream is declared complete.

**Documentation updates:**
- `docs/E2E_TEST_SPEC.md` — finalize the unknown-link regression section.
- Release report.
- `docs/PM_WORKING_MEMORY.md` — record Workstream L completion (PM updates;
  this planning task does not touch it).
- `docs/PROJECT_HISTORY.md` — append the Workstream L completion entry.

**Risks/decision gates:**
- **Release gate:** if the CLI-inserted Strategy does NOT execute in Desktop
  without TSDev edit/re-verify, the workstream is NOT complete. Re-enter the
  DISAPPROVE loop and investigate where production diverges from the
  confirmed 6689 probe.
- **Risk:** automating every Desktop UI/runtime check. A controlled manual
  release gate is acceptable if documented (per research plan §M non-blockers,
  `docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md`).
- **Risk:** Desktop state pollution across test runs. Use unique names and
  cleanup.

**Handoff state:** Workstream L complete. Runtime linkability confirmed end-
to-end across all entry paths and component types.

> **FALSIFICATION NOTE (2026-08-23):** The "Workstream L complete. Runtime
> linkability confirmed end-to-end across all entry paths and component types"
> handoff-state claim above is FALSIFIED for the L.5 completion claim. The L.5
> "full verification matrix + Desktop runtime gate across all 5 types" claim
> was falsified jointly by the same persistence and deletion fault injection
> that falsified L.4: Fault A (stale-ELL modify, audit
> `TS_AUTO_AUDIT_STALE_20260823_154324`, ProgId 7333) and Fault B
> (orphan-ELL delete, audit `TS_AUTO_AUDIT_DELETE_20260823_160907`,
> ProgId 7456) each returned `exit 0` / `success=true` while leaving the
> library inconsistent, contradicting the matrix's artifact-integrity and
> cleanup invariants. The historical L.5 Desktop runtime gate observed no
> "Unknown link error occurred" for any CLI-inserted item before/after Desktop
> restart, but that gate did not exercise the persistence/delete fault paths
> whose failures falsified L.4/L.5. **No new Desktop runtime re-gate is
> claimed without an L.17 result.** L.5 is FALSIFIED and superseded by the
> L.6–L.15 recovery (implemented on branch `workstream-L-recovery`;
> test-oracle repair by L.6; complete evidence re-capture by L.7; production
> fixes by L.9/L.12). See `docs/PM_WORKING_MEMORY.md`. The handoff-state line
> is retained above for provenance only and must NOT be read as a current
> completion statement.

---

## 3. HRESULT handling and same-thread CoUninitialize ownership (applies to
L.2 and all later TVSs)

- `CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)` is called exactly once
  per owning worker lifetime, as the first effective action of `TsDevLib`
  construction, before any orlib load/init.
- `S_OK`: success. Receive exactly one `CoUninitialize` on the same thread at
  `TsDevLib` destruction (after `OrlibExit` and window destruction).
- `S_FALSE`: success (the apartment was already initialized on this thread).
  Still receive exactly one `CoUninitialize` on the same thread — balancing
  `S_FALSE` is mandatory.
- `RPC_E_CHANGED_MODE`: hard initialization failure. The owning thread was
  already initialized incompatibly (normally MTA). Return existing
  `init_failure` / exit 4 semantics with a specific diagnostic including the
  HRESULT. Do NOT load/init orlib, create windows, create entries, or touch
  ELL/XML. Never continue in MTA. Never fall back to the patch.
- Any other failed HRESULT: hard initialization failure. Return
  `init_failure` / exit 4 with the hexadecimal HRESULT in the diagnostic. Same
  no-mutation rule.
- **Same-thread ownership:** the thread that calls `CoInitializeEx` MUST be
  the thread that calls `CoUninitialize`. The COM guard is a member of
  `TsDevLib`; `TsDevLib` is constructed, used, and destroyed on one 64 MiB
  worker thread. Never initialize COM on the main thread for work later
  performed on another thread.
- **Constructor unwind:** if any step after a successful `CoInitializeEx`
  throws, the guard must balance COM exactly once on the same thread during
  stack unwinding. Forced failure tests are mandatory.
- **Cleanup ordering on normal exit:** finish operation → close warehouse
  where mode requires → destroy `TsDevLib` (`OrlibExit`, windows) →
  `CoUninitialize` on the same thread.
- **No suppressions:** do not catch and ignore COM failures or downgrade them
  to warnings.

---

## 4. Entry-path coverage (applies to L.2–L.5)

| Entry path | COM initialization | TsDevLib ownership | Notes |
|---|---|---|---|
| `insert` (single) | per-command worker, via `TsDevLib` ctor | `src/tsdev_cli.cpp` L643-L665 | STA before orlib load/init. |
| `modify` (single) | per-command worker, via `TsDevLib` ctor | L758-L779 | Same. |
| `upsert` (single) | per-command worker, via `TsDevLib` ctor | L1052-L1073 | Same. |
| `delete` / `exists` / `get-source` | per-command worker, via `TsDevLib` ctor | (existing single-worker paths) | Inherits the COM prerequisite for a consistent library contract. |
| `batch-insert`/`modify`/`delete`/`upsert` (manifest or `--batch-stdin`) | one STA init at `runBatch` start | one `TsDevLib` across all items, L1465-L1535 | One `CoUninitialize` after final close/`OrlibExit`; NOT per item. |
| `batch-*-dir` (directory batch) | same as batch (shares `runBatch`) | same | Same. |
| `serve` (broker reuse) | one STA init on `runServe` before the long-lived `TsDevLib` | L2137-L2156, L2244-L2248 | Retained across clients/requests; released only after final warehouse close and `OrlibExit`. Do NOT initialize per request. |
| Python wrapper (direct `subprocess.run`, elevated session) | inherits from the spawned CLI worker | n/a (Python is 64-bit, does not load 32-bit orlib) | No Python protocol change. |
| Python wrapper (serve broker, non-elevated session) | inherits from the broker's `runServe` worker | n/a | No Python protocol change. COM failures arrive as existing `init_failure` / exit 4 responses; broker startup failures propagate as broker-level errors. |

**Python wrapper implications (L.2–L.5):**
- No Python protocol or public method change is required.
- Direct non-serve calls spawn the fixed CLI; `use_serve=True` uses the fixed
  long-lived serve worker; Python batch-over-serve loops over requests but
  shares that worker/session.
- Preserve current JSON schemas and exit-code mapping.
- Do NOT initialize COM in 64-bit Python because it is not the thread/process
  loading 32-bit orlib.
- Add Python tests only to verify routing/error propagation remains unchanged.

---

## 5. Cleanup inventory for the persistence patch (L.3)

The persistence patch is currently one of 25 signatures / 12 patch sites
(`docs/SIGNATURE_CATALOG.md` L5-L12); codegen enforces exactly 25 signatures,
3 globals, and 10 exports (`tools/sigcodegen.py` L35-L40). Removal must be
coordinated in a single TVS (L.3):

1. **Signature JSON:** delete `FUN_10011230_persistence_patch` from
   `signatures/v9.50.01.3344.json` L156-L175. Expected totals become
   **24 signatures, 11 patch sites, 3 data globals, 10 exports** unless
   another independently justified signature is added (do NOT add a dummy to
   preserve a count).
2. **Codegen counts:** change `EXPECTED_SIGNATURE_COUNT` from 25 to 24;
   globals/exports remain 3/10. Regenerate `signatures.gen.h` through the
   normal build, NOT by hand.
3. **TsDevLib persistence-patch surface:** remove public
   `validatePersistencePatchBytes` (`src/tsdev_lib.h` L198-L202);
   `m_persistencePatchRva` and `m_persistenceOriginalBytes` (L211-L219);
   `applyPersistencePatch` declaration (L369-L373); corresponding constants,
   implementation, constructor call, and destructor restoration logic in
   `src/tsdev_lib.cpp`. Preserve and, where needed, rename generic
   `m_patchesApplied` bookkeeping for the 11 unrelated thread-check patches.
   Additionally correct the stale "Restore all 10 patch sites" comment in
   `src/tsdev_lib.cpp` (destructor restoration region): the "10" was an
   outdated thread-check count and did not include the separately restored
   persistence patch, so after L.3 the comment must describe exactly 11
   thread-check patch sites and read "Restore all 11 patch sites" (or
   equivalent).
4. **OrlibResolver APIs/state/comments:** remove
   `OrlibResolver::resolvePersistencePatch()` and its implementation
   (`src/orlib_resolver.h` L843-L851; `src/orlib_resolver.cpp` L940-L945).
   Update ALL persistence-specific resolver comments/references at
   `src/orlib_resolver.h` L153, L162, L168, L328, L771, L788, L811, L835,
   L843-L851. Additionally remove or generalize the persistence-specific
   mask/comment branch at `src/orlib_resolver.cpp` L868-L875 (a
   persistence-patch-only mask and accompanying comment that becomes dead
   after the signature is removed); after signature removal it must either be
   deleted or rewritten around a remaining thread-check patch so no
   persistence-only mask/comment survives. Keep generic patch machinery and
   rewrite examples around a remaining thread-check patch.
5. **Resolver tests:** update tests that hard-code `kSignatures[8]` as
   persistence and all following numeric indices
   (`tests/test_orlib_resolver.cpp` L1218-L1227). Prefer name lookup over
   positional indices so future catalog edits do not cascade silently. Remove
   the validate/apply persistence case at L1503-L1535.
6. **`tests/test_persistence_validation.cpp`:** REPURPOSE rather than delete
   into a behavior-based finalization regression under the new COM ownership
   contract: construct `TsDevLib`, compile a disposable valid item through the
   real library path, validate fresh paired artifacts/GUID agreement and
   cleanup, and prove no persistence-patch API remains. Do **not** resolve or
   inspect `FUN_10011230` by hardcoded RVA, retain its removed production
   signature solely for testing, or duplicate a private test-only signature.
   Source/catalog absence checks plus successful real-path behavior establish
   that the bypass is gone. Rename the file/target only if that improves
   clarity. If retained under its current target name, update descriptions but
   keep registrations; if renamed, update all of them together:
   `CMakeLists.txt` L103-L104 (`add_executable`/`target_link_libraries`),
   L372 (live-test list), L476 (`test_regression` dependency), and
   `tests/run_unit_tests.ps1` L22, L111 (menu and `RequiresTs` table). Do NOT
   leave calls to `validatePersistencePatchBytes`, which would fail to compile
   after step 3.
7. **CMake/test runner registrations:** per step 6 if the test is renamed;
   otherwise no registration change beyond the new COM guard test (L.2) and
   the new regression harness (L.1).
8. **sigtool expectations:** update sigtool's 25/12 expectations to 24/11
   (`tests/test_sigtool.ps1` L34-L40, L193-L201) and stale "20/20" comments
   in that script.
9. **SIGNATURE_CATALOG.md:** rewrite totals/table/section 4.9/scan results
   and renumber carefully, OR retain stable target IDs with an explicit
   retired gap while ensuring generated array indices are not inferred from
   IDs. Every table row and scan-results row that references
   `FUN_10011230_persistence_patch` must be removed or renumbered — at
   minimum the table row at `docs/SIGNATURE_CATALOG.md` L128 and the
   scan-results row at `docs/SIGNATURE_CATALOG.md` L1192 (and any other
   occurrence surfaced by grepping the file for
   `FUN_10011230_persistence_patch`); after removal, renumber all
   subsequent table/scan row indices and any prose that cites them by
   position so no dangling or off-by-one reference survives. Explicitly
   correct the stale `__fastcall`/callee-cleanup statements in
   `docs/SIGNATURE_CATALOG.md` L106, L380, L386, L404 and
   `docs/FEASIBILITY.md` L338-L340 to match the appendix's caller
   `ADD ESP,0x0C`, original plain `RET`, and patched `RET 0x0C` evidence
   (`docs/RE/RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md` §1). Machine-readable JSON
   remains authoritative.
10. **Stale docs:** correct operational docs and source comments that say the
    patch provides persistence or XML omission is acceptable. In historical
    documents, preserve history but prepend a conspicuous dated correction
    rather than silently rewriting events. In particular, supersede
    `docs/PROJECT_HISTORY.md` L26-L34: the bypass produced runtime-unlinkable
    output; XML absence was not acceptable evidence of complete verification;
    STA COM plus removal of the bypass is dynamically confirmed; no missing
    native tables or exact warehouse-GUID mismatch has been established.
11. **Version detection:** verify unchanged — manifest identity, PE
    timestamp/file size, and SHA-256 are properties of the unmodified on-disk
    DLL, not signature array length. Run all Tier 1/2/3 mismatch tests after
    index cleanup.
12. **Durable-path references for former `tmp/unknown_link_*` research docs:**
    the former `tmp/unknown_link_root_cause_and_plan.md` and
    `tmp/unknown_link_ghidra_evidence.md` research artifacts are now durably
    promoted to `docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md` and
    `docs/RE/RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md`. **Current invariant:** any
    authoritative reference to that former research in `docs/FEASIBILITY.md` or
    `docs/SIGNATURE_CATALOG.md` must cite the durable `docs/RE/` paths, not the
    transient `tmp/unknown_link_*` paths. **Future validation:** L.3's
    doc-cleanup review must verify this invariant holds in
    `docs/FEASIBILITY.md` and `docs/SIGNATURE_CATALOG.md` and correct any
    residual `tmp/unknown_link_*` reference to its durable `docs/RE/` path.

---

## 6. Runtime acceptance — XML existence is not runtime linkability (applies
to L.1 and L.5)

- **XML is a completion marker and debugger/source-map artifact, NOT what
  Desktop links.** ELRte/ELMDCore runtime loading uses the `.ell`; the XML
  reader identified in the evidence belongs to `ELDebugger.dll!SymReader`
  (`docs/RE/RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md` §4). XML absence is a
  highly visible marker that the original path was skipped; it is not the
  causal linkability mechanism.
- **The causal statement:** the bypassed path produces runtime-unlinkable
  output; the restored path under STA produces output that executes in
  Desktop. Static evidence identifies the skipped finalization/symbol
  operations; the precise persistent ELL/warehouse mismatch remains
  unisolated (see §9.3).
- **L.1 requirement:** the RED harness must explicitly distinguish XML
  evidence from real runtime linkability in its assertions and/or
  documentation. A future reader must not conflate "XML present" with
  "Desktop links."
- **L.5 requirement (release gate):**
  - Insert a unique Strategy via the production CLI.
  - **Execute it in Desktop WITHOUT opening or re-verifying it in TSDev.**
    A successful order/strategy calculation is the release gate; merely
    adding it to a chart is insufficient if execution can be observed
    directly.
  - **Restart Desktop** and confirm the strategy still executes (durable,
    not live-cache). A full Desktop restart did not repair a broken CLI item
    under the old bypass (`docs/PM_WORKING_MEMORY.md` L28-L30); the fix must
    survive restart.
  - All five component types (Strategy, Indicator, Function, TradingApp,
    ShowMe) receive appropriate artifact/semantic tests per the verification
    matrix (§7). Executable/visual types receive Desktop smokes where
    feasible: Strategy executes (causal gate), Indicator/ShowMe plot,
    TradingApp launches, Function is exercised through a verified caller
    where practical.
- The release report must state which Desktop checks were automated versus
  manually observed. The confirmed 6689 probe run is proof of direction, NOT
  a substitute for testing the final production binary.

---

## 7. Verification matrix (L.5)

Use the valid type-specific source fixtures and semantic errors defined by
`docs/E2E_TEST_SPEC.md` L19-L25 and mappings at L168-L176.

| Dimension | Required cases | Required assertions |
|---|---|---|
| Component type | Strategy, Indicator, Function, TradingApp, ShowMe | Successful valid compile; correct expanded filename type; fresh ELL/XML; XML GUID embedded once in ELL; source round-trip. |
| Mutation | insert, modify, upsert-new, upsert-existing | Correct success/ProgId semantics; modify/upsert-existing preserve ProgId; artifact pair refreshed. |
| Execution mode | single CLI, manifest/stdin batch, directory batch, serve single, Python batch-over-serve | COM initialized once per owning worker lifetime; all results/protocols unchanged; subsequent operation succeeds after an earlier per-item compile error. |
| Compile failures | syntax error plus each type-specific semantic error | Nonzero / `compile_error`; real compiler text retained; no false verified success; cleanup rules per §L.4. |
| COM outcomes | `S_OK`, `S_FALSE`, `RPC_E_CHANGED_MODE`, generic failure; exception after COM success during later construction | success and exact balancing for first two; loud init failure and no orlib mutation for failures; no leaked COM count on unwind. |
| Artifact integrity | no pre-existing target, stale pre-existing pair on modify, induced compile/link failure | success requires fresh ELL/XML pair in tests; parse XML filename/GUID; GUID byte match in ELL; no trailing-GUID assumption; no orphan from failed insert. |
| Resolver/signatures | known baseline and version mismatch cases | new exact counts (24/11/3/10); every remaining signature unique; every remaining patch byte validates; version detection tiers unchanged. |
| Runtime | at minimum Strategy for causal gate; smoke all executable/visual types feasible in Desktop | Strategy actually executes without TSDev edit/reverify, before and after restart. Indicator/ShowMe plot, TradingApp launches, Function exercised through a verified caller where practical. |
| Lifecycle/stress | repeated sequential instances; hundreds of batch/serve ops | no second-instance signature failure, COM imbalance, stack-cleanup regression, crash reporter, handle/window leak, or stale artifacts. |

The release report must state which Desktop checks were automated versus
manually observed.

---

## 8. Atomic execution decomposition (for the future PM)

Per `AGENTS.md` hard rules: one subagent → one narrowly scoped subtask. No
multi-stage, multi-concern, or complex subtasks for any individual subagent.
Every substantive result must be adversarially reviewed by an independent
subagent (APPROVE/DISAPPROVE loop until APPROVED). The future PM must split
each TVS into separate narrow subagents for edit / build / test / review /
e2e. Use `universal` by default for narrow subtasks; fall back to
`subagent_general` only if `universal` fails, and reserve `subagent_explore`
for trivial read-only lookups. Suggested decomposition (the PM may refine):

- **Per TVS:**
  1. **Edit subagent(s):** one narrowly scoped code/test/doc change per
     subagent. For L.3, split the cleanup inventory into separate edit
     subagents (signature JSON, codegen counts, TsDevLib surface, resolver
     API+comments, resolver tests, persistence validation test repurpose,
     sigtool expectations, SIGNATURE_CATALOG, FEASIBILITY, PROJECT_HISTORY).
     Each edit subagent gets exactly one inventory item.
  2. **Build subagent:** build Win32 Release; report errors only (do not fix
     root cause — return to edit subagent).
  3. **Test subagent:** run the relevant unit/regression suites; report
     failures only.
  4. **Adversarial review subagent:** independent; receives the original
     subtask instructions and the result; looks for complete execution, no
     scope creep, no drift, no undue shortcuts/hacks. APPROVE or DISAPPROVE
     with findings. On DISAPPROVE, loop: edit subagent fixes → review again.
  5. **E2E subagent:** run the mandatory real CLI E2E per `AGENTS.md` and
     `docs/E2E_TEST_SPEC.md`; for L.5, include Desktop runtime acceptance.
     Report PASS/FAIL with evidence.
- **Each substantive result reviewed:** every edit, every test run, every
  E2E result, every doc change. No result is accepted without adversarial
  review.
- **Commits:** each numbered step in a TVS should be a separate reviewable
  commit/change unit where practical. Do not combine the immediate root fix
  with the parity follow-ups (research plan §L,
  `docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md`).

---

## 9. Exit criteria and rollback strategy

### 9.1 Exit criteria (Workstream L complete)
1. L.1 RED harness exists and was recorded as RED on the pre-fix baseline.
2. L.2 STA COM guard is in place across single/batch/serve with exact HRESULT
   semantics and same-thread `CoUninitialize` balancing; forced-failure tests
   pass.
3. L.3 persistence patch is fully removed; real finalization path runs;
   signature/resolver/test/doc cleanup is complete (24/11/3/10); no stale
   references; L.1 harness is GREEN with the real production binary.
4. L.4 persistence validation and partial-artifact cleanup/rollback are
   hardened; failed insert leaves no orphan; failed modify never deletes the
   existing component; absent/stale XML or GUID disagreement is rejected.
5. L.5 full verification matrix passes; a CLI-inserted Strategy executes in
   Desktop without TSDev edit/re-verify, before and after a Desktop restart;
   all 5 component types pass artifact + semantic tests; executable/visual
   types pass Desktop smokes where feasible; the release report documents
   automated vs. manual observations.
6. All adversarial reviews APPROVED; all mandatory real CLI E2E runs PASS.

### 9.2 Rollback strategy
- **Per-TVS rollback:** each TVS is a reviewable commit/change unit. If a TVS
  fails its adversarial review or E2E, revert the TVS's commits and re-enter
  the DISAPPROVE loop. Do not carry a broken TVS forward.
- **L.2 rollback:** if the COM guard breaks existing tests and cannot be
  resolved, revert the guard. The patch is still present in L.2, so reverting
  the guard restores the exact pre-L.2 behavior. Investigate the ordering
  issue before retrying.
- **L.3 rollback:** if removing the patch causes a crash or runtime failure
  that STA COM does not resolve in production (despite the 6689 probe), revert
  the patch removal and the cleanup. This is a decision gate: production
  divergence from the confirmed probe is new implementation evidence that may
  justify revisiting the causal boundary. Do NOT ship a half-removed state
  (patch removed but cleanup incomplete, or cleanup done but patch restored).
- **L.4 rollback:** if failed-modify behavior can replace a known-good pair,
  the modify/upsert-existing path is blocked until a confirmed TSDev/orlib
  recovery sequence and cross-store consistency proof are available. Do not
  ship direct artifact restoration as a workaround. Insert/upsert-new
  hardening can still ship if independently safe.
- **L.5 rollback:** if the CLI-inserted Strategy does not execute in Desktop
  without TSDev edit/re-verify, the workstream is NOT complete. Re-enter the
  DISAPPROVE loop and investigate where production diverges from the 6689
  probe.
- **No silent suppressions:** rollback never restores the bypass
  conditionally, adds compensating stack manipulation for the removed patch,
  or downgrades failures to warnings. A rollback is a revert to a known-good
  state, not a hack to force green.

### 9.3 Non-blocking open questions (must NOT trigger research before L.1)
- The exact persistent mismatch inside the otherwise structurally similar
  failing/working ELL and warehouse state, including semantic ownership of
  ELL blob GUID records beyond the confirmed XML-version match, is
  **non-blocking** and must not trigger more research before L.1 unless
  implementation evidence contradicts the approved causal boundary.
- Automating every Desktop UI/runtime check; a controlled manual release gate
  is acceptable if documented.
- Compiler factory/field parity, SaveModule flags/CELProperties, event
  `0x40A`, and reverse-dependent traversal (research plan §L follow-ups,
  `docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md`),
  provided regression tests show root insert/modify/upsert linkability.
- Whether XML should become a production postcondition. It is mandatory in
  E2E as a parity/completion marker; production enforcement depends on
  fault-injection results (L.4) and format-stability cost.

### 9.4 Known analysis caveat
`tmp/com_nopatch_probe.stdout.txt` reports a false GUID mismatch because its
first extractor used the trailing 16 bytes. The corrected analysis at
`tmp/com_nopatch_guid_analysis.json` (L1-L16) is authoritative. Any new test
must encode GUID bytes with Windows mixed-endian `GUID` layout and search the
complete ELL.
