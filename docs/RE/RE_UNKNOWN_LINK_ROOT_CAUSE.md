# TradeStation “Unknown link error” — root-cause analysis and implementation plan

> **Document status:** Durable tracked RE document (promoted from
> `tmp/unknown_link_root_cause_and_plan.md`). Research complete and
> adversarially approved (Round 2 APPROVED). The production fix is outside this
> research deliverable; no production source, signatures, tests, or documentation
> were changed as part of this research deliverable.
> **Final PM review:** Hands-on adversarial PM review APPROVED on 2026-08-23; implementation-plan consistency corrections were applied to RED-suite gating, rollback policy, behavioral finalization testing, and early production-runtime validation.
> **Current status:** Workstream L is closed because its team completed it and a
> final independent review was completed.
>
> The implementation-plan and evidence chronology below is historical provenance.
> Current Workstream M status is: M.1/M.2/M.3 are approved historical slices;
> M.3b is planned; M.4 implementation exists but is blocked by the CLI-created
> object-code defect and separate pre-existing review escalations; M.5 is pending.
> No approved post-TVS production-E2E claim exists for M.4.
> **Relationship to workstreams:** This document is the approved research
> base for **Workstream L** (`../workstream_unknown_link_fix_plan.md`, the
> immediate runtime-link root-cause fix). Its §L follow-up parity section is
> the source authority for **Workstream M**
> (`../workstream_tsdev_parity_plan.md`, TSDev verify/library parity
> follow-ups). The companion static evidence appendix is
> `RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md`. Promotion of this artifact (and the
> appendix) satisfied Workstream M's durable-evidence prerequisite gate for
> the citations it carries. The original hard-dependency wording was recorded
> before Workstream L's closure; current M statuses are maintained in
> `docs/PM_WORKING_MEMORY.md` §2 and `docs/workstream_tsdev_parity_plan.md`.
>
> Raw experiment paths under `tmp/` in this document are gitignored
> historical/transient artifacts, not tracked repository evidence. If a named
> artifact is absent, its evidence is unavailable; no replacement artifact or
> current result is implied. The raw line anchors are retained only for
> historical traceability.

**Scope:** research and planning only. **No production implementation was made in
this research deliverable.**
**Target baseline:** `orlib20.dll` 9.50.01.3344, SHA-256 `318DB24A…22EC18`, x86.
**Conclusion confidence:** **High for the production fix; bounded for the persistent byte-level mechanism.** A controlled A/B test proves that STA COM plus removal of the `FUN_10011230` bypass produces runtime-linkable output: the resulting strategy executed in TradeStation Desktop without a TSDev UI reverify. Static control flow proves that the bypass skips `FUN_10011230`'s compiler/link-finalization call, symbol processing, and symbol save. The exact persistent mismatch within the otherwise structurally similar failing and working ELL/warehouse state has not been isolated.

## A. Executive summary

The pre-L.3 CLI patched `orlib20!FUN_10011230` (RVA `0x11230`, preferred VA `0x10011230`) to `MOV EAX,1; RET 0x0C`, bypassing the original path while forcing success. Durable Ghidra evidence proves that the original function's ordered work includes a call to `FUN_1008E640` (RVA `0x8E640`, preferred VA `0x1008E640`), `SymInfo::AdjustOffsets`, and `SymWriter::Save`; `FUN_1008E640` is conservatively characterized as compiler/link-finalization behavior, not as the writer of any identified RNAM section ([appendix §2](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#2-complete-direct-call-inventory-and-the-skipped-work)). The controlled dynamic A/B proves that bypassing this path causes runtime-unlinkable output and that restoring it under STA produces runtime-linkable output ([Section E](#e-dynamic-ab-proof-and-runtime-validation)).

The byte comparison does **not** show missing native method/class table bodies or a structurally incomplete ELL. The ten compared metadata-table structural bodies plus `%Strings`, `%csStrings`, and `%Signatures` are byte-identical; the only semantic byte differences found are a timestamp and the `%Blobs` record-3 GUID (historical analysis report L15–L26 and L28–L45 (raw report transient/unavailable in the tracked checkout) — *transient/reproducible raw experiment artifact*). The static loader does compare a caller-supplied expected GUID with the loaded ELL GUID, but neither static evidence nor this comparison isolates the exact persistent mismatch in the failing ELL/warehouse state or proves that a warehouse expected-GUID mismatch caused this observed failure ([appendix §4](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#4-runtime-loader-ell-guid-check-null-return-xml-is-debugger-only), [appendix limits §7](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#7-evidentiary-conclusions-and-limits)).

Historically, the patch was introduced to avoid a native C++ exception (`0xE06D7363`) reached by the unpatched symbol writer. Static same-main-thread ordering in TSDev shows `CoInitialize(NULL)` before `_OrlibInit(NULL,1)` ([appendix §5](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#5-tsdev-com-initialization-and-orlib-initialization-order)); this was not observed in the Frida trace. Separately, the dynamic probe initialized the actual CLI worker with `CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)`, constructed `TsDevLib`, restored the original `FUN_10011230` bytes in memory, and inserted ProgId 6689. It exited normally, produced paired ELL/XML files with a matching embedded version GUID, and the user confirmed that strategy ran in Desktop without UI reverify. Thus **STA COM plus removal of the bypass remains the confirmed production fix**, even though the exact durable mismatch is still unisolated.

The production fix is therefore narrow:

1. establish an STA COM apartment on every thread that owns/uses `TsDevLib`, **before loading/initializing orlib**;
2. retain that apartment for the complete `TsDevLib`/orlib lifetime and balance it with `CoUninitialize` on the same thread;
3. remove the `FUN_10011230` persistence patch completely so the original finalization/symbol path runs;
4. fail loudly on COM initialization, compile, link, or persistence failure; and
5. add a regression that fails on today’s code by requiring a newly inserted strategy to have a newly generated ELL/XML pair whose XML version GUID occurs in the ELL, followed by a real Desktop execution gate.

XML is a useful completion marker and debugger/source-map artifact; **XML itself is not what Desktop links** ([appendix §4](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#4-runtime-loader-ell-guid-check-null-return-xml-is-debugger-only)). The proven causal statement is that the bypassed path produces runtime-unlinkable output; static evidence identifies the skipped finalization/symbol operations, while the precise persistent ELL/warehouse mismatch remains unisolated.

## B. Symptom and reproduction facts

1. Tool-inserted strategies appear verified in TSDev but fail in Desktop with “Unknown link error occurred.” Opening one, making a harmless edit, and re-verifying in TSDev fixes it ([§B](#b-symptom-and-reproduction-facts)).
2. TSDev disables Verify for an unmodified CLI-created item, consistent with the warehouse already carrying a verified state despite an unusable artifact ([§B](#b-symptom-and-reproduction-facts)).
3. UI Verify is command `WM_COMMAND 0x296A`; the trace captures it at historical trace L19395–L19396 (raw trace transient/unavailable in the tracked checkout) — *transient/reproducible raw experiment artifact*.
4. The UI path saves before compiling: `_SaveModule_New@20` enters/returns success at historical trace L20412–L20414 (raw trace transient/unavailable in the tracked checkout) and historical trace L20611–L20612 (raw trace transient/unavailable in the tracked checkout); `StartCompilingProcess` follows at historical trace L21662–L21663 (raw trace transient/unavailable in the tracked checkout), then `_VerifyModule_New@20` at historical trace L21682–L21683 (raw trace transient/unavailable in the tracked checkout). — *transient/reproducible raw experiment artifacts*
5. The full **static** TSDev call chain for command `0x296A` is `TSDev!FUN_0047B0E0 → FUN_00451800 → FUN_004D9480 → FUN_004D91E0 → orlib!_VerifyModule_New@20`, with every direct call instruction cited in the Ghidra appendix ([appendix §6](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#6-ui-verify-command-chain-and-the-omitted-trace-frame)). The cited dynamic backtrace actually shows `FUN_004D91E0`, `_VerifyModule_New@20`, `FUN_004D9480`, and `FUN_0047B0E0`; it does not show `FUN_00451800` (historical trace L21682–L21683 (raw trace transient/unavailable in the tracked checkout) — *transient/reproducible raw experiment artifact*). The broader UI flow includes save, a 0x58-byte compiler object, start/verify/dependent handling/end, event `0x40A`, refresh, and destruction ([§B](#b-symptom-and-reproduction-facts)).
6. Controlled ProgId 6688 used a space/backspace solely to make the editor dirty, leaving source unchanged. UI Verify rewrote the same-size 5101-byte ELL to a different hash and created the previously absent 1135-byte XML ([§E.1](#e1-a-historical-pre-l3-patched-behavior-superseded)).
7. A full Desktop restart did not repair a broken CLI item, excluding a purely live-cache explanation ([§B](#b-symptom-and-reproduction-facts)).

## C. Evidence-bounded causal chain

### C.1 Historical pre-L.3 source snapshot (superseded; not current behavior)

The following bullets preserve the pre-L.3 persistence-patch source snapshot. They
are historical provenance only; the current tree no longer applies that bypass
(the retired target is recorded in [`docs/SIGNATURE_CATALOG.md` §4.9](../SIGNATURE_CATALOG.md#49-target-9-retired-fun_10011230-persistence-patch--removed-in-tvs-l3)).

* The pre-L.3 `TsDevLib` constructor was documented as applying the persistence patch.
* The pre-L.3 replacement bytes were exactly `B8 01 00 00 00 C2 0C 00` (`MOV EAX,1; RET 0x0C`).
* In that source snapshot, construction loaded/resolved orlib and then called `applyPersistencePatch()` before `OrlibInit(NULL,1)`.
* In that source snapshot, `applyPersistencePatch()` resolved and overwrote `FUN_10011230`; the historical machine-readable signature described the purpose as “bypasses XmlDocument.Save crash” and recorded RVA `0x11230` and the same bytes.
* The corresponding historical `verifyModule()` path allocated a 0x58-byte compiler, started compilation, called `_VerifyModule_New@20`, and treated return 1/no collected errors as success.

### C.2 Binary control flow and its limits

Durable static analysis of baseline `orlib20.dll` establishes:

1. `CELBasicCompiler::CompileText` has exactly one direct call to `FUN_10011230`, at `0x1001E0BC` ([appendix §1](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#1-compiletext---fun_10011230-actual-cleanup-contract), [appendix §2](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#2-complete-direct-call-inventory-and-the-skipped-work));
2. the unpatched `FUN_10011230` constructs symbol information, directly calls `FUN_1008E640` at `0x100113D2`, then calls `SymInfo::AdjustOffsets` and `SymWriter::Save` in that order ([appendix §2](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#2-complete-direct-call-inventory-and-the-skipped-work));
3. `FUN_1008E640` spans `0x1008E640-0x1008F777` and supports the bounded description **compiler/link-finalization routine**. The evidence does not attribute symbol-tree construction to it and does not identify it as the writer of any particular RNAM section ([appendix §2](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#2-complete-direct-call-inventory-and-the-skipped-work));
4. the entry-point replacement skips the entire original function, including that finalization call and symbol-save path, and forces `EAX=1`; on the original success edge, `CompileText` later executes `ModuleInfo::SetGuid` and `MDVault::SaveModule` ([appendix §3](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#3-compiletext-after-fun_10011230-failure-versus-ell-persistence)); and
5. the pre-L.3 replacement changed the original stack-cleanup contract: the caller executes `ADD ESP,0x0C` after the call (`0x1001E0C1`), the original callee ends in plain `RET` (`0x10011454`), and the replacement ends in `RET 0x0C` ([appendix §1](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#1-compiletext---fun_10011230-actual-cleanup-contract)). The pre-L.3 catalog narrative that labeled this as callee-cleanup `__fastcall` was superseded during patch cleanup. This is a static ABI hazard only; there is no claim that an observed historical crash was caused by it.

Static analysis does **not** prove that the bypass removes native method/class table bodies or yields a structurally incomplete ELL. Dynamic A/B evidence, rather than disassembly alone, proves that the bypassed path is causal for runtime unlinkability. The byte comparison found structurally similar ELLs whose compared semantic bodies are identical except for timestamp/GUID values; the exact durable ELL/warehouse mismatch remains unidentified ([appendix evidentiary limits §7](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#7-evidentiary-conclusions-and-limits)).

### C.3 Why the patch appeared necessary

The old standalone unpatched path raised native C++ exception code `0xE06D7363`. The prior explanation (“partially initialized CLR”) was not established. The evidence is:

* `SymWriter::Save` uses ECL’s MSXML COM wrapper;
* **static Ghidra ordering:** TSDev's synchronous UI/main application-initialization routine calls `CoInitialize(NULL)` and later reaches `_OrlibInit(NULL,1)` with no worker dispatch in that chain ([appendix §5](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#5-tsdev-com-initialization-and-orlib-initialization-order)); a separate TSDev worker-style routine also explicitly brackets work with `CoInitialize(NULL)`/`CoUninitialize` ([appendix §5](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#5-tsdev-com-initialization-and-orlib-initialization-order));
* **dynamic probe evidence:** STA initialization on the actual CLI worker plus restoration of the original function allowed insert, paired artifact production, normal exit, and successful Desktop execution (Section E.2); and
* The current `TsDevLib::TsDevLib` constructor initializes STA COM on the
  owning thread before loading orlib (`src/tsdev_lib.cpp`).

The Frida trace did not hook or observe TSDev's `CoInitialize` call; the
placement claim above is static same-main-thread evidence, not dynamic trace
evidence. The CLI runs library work in the thread-entry functions `runInsert`,
`runModify`, `runUpsert`, `runBatch`, and `runServe` in `src/tsdev_cli.cpp`; each
constructs `TsDevLib`, whose constructor owns COM initialization on that worker.

### C.4 Resulting user-visible failure

The historical evidence supports this bounded sequence: `save source → compile reaches patched FUN_10011230 → bypass forces return 1 and skips original finalization/symbol work → persistence/verified state proceeds → TSDev disables Verify → Desktop cannot load/link the resulting output → Unknown link error`.

A harmless UI edit re-enables Verify; TSDev then runs its original path and Desktop runs the rewritten artifact. The failing and working ELLs are otherwise structurally similar in the compared regions: all ten metadata-table structural bodies and the three string/signature heaps are byte-identical, with semantic differences limited to timestamp and GUID blob values (historical analysis report L15–L26 and L28–L45 (raw report transient/unavailable in the tracked checkout) — *transient/reproducible raw experiment artifact*). Static ELRte analysis proves an expected-GUID comparison and rejection edge, but the observed failure's precise ELL/warehouse mismatch—and which producer, if any, is wrong—has not been isolated ([appendix §4](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#4-runtime-loader-ell-guid-check-null-return-xml-is-debugger-only)).

## D. Why TSDev UI succeeds and CLI fails

| Concern | TSDev UI | Current CLI | Consequence |
|---|---|---|---|
| Trigger | `WM_COMMAND 0x296A` | Direct operation methods | Different outer orchestration, but not itself causal. |
| Save ordering | `_SaveModule_New@20` before compile | `saveSource()` before `verifyModule()` | Broadly aligned. |
| Thread | TSDev verify/UI thread | Dedicated 64 MiB worker | Both are thread-affine. |
| COM | Static same-main-thread order: `CoInitialize(NULL)` before `_OrlibInit` ([appendix §5](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#5-tsdev-com-initialization-and-orlib-initialization-order)) | `TsDevLib` initializes STA COM before loading orlib (`src/tsdev_lib.h`, `TsDevLib` same-thread contract) | The current CLI satisfies the prerequisite for the restored path. |
| `FUN_10011230` | Original function | Original function; the persistence bypass was removed in L.3 | The current CLI runs the statically identified compiler/link-finalization and symbol operations. |
| Return semantics | Original finalization result | Original finalization result | The current CLI no longer forces `EAX=1` through the removed bypass. |
| Post-success parity | dependent handling, event `0x40A`, refresh | Implemented parity paths, with M.4 still blocked | Follow-up parity remains distinct from the durable linkability cause. |

Single insert constructs `TsDevLib` directly in `runInsert`; modify and upsert do
the same in `runModify` and `runUpsert`. Batch owns one instance for the whole
loop in `runBatch`, and serve owns one instance for the whole broker session in
`runServe`. These worker functions are in `src/tsdev_cli.cpp`; the `TsDevLib`
constructor initializes STA COM before loading orlib, as documented by the
`TsDevLib` same-thread contract in `src/tsdev_lib.h`.

## E. Dynamic A/B proof and runtime validation

### E.1 A: historical pre-L.3 patched behavior (superseded)

* ProgId 6688 initially had only `elmodule_0_6688.ell`; UI Verify subsequently created XML at the same time as the rewritten ELL ([§E.1](#e1-a-historical-pre-l3-patched-behavior-superseded)).
* The UI-produced XML identifies `file="elmodule_0_6688"` and version GUID `{65B9F36C-1925-44A8-988A-F4AE52241916}` (historical/transient `tmp/artifact_after_ui_verify.xml` lines 1–8; unavailable in the tracked checkout — *transient/reproducible raw experiment artifact*).
* ELL comparison found unchanged semantic table bodies and string/signature heaps; changed semantic bytes were timestamp/GUID blob values, not a `0xA02 → 6688` identity correction (historical analysis report L15–L26 and L28–L45 (raw report transient/unavailable in the tracked checkout) — *transient/reproducible raw experiment artifact*).

### E.2 B: STA COM plus original function

The temporary probe’s actual worker executes `ComApartment apartment; TsDevLib library; restoreOriginalPersistenceBytes();` before insertion (historical/transient `tmp/com_unpatched_probe/com_unpatched_probe.cpp` lines 533–545; unavailable in the tracked checkout — *transient/reproducible raw experiment artifact*). Its guard calls `CoInitializeEx(..., COINIT_APARTMENTTHREADED)`, accepts only `S_OK`/`S_FALSE`, and balances successful calls with `CoUninitialize` (historical/transient raw probe lines 47–74; unavailable in the tracked checkout).

Observed results:

* STA initialization returned `S_OK`; original loaded bytes matched disk and were restored; insertion succeeded as ProgId 6689; worker exited 0 (historical/transient `tmp/com_nopatch_probe.stdout.txt` lines 1–15 and 27; unavailable in the tracked checkout — *transient/reproducible raw experiment artifact*).
* New ELL (5099 bytes) and XML (1135 bytes) both existed (historical/transient probe stdout lines 16–23; `tmp/com_nopatch_probe.stdout.txt` unavailable in the tracked checkout); source round-tripped byte-for-byte and neither artifact pre-existed (historical/transient artifact inspection lines 3–7; `tmp/com_nopatch_artifact_inspection.json` unavailable in the tracked checkout — *transient/reproducible raw experiment artifacts*).
* The probe’s first printed `guid.match=false` was an **inspection bug**, not an artifact mismatch: it incorrectly treated the ELL’s trailing 16 zero bytes as the version GUID (historical/transient probe stdout lines 24–26; `tmp/com_nopatch_probe.stdout.txt` unavailable in the tracked checkout). Correct follow-up parsing found the XML GUID exactly once in Windows GUID byte layout at ELL offset decimal 5062 (`0x13C6`) (historical/transient `tmp/com_nopatch_guid_analysis.json` lines 1–16; unavailable in the tracked checkout — *transient/reproducible raw experiment artifact*). The XML version is `{314164A7-DCB4-45D9-BE2D-B6086227CA2E}` (historical/transient `tmp/com_nopatch_6689.xml` lines 1–8; unavailable in the tracked checkout — *transient/reproducible raw experiment artifact*).
* The user then ran `TS_AUTO_COM_NOPATCH_01` in Desktop successfully **without** opening or re-verifying it in TSDev ([§E.2](#e2-b-sta-com-plus-original-function)).

This A/B changes the two production-fix conditions together (STA added, bypass removed) and proves the combined fix: the bypassed path produces runtime-unlinkable output, while the restored path under STA produces output that executes in Desktop. Static analysis identifies the restored work as `FUN_10011230`'s compiler/link-finalization call and symbol processing/save ([appendix §2](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#2-complete-direct-call-inventory-and-the-skipped-work)). It does not isolate which timestamp/GUID-related persistent state difference makes the failing output unlinkable.

## F. Disproved or non-causal hypotheses

1. **Compiler `+0x10 = 0xA02` causes wrong compiled identity — disproved for the current direct call.** `_VerifyModule_New` uses the explicit, correctly passed ProgId through lookup, `CompileText`, token writer, metadata, and persistence. `+0x10` is consumed by TSDev’s outer recursive wrapper, which CLI bypasses. ELL analysis found neither LE `0xA02` nor LE 6688 identity replacement ([`docs/M1_HANDOFF.md` §R-M.1.2](../M1_HANDOFF.md); historical analysis report L28–L45 — raw report transient/unavailable in the tracked checkout). The former field-assignment mismatch was corrected by M.1; the current TSDev-parity writes are in `TsDevLib::verifyModule()` in `src/tsdev_lib.cpp`.
2. **XML alone is required by Desktop — false.** XML is symbols/source-map data. Its absence is a highly visible marker that the original path was skipped; ELRte/ELMDCore runtime loading uses the ELL, while the XML reader identified here is debugger-side ([appendix §4](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#4-runtime-loader-ell-guid-check-null-return-xml-is-debugger-only)). The successful result is explained by the combined STA-plus-no-bypass A/B, not by XML presence alone.
3. **Event `0x40A`/live refresh is the durable root — disproved.** Restarting Desktop did not fix a broken item. `0x40A` remains a parity requirement for already-running consumers, not the durable link fix ([§B](#b-symptom-and-reproduction-facts)).
4. **Close/flush/cache timing alone — disproved as root.** A process restart also reloads durable state and still failed. Close semantics can affect visibility and cleanup, but the evidence does not attribute the failure to missing native code or structurally absent ELL tables.
5. **Dependency wrapper is necessary for root strategy linkability — disproved.** The outer wrapper propagates signature/contract changes to reverse dependents; direct verification of an independent Strategy is linkable under the confirmed STA-plus-original-path conditions ([`docs/RE/RE_M4_REVERSE_DEPENDENT_REVALIDATION.md` §1.2](RE_M4_REVERSE_DEPENDENT_REVALIDATION.md); [§E.2](#e2-b-sta-com-plus-original-function)).
6. **The non-NULL password-shaped/internal metadata argument is link-related — false.** It controls internal source encryption/protection behavior, not final linking, and is not a user password feature ([`docs/RE/RE_M2_SAVEMODULE_NEW.md` §6](RE_M2_SAVEMODULE_NEW.md)).

## G. Production fix design

### G.1 COM ownership and exact lifecycle

Add a small non-copyable RAII STA guard owned by `TsDevLib` (preferably an incomplete private implementation member so COM mechanics do not leak into the public API):

* Construct it as the **first effective action/member** of `TsDevLib`, before `SetDllDirectoryW`, `LoadLibraryW`/`OrlibResolver`, support DLL loads, patch resolution, or `_OrlibInit`.
* Call `CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)`.
* Treat `S_OK` and `S_FALSE` as success. Each successful call must receive one `CoUninitialize`, including `S_FALSE`.
* Treat `RPC_E_CHANGED_MODE` as a hard initialization failure with a specific diagnostic: the owning thread was already initialized incompatibly (normally MTA). Treat every other failed HRESULT as a hard initialization failure and include the hexadecimal HRESULT.
* Keep the guard alive until after hidden windows are destroyed and `OrlibExit` has completed. Member RAII must also unwind/balance COM if any later constructor step throws.
* Assert/document the existing contract: construct, use, and destroy a `TsDevLib` on one thread. Never initialize COM on the main thread for work later performed on another thread.

Owning the guard in `TsDevLib`, rather than repeating calls in CLI entry points, covers all current and future library consumers and prevents omission in tests/tools. It still places initialization on the actual worker because each current worker constructs `TsDevLib` itself.

Exact mode behavior:

* **Single insert/modify/upsert:** the per-command worker enters STA through
  `TsDevLib` construction, performs the operation, destroys `TsDevLib`/calls
  `OrlibExit`, then leaves STA. The current worker functions are `runInsert`,
  `runModify`, and `runUpsert` in `src/tsdev_cli.cpp`.
* **Batch:** one STA initialization at `runBatch` thread/session start, one
  `TsDevLib` across all items, one `CoUninitialize` after final close/`OrlibExit`;
  not per item. The current ownership is implemented by `runBatch` in
  `src/tsdev_cli.cpp`.
* **Serve:** one STA initialization on `runServe` before the long-lived
  `TsDevLib`, retained across clients/requests, released only after final
  warehouse close and `OrlibExit`. Do not initialize per request. The current
  ownership is implemented by `runServe` in `src/tsdev_cli.cpp`.
* **Other library-backed commands:** they inherit the same correct prerequisite. This avoids an inconsistent library contract even where no compile occurs.

### G.2 Remove the causal patch

Remove, rather than disable behind a flag:

* patch byte constants and validation helpers;
* constructor call to `applyPersistencePatch()`;
* cached persistence RVA/original-byte members and destructor restoration branch;
* resolver convenience API specific to that patch;
* signature/catalog/generated/test entries for `FUN_10011230_persistence_patch`.

Keep the unrelated, still-required warehouse thread-check patches. Update patch bookkeeping so it represents only those sites. Do not retain a fallback that returns success after link/symbol failure.

### G.3 Python wrapper implications

No Python protocol or public method change is required. Direct non-serve calls spawn the fixed CLI; `use_serve=True` uses the fixed long-lived serve worker; Python batch-over-serve loops over requests but shares that worker/session. Preserve current JSON schemas and exit-code mapping. COM failures should arrive as existing `init_failure`/exit 4 responses; broker startup failures should continue to propagate as broker-level errors. Add Python tests only to verify routing/error propagation remains unchanged; do not initialize COM in 64-bit Python because it is not the thread/process loading 32-bit orlib.

## H. Historical Thin Vertical Slice implementation plan (TDD, red → green)

The following plan records the pre-L.3 implementation sequence; current Workstream
L is closed and the retained procedure/results are historical provenance.

### H.1 Historical RED: add the causal regression first

1. Add a focused live E2E test that inserts a unique, minimal Strategy through the **production CLI**.
2. Before insert, assert neither target ELL nor XML exists for the assigned test entry (or use a unique name and discover ProgId from JSON).
3. After reported success, assert:
   * ELL and XML both exist and were created/updated by this operation;
   * XML parses as `<symbols file="elmodule_0_<ProgId>" version="{GUID}">`;
   * that GUID’s 16 Windows-layout bytes occur exactly once in the ELL (do not use “last 16 bytes”; the corrected 6689 offset proves that assumption wrong);
   * source round-trips unchanged; and
   * cleanup removes the warehouse entry and both artifacts.
4. The pre-L.3 run was to be performed against production code and was
   expected to record RED because XML was absent (and therefore the GUID check
   could not pass). That result is historical/transient, not current acceptance
   evidence. The current harness documents the post-L.3 desired-state checks;
   L7 fault injection remains opt-in.

### H.2 GREEN: smallest causal implementation

5. Add unit tests for an injectable/narrow COM apartment guard: `S_OK`, `S_FALSE`, `RPC_E_CHANGED_MODE`, generic failed HRESULT, exactly-once balancing, and constructor-unwind balancing. Keep production calls unsuppressed and test via a seam/function wrapper rather than process-global apartment manipulation in a shared test runner.
6. Implement the guard and place it before all orlib loading/initialization in `TsDevLib`; link `Ole32` explicitly if not already transitively linked.
7. Remove the persistence patch from `TsDevLib` and resolver APIs/bookkeeping. Do not alter unrelated thread-check patches.
8. Update signature JSON, generator counts, generated output flow, resolver tests, sigtool tests, and catalog together (Section K).
9. Build Win32 Release and run the causal harness. It must turn GREEN with no process-local restoration trick; only then register it in the standard regression runner and prove the complete registered suite remains green. Before accepting the causal implementation, execute one Strategy inserted by the final production CLI in Desktop without TSDev reverify; the later release matrix repeats this with restart and wider coverage.

### H.3 Harden without broadening the causal TVS

10. Extend the causal test to modify and upsert-existing/new paths, batch, and serve; assert per-operation artifact freshness and GUID agreement.
11. Run existing compile-error tests and add post-failure artifact/warehouse assertions (Section J).
12. Run the full resolver/signature/unit/regression suites, Python non-E2E and serve tests, then the project’s required real CLI E2E suite.
13. Execute the new strategy in real Desktop without opening/re-verifying in TSDev. A successful order/strategy calculation is the release gate; merely adding it to a chart is insufficient if execution can be observed directly.
14. Independently adversarially review implementation and evidence. Fix findings and repeat until approved.

Each numbered step should be a separate reviewable commit/change unit where practical. Do not combine immediate root fix with the parity follow-ups in Section L.

## I. Verification matrix

Use the valid type-specific source fixtures and semantic errors defined by
`docs/E2E_TEST_SPEC.md` §4.1 ("Test source table") and §5.1 ("Test case table"),
with the component-type constraints in §10 ("Reference: Known Type-Specific
Constraints").

| Dimension | Required cases | Required assertions |
|---|---|---|
| Component type | Strategy, Indicator, Function, TradingApp, ShowMe | Successful valid compile; correct expanded filename type; fresh ELL/XML; XML GUID embedded once in ELL; source round-trip. |
| Mutation | insert, modify, upsert-new, upsert-existing | Correct success/ProgId semantics; modify/upsert-existing preserve ProgId; artifact pair refreshed. |
| Execution mode | single CLI, manifest/stdin batch, directory batch, serve single, Python batch-over-serve | COM initialized once per owning worker lifetime; all results/protocols unchanged; subsequent operation succeeds after an earlier per-item compile error. |
| Compile failures | syntax error plus each type-specific semantic error | Nonzero/`compile_error`; real compiler text retained; no false verified success; cleanup rules below. |
| COM outcomes | `S_OK`, `S_FALSE`, `RPC_E_CHANGED_MODE`, generic failure; exception after COM success during later construction | success and exact balancing for first two; loud init failure and no orlib mutation for failures; no leaked COM count on unwind. |
| Artifact integrity | no pre-existing target, stale pre-existing pair on modify, induced compile/link failure | success requires fresh ELL/XML pair in tests; parse XML filename/GUID; GUID byte match in ELL; no trailing-GUID assumption; no orphan from failed insert. |
| Resolver/signatures | known baseline and version mismatch cases | new exact counts; every remaining signature unique; every remaining patch byte validates; version detection tiers unchanged. |
| Runtime | at minimum Strategy for causal gate; smoke all executable/visual types feasible in Desktop | Strategy actually executes without TSDev edit/reverify. Indicator/ShowMe plot, TradingApp launches, Function is exercised through a verified caller where practical. |
| Lifecycle/stress | repeated sequential instances; hundreds of batch/serve ops | no second-instance signature failure, COM imbalance, stack-cleanup regression, crash reporter, handle/window leak, or stale artifacts. |

The release report must state which Desktop checks were automated versus manually observed. The confirmed 6689 run is proof of direction, not a substitute for testing the final production binary.

## J. Error handling and rollback/cleanup semantics

1. **COM failure occurs before mutation.** On `RPC_E_CHANGED_MODE` or another failed HRESULT, do not load/init orlib, create windows, create entries, or touch ELL/XML. Return existing init failure semantics with HRESULT detail. Never continue in MTA and never fall back to the patch.
2. **Compile/link failure is failure.** Preserve compiler diagnostics; if the original linker/symbol path returns failure or raises an SEH/C++ exception, return non-success. Do not force return 1, suppress exceptions, downgrade warnings/errors, or claim verification.
3. **Failed insert/upsert-new:** the current CLI routes post-create failures
   through the single `cleanupCreatePathFailure`/`doInsert` owner in
   `src/tsdev_cli.cpp`; tests must require that both target `.ell` and `.xml`
   are absent afterward. Cleanup errors must be reported alongside—not replace—
   the primary compile/link diagnostic.
4. **Failed modify/upsert-existing:** never delete the existing warehouse component. First characterize the unpatched TSDev/orlib failure and recovery sequence, including artifact and warehouse/property state. Reproduce a confirmed TSDev/orlib transaction or recovery API rather than inventing direct file backup/restore, which may desynchronize cross-store state and violate the emulate-TSDev rule. If no safe confirmed recovery sequence exists, keep the path blocked instead of shipping a file-level workaround. Preserve TSDev's source-state behavior without parsing source or recreating metadata.
5. **Absent/stale pair on success path:** a production operation must never silently report success when the expected ELL/XML persistence markers are absent or stale. The immediate guarantee comes from using the original return path; E2E must additionally reject an absent/stale XML or disagreement between its version GUID and the ELL. If fault injection proves the original function can return success with absent/stale files, promote these checks into production postconditions using version-scoped parsing; otherwise keep binary-format parsing in tests to minimize runtime coupling.
6. **Close/flush failure:** report it if an API provides a detectable failure. Do not delete known-good modify artifacts merely because final close is uncertain. Preserve enough path/ProgId detail for recovery.
7. **Cleanup ordering:** on normal worker exit: finish operation → close warehouse where mode requires → destroy `TsDevLib` (`OrlibExit`, windows) → `CoUninitialize` on the same thread. Constructor unwind must release only resources successfully acquired.
8. **No suppressions/hacks:** do not catch and ignore COM/link failures, restore the bypass conditionally, add compensating stack manipulation for the removed patch, or parse mutable source text for identity/cleanup. Names, types, and ProgIds remain warehouse/API-derived.

## K. Historical pre-L.3 signatures, resolver, tests, and documentation plan

At the time this plan was written, the persistence patch was one of 25
signatures/12 patch sites, and codegen enforced exactly 25 signatures, 3 globals,
and 10 exports. These are historical plan values. The numbered actions below
are retained as provenance, not as current work instructions; current catalog
and codegen facts are maintained in `docs/SIGNATURE_CATALOG.md` and
`tools/sigcodegen.py`.

1. The historical cleanup removed `FUN_10011230_persistence_patch` from the
   versioned signature set. The current JSON has no entry with that name; the
   retired-target note in `docs/SIGNATURE_CATALOG.md` records the stable-ID gap.
2. The historical 25-to-24 signature-count transition was superseded by later
   catalog changes. Current codegen enforces 22 signature definitions, 3 data
   globals, and 13 tracked exports; generated output is produced by the normal
   codegen flow.
3. The current `TsDevLib` surface (`src/tsdev_lib.h` and `src/tsdev_lib.cpp`)
   has no persistence-patch validation helper, persistence RVA/original-byte
   member, or `applyPersistencePatch()` call. Generic bookkeeping remains for
   the 11 unrelated thread-check patches.
4. The current `OrlibResolver` surface (`src/orlib_resolver.h` and
   `src/orlib_resolver.cpp`) has no `resolvePersistencePatch()` API or
   persistence-specific resolver path; generic resolution and thread-check
   patch machinery remain.
5. The historical resolver-test cleanup replaced positional persistence-target
   assumptions with current catalog-aware checks in
   `tests/test_orlib_resolver.cpp`; current tests must continue to prefer name
   lookup over signature-array positions.
6. A dedicated persistence-validation test was proposed in the historical
   plan, but no current file with that role/name is present. Current
   behavior-based finalization, artifact, and cleanup coverage is in
   `tests/test_unknown_link_regression.ps1`; no current test claim is made for
   the absent historical proposal.
7. The historical sigtool expectations and comments were superseded. Current
   checks are in `tests/test_sigtool.ps1` and are driven by the current catalog
   and `tools/sigcodegen.py`, not by positional indices.
8. The current catalog retains the retired target ID as a documented gap and
   records the corrected ABI narrative: the original callee uses plain `RET`,
   the caller performs `ADD ESP,0x0C`, and the historical replacement used
   `RET 0x0C`. The durable source for that finding is
   `docs/RE/RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md` §1.
9. Current documentation distinguishes the runtime-unlinkable pre-L.3 bypass
   from the STA-COM-plus-original-path fix. Historical documents preserve their
   chronology under dated correction/status notices; no current guidance treats
   XML omission or the removed bypass as acceptable.
10. Version detection remains based on manifest identity, PE timestamp/file size,
    and SHA-256 of the unmodified DLL, not signature-array length. The current
    Tier 1/2/3 checks remain the verification source.

## L. Historical follow-up TSDev parity work — explicitly separate from the immediate fix

The following was the pre-M.1–M.3 follow-up plan. Current Workstream M status is maintained in
`docs/PM_WORKING_MEMORY.md` §§2–4 and `docs/workstream_tsdev_parity_plan.md`; the
historical bullets are retained for provenance.

1. **Compiler object setup/factory:** TSDev sets category at `+0x08`, subtype at `+0x0C`, current ProgId at `+0x10`, notification HWND at `+0x18`, and clears `+0/+4`; at the time of this plan, the code hard-coded `+0x10=0xA02`, omitted category/subtype, and wrote 6 into the empty inline string area at `+0x28` (historical baseline recorded in [`docs/M1_HANDOFF.md` §R-M.1.2–§R-M.1.3`](../M1_HANDOFF.md)). The M.1 handoff records the later ordinal 66/67 lifecycle correction.
2. **SaveModule parity:** reproduce TSDev’s non-NULL flags, internal metadata/encryption buffer, and CELProperties state after parameter semantics are confirmed. This is not a user password feature and is not the link fix.
3. **Post-success `0x40A`:** reproduce live notification for the actual registered TradeStation consumers; do not assume TSDev.exe is a receiver. It cannot replace durable linking.
4. **Reverse-dependent reverify:** reproduce the behavior of TSDev.exe’s dependent-program traversal with supported orlib/warehouse primitives so changed Function/contract definitions revalidate callers. Do not couple the standalone CLI to private TSDev.exe code. This is not required to make the root Strategy itself linkable.

The historical plan did not hold the immediate fix for these items unless the
new causal E2E exposed a direct dependency.

> **Current status note:** The four items above are the historical source plan
> for Workstream M (`../workstream_tsdev_parity_plan.md`). Current status is:
> M.1/M.2/M.3 are approved historical slices; M.3b is planned; M.4
> implementation exists but is blocked by the CLI-created object-code defect and
> separate pre-existing review escalations; M.5 is pending. Workstream L is closed
> because its team completed it and a final independent review was completed.

## M. Historical risks and open questions

The risks and open questions below are retained from the historical L plan; they
are not current Workstream M status. Current M status is maintained in
`docs/workstream_tsdev_parity_plan.md`.

### Historical blockers before production release

* **Final production binary runtime gate:** a strategy inserted by the STA-initialized production CLI with the bypass removed must execute in Desktop without UI reverify. The probe proves the combined fix, not the final integration.
* **Failure-state characterization for modify:** determine whether an unpatched late failure can damage a prior ELL/XML pair or alter warehouse GUID state; neither behavior is currently established. If observed, implement a confirmed transactional rollback sequence; artifact-only restore must not be shipped if it can create GUID inconsistency.
* **COM guard unwind correctness:** constructor failures after `S_OK`/`S_FALSE` must balance exactly once on the same thread; forced failure tests are mandatory.

### Historical non-blockers for the immediate causal TVS

* The exact persistent mismatch inside the otherwise structurally similar failing/working ELL and warehouse state, including semantic ownership of ELL blob GUID records beyond the confirmed XML-version match.
* Automating every Desktop UI/runtime check; a controlled manual release gate is acceptable if documented.
* Compiler factory/field parity, SaveModule flags/CELProperties, event `0x40A`, and reverse-dependent traversal (Section L), provided regression tests show root insert/modify/upsert linkability.
* Whether XML should become a production postcondition. It is mandatory in E2E as a parity/completion marker; production enforcement depends on fault-injection results and format-stability cost.

### Known analysis caveat

`tmp/com_nopatch_probe.stdout.txt` reports a false GUID mismatch because its first extractor used the trailing 16 bytes. The corrected analysis is authoritative (historical/transient `tmp/com_nopatch_guid_analysis.json` lines 1–16; unavailable in the tracked checkout — *transient/reproducible raw experiment artifact*). Any new test must encode GUID bytes with Windows mixed-endian `GUID` layout and search the complete ELL.

## N. Evidence index

### Primary factual record

* [Durable Ghidra evidence appendix — control flow, ABI, runtime loader, COM ordering, UI call chain, and explicit limits](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md)
* [Current workstream status and audit — `docs/PM_WORKING_MEMORY.md` §§2–4](../PM_WORKING_MEMORY.md#2-workstream-status)
* UI Verify command — historical `tmp/frida_trace_verify.log` L19395–L19396
  (transient/unavailable in the tracked checkout).
* UI SaveModule — historical trace L20412–L20414 and L20611–L20612
  (raw `tmp/frida_trace_verify.log` transient/unavailable in the tracked checkout).
* UI Start/Verify — historical trace L21662–L21663 and L21682–L21683
  (raw `tmp/frida_trace_verify.log` transient/unavailable in the tracked checkout).

### Artifact evidence (historical/transient raw experiment artifacts under `tmp/`)

The following artifacts are untracked and unavailable in a fresh tracked checkout;
their names and findings are retained for chronology only:

* 6688 ELL structured comparison report: `tmp/artifact_ui_verify_analysis/report.txt`
* 6688 exact byte ranges: `tmp/artifact_ui_verify_analysis/focused_diff.txt`
* 6688 machine-readable complete comparison: `tmp/artifact_ui_verify_analysis/diff.json`
* 6688 UI-generated XML: `tmp/artifact_after_ui_verify.xml`
* 6689 probe stdout: `tmp/com_nopatch_probe.stdout.txt`
* 6689 artifact/source inspection: `tmp/com_nopatch_artifact_inspection.json`
* 6689 corrected GUID analysis: `tmp/com_nopatch_guid_analysis.json`
* 6689 ELL: `tmp/com_nopatch_6689.ell`; XML: `tmp/com_nopatch_6689.xml`
* Temporary probe COM guard and worker sequence: `tmp/com_unpatched_probe/com_unpatched_probe.cpp`
  lines 47–74 and 533–571

### Historical pre-L.3 patch evidence and current source/catalog references

* [Historical patch bytes — durable Ghidra evidence appendix §1](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#1-compiletext---fun_10011230-actual-cleanup-contract)
* [Historical constructor/destructor patch behavior — signature catalog §4.9](../SIGNATURE_CATALOG.md#49-target-9-retired-fun_10011230-persistence-patch--removed-in-tvs-l3)
* [Current verify flow/compiler fields — `TsDevLib::verifyModule()`](../../src/tsdev_lib.cpp)
* [Current codegen exact counts — `EXPECTED_*_COUNT` constants](../../tools/sigcodegen.py)
* [Current human signature catalog totals — "Current tracked catalog totals after M.3"](../SIGNATURE_CATALOG.md)
* [Current worker ownership — `runInsert`, `runModify`, `runUpsert`, `runBatch`, and `runServe`](../../src/tsdev_cli.cpp)

---

**Implementation status:** none. Production source, signatures, tests, and documentation were not changed as part of this research deliverable. This document is the durable tracked record of the approved research; the production fix is owned by Workstream L (`../workstream_unknown_link_fix_plan.md`), and the §L parity follow-ups are owned by Workstream M (`../workstream_tsdev_parity_plan.md`).
