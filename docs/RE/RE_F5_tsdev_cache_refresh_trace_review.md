# Adversarial Review: RE_F5_tsdev_cache_refresh_trace.md

**Reviewer:** Independent adversarial subagent
**Date:** 2025
**Subject:** `docs/RE/RE_F5_tsdev_cache_refresh_trace.md` (Frida cache-refresh trace findings)
**Cross-referenced against:** `tmp/frida_cache_refresh_trace.log` (2688 lines), Ghidra MCP decompilation/disassembly of orlib20.dll, `src/tsdev_lib.cpp`, `src/tsdev_lib.h`, `src/signatures.gen.h`

---

## Verdict: APPROVE (with findings)

The core claims of the findings document are **independently verified** against the
raw Frida trace log and Ghidra disassembly. The central conclusion — that TSDev
refreshes its warehouse cache via `FUN_101d0950` (cmd 0x0F, sets `[wh+0x5E]`)
followed by `FUN_101c09f0(0xC)` (which dispatches cmd 0x4E) — is correct and
well-evidenced. The calling-convention analysis is confirmed by disassembly.

However, several **secondary claims contain inaccuracies** and a few **risks are
under-analyzed**. These are documented below as findings. None rise to the level
of invalidating the document, but the implementer should be aware of them.

---

## Findings (numbered, with severity)

### F1 — MEDIUM: §3.1 incorrectly claims cmd 0x09 sets `[wh+0x5E]`

**Claim (§3.1):** "the first cmd dispatch is cmd 0x09 (ProgramByName) at +1.199s,
which **sets `[warehouse+0x5E]`** to a valid COM interface pointer (0xb3af0d4)."

**Evidence (trace log line 66, event [2]):**
```
[2] +1.199s >>> ENTER FUN_101cf5b0 (cmd dispatcher)
  *** CMD DISPATCH *** wh=0xb275b80 [wh+04]=0x9 ... [wh+5E]=0xb3af0d4 ...
```

The ENTER snapshot of the very first cmd dispatch already shows
`[wh+5E]=0xb3af0d4`. This means `[wh+0x5E]` was set **during warehouse
initialization** (before any hooked cmd dispatch ran), **not** by cmd 0x09. The
finding's causal claim is wrong. This matters because §7.1–§7.3 builds an
explanation on the premise that "cmd 0x09 sets `[wh+0x5E]`" and then puzzles over
why our CLI's cmd 0x09 doesn't set it (§7.3). The real explanation is simpler:
`[wh+0x5E]` is set during COM server initialization (likely during `GetWarehouse`
/ `OrlibInit`), and our CLI's initialization path differs from TSDev's in a way
that leaves it NULL.

### F2 — MEDIUM: §8.5 "recommended approach" silently skips FUN_101c09f0's cache-flag invalidation

**Claim (§8.1):** "Replicate TSDev's Cache Refresh Sequence"
**Claim (§8.5):** The pseudo-code does: `FUN_101d0950` → set `[wh+0x12]=0xC` →
`FUN_101cf5b0(wh, 0x4E)`.

TSDev's actual sequence is `FUN_101d0950` → `FUN_101c09f0(0xC)`. Per the Ghidra
disassembly of `FUN_101c09f0` (0x101C09F0), before dispatching cmd 0x4E it
performs **Phase 1: cache-flag invalidation** based on the type code. For type
0xC (confirmed at `101c0b48`–`101c0b60`):
- Calls `FUN_101e0fb0` (with `ECX = [module+0x1DF]`)
- Sets `[module+0x1DE] = 1` (dirty flag)
- Sets `[module+0x269] = 0`

The §8.5 approach skips this entirely. The findings never analyze whether cmd
0x4E alone is sufficient or whether the local cache flags are needed for
subsequent cmd 0x08 (`_LoadMasterRecsEX`) to re-fetch.

**Mitigating factor:** In TSDev's flow, the module object is a **temporary
stack-local** CELProgramWriter (`local_2d4` at `[EBP-0x2D4]` in `FUN_101a8620`,
initialized by `FUN_1000dd30`). It is destroyed when `FUN_101a8620` returns. The
cache flags on this temporary object do not persist. The actual persistent cache
refresh is performed by cmd 0x4E on the **warehouse**. So skipping Phase 1 is
**probably safe**, but the findings should have stated this reasoning rather than
leaving the gap silent.

### F3 — MEDIUM: §8.5/§9.5 ProgId requirement for DELETE is inadequately addressed

**Claim (§9.5):** "After delete, the ProgId is available from `findExistPos`
(called before deletion)."

`FUN_101d0950` (cmd 0x0F) calls `IWHouse::ProgramByIdTypes` on the server with
the given ProgId. After a **delete**, that ProgId no longer exists on the server,
so cmd 0x0F may **fail** (return 0), leaving `[wh+0x5E]` unset and blocking the
cache refresh. The findings do not address this case.

**Note:** The actual implementation (`src/tsdev_lib.cpp:1071–1097`,
`refreshCache()`) correctly avoids this by obtaining a ProgId from the **cached
enumeration** (any existing item) rather than from the just-completed operation.
This divergence from the findings' recommendation is a good practical
improvement, but the findings document itself should have flagged the delete case
as a risk.

### F4 — LOW: §3.6/§5.1 claim `[wh+0x5E]=0xb49d714` throughout the dialog — it changes

**Claim (§5.1 table):** "Open dialog → cmd 0x08 | 0xb49d714 | non-null throughout"
**Claim (§3.6):** "All dialog cmd 0x08 calls use ... `[wh+5E]=0xb49d714`"

**Evidence (trace log lines 3183–3224, events [1045]–[1062]):**
The dialog cmd 0x08 dispatches show `[wh+5E]` changing per type:
- type=0x2 (Indicator): `[wh+5E]=0x15d52534`
- type=0x502: `[wh+5E]=0x13ae2e0c`
- type=0x402: `[wh+5E]=0x1b1b03fc`
- type=0x4 (Function): `[wh+5E]=0x1e5d52bc`

The value is **non-null throughout** (the key point), but the specific value
`0xb49d714` only appears for the first type (0x1402). The findings' specific-value
claim is inaccurate. The cmd 0x08 dispatch itself updates `[wh+0x5E]` per type.

### F5 — LOW: §10 evidence citation for "cmd 0x4F clears `[wh+0x5E]`" is imprecise

**Claim (§10):** "cmd 0x4F clears `[warehouse+0x5E]` | Live trace: event [1027],
NULL after Release"

Event [1027] is the **ENTER** of `FUN_101d0950`, not the exit of cmd 0x4F. The
actual cmd 0x4F exit snapshots (events [985] and [1026]) both show
`[wh+5E]=0xb49d714` (still non-null). The NULL first appears at event [1027]
ENTER, meaning the clearing happened in the **caller** after cmd 0x4F returned,
not inside cmd 0x4F itself. The claim is plausible but the cited evidence does
not directly prove it.

### F6 — LOW: §4.3 decompilation of FUN_101d0950 is misleading (but conclusion is correct)

**Claim (§4.3):** Shows `FUN_101c4ab0(param_1, *(undefined4 *)(param_1 + 0x5e))`
implying `param_1` (warehouse) is passed as the first arg to `FUN_101c4ab0`.

**Disassembly (0x101D09C7–0x101D09D0) shows:**
```asm
101d09c7:  PUSH dword ptr [ESI + 0x5e]    ; push [warehouse+0x5E]
101d09ca:  PUSH ESI                        ; push warehouse
101d09cb:  MOV ESI, dword ptr [EBP + 0x10] ; ESI = param_4 (module/outputPtr)
101d09ce:  MOV ECX, ESI                    ; ECX = module  ← reassigned!
101d09d0:  CALL 0x101c4ab0
```

ECX is **reassigned** from warehouse to the module buffer (`param_4`) before the
call. The Ghidra decompiler lost track of this reassignment and rendered it
incorrectly. The findings copied the misleading decompilation. However, the
**conclusion** in §4.4 (`FUN_101c4ab0` sets `[module+0x0C]`=warehouse,
`[module+0x10]`=COM interface) is **correct** — independently confirmed by
disassembly of both `FUN_101d0950` and `FUN_101c4ab0`, and by the trace (event
[980]: `[module+0C]=0xb275b80 [module+10]=0xb49d714`).

### F7 — LOW: §8.1/§9.7 framing contradiction

§8.1 is titled "Replicate TSDev's Cache Refresh Sequence" but §8.5 does **not**
replicate it (it skips `FUN_101c09f0`). §9.7 presents the same skip-`FUN_101c09f0`
approach as an "alternative," but §8.5 and §9.7 are **identical**. The document
should either (a) acknowledge that §8.5 already deviates from TSDev by skipping
`FUN_101c09f0`, or (b) present the true replication (calling `FUN_101c09f0`) as
the primary recommendation.

### F8 — INFO: Actual implementation diverges from findings (cmd 0x0D step)

The findings' §8.5 recommends: cmd 0x0F → set `[wh+0x12]=0xC` → cmd 0x4E.
The actual implementation (`src/tsdev_lib.cpp:1122–1141`, `refreshCache()`) adds
a **Step 2**: call `FUN_101cfce0` (cmd 0x0D) to obtain a "fresh" COM interface,
claiming the cmd 0x0F interface is "stale" after create/modify. **TSDev does not
call cmd 0x0D** in the cache-refresh sequence (verified: no cmd 0x0D dispatch
appears between events [976] and [986] in the trace). This divergence is outside
the findings document's scope but is noted for completeness — the implementer
added logic not supported by the trace evidence.

---

## Verified Claims (independently confirmed)

| # | Claim | Verification Method | Result |
|---|-------|---------------------|--------|
| V1 | Call sequence: `FUN_101a8620` → `FUN_101d0950` (cmd 0x0F) → `FUN_101c09f0(0xC)` → cmd 0x4E | Trace log events [975]–[983] | **CONFIRMED** |
| V2 | `[warehouse+0x5E]` is non-null when cmd 0x4E is dispatched | Trace event [981]: `[wh+5E]=0xb49d714` | **CONFIRMED** |
| V3 | Same warehouse (0xb275b80) used throughout all operations | Trace events [2], [976], [980], [981], [1043] all show `wh=0xb275b80` | **CONFIRMED** |
| V4 | `FUN_101d0950` is `__thiscall`: ECX=warehouse, stack=[typeArray, progId, outputPtr] | Disassembly of call site (0x101A8688–0x101A8690): `MOV ECX,EDI` (warehouse), 3 PUSHes; `RET 0xc` at 0x101D09E6 | **CONFIRMED** |
| V5 | `FUN_101cf5b0` is `__thiscall`: ECX=warehouse, stack=cmdId | Disassembly of call site in `FUN_101c09f0` (0x101C0C61–0x101C0C72): `MOV ECX,[EDI+0x0C]` (warehouse), `PUSH 0x4e` | **CONFIRMED** |
| V6 | `FUN_101c4ab0` sets `[module+0x0C]`=warehouse, `[module+0x10]`=COM interface | Disassembly 0x101D09C7–0x101D09D0 (ECX reassigned to module) + decompilation of `FUN_101c4ab0` (`param_1[3]=param_2; param_1[4]=param_3`) | **CONFIRMED** |
| V7 | `FUN_101c09f0` copies `[module+0x10]`→`[wh+0x5E]`, sets `[wh+0x12]`=type, dispatches 0x4E | Disassembly 0x101C0C61–0x101C0C72 | **CONFIRMED** |
| V8 | `FUN_101d43a0` is resolved and available in our code | `signatures.gen.h:260` (signature `FUN_101d43a0_build_type_array`), `tsdev_lib.cpp:1067` (`callBuildTypeArray`), `tsdev_lib.h:315` (`m_pBuildTypeArray`) | **CONFIRMED** |
| V9 | `FUN_101d0950` is resolved and INT3-patched (offset 67) | `signatures.gen.h:167` (offset `0x43`=67), `tsdev_lib.h:310` (`m_pCmd0FDispatcher`) | **CONFIRMED** |
| V10 | `FUN_101cf5b0` is resolved and INT3-patched (offset 36) | `signatures.gen.h:179` (offset `0x24`=36), `tsdev_lib.h:304` (`m_pCmdDispatcher`) | **CONFIRMED** |
| V11 | cmd 0x0F sets `[wh+0x5E]` (NULL → non-null) | Trace events [1027]–[1028]: ENTER `[wh+5E]=NULL`, EXIT `[wh+5E]=0xb49d714` | **CONFIRMED** |
| V12 | Path is real MyWork dir, not "50 spaces" | Trace event [975]: `path="C:\Program Files (x86)\TradeStation 9.5\MyWork\"` | **CONFIRMED** |
| V13 | `[wh+0xC2]=1` (SERVER mode) throughout | All trace events show `[wh+C2]=1(SERVER)` | **CONFIRMED** |
| V14 | `FUN_101a8620` is `__fastcall`: ECX=category, EDX=progId, stack=path | Disassembly 0x101A8647–0x101A8649: `MOV EBX,EDX; MOV ESI,ECX`; path at `[EBP+0x8]` | **CONFIRMED** |

---

## Required Fixes

None required for APPROVAL. The following are **recommended corrections** for a
future revision of the document:

1. **§3.1:** Correct the claim that cmd 0x09 sets `[wh+0x5E]`. State that
   `[wh+0x5E]` is already non-null at the first cmd dispatch (set during
   warehouse/COM init, before any hooked cmd). Revise §7.1–§7.3 accordingly.

2. **§8.5:** Add an explicit note that the recommended approach **skips**
   `FUN_101c09f0`'s Phase 1 cache-flag invalidation, and explain why this is safe
   (the module object is temporary; cmd 0x4E operates on the warehouse's
   server-side cache directly).

3. **§9.5:** Add a warning that for DELETE, the deleted item's ProgId may cause
   cmd 0x0F to fail. Recommend using any existing item's ProgId from the cached
   enumeration (as the actual implementation does).

4. **§3.6/§5.1:** Correct the claim that `[wh+0x5E]=0xb49d714` throughout the
   dialog. State that it is non-null throughout but changes per type (cmd 0x08
   updates it).

5. **§4.3:** Add a note that the Ghidra decompiler's rendering of the
   `FUN_101c4ab0` call is misleading (ECX is reassigned to the module buffer
   before the call); reference the disassembly for the accurate picture.

---

## Implementation Recommendations

For the implementer working from these findings:

1. **Calling conventions are verified.** Use:
   - `FUN_101d0950`: `__thiscall`, ECX=warehouse, stack=[typeArray, progId, &outBuf]. Output buffer must be ≥0x280 bytes and initialized by `FUN_1000dd30` (CELProgramWriter ctor) because `FUN_101d0950` writes `[outBuf+0x26D]=1` on success and calls `FUN_101c4ab0` which dereferences `outBuf` as a CELProgramWriter (vtable calls at `[outBuf+0]` and `[outBuf+0xC]`).
   - `FUN_101cf5b0`: `__thiscall`, ECX=warehouse, stack=cmdId.
   - `FUN_101d43a0`: `__stdcall`, stack=[category, &outArray].

2. **ProgId for cmd 0x0F:** Do NOT use the deleted item's ProgId after a delete
   operation. Instead, enumerate the cache (`_LoadMasterRecsEX@16`) and use any
   existing item's ProgId (first record, offset 0x00, short). The actual
   implementation (`tsdev_lib.cpp:1071–1097`) already does this correctly.

3. **Output buffer initialization is mandatory.** `FUN_101d0950` calls
   `FUN_101c4ab0(ECX=outBuf, ...)` on success, which dereferences
   `*(int*)outBuf` as a vtable pointer and calls `vtable[1]` and `vtable[3]`.
   If the buffer is zeroed but not initialized by `FUN_1000dd30`, this will
   crash (NULL vtable dereference). The actual implementation correctly calls
   `callCelProgramWriterCtor` before `callCmd0FDispatcher`.

4. **SEH protection:** Wrap both `FUN_101d0950` and `FUN_101cf5b0` calls in
   `__try/__except(EXCEPTION_EXECUTE_HANDLER)`. The INT3 thread-check patches
   are already applied, but COM calls inside the worker thread can still fault.

5. **Skip cmd 0x0D.** The findings' recommended approach (cmd 0x0F + cmd 0x4E)
   matches TSDev's trace. The actual implementation's extra cmd 0x0D step is not
   supported by the trace evidence and adds complexity/risk. If the simpler
   approach works in testing, prefer it. If cmd 0x4E alone proves insufficient
   (stale enumeration after refresh), investigate whether cmd 0x08's cache
   invalidation flags need to be set — but note those flags live on a temporary
   module object in TSDev's flow, so they may not be the mechanism.

6. **Type code 0xC is correct for program-list cache refresh.** This is what
   TSDev uses (trace event [980]: `typeCode=12 (0xC)`). For a broader refresh,
   type 0 can be used, but 0xC matches TSDev exactly.

7. **Verify `[wh+0x5E]` after cmd 0x0F.** Check the return value of
   `FUN_101d0950` (1=success) AND re-read `[wh+0x5E]` to confirm it is non-null
   before dispatching cmd 0x4E. If cmd 0x0F fails, abort the cache refresh
   gracefully.
