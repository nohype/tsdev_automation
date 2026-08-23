# TradeStation “Unknown link error” — root-cause analysis and implementation plan

> **Document status:** Durable tracked RE document (promoted from
> `tmp/unknown_link_root_cause_and_plan.md`). Research complete and
> adversarially approved (Round 2 APPROVED). **Production fix not
> implemented** — no production source, signatures, tests, or documentation
> were changed as part of this research deliverable.
> **Final PM review:** Hands-on adversarial PM review APPROVED on 2026-08-23; implementation-plan consistency corrections were applied to RED-suite gating, rollback policy, behavioral finalization testing, and early production-runtime validation.
> **Relationship to workstreams:** This document is the approved research
> base for **Workstream L** (`../workstream_unknown_link_fix_plan.md`, the
> immediate runtime-link root-cause fix). Its §L follow-up parity section is
> the source authority for **Workstream M**
> (`../workstream_tsdev_parity_plan.md`, TSDev verify/library parity
> follow-ups). The companion static evidence appendix is
> `RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md`. Promotion of this artifact (and the
> appendix) satisfies Workstream M's durable-evidence prerequisite gate for
> the citations it carries; it does **not** weaken Workstream M's hard
> dependency on Workstream L being approved and merged.

**Scope:** research and planning only. **No production implementation was made.**
**Target baseline:** `orlib20.dll` 9.50.01.3344, SHA-256 `318DB24A…22EC18`, x86.
**Conclusion confidence:** **High for the production fix; bounded for the persistent byte-level mechanism.** A controlled A/B test proves that STA COM plus removal of the `FUN_10011230` bypass produces runtime-linkable output: the resulting strategy executed in TradeStation Desktop without a TSDev UI reverify. Static control flow proves that the bypass skips `FUN_10011230`'s compiler/link-finalization call, symbol processing, and symbol save. The exact persistent mismatch within the otherwise structurally similar failing and working ELL/warehouse state has not been isolated.

## A. Executive summary

The CLI patches `orlib20!FUN_10011230` (RVA `0x11230`, preferred VA `0x10011230`) to `MOV EAX,1; RET 0x0C`, bypassing the original path while forcing success. Durable Ghidra evidence proves that the original function's ordered work includes a call to `FUN_1008E640` (RVA `0x8E640`, preferred VA `0x1008E640`), `SymInfo::AdjustOffsets`, and `SymWriter::Save`; `FUN_1008E640` is conservatively characterized as compiler/link-finalization behavior, not as the writer of any identified RNAM section ([appendix §2](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#2-complete-direct-call-inventory-and-the-skipped-work)). The controlled dynamic A/B proves that bypassing this path causes runtime-unlinkable output and that restoring it under STA produces runtime-linkable output ([Section E](#e-dynamic-ab-proof-and-runtime-validation)).

The byte comparison does **not** show missing native method/class table bodies or a structurally incomplete ELL. The ten compared metadata-table structural bodies plus `%Strings`, `%csStrings`, and `%Signatures` are byte-identical; the only semantic byte differences found are a timestamp and the `%Blobs` record-3 GUID ([analysis report L15-L26 and L28-L45](../../tmp/artifact_ui_verify_analysis/report.txt#L15-L45) — *transient/reproducible raw experiment artifact*). The static loader does compare a caller-supplied expected GUID with the loaded ELL GUID, but neither static evidence nor this comparison isolates the exact persistent mismatch in the failing ELL/warehouse state or proves that a warehouse expected-GUID mismatch caused this observed failure ([appendix §4](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#4-runtime-loader-ell-guid-check-null-return-xml-is-debugger-only), [appendix limits §7](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#7-evidentiary-conclusions-and-limits)).

The patch was introduced to avoid a native C++ exception (`0xE06D7363`) reached by the unpatched symbol writer. Static same-main-thread ordering in TSDev shows `CoInitialize(NULL)` before `_OrlibInit(NULL,1)` ([appendix §5](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#5-tsdev-com-initialization-and-orlib-initialization-order)); this was not observed in the Frida trace. Separately, the dynamic probe initialized the actual CLI worker with `CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)`, constructed `TsDevLib`, restored the original `FUN_10011230` bytes in memory, and inserted ProgId 6689. It exited normally, produced paired ELL/XML files with a matching embedded version GUID, and the user confirmed that strategy ran in Desktop without UI reverify. Thus **STA COM plus removal of the bypass remains the confirmed production fix**, even though the exact durable mismatch is still unisolated.

The production fix is therefore narrow:

1. establish an STA COM apartment on every thread that owns/uses `TsDevLib`, **before loading/initializing orlib**;
2. retain that apartment for the complete `TsDevLib`/orlib lifetime and balance it with `CoUninitialize` on the same thread;
3. remove the `FUN_10011230` persistence patch completely so the original finalization/symbol path runs;
4. fail loudly on COM initialization, compile, link, or persistence failure; and
5. add a regression that fails on today’s code by requiring a newly inserted strategy to have a newly generated ELL/XML pair whose XML version GUID occurs in the ELL, followed by a real Desktop execution gate.

XML is a useful completion marker and debugger/source-map artifact; **XML itself is not what Desktop links** ([appendix §4](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#4-runtime-loader-ell-guid-check-null-return-xml-is-debugger-only)). The proven causal statement is that the bypassed path produces runtime-unlinkable output; static evidence identifies the skipped finalization/symbol operations, while the precise persistent ELL/warehouse mismatch remains unisolated.

## B. Symptom and reproduction facts

1. Tool-inserted strategies appear verified in TSDev but fail in Desktop with “Unknown link error occurred.” Opening one, making a harmless edit, and re-verifying in TSDev fixes it ([working memory L6-L11](../PM_WORKING_MEMORY.md#L6-L11)).
2. TSDev disables Verify for an unmodified CLI-created item, consistent with the warehouse already carrying a verified state despite an unusable artifact ([working memory L18-L20](../PM_WORKING_MEMORY.md#L18-L20)).
3. UI Verify is command `WM_COMMAND 0x296A`; the trace captures it at [trace L19395-L19396](../../tmp/frida_trace_verify.log#L19395-L19396) — *transient/reproducible raw experiment artifact*.
4. The UI path saves before compiling: `_SaveModule_New@20` enters/returns success at [trace L20412-L20414](../../tmp/frida_trace_verify.log#L20412-L20414) and [trace L20611-L20612](../../tmp/frida_trace_verify.log#L20611-L20612); `StartCompilingProcess` follows at [trace L21662-L21663](../../tmp/frida_trace_verify.log#L21662-L21663), then `_VerifyModule_New@20` at [trace L21682-L21683](../../tmp/frida_trace_verify.log#L21682-L21683). — *transient/reproducible raw experiment artifacts*
5. The full **static** TSDev call chain for command `0x296A` is `TSDev!FUN_0047B0E0 → FUN_00451800 → FUN_004D9480 → FUN_004D91E0 → orlib!_VerifyModule_New@20`, with every direct call instruction cited in the Ghidra appendix ([appendix §6](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#6-ui-verify-command-chain-and-the-omitted-trace-frame)). The cited dynamic backtrace actually shows `FUN_004D91E0`, `_VerifyModule_New@20`, `FUN_004D9480`, and `FUN_0047B0E0`; it does not show `FUN_00451800` ([trace L21682-L21683](../../tmp/frida_trace_verify.log#L21682-L21683) — *transient/reproducible raw experiment artifact*). The broader UI flow includes save, a 0x58-byte compiler object, start/verify/dependent handling/end, event `0x40A`, refresh, and destruction ([working memory L13-L17](../PM_WORKING_MEMORY.md#L13-L17)).
6. Controlled ProgId 6688 used a space/backspace solely to make the editor dirty, leaving source unchanged. UI Verify rewrote the same-size 5101-byte ELL to a different hash and created the previously absent 1135-byte XML ([working memory L18-L20 and L28-L30](../PM_WORKING_MEMORY.md#L18-L30)).
7. A full Desktop restart did not repair a broken CLI item, excluding a purely live-cache explanation ([working memory L28-L30](../PM_WORKING_MEMORY.md#L28-L30)).

## C. Evidence-bounded causal chain

### C.1 Current source behavior

* `TsDevLib` is explicitly documented as applying the persistence patch in its constructor ([`src/tsdev_lib.h` L65-L71](../../src/tsdev_lib.h#L65-L71)).
* The replacement bytes are exactly `B8 01 00 00 00 C2 0C 00` (`MOV EAX,1; RET 0x0C`) ([`src/tsdev_lib.cpp` L41-L63](../../src/tsdev_lib.cpp#L41-L63)).
* Construction loads/resolves orlib and then calls `applyPersistencePatch()` before `OrlibInit(NULL,1)` ([`src/tsdev_lib.cpp` L771-L840](../../src/tsdev_lib.cpp#L771-L840)).
* `applyPersistencePatch()` resolves and overwrites `FUN_10011230` ([`src/tsdev_lib.cpp` L1611-L1639](../../src/tsdev_lib.cpp#L1611-L1639)); the machine-readable signature calls the purpose “bypasses XmlDocument.Save crash” and records RVA `0x11230` and the same bytes ([`signatures/v9.50.01.3344.json` L156-L175](../../signatures/v9.50.01.3344.json#L156-L175)).
* `verifyModule()` then allocates a 0x58-byte compiler, starts compilation, calls `_VerifyModule_New@20`, and treats return 1/no collected errors as success ([`src/tsdev_lib.cpp` L1170-L1234](../../src/tsdev_lib.cpp#L1170-L1234)).

### C.2 Binary control flow and its limits

Durable static analysis of baseline `orlib20.dll` establishes:

1. `CELBasicCompiler::CompileText` has exactly one direct call to `FUN_10011230`, at `0x1001E0BC` ([appendix §1](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#1-compiletext---fun_10011230-actual-cleanup-contract), [appendix §2](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#2-complete-direct-call-inventory-and-the-skipped-work));
2. the unpatched `FUN_10011230` constructs symbol information, directly calls `FUN_1008E640` at `0x100113D2`, then calls `SymInfo::AdjustOffsets` and `SymWriter::Save` in that order ([appendix §2](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#2-complete-direct-call-inventory-and-the-skipped-work));
3. `FUN_1008E640` spans `0x1008E640-0x1008F777` and supports the bounded description **compiler/link-finalization routine**. The evidence does not attribute symbol-tree construction to it and does not identify it as the writer of any particular RNAM section ([appendix §2](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#2-complete-direct-call-inventory-and-the-skipped-work));
4. the entry-point replacement skips the entire original function, including that finalization call and symbol-save path, and forces `EAX=1`; on the original success edge, `CompileText` later executes `ModuleInfo::SetGuid` and `MDVault::SaveModule` ([appendix §3](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#3-compiletext-after-fun_10011230-failure-versus-ell-persistence)); and
5. the replacement also changes the original stack-cleanup contract: the caller executes `ADD ESP,0x0C` after the call (`0x1001E0C1`), the original callee ends in plain `RET` (`0x10011454`), and the patch ends in `RET 0x0C` ([appendix §1](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#1-compiletext---fun_10011230-actual-cleanup-contract)). `../SIGNATURE_CATALOG.md` currently labels the convention as callee-cleanup `__fastcall`; that documentation is stale and must be corrected during patch cleanup. This is a static ABI hazard only; there is no claim that an observed historical crash was caused by it.

Static analysis does **not** prove that the bypass removes native method/class table bodies or yields a structurally incomplete ELL. Dynamic A/B evidence, rather than disassembly alone, proves that the bypassed path is causal for runtime unlinkability. The byte comparison found structurally similar ELLs whose compared semantic bodies are identical except for timestamp/GUID values; the exact durable ELL/warehouse mismatch remains unidentified ([appendix evidentiary limits §7](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#7-evidentiary-conclusions-and-limits)).

### C.3 Why the patch appeared necessary

The old standalone unpatched path raised native C++ exception code `0xE06D7363`. The prior explanation (“partially initialized CLR”) was not established. The evidence is:

* `SymWriter::Save` uses ECL’s MSXML COM wrapper;
* **static Ghidra ordering:** TSDev's synchronous UI/main application-initialization routine calls `CoInitialize(NULL)` and later reaches `_OrlibInit(NULL,1)` with no worker dispatch in that chain ([appendix §5](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#5-tsdev-com-initialization-and-orlib-initialization-order)); a separate TSDev worker-style routine also explicitly brackets work with `CoInitialize(NULL)`/`CoUninitialize` ([appendix §5](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#5-tsdev-com-initialization-and-orlib-initialization-order));
* **dynamic probe evidence:** STA initialization on the actual CLI worker plus restoration of the original function allowed insert, paired artifact production, normal exit, and successful Desktop execution (Section E.2); and
* CLI workers construct and use `TsDevLib` without calling `CoInitialize[Ex]`, so the original COM-dependent path lacks the TSDev prerequisite ([working memory L32-L37](../PM_WORKING_MEMORY.md#L32-L37)).

The Frida trace did not hook or observe TSDev's `CoInitialize` call; the placement claim above is static same-main-thread evidence, not dynamic trace evidence. The CLI intentionally runs all orlib work on thread-affine 64 MiB workers ([`src/tsdev_cli.cpp` L60-L64](../../src/tsdev_cli.cpp#L60-L64)), but a large stack does not initialize COM.

### C.4 Resulting user-visible failure

The evidence supports this bounded sequence:

`save source → compile reaches patched FUN_10011230 → bypass forces return 1 and skips original finalization/symbol work → persistence/verified state proceeds → TSDev disables Verify → Desktop cannot load/link the resulting output → Unknown link error`.

A harmless UI edit re-enables Verify; TSDev then runs its original path and Desktop runs the rewritten artifact. The failing and working ELLs are otherwise structurally similar in the compared regions: all ten metadata-table structural bodies and the three string/signature heaps are byte-identical, with semantic differences limited to timestamp and GUID blob values ([analysis report L15-L26 and L28-L45](../../tmp/artifact_ui_verify_analysis/report.txt#L15-L45) — *transient/reproducible raw experiment artifact*). Static ELRte analysis proves an expected-GUID comparison and rejection edge, but the observed failure's precise ELL/warehouse mismatch—and which producer, if any, is wrong—has not been isolated ([appendix §4](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#4-runtime-loader-ell-guid-check-null-return-xml-is-debugger-only)).

## D. Why TSDev UI succeeds and CLI fails

| Concern | TSDev UI | Current CLI | Consequence |
|---|---|---|---|
| Trigger | `WM_COMMAND 0x296A` | Direct operation methods | Different outer orchestration, but not itself causal. |
| Save ordering | `_SaveModule_New@20` before compile | `saveSource()` before `verifyModule()` | Broadly aligned. |
| Thread | TSDev verify/UI thread | Dedicated 64 MiB worker | Both are thread-affine. |
| COM | Static same-main-thread order: `CoInitialize(NULL)` before `_OrlibInit` ([appendix §5](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#5-tsdev-com-initialization-and-orlib-initialization-order)) | No COM initialization on worker | Dynamic probe separately confirms STA is the missing prerequisite for the restored path. |
| `FUN_10011230` | Original function | Replaced at RVA `0x11230` | CLI skips the statically identified compiler/link-finalization and symbol operations. |
| Return semantics | Original finalization result | Forced `EAX=1` | Bypass is allowed to proceed as successful verification. |
| Post-success parity | dependent handling, event `0x40A`, refresh | partial equivalents only | Follow-up parity work; not immediate linkability cause. |

Single insert constructs `TsDevLib` directly in `runInsert` ([`src/tsdev_cli.cpp` L640-L665](../../src/tsdev_cli.cpp#L640-L665)); modify and upsert do the same at [L755-L779](../../src/tsdev_cli.cpp#L755-L779) and [L1049-L1073](../../src/tsdev_cli.cpp#L1049-L1073). Batch owns one instance for the whole loop ([L1456-L1535](../../src/tsdev_cli.cpp#L1456-L1535)). Serve owns one instance for the whole broker session ([L2137-L2156](../../src/tsdev_cli.cpp#L2137-L2156), [L2244-L2248](../../src/tsdev_cli.cpp#L2244-L2248)). None establishes COM first.

## E. Dynamic A/B proof and runtime validation

### E.1 A: current patched behavior

* ProgId 6688 initially had only `elmodule_0_6688.ell`; UI Verify subsequently created XML at the same time as the rewritten ELL ([working memory L28-L32](../PM_WORKING_MEMORY.md#L28-L32)).
* The UI-produced XML identifies `file="elmodule_0_6688"` and version GUID `{65B9F36C-1925-44A8-988A-F4AE52241916}` ([`tmp/artifact_after_ui_verify.xml` L1-L8](../../tmp/artifact_after_ui_verify.xml#L1-L8) — *transient/reproducible raw experiment artifact*).
* ELL comparison found unchanged semantic table bodies and string/signature heaps; changed semantic bytes were timestamp/GUID blob values, not a `0xA02 → 6688` identity correction ([analysis report L15-L26 and L28-L45](../../tmp/artifact_ui_verify_analysis/report.txt#L15-L45) — *transient/reproducible raw experiment artifact*).

### E.2 B: STA COM plus original function

The temporary probe’s actual worker executes `ComApartment apartment; TsDevLib library; restoreOriginalPersistenceBytes();` before insertion ([`tmp/com_unpatched_probe/com_unpatched_probe.cpp` L533-L545](../../tmp/com_unpatched_probe/com_unpatched_probe.cpp#L533-L545) — *transient/reproducible raw experiment artifact*). Its guard calls `CoInitializeEx(..., COINIT_APARTMENTTHREADED)`, accepts only `S_OK`/`S_FALSE`, and balances successful calls with `CoUninitialize` ([L47-L74](../../tmp/com_unpatched_probe/com_unpatched_probe.cpp#L47-L74)).

Observed results:

* STA initialization returned `S_OK`; original loaded bytes matched disk and were restored; insertion succeeded as ProgId 6689; worker exited 0 ([`tmp/com_nopatch_probe.stdout.txt` L1-L15 and L27](../../tmp/com_nopatch_probe.stdout.txt#L1-L15) — *transient/reproducible raw experiment artifact*).
* New ELL (5099 bytes) and XML (1135 bytes) both existed ([stdout L16-L23](../../tmp/com_nopatch_probe.stdout.txt#L16-L23)); source round-tripped byte-for-byte and neither artifact pre-existed ([artifact inspection L3-L7](../../tmp/com_nopatch_artifact_inspection.json#L3-L7) — *transient/reproducible raw experiment artifacts*).
* The probe’s first printed `guid.match=false` was an **inspection bug**, not an artifact mismatch: it incorrectly treated the ELL’s trailing 16 zero bytes as the version GUID ([stdout L24-L26](../../tmp/com_nopatch_probe.stdout.txt#L24-L26)). Correct follow-up parsing found the XML GUID exactly once in Windows GUID byte layout at ELL offset decimal 5062 (`0x13C6`) ([`tmp/com_nopatch_guid_analysis.json` L1-L16](../../tmp/com_nopatch_guid_analysis.json#L1-L16) — *transient/reproducible raw experiment artifact*). The XML version is `{314164A7-DCB4-45D9-BE2D-B6086227CA2E}` ([`tmp/com_nopatch_6689.xml` L1-L8](../../tmp/com_nopatch_6689.xml#L1-L8) — *transient/reproducible raw experiment artifact*).
* The user then ran `TS_AUTO_COM_NOPATCH_01` in Desktop successfully **without** opening or re-verifying it in TSDev ([working memory L34-L36](../PM_WORKING_MEMORY.md#L34-L36)).

This A/B changes the two production-fix conditions together (STA added, bypass removed) and proves the combined fix: the bypassed path produces runtime-unlinkable output, while the restored path under STA produces output that executes in Desktop. Static analysis identifies the restored work as `FUN_10011230`'s compiler/link-finalization call and symbol processing/save ([appendix §2](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#2-complete-direct-call-inventory-and-the-skipped-work)). It does not isolate which timestamp/GUID-related persistent state difference makes the failing output unlinkable.

## F. Disproved or non-causal hypotheses

1. **Compiler `+0x10 = 0xA02` causes wrong compiled identity — disproved for the current direct call.** `_VerifyModule_New` uses the explicit, correctly passed ProgId through lookup, `CompileText`, token writer, metadata, and persistence. `+0x10` is consumed by TSDev’s outer recursive wrapper, which CLI bypasses. ELL analysis found neither LE `0xA02` nor LE 6688 identity replacement ([working memory L22-L28](../PM_WORKING_MEMORY.md#L22-L28); [analysis report L28-L45](../../tmp/artifact_ui_verify_analysis/report.txt#L28-L45) — *transient/reproducible raw experiment artifact*). The current field assignments remain inaccurate UI emulation ([`src/tsdev_lib.cpp` L1187-L1192](../../src/tsdev_lib.cpp#L1187-L1192)) and should be fixed separately.
2. **XML alone is required by Desktop — false.** XML is symbols/source-map data. Its absence is a highly visible marker that the original path was skipped; ELRte/ELMDCore runtime loading uses the ELL, while the XML reader identified here is debugger-side ([appendix §4](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#4-runtime-loader-ell-guid-check-null-return-xml-is-debugger-only)). The successful result is explained by the combined STA-plus-no-bypass A/B, not by XML presence alone.
3. **Event `0x40A`/live refresh is the durable root — disproved.** Restarting Desktop did not fix a broken item. `0x40A` remains a parity requirement for already-running consumers, not the durable link fix ([working memory L28-L30](../PM_WORKING_MEMORY.md#L28-L30)).
4. **Close/flush/cache timing alone — disproved as root.** A process restart also reloads durable state and still failed. Close semantics can affect visibility and cleanup, but the evidence does not attribute the failure to missing native code or structurally absent ELL tables.
5. **Dependency wrapper is necessary for root strategy linkability — disproved.** The outer wrapper propagates signature/contract changes to reverse dependents; direct verification of an independent Strategy is linkable under the confirmed STA-plus-original-path conditions ([working memory L37-L40](../PM_WORKING_MEMORY.md#L37-L40)).
6. **The non-NULL password-shaped/internal metadata argument is link-related — false.** It controls internal source encryption/protection behavior, not final linking, and is not a user password feature ([working memory L39-L40](../PM_WORKING_MEMORY.md#L39-L40)).

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

* **Single insert/modify/upsert:** the per-command worker enters STA through `TsDevLib` construction, performs the operation, destroys `TsDevLib`/calls `OrlibExit`, then leaves STA. Current worker locations are [insert L643-L665](../../src/tsdev_cli.cpp#L643-L665), [modify L758-L779](../../src/tsdev_cli.cpp#L758-L779), and [upsert L1052-L1073](../../src/tsdev_cli.cpp#L1052-L1073).
* **Batch:** one STA initialization at `runBatch` thread/session start, one `TsDevLib` across all items, one `CoUninitialize` after final close/`OrlibExit`; not per item ([`src/tsdev_cli.cpp` L1465-L1535](../../src/tsdev_cli.cpp#L1465-L1535)).
* **Serve:** one STA initialization on `runServe` before the long-lived `TsDevLib`, retained across clients/requests, released only after final warehouse close and `OrlibExit` ([`src/tsdev_cli.cpp` L2137-L2156](../../src/tsdev_cli.cpp#L2137-L2156), [L2244-L2248](../../src/tsdev_cli.cpp#L2244-L2248)). Do not initialize per request.
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

## H. Thin Vertical Slice implementation plan (TDD, red → green)

### H.1 RED: add the causal regression first

1. Add a focused live E2E test that inserts a unique, minimal Strategy through the **production CLI**.
2. Before insert, assert neither target ELL nor XML exists for the assigned test entry (or use a unique name and discover ProgId from JSON).
3. After reported success, assert:
   * ELL and XML both exist and were created/updated by this operation;
   * XML parses as `<symbols file="elmodule_0_<ProgId>" version="{GUID}">`;
   * that GUID’s 16 Windows-layout bytes occur exactly once in the ELL (do not use “last 16 bytes”; the corrected 6689 offset proves that assumption wrong);
   * source round-trips unchanged; and
   * cleanup removes the warehouse entry and both artifacts.
4. Run it directly against current production code and record the expected RED failure: XML is absent (and therefore GUID check cannot pass). Keep this desired-state harness opt-in and outside the standard regression runner while intentionally red; all registered suites and mandatory real-CLI E2E must remain green. Register the harness only after the production fix makes it pass.

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

Use the valid type-specific source fixtures and semantic errors defined by [`docs/E2E_TEST_SPEC.md` L19-L25](../E2E_TEST_SPEC.md#L19-L25) and mappings at [L168-L176](../E2E_TEST_SPEC.md#L168-L176).

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
3. **Failed insert/upsert-new:** retain the existing newly-created-entry cleanup intent ([`src/tsdev_lib.cpp` L1236-L1258](../../src/tsdev_lib.cpp#L1236-L1258)), but extend its tests to require that both target `.ell` and `.xml` are absent afterward. Cleanup errors must be reported alongside—not replace—the primary compile/link diagnostic. The current empty catch is not sufficient for production observability.
4. **Failed modify/upsert-existing:** never delete the existing warehouse component. First characterize the unpatched TSDev/orlib failure and recovery sequence, including artifact and warehouse/property state. Reproduce a confirmed TSDev/orlib transaction or recovery API rather than inventing direct file backup/restore, which may desynchronize cross-store state and violate the emulate-TSDev rule. If no safe confirmed recovery sequence exists, keep the path blocked instead of shipping a file-level workaround. Preserve TSDev's source-state behavior without parsing source or recreating metadata.
5. **Absent/stale pair on success path:** a production operation must never silently report success when the expected ELL/XML persistence markers are absent or stale. The immediate guarantee comes from using the original return path; E2E must additionally reject an absent/stale XML or disagreement between its version GUID and the ELL. If fault injection proves the original function can return success with absent/stale files, promote these checks into production postconditions using version-scoped parsing; otherwise keep binary-format parsing in tests to minimize runtime coupling.
6. **Close/flush failure:** report it if an API provides a detectable failure. Do not delete known-good modify artifacts merely because final close is uncertain. Preserve enough path/ProgId detail for recovery.
7. **Cleanup ordering:** on normal worker exit: finish operation → close warehouse where mode requires → destroy `TsDevLib` (`OrlibExit`, windows) → `CoUninitialize` on the same thread. Constructor unwind must release only resources successfully acquired.
8. **No suppressions/hacks:** do not catch and ignore COM/link failures, restore the bypass conditionally, add compensating stack manipulation for the removed patch, or parse mutable source text for identity/cleanup. Names, types, and ProgIds remain warehouse/API-derived.

## K. Signatures, resolver, tests, and documentation cleanup

The persistence patch is currently one of 25 signatures/12 patch sites ([`docs/SIGNATURE_CATALOG.md` L5-L12](../SIGNATURE_CATALOG.md#L5-L12)); codegen enforces exactly 25 signatures, 3 globals, and 10 exports ([`tools/sigcodegen.py` L35-L40](../../tools/sigcodegen.py#L35-L40)). Removal must be coordinated:

1. Delete `FUN_10011230_persistence_patch` from [`signatures/v9.50.01.3344.json` L156-L175](../../signatures/v9.50.01.3344.json#L156-L175). Expected totals become **24 signatures, 11 patch sites, 3 data globals, 10 exports** unless another independently justified signature is added (do not add a dummy to preserve a count).
2. Change `EXPECTED_SIGNATURE_COUNT` from 25 to 24; globals/exports remain 3/10. Regenerate `signatures.gen.h` through the normal build, not by hand.
3. Remove the complete `TsDevLib` persistence-patch surface: public `validatePersistencePatchBytes` ([`src/tsdev_lib.h` L198-L202](../../src/tsdev_lib.h#L198-L202)); `m_persistencePatchRva` and `m_persistenceOriginalBytes` ([L211-L219](../../src/tsdev_lib.h#L211-L219)); `applyPersistencePatch` declaration ([L369-L373](../../src/tsdev_lib.h#L369-L373)); corresponding constants, implementation, constructor call, and destructor restoration logic in `src/tsdev_lib.cpp`. Preserve and, where needed, rename generic `m_patchesApplied` bookkeeping for the 11 unrelated thread-check patches.
4. Remove `OrlibResolver::resolvePersistencePatch()` and its implementation ([`src/orlib_resolver.h` L843-L851](../../src/orlib_resolver.h#L843-L851); [`src/orlib_resolver.cpp` L940-L945](../../src/orlib_resolver.cpp#L940-L945)). Update **all** persistence-specific resolver comments/references at [`src/orlib_resolver.h` L153, L162, L168](../../src/orlib_resolver.h#L153-L168), [L328](../../src/orlib_resolver.h#L328), [L771](../../src/orlib_resolver.h#L771), [L788](../../src/orlib_resolver.h#L788), [L811](../../src/orlib_resolver.h#L811), [L835](../../src/orlib_resolver.h#L835), and [L843-L851](../../src/orlib_resolver.h#L843-L851). Keep generic patch machinery and rewrite examples around a remaining thread-check patch.
5. Update resolver tests that hard-code `kSignatures[8]` as persistence and all following numeric indices ([`tests/test_orlib_resolver.cpp` L1218-L1227](../../tests/test_orlib_resolver.cpp#L1218-L1227)). Prefer name lookup over positional indices so future catalog edits do not cascade silently. Remove the validate/apply persistence case at [L1503-L1535](../../tests/test_orlib_resolver.cpp#L1503-L1535).
6. **Repurpose rather than delete** [`tests/test_persistence_validation.cpp`](../../tests/test_persistence_validation.cpp) into a behavior-based finalization regression: construct `TsDevLib`, compile a disposable item through the real path, validate fresh paired artifacts/GUID agreement and cleanup, and verify the persistence-patch API/catalog entry is absent. Do not resolve the removed target by hardcoded RVA, retain its production signature solely for testing, or duplicate a test-only signature. If renamed, update [`CMakeLists.txt` L103-L104](../../CMakeLists.txt#L103-L104), [L372](../../CMakeLists.txt#L372), [L476](../../CMakeLists.txt#L476), and [`tests/run_unit_tests.ps1` L22](../../tests/run_unit_tests.ps1#L22), [L111](../../tests/run_unit_tests.ps1#L111) together. Do not leave calls to `validatePersistencePatchBytes`, which would fail to compile after step 3.
7. Update sigtool’s 25/12 expectations to 24/11 ([`tests/test_sigtool.ps1` L34-L40](../../tests/test_sigtool.ps1#L34-L40), [L193-L201](../../tests/test_sigtool.ps1#L193-L201)) and stale “20/20” comments in that script.
8. Rewrite the catalog totals/table/section 4.9/scan results and renumber carefully, or retain stable target IDs with an explicit retired gap while ensuring generated array indices are not inferred from IDs. Explicitly correct the stale `__fastcall`/callee-cleanup statements in [`docs/SIGNATURE_CATALOG.md` L106](../SIGNATURE_CATALOG.md#L106), [L380, L386, L404](../SIGNATURE_CATALOG.md#L380-L404) and [`docs/FEASIBILITY.md` L338-L340](../FEASIBILITY.md#L338-L340) to match the appendix's caller `ADD ESP,0x0C`, original plain `RET`, and patched `RET 0x0C` evidence ([appendix §1](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md#1-compiletext---fun_10011230-actual-cleanup-contract)). Machine-readable JSON remains authoritative.
9. Correct operational docs and source comments that say the patch provides persistence or XML omission is acceptable. In historical documents, preserve history but prepend a conspicuous dated correction rather than silently rewriting events. In particular, supersede [`docs/PROJECT_HISTORY.md` L26-L34](../PROJECT_HISTORY.md#L26-L34): the bypass produced runtime-unlinkable output; XML absence was not acceptable evidence of complete verification; STA COM plus removal of the bypass is dynamically confirmed; no missing native tables or exact warehouse-GUID mismatch has been established.
10. Verify version detection is unchanged: manifest identity, PE timestamp/file size, and SHA-256 are properties of the unmodified on-disk DLL, not signature array length. Run all Tier 1/2/3 mismatch tests after index cleanup.

## L. Follow-up TSDev parity work — explicitly separate from the immediate fix

These should be individually researched/implemented after the causal TVS is green:

1. **Compiler object setup/factory:** TSDev sets category at `+0x08`, subtype at `+0x0C`, current ProgId at `+0x10`, notification HWND at `+0x18`, and clears `+0/+4`; current code hard-codes `+0x10=0xA02`, omits category/subtype, and writes 6 into the empty inline string area at `+0x28` ([working memory L22-L27](../PM_WORKING_MEMORY.md#L22-L27); [`src/tsdev_lib.cpp` L1187-L1192](../../src/tsdev_lib.cpp#L1187-L1192)). Confirm and use TSDev’s ordinal 66/67 factory/destruction path if required.
2. **SaveModule parity:** reproduce TSDev’s non-NULL flags, internal metadata/encryption buffer, and CELProperties state after parameter semantics are confirmed. This is not a user password feature and is not the link fix.
3. **Post-success `0x40A`:** reproduce live notification for the actual registered TradeStation consumers; do not assume TSDev.exe is a receiver. It cannot replace durable linking.
4. **Reverse-dependent reverify:** reproduce the behavior of TSDev.exe’s dependent-program traversal with supported orlib/warehouse primitives so changed Function/contract definitions revalidate callers. Do not couple the standalone CLI to private TSDev.exe code. This is not required to make the root Strategy itself linkable.

Do not hold the immediate fix for these unless the new causal E2E exposes a direct dependency.

> **Note:** The four items above are the source authority for Workstream M
> (`../workstream_tsdev_parity_plan.md`). Workstream M is a separate,
> independently planned follow-up that depends on Workstream L being approved
> and merged; it must not be bundled into the immediate causal fix.

## M. Risks and open questions

### Blockers before production release

* **Final production binary runtime gate:** a strategy inserted by the STA-initialized production CLI with the bypass removed must execute in Desktop without UI reverify. The probe proves the combined fix, not the final integration.
* **Failure-state characterization for modify:** determine whether an unpatched late failure can damage a prior ELL/XML pair or alter warehouse GUID state; neither behavior is currently established. If observed, implement a confirmed transactional rollback sequence; artifact-only restore must not be shipped if it can create GUID inconsistency.
* **COM guard unwind correctness:** constructor failures after `S_OK`/`S_FALSE` must balance exactly once on the same thread; forced failure tests are mandatory.

### Non-blockers for the immediate causal TVS

* The exact persistent mismatch inside the otherwise structurally similar failing/working ELL and warehouse state, including semantic ownership of ELL blob GUID records beyond the confirmed XML-version match.
* Automating every Desktop UI/runtime check; a controlled manual release gate is acceptable if documented.
* Compiler factory/field parity, SaveModule flags/CELProperties, event `0x40A`, and reverse-dependent traversal (Section L), provided regression tests show root insert/modify/upsert linkability.
* Whether XML should become a production postcondition. It is mandatory in E2E as a parity/completion marker; production enforcement depends on fault-injection results and format-stability cost.

### Known analysis caveat

`tmp/com_nopatch_probe.stdout.txt` reports a false GUID mismatch because its first extractor used the trailing 16 bytes. The corrected analysis is authoritative ([`tmp/com_nopatch_guid_analysis.json` L1-L16](../../tmp/com_nopatch_guid_analysis.json#L1-L16) — *transient/reproducible raw experiment artifact*). Any new test must encode GUID bytes with Windows mixed-endian `GUID` layout and search the complete ELL.

## N. Evidence index

### Primary factual record

* [Durable Ghidra evidence appendix — control flow, ABI, runtime loader, COM ordering, UI call chain, and explicit limits](RE_UNKNOWN_LINK_GHIDRA_EVIDENCE.md)
* [Current investigation context — `docs/PM_WORKING_MEMORY.md` L6-L41](../PM_WORKING_MEMORY.md#L6-L41)
* [UI Verify command — `tmp/frida_trace_verify.log` L19395-L19396](../../tmp/frida_trace_verify.log#L19395-L19396) — *transient/reproducible raw experiment artifact*
* [UI SaveModule — trace L20412-L20414, L20611-L20612](../../tmp/frida_trace_verify.log#L20412-L20414) — *transient/reproducible raw experiment artifact*
* [UI Start/Verify — trace L21662-L21663, L21682-L21683](../../tmp/frida_trace_verify.log#L21662-L21663) — *transient/reproducible raw experiment artifact*

### Artifact evidence (all transient/reproducible raw experiment artifacts under `tmp/`)

* [6688 ELL structured comparison report](../../tmp/artifact_ui_verify_analysis/report.txt)
* [6688 exact byte ranges](../../tmp/artifact_ui_verify_analysis/focused_diff.txt)
* [6688 machine-readable complete comparison](../../tmp/artifact_ui_verify_analysis/diff.json)
* [6688 UI-generated XML](../../tmp/artifact_after_ui_verify.xml)
* [6689 probe stdout](../../tmp/com_nopatch_probe.stdout.txt)
* [6689 artifact/source inspection](../../tmp/com_nopatch_artifact_inspection.json)
* [6689 corrected GUID analysis](../../tmp/com_nopatch_guid_analysis.json)
* [6689 ELL](../../tmp/com_nopatch_6689.ell) and [XML](../../tmp/com_nopatch_6689.xml)
* [Temporary probe COM guard and worker sequence — `tmp/com_unpatched_probe/com_unpatched_probe.cpp` L47-L74, L533-L571](../../tmp/com_unpatched_probe/com_unpatched_probe.cpp#L47-L74)

### Current production source/signature evidence

* [Patch bytes — `src/tsdev_lib.cpp` L41-L63](../../src/tsdev_lib.cpp#L41-L63)
* [Constructor applies patch before OrlibInit — L771-L844](../../src/tsdev_lib.cpp#L771-L844)
* [Destructor restores patch — L854-L900](../../src/tsdev_lib.cpp#L854-L900)
* [Current verify flow/compiler fields — L1170-L1234](../../src/tsdev_lib.cpp#L1170-L1234)
* [Patch application — L1611-L1639](../../src/tsdev_lib.cpp#L1611-L1639)
* [Public/member patch surface — `src/tsdev_lib.h` L65-L71, L198-L225](../../src/tsdev_lib.h#L65-L71)
* [Patch signature, RVA, and replacement — `signatures/v9.50.01.3344.json` L156-L175](../../signatures/v9.50.01.3344.json#L156-L175)
* [Codegen exact counts — `tools/sigcodegen.py` L35-L40](../../tools/sigcodegen.py#L35-L40)
* [Human signature catalog totals and target — `docs/SIGNATURE_CATALOG.md` L5-L12, L103-L128](../SIGNATURE_CATALOG.md#L5-L12)
* [Single worker ownership — `src/tsdev_cli.cpp` L630-L665](../../src/tsdev_cli.cpp#L630-L665)
* [Batch worker ownership — L1456-L1535](../../src/tsdev_cli.cpp#L1456-L1535)
* [Serve worker ownership — L2137-L2156, L2244-L2248](../../src/tsdev_cli.cpp#L2137-L2156)

---

**Implementation status:** none. Production source, signatures, tests, and documentation were not changed as part of this research deliverable. This document is the durable tracked record of the approved research; the production fix is owned by Workstream L (`../workstream_unknown_link_fix_plan.md`), and the §L parity follow-ups are owned by Workstream M (`../workstream_tsdev_parity_plan.md`).
