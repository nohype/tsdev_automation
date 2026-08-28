# M.3b.1 — CLI Object-Code Version/Count Defect (Root-Cause Diagnosis)

> **M.3b.2a UPDATE (2026-08-27):** Phase 1 (high-level TSDev vs CLI path
> comparison) is COMPLETE. The RED test confirmed the defect at runtime, and
> Phase 1 found NO higher-level TSDev mechanism missing from the CLI. The
> fallback condition in §6.0 is met — the fix must be at the deep level
> (Phase 2 / M.3b.2b). See §10 (Phase 1 results) and §11 (bounded fix design
> for M.3b.2b) below. Residuals 1 and 2 from §7 are now RESOLVED.
>
> **Status:** M.3b.1 diagnosis-only TVS, 2026-08-27. Root cause substantially
> identified but with a primary residual (INFOREC count population not
> confirmed at runtime); residuals documented in §7. This note corrects the
> `+0x24`-as-version offset error in the M.3b plan baseline and the M.4 note
> §8.8 "version=0" claim (see §8), and revises the root cause per the
> linker-populator INFOREC source evidence (populators read the .ell file's
> object code INFOREC, not the CPrgList stored record; see §3.1 and §3.6).
> **Date:** 2026-08-27
>
> **CORRECTION NOTICE (2026-08-27):** The M.3b plan baseline
> (`docs/workstream_tsdev_parity_plan.md` §M.3b, "Known factual baseline",
> lines 275–276) states that the populators read "the decompressed object-code
> version at `buffer+0x24`" and that "CLI-compiled items have `buffer+0x24 = 0`".
> This is incorrect. The version is at INFOREC `+0x9`; the populators read COUNTS
> at `+0x29`/`+0x1d`/`+0x31` and return NULL when a count is 0. The `+0x24` in
> the M.4 note §1.5 (`RE_M4_REVERSE_DEPENDENT_REVALIDATION.md` line 353,
> `*(uint*)(record+0x24) > 0xf`) is a string-length gate on BST warehouse nodes
> — a different record type, not the object-code version. The M.4 note §8.8
> says "version=0, element count=0" without citing `+0x24` as the version
> offset; the `+0x24`-as-version claim originates in the M.3b plan baseline.
> CLI-compiled items have empty element arrays because the INFOREC count
> fields (`+0x29`/`+0x1d`/`+0x31`) are not populated during CLI compilation
> (the primary root cause, see this note §3.6.1); a secondary finding is that
> the master record count fields (`+0xc6`/`+0xca`/`+0xe4`) are also not
> updated during compilation, leaving the CPrgList stored record with 0
> counts on a separate storage path (see §3.6.2). Per the correction-prepend
> convention demonstrated by the M.4 note's own inline "Correction to a
> prior draft" pattern (`RE_M4_REVERSE_DEPENDENT_REVALIDATION.md` line 487),
> this correction is recorded here rather than by rewriting the M.3b plan or
> M.4 note; consult them with this correction in mind.
> **Scope:** Root-cause RE research for the "CLI-compiled items have empty
> element arrays" defect that blocks M.4's reverse-dependent enumeration. No
> production or test code was changed for this task.
> **Method:** Five focused Ghidra investigations against `orlib20.dll`,
> `TSDev.exe`, and `elbardb.dll` (the WHServer COM server, newly identified and
> imported into the `TSDevAnalysis` Ghidra project). Static decompilation and
> disassembly only; no dynamic trace was captured. Whole-program instruction
> scan of `orlib20.dll` (576,374 instructions) used to confirm the absence of
> writes to specific master-record offsets in the compilation call chain.
> Scan methodology: searched all instructions for operands matching
> `[reg+0xc6]`, `[reg+0xca]`, and `[reg+0xe4]` (write sites only, excluding
> reads and `0xcafecafe` sentinels); cross-referenced each hit against the
> `CompileText` call chain (functions reachable from RVA 0x1DD80) to determine
> whether any write site falls within the compilation path. The compilation
> call chain was bounded by transitive xrefs from `CompileText` (0x1DD80)
> through `UpdateProperties` (0x1FF60), `FUN_101d3c00` (0x101D3C00), and
> downstream calls within `CompileText` itself (0x1001E185–0x1001E219).
> **Binaries analyzed (read-only):**
> `C:\Program Files (x86)\TradeStation 9.5\Program\orlib20.dll` (baseline
> `9.50.01.3344`, SHA-256
> `318DB24A4F41BDB9FF69E2E647026B788706904CF887CBB0A953E003DB22EC18`),
> `C:\Program Files (x86)\TradeStation 9.5\Program\TSDev.exe`, and
> `C:\Program Files (x86)\TradeStation 9.5\Program\elbardb.dll` (the WHServer
> COM server).

---

## 1. Executive Summary

The linker populators read the INFOREC from the .ell file's object code
(decompressed from the warehouse COM SAFEARRAY), NOT the CPrgList warehouse
stored record. The primary root cause is that the INFOREC count fields
(`+0x29`/`+0x1d`/`+0x31`) are not populated during CLI compilation — they are
zeroed by `InitTokenListNew` (`0x1D900`) and expected to be filled by the
compiler's token-list building process during `CompileText`, then written to
the .ell file's object code by orlib20's `WriteTokens`. If the INFOREC counts
are 0 when `WriteTokens` runs, the .ell file's object code has 0 counts, and
the populators return NULL when the linker later reads that .ell file. This
primary root cause is substantially identified but with a primary residual:
the INFOREC count population is NOT yet confirmed at runtime — the RED test
will confirm whether the INFOREC counts are 0 after a successful CLI
compilation.

A secondary finding (separate storage path): the compilation path also does
not update the master record's count fields
(`SIGNAL_RECORD+0xc6`/`+0xca`/`+0xe4`); `UpdateProperties` instead writes the
INFOREC element count to a different field (`SIGNAL_RECORD+0x75`).
`_GetMasterRecByType_16` re-fetches the master record (with 0 counts for a
newly created item) immediately before `UpdateProperties`, and the WHServer
cmd-0x0E handler faithfully copies those 0 counts into the CPrgList stored
record. This is a SEPARATE storage path from the .ell file's INFOREC that the
populators read; it may share a common cause with the INFOREC count gap or be
an independent defect. Its impact on the linker or other consumers is not yet
determined.

The M.4 note's `+0x24` offset was incorrect: the object-code version is at
INFOREC `+0x9` and the counts are at `+0x29`/`+0x1d`/`+0x31`. The subtype
hypothesis (R-M.3b.5) is falsified — TSDev passes `subtype=0` for
Strategy/Indicator/Function, identical to the CLI.

---

## 2. The Defect

CLI-compiled items have empty element arrays when the linker (ordinal 85,
`FUN_101a3340`, RVA `0x1A3340`) reads them. The linker's three populators
(`FUN_101a25a0`, `FUN_101a2430`, `FUN_101a22c0`) each return `NULL` when their
respective count is 0, so the linker produces no element/variable/array
entries for newly created CLI items. The original (now-corrected) description
of this behavior is in `RE_M4_REVERSE_DEPENDENT_REVALIDATION.md` §8.7–§8.8;
the correction is recorded in §8 of this note.

---

## 3. Root Cause (R-M.3b.2 answer)

### 3.1 R-M.3b.1 — Where is the version written?

The orlib20 INFOREC (0xDE bytes) carries the object-code version at `+0x9`,
NOT at `+0x24` as the M.4 note stated. The version is set to the constant
`DAT_10230394` in `InitTokenListNew` (RVA `0x1D900`):

```c
// InitTokenListNew @ 0x101D900
*(undefined4*)(_Dst + 9) = DAT_10230394;   // INFOREC+0x9 = version constant
```

The constant `DAT_10230394` has the value 147 (0x93), confirmed by reading
memory at `0x10230394` (bytes `93 00 00 00`, little-endian int32).

The linker `FUN_101a3340` (ordinal 85, RVA `0x1A3340`) reads the version at
`INFOREC+0x9` and applies a version gate:

```c
// FUN_101a3340 @ 0x101A3340 (linker, ordinal 85)
iVar4 = *(int*)((int)puVar5 + 9);          // INFOREC+0x9 = version
if (iVar4 > DAT_10230394) return <reject>; // always-reject branch
if (server_mode && iVar4 != DAT_10230394) return <reject>;
```

The three populators read COUNTS at `+0x29` (element), `+0x1d` (variable), and
`+0x31` (array) — NOT `+0x24`. Each returns `NULL` when its count is 0:

```c
// FUN_101a25a0 (element populator)
if (*(int*)(param_1 + 0x29) == 0) return (void*)0x0;
// FUN_101a2430 (variable populator)
if (*(int*)(param_1 + 0x1d) == 0) return (void*)0x0;
// FUN_101a22c0 (array populator)
if (*(int*)(param_1 + 0x31) == 0) return (void*)0x0;
```

The populators' `param_1` is a malloc'd copy of the INFOREC, decompressed from
the .ell file's object code via the warehouse COM storage — NOT the CPrgList
warehouse stored record. The evidence chain in `FUN_101a3340` (RVA `0x1A3340`,
ordinal 85) is:

1. `FUN_101d04f0(type, id, &local_18)` obtains the `CELWHouse` warehouse object
   for `(type, id)`.
2. `FUN_101c2a30(local_18)` (`0x101C2A30`) reads the object code SAFEARRAY from
   the warehouse COM storage (`*(local_18+0xc)+0x26`), copies it via
   `SafeArrayAccessData`/`memcpy` (`FUN_101c1170`), and decompresses it via
   `FUN_101dd260` (3 compression methods) → produces the INFOREC (0xDE bytes)
   at `local_18+0x1c1`. `FUN_101c2b90(local_18)` returns the INFOREC size at
   `local_18+0x1c5`.
3. The linker mallocs 0xDE bytes (at `0x101A3414`), zeroes it (at `0x101A342A`),
   copies the INFOREC from `local_1c` (the decompressed INFOREC from
   `FUN_101c2a30`) via the memcpy at `0x101A3435`–`0x101A34A6`, and reallocs it
   (`FUN_101215d0`) at `0x101A3552`. The counts at `+0x29`/`+0x1d`/`+0x31` are
   within the first 0xDE bytes, copied from `local_1c` via that memcpy.
4. The populators receive this realloc'd INFOREC copy as `param_1` via `ECX`
   (`ESI`):

   ```
   0x101a3626  MOV ECX,ESI          ; param_1 of FUN_101a25a0 (element)
   0x101a3633  CALL 0x101a25a0
   0x101a3655  MOV ECX,ESI          ; param_1 of FUN_101a2430 (variable)
   0x101a3664  CALL 0x101a2430
   0x101a3686  MOV ECX,ESI          ; param_1 of FUN_101a22c0 (array)
   0x101a3695  CALL 0x101a22c0
   ```

This is the .ell file's object code INFOREC, NOT the CPrgList warehouse stored
record. The SIGNAL_RECORD/CPrgList path (via elbardb cmd-0x0E) is a SEPARATE
storage path documented in §3.6 and §5.

The `+0x24` referenced in the M.4 note §1.5 (line 353,
`*(uint*)(record+0x24) > 0xf`) is a string-length gate on BST warehouse nodes —
a different record type, not the object-code version.

The compilation call chain is:

```
_VerifyModule_New@20   (0x169630)
  -> CELBasicCompiler::CompileText   (0x1DD80)
       -> UpdateProperties           (0x1FF60)
            -> FUN_101d3c00          (0x101D3C00, cmd-0x0E signal)
                 -> WHServer worker FUN_101d2150 case 0x0E
                      -> IWhouse::vtable+0x3c
```

### 3.2 R-M.3b.5 — Subtype hypothesis (FALSIFIED)

TSDev.exe passes `subtype=0` for Strategy, Indicator, and Function — identical
to the CLI's `getSubType(type)` (which returns 0 for these types). The TSDev
call site is `FUN_0044ba50` at `0x0044bbdf`, reading `doc+0x1ea` (the subtype
field). Per-type subtypes come from the document-class constructors:

| Type       | Constructor     | subtype |
|---|---|---|
| Strategy   | `FUN_0047ab90`  | 0       |
| Indicator  | `FUN_004588d0`  | 0       |
| Function   | `FUN_004140c0`  | 0       |
| TradingApp | `FUN_00413590`  | 0x14    |
| ShowMe     | `FUN_0047a4b0`  | 0x05    |

`FUN_101d4280(category, subtype)` (RVA `0x1D4280`) is the per-(category,
subtype) → version table:

| (category, subtype) | version | type      | note |
|---|---|---|---|
| (0, 0)  | 0     | Strategy  | returns 0x200 only when subtype ∈ {2, 3} |
| (2, 0)  | 2     | Indicator | |
| (4, 0)  | 4     | Function  | always 4 regardless of subtype |
| (2, 0x14) | 0x1402 | TradingApp | |
| (2, 0x05) | 0x502  | ShowMe     | |

**Conclusion:** `version=0` for Strategy is the inherent result of
`subtype=0` — TSDev's own New-Strategy path produces it too. No TSDev.exe path
sets the Strategy subtype to 2 or 3. Existing `0x200` Strategies were created
by a different source (platform/import), not by the TSDev New-Strategy path.

The subtype is stored in the master record at `+0xe9` by `FUN_10158fb0` during
`_CreateEntryEX@24`. It is read back during compilation via
`FUN_101cff60` (master-record lookup) → `local_44 >> 8` =
`master_record[+0xe9]`.

### 3.3 R-M.3b.3 — Compiler fields beyond M.1

The version passed to `CompileText` is
`FUN_101d4280(category, master_record[+0xe9])`, NOT a CELCompiler field. It is
derived from the master record's subtype byte, not from `+0x08`/`+0x0C`/`+0x10`/
`+0x18` (which M.1 sets). The `+0x28` field is not written by TSDev (per the
M.1 finding) and does not affect the version.

### 3.4 R-M.3b.4 — SaveModule flags / CELProperties

The version does not depend on `_SaveModule_New@20` flags or `CELProperties`.
It is computed from `(category, subtype)` via `FUN_101d4280` and passed as an
argument to `CompileText`.

### 3.5 R-M.3b.6 — Post-compilation step

There is no separate post-compilation "finalize" step that sets the version.
The version is computed in `_VerifyModule_New@20` before `CompileText` and
passed as an argument.

### 3.6 R-M.3b.2 — Why CLI produces version=0 / empty arrays (the actual divergence)

#### 3.6.1 Primary root cause — INFOREC counts not populated in the .ell file's object code

The populators read the INFOREC from the .ell file's object code (per §3.1),
NOT the CPrgList warehouse stored record. The INFOREC count fields
(`+0x29`/`+0x1d`/`+0x31`) are zeroed by `InitTokenListNew` (`0x1D900`) when it
creates the INFOREC:

```c
// InitTokenListNew @ 0x101D900
*(undefined4 *)(_Dst + 0x1d) = 0;  // variable count
*(undefined4 *)(_Dst + 0x29) = 0;  // element count
*(undefined4 *)(_Dst + 0x31) = 0;  // array count
```

They are expected to be populated by the compiler's token-list building
process during `CompileText` (`0x1DD80`). The .ell file's object code is
written by orlib20's `WriteTokens` (`FUN_0040FB50` in elbardb.dll, called
conditionally by the cmd-0x0E handler `FUN_00410390`) from the INFOREC after
compilation.

**Primary root cause (substantially identified, with a primary residual):** If
the INFOREC counts (`+0x29`/`+0x1d`/`+0x31`) are 0 when `WriteTokens` runs,
the .ell file's object code will have 0 counts, and the populators will return
NULL when the linker later reads that .ell file. The defect is therefore that
the INFOREC counts are not populated during CLI compilation (or are populated
but not written to the .ell file by `WriteTokens`). This is NOT yet confirmed
at runtime — the RED test will confirm whether the INFOREC counts are 0 after
a successful CLI compilation. If they are 0, M.3b.2 must investigate why the
compiler's token-list building process does not populate them for CLI items
(or whether `WriteTokens` drops them despite the compiler populating them).

#### 3.6.2 Secondary finding — SIGNAL_RECORD/CPrgList counts not updated (separate storage path)

The SIGNAL_RECORD/CPrgList path (via elbardb cmd-0x0E) is a SEPARATE storage
path from the .ell file's object code INFOREC that the populators read. It is
NOT what the populators consume. The compilation path does NOT update
`SIGNAL_RECORD+0xc6`/`+0xca`/`+0xe4` (the master record count fields that the
WHServer cmd-0x0E handler reads). Specifically:

- `UpdateProperties` (`0x1FF60`) writes the INFOREC element count to
  `SIGNAL_RECORD+0x75` (a different field), NOT to `+0xc6`. Disassembly:

  ```
  MOVZX EAX, word ptr [EDI+0x29]   ; INFOREC+0x29 = element count
  MOV    word ptr [ESI+0x75], AX   ; SIGNAL_RECORD+0x75 = (wrong field)
  ```

- No function in the `CompileText` call chain writes to
  `SIGNAL_RECORD+0xc6`/`+0xca`/`+0xe4`. Confirmed by a whole-program
  instruction scan of `orlib20.dll` (576,374 instructions).
- `_GetMasterRecByType_16` (`0x101A0460`) re-fetches the full 0x10A-byte
  master record into the `SIGNAL_RECORD` stack buffer immediately before
  `UpdateProperties` (the call is at `0x1001E19B` inside `CompileText`). For a
  newly created item, the master record's count fields are 0.
- `FUN_101d3c00` (`0x101D3C00`) passes the `SIGNAL_RECORD` buffer as-is via
  SafeArray (raw `memcpy` of 0x10A bytes).
- The WHServer cmd-0x0E handler (elbardb.dll `FUN_00421220` →
  `FUN_00410390`) faithfully copies:
  - `SIGNAL_RECORD+0xc6` → stored record `+0x29` (element count)
  - `SIGNAL_RECORD+0xca` → stored record `+0x2d` (variable count)
  - `SIGNAL_RECORD+0xe4` → stored record `+0x31` (array count)

Therefore: for ANY newly created item (CLI or TSDev UI), the CPrgList stored
record gets 0 counts because the compilation path doesn't propagate them from
the INFOREC into `SIGNAL_RECORD+0xc6`/`+0xca`/`+0xe4`. This is a real defect
in a separate storage path, but it is NOT the primary root cause of the
populators returning NULL. It may share a common cause with the INFOREC count
gap (both reflect the compilation path not propagating counts), or it may be
an independent defect. Its impact (if any) on the linker or other consumers is
not yet determined.

---

## 4. The Version Computer `FUN_101d4280(category, subtype)`

`FUN_101d4280` (RVA `0x1D4280`) is the per-(category, subtype) → version
table. Decompiled logic (the switch statement):

```c
// FUN_101d4280 @ 0x101D4280
undefined4 FUN_101d4280(int category, int subtype) {
  switch (category) {
    case 0:  // Strategy
      // ConvertToPowerType (0x1584f0): returns 2 only when subtype in {2,3}
      if ((subtype == 2) || (subtype == 3)) return 0x200;
      return 0;
    case 2:  // Indicator / TradingApp / ShowMe (subtype-disambiguated)
      return ConvertToPowerType_subtype_table(subtype); // 0x1402, 0x502, ...
    case 4:  // Function
      return 4;  // always 4, regardless of subtype
    default:
      return 0;
  }
}
```

Per-(category, subtype) → version table for all 5 supported types:

| (category, subtype) | version | type |
|---|---|---|
| (0, 0)    | 0     | Strategy   |
| (0, 2)    | 0x200 | Strategy (alt) |
| (0, 3)    | 0x200 | Strategy (alt) |
| (2, 0)    | 2     | Indicator  |
| (2, 0x14) | 0x1402| TradingApp |
| (2, 0x05) | 0x502 | ShowMe     |
| (4, 0)    | 4     | Function   |

`ConvertToPowerType` (`0x1584F0`) logic: category 0 returns 2 only when
subtype ∈ {2, 3}; category 2 maps subtypes to power types (e.g. 0x14 →
0x1402, 0x05 → 0x502).

---

## 5. The WHServer COM Server (elbardb.dll)

- **GUID:** `{7CEEA90C-D59E-11D3-8394-00C04F7BDF10}` (read from orlib20 at
  `0x101FAAB8`).
- **Implementing binary:** `C:\Program Files (x86)\TradeStation 9.5\Program\elbardb.dll`
  (confirmed by GUID byte search plus "ELWHouse"/"WHServer" string search).
- **IWhouse vtable:** at `0x00431704` (in `elbardb.dll`); slot 15 (`+0x3c`) =
  `FUN_00421220`.
- **cmd-0x0E handler chain:** `FUN_00421220` → `FUN_00410110` →
  `FUN_00410390` (the field updater).
- **`FUN_00410390` writes:**
  - `+0x29` from `SIGNAL_REC+0xc6` (element count)
  - `+0x2d` from `SIGNAL_REC+0xca` (variable count)
  - `+0x31` from `SIGNAL_REC+0xe4` (array count)
  - Conditionally calls `WriteTokens` (`FUN_0040FB50`).
- The version is NOT overwritten by cmd-0x0E; it is used only to locate the
  PrgList via `FUN_0041D310`.

**Storage-path note:** The WHServer cmd-0x0E path writes to the CPrgList (the
warehouse stored record), which is a SEPARATE storage path from the .ell
file's object code INFOREC that the populators read (see §3.1 and §3.6). The
cmd-0x0E path is NOT what the populators consume; the populators read the
INFOREC decompressed from the .ell file's object code via the warehouse COM
SAFEARRAY (`FUN_101c2a30`), not the CPrgList stored record. The cmd-0x0E
count gap is therefore a secondary finding (§3.6.2), not the primary root
cause of the populators returning NULL.

---

## 6. Fix Design (R-M.3b.7)

### 6.0 Investigation order — high-level TSDev emulation first (PM directive)

Per the AGENTS.md rule "Emulate TSDev.exe: replicate its APIs, enumeration
sequences, cache refreshes, and observable behavior; do not invent workarounds
where the application has a proper mechanism," M.3b.2 must prioritize the
**highest-level investigation first** and only descend to a deep fix if no
higher-level TSDev mechanism is found. The fix designs in §6.1 and §6.2 below
are **fallbacks** — only if the high-level investigation (Phase 1 of the
M.3b.2 strategy directive in `docs/workstream_tsdev_parity_plan.md` §M.3b.2)
finds no higher-level TSDev mechanism that the CLI is not already emulating.

The critical unanswered question (§7 residual 2) is: **what does TSDev do
differently at a higher level that results in non-zero INFOREC counts?** The
CLI already calls `_VerifyModule_New@20` with the same category/subtype/ProgId
as TSDev (M.1 confirmed compiler object fields match; M.3b.1 confirmed
subtype=0 for both). If the CLI is calling the same function with the same
arguments, the difference must be in a step TSDev performs that the CLI
doesn't — a pre/post-verify call, a different save+verify ordering, a cache
refresh, a warehouse flush, or a finalize step. M.3b.2 Phase 1 must trace
TSDev's verify/save path from the UI button down and compare it with the
CLI's `saveSource` + `verifyModule` sequence before considering any deep fix.

If Phase 1 finds a higher-level difference, the fix is to emulate the TSDev
path — not to patch INFOREC buffers or SIGNAL_RECORD fields. Only if Phase 1
confirms the CLI already does exactly what TSDev does (same functions, same
order, same arguments) is the deep fix in §6.1 justified, and it would be a
workaround for an orlib20 behavior, not a TSDev emulation gap.

### 6.1 Fallback fix — INFOREC count population (only if Phase 1 finds no higher-level difference)

The primary root cause (§3.6.1) is that the INFOREC counts
(`+0x29`/`+0x1d`/`+0x31`) are not populated during CLI compilation, so
`WriteTokens` writes 0 counts to the .ell file's object code, and the
populators return NULL when the linker later reads that .ell file.

**Fallback fix (only if M.3b.2 Phase 1 finds no higher-level TSDev mechanism):**
The fix must ensure the INFOREC counts are populated by the compiler's
token-list building process during `CompileText` (`0x1DD80`), so that
`WriteTokens` (`FUN_0040FB50`) writes non-zero counts to the .ell file's
object code. The exact mechanism — why the compiler doesn't populate the
counts for CLI items, or whether `WriteTokens` writes 0 counts despite the
compiler populating them — is NOT yet determined. This is the primary residual
for M.3b.2 and is gated on the RED test confirmation (§7 residual 1) that the
INFOREC counts are actually 0 after a successful CLI compilation.

The fix location and mechanism are deferred to M.3b.2, pending that RED test
confirmation. No production code change is specified here; specifying one now
would be premature given the unconfirmed runtime behavior. The deep fix must
be the cleanest possible form — prefer an additional orlib20 call that
triggers count population over directly patching INFOREC buffers.

### 6.2 Secondary fix — SIGNAL_RECORD count propagation (NOT what unblocks the populators)

The previous fix design (propagating INFOREC counts into `SIGNAL_RECORD`
before cmd-0x0E) addresses the secondary finding in §3.6.2, NOT the primary
root cause. It is reproduced below for reference but is NOT what unblocks the
populators:

```
SIGNAL_REC+0xc6 = INFOREC+0x29   ; element count
SIGNAL_REC+0xca = INFOREC+0x1d   ; variable count
SIGNAL_REC+0xe4 = INFOREC+0x31   ; array count
```

This would have to happen AFTER `_GetMasterRecByType_16` re-fetches the master
record (which overwrites the `SIGNAL_RECORD` with 0 counts) and BEFORE
`FUN_101d3c00` sends cmd-0x0E. It may still be needed as a secondary fix (the
CPrgList warehouse storage also has 0 counts), but it is NOT what unblocks the
populators, because the populators do not read the CPrgList stored record.

**Hack (NOT recommended):** Patching the stored record's counts after
cmd-0x0E returns. This would be a post-hoc buffer patch, not a
compilation-path fix, and would violate the no-hacks rule
(`AGENTS.md` hard rule 5).

**Note:** The version at INFOREC `+0x9` is set to `DAT_10230394` (value 147 /
0x93, see §3.1) by `InitTokenListNew` and is NOT the problem. The `version=0`
observation in the M.3b plan was based on the incorrect `+0x24` offset.

---

## 7. Open Residuals

1. **INFOREC count population (PRIMARY residual).** The research confirms the
   populators read the .ell file's INFOREC counts (`+0x29`/`+0x1d`/`+0x31`,
   per §3.1), and that `InitTokenListNew` (`0x1D900`) zeros them, but does NOT
   confirm whether the compiler's token-list building process populates them
   during `CompileText` (`0x1DD80`), nor whether `WriteTokens`
   (`FUN_0040FB50`) writes them to the .ell file. The RED test will confirm
   whether the INFOREC counts are 0 after a successful CLI compilation. If
   they are 0, M.3b.2 must investigate why the compiler doesn't populate them
   (or whether `WriteTokens` drops them despite the compiler populating them).
2. **How existing TSDev-created items get non-zero INFOREC counts (CRITICAL
   for M.3b.2 Phase 1).** If the INFOREC counts are 0 for CLI-compiled items,
   how do existing TSDev-created items get non-zero INFOREC counts in their
   .ell files? This is the highest-priority question for M.3b.2: the answer
   determines whether the fix is a high-level TSDev emulation (emulate a
   missing step) or a deep internal patch (§6.1 fallback). Possibilities:
   (a) a different compilation path (not `_VerifyModule_New@20`) that does
   populate the counts; (b) the platform/runtime updates them; (c) an import
   path sets them; (d) a TSDev UI pre/post-verify step that the CLI doesn't
   replicate (a finalize, commit, cache refresh, warehouse flush, or second
   verify pass); (e) a different save+verify ordering. M.3b.2 Phase 1 must
   answer this by tracing TSDev's verify/save path from the UI button down
   and comparing it with the CLI's `saveSource` + `verifyModule` sequence.
3. **SIGNAL_RECORD/CPrgList counts (SECONDARY).** The SIGNAL_RECORD counts
   (`+0xc6`/`+0xca`/`+0xe4`) are not updated during compilation (§3.6.2), so
   the CPrgList warehouse storage also has 0 counts. This is a SEPARATE
   storage path from the .ell file's object code INFOREC. It may share a
   common cause with the INFOREC count gap, or it may be an independent
   defect. Its impact (if any) on the linker or other consumers is not yet
   determined.
4. **The `+0x75` field.** `UpdateProperties` writes the INFOREC element count
   to `SIGNAL_RECORD+0x75`. The semantic of this field is unconfirmed
   (possibly "max inputs" or a display attribute). It is NOT the field the
   linker reads.
5. **Variable count offset discrepancy.** The WHServer handler writes
   variable count to stored record `+0x2d` (from `SIGNAL_REC+0xca`), but the
   orlib20 INFOREC has variable count at `+0x1d`. These are different
   structures (master record vs INFOREC); the offset difference is expected.

---

## 8. Correction to the M.3b plan baseline and M.4 note §8.8

The M.3b plan baseline (`docs/workstream_tsdev_parity_plan.md` §M.3b, "Known
factual baseline", lines 275–276) states that the populators read "the
decompressed object-code version at `buffer+0x24`" and that "CLI-compiled
items have `buffer+0x24 = 0`". The M.4 note §8.8
(`RE_M4_REVERSE_DEPENDENT_REVALIDATION.md` line 1416) says "CLI-compiled items
have empty element arrays due to the separate CLI-created object-code defect
(version=0, element count=0)" without citing `+0x24` as the version offset.
Both are incorrect in their characterization of the mechanism.

**Correct statement:**
- The version is at INFOREC `+0x9` (constant `DAT_10230394`, value 147 / 0x93,
  set by `InitTokenListNew` at RVA `0x1D900`).
- The populators read COUNTS at `+0x29`/`+0x1d`/`+0x31` from the INFOREC
  decompressed from the .ell file's object code (NOT the CPrgList stored
  record), and return `NULL` when a count is 0.
- The `+0x24` in the M.4 note §1.5 (line 353,
  `*(uint*)(record+0x24) > 0xf`) is a string-length gate on BST warehouse
  nodes — a different record type, not the object-code version.
- CLI-compiled items have empty element arrays because the INFOREC counts
  (`+0x29`/`+0x1d`/`+0x31`) are not populated during CLI compilation (the
  primary root cause, see §3.6.1). A secondary finding is that the master
  record counts (`+0xc6`/`+0xca`/`+0xe4`) are also not updated during
  compilation, leaving the CPrgList stored record with 0 counts on a separate
  storage path (see §3.6.2).

Per the correction-prepend convention demonstrated by the M.4 note's own
inline "Correction to a prior draft" pattern (line 487), this correction is
recorded here rather than by rewriting the M.3b plan or M.4 note. Both should
be consulted with this correction in mind.

---

## 9. Relationship to M.4

This diagnosis identifies the primary root cause of empty element arrays for
CLI-compiled items: the INFOREC counts (`+0x29`/`+0x1d`/`+0x31`) are not
populated during CLI compilation (§3.6.1), so `WriteTokens` writes 0 counts to
the .ell file's object code, and the populators return NULL. This primary root
cause must be confirmed by the RED test (§7 residual 1) and fixed in M.3b.2
before M.4's `test_reverse_dependent_revalidation.cpp` dependency cases can
work for CLI-compiled items — the linker reads the .ell file's INFOREC, not
the CPrgList stored record, so M.4's dependency test cannot pass until the
.ell file's INFOREC has non-zero counts.

The SIGNAL_RECORD/CPrgList count propagation (§6.2, the previous fix design)
is a secondary fix for a separate storage path (§3.6.2). It is NOT what
unblocks the populators and is NOT a prerequisite for M.4's dependency test.
It may still be needed for other consumers of the CPrgList stored record, but
that is independent of the M.4 dependency-test blocker.

---

## 10. M.3b.2a Phase 1 Results — RED Test + High-Level Path Comparison

**Date:** 2026-08-27
**Scope:** Diagnosis-only. No production code changed. RED test wired into
CMake as a manual admin target (not in `test_regression` or
`$TestDefinitions`). Full Phase 1 research report:
`tmp/m3b2a_phase1_research.md` (transient); durable findings below.

### 10.1 RED Test — Runtime Confirmation of the Defect

The RED test (`tests/test_m3b_object_code_version.cpp`) was built and run from
an elevated process (`p14s\user`, `Elevated=True`) against live
TradeStation. Result: **FAIL (exit code 1) — RED pin confirmed.**

| Observation | Value |
|---|---|
| Function `TS_AUTO_M3B_VER_FUNC` | inserted + verified OK (ProgId=1489) |
| `verifyModule` | returned OK (no compilation errors) |
| `Ordinal_28(4, 1489, 0, buf)` | ret=1 (partial master record retrieved) |
| Partial copy `+0x29` elemCount | **0** (master record also shows 0) |
| Linker (ordinal 85) | returned a record (rec=381D5480, no SEH) |
| `record+0x29` (elemCount) | **0** [FAIL — expected > 0] |
| `record+0x4f` (elemArrayPtr) | **NULL** [FAIL — expected != NULL] |
| Cleanup | `deleteEntry` returned true; `.ell` file gone |
| `TSCrashReport.exe` | NOT running before or after (no crash) |

**Conclusion:** §7 residual 1 is RESOLVED. The INFOREC counts (`+0x29`/
`+0x1d`/`+0x31`) ARE 0 after a successful CLI compilation. The linker returns
a record but with empty element arrays (elemCount=0, elemArrayPtr=NULL). The
partial master record (from `Ordinal_28`) also shows elemCount=0 at `+0x29`,
consistent with §3.6.2 (master record counts also not updated).

Raw test output preserved at `tmp/m3b2a_red_test_result.txt`.

### 10.2 Phase 1 — High-Level TSDev vs CLI Path Comparison

The full TSDev verify chain was traced from the WM_COMMAND 0x296A handler
down through four function levels to `_VerifyModule_New@20`. Every level was
decompiled and every orlib20 ordinal resolved via the IAT.

**TSDev verify call chain:**
```
FUN_0047B0E0  (WM_COMMAND 0x296A handler)
  -> FUN_00451800  (verify entry: save + verify + post)
       -> FUN_00452290  (save: _SaveModule_New@20)
       -> Ordinal_25  (add to "verifying" cache — bookkeeping only)
       -> Ordinal_56  (clear DAT_1023aac8 — bookkeeping only)
       -> Ordinal_66  (allocate CELCompiler)
       -> set compiler[2/3/4/6] = category/subtype/progId/hwnd
       -> FUN_004D9480  (verify worker)
            -> StartCompilingProcess
            -> zero compiler[0]/compiler[1]
            -> FUN_004D91E0  (verify + reverse-dependent recursion)
                 -> _VerifyModule_New@20(hwnd, cat, progId, flag=1, compiler)
            -> EndCompilingProcess
       -> Ordinal_67  (destroy CELCompiler)
       -> Ordinal_17  (remove from "verifying" cache — bookkeeping only)
       -> FUN_00495590(0x400, 0)  (post 0x40A notification)
       -> UpdateAllViews  (MFC UI refresh)
```

**Side-by-side comparison (steps that touch .ell or compiler):**

| Step | CLI | TSDev | Same? |
|---|---|---|---|
| Save source | `_SaveModule_New@20(cat, progId, &flags, hwnd, pw)` | `_SaveModule_New@20(cat, sub, flags, hwnd, pw)` | **YES** |
| Allocate compiler | `Ordinal_66` | `Ordinal_66` | **YES** |
| Set compiler fields | `+0x08/+0x0C/+0x10/+0x18` | `compiler[2/3/4/6]` (same offsets) | **YES** |
| Start compiling | `StartCompilingProcess` | `StartCompilingProcess` | **YES** |
| Zero result fields | `compiler[0]=0, [1]=0` | `*param_1=0, [1]=0` | **YES** |
| Primary verify | `_VerifyModule_New@20(hwnd, cat, progId, flag=1, comp)` | same (flag=1 for primary) | **YES** |
| End compiling | `EndCompilingProcess(0)` | `EndCompilingProcess(0)` | **YES** |
| Destroy compiler | `Ordinal_67` | `Ordinal_67` | **YES** |
| 0x40A notification | `Ordinal_57` (on success) | `FUN_00495590(0x400, 0)` | **YES** |

**Five TSDev-only steps (all UI-only or in-process bookkeeping, no .ell write):**

1. `Ordinal_25` / `Ordinal_17` — "verifying" cache (re-entrancy guard for UI
   recursive dependent verify). No .ell write.
2. `Ordinal_56` — clears `DAT_1023aac8` (already 0 in a fresh CLI process).
   No .ell write.
3. `Ordinal_28` + `Ordinal_69` — read-only INFOREC open for the
   skip-same-dependent comparison. No .ell write.
4. MFC `UpdateAllViews` / `OnIdle` — UI refresh. No orlib20 call.
5. No orlib20/elbardb call between `EndCompilingProcess` and `Ordinal_67`
   writes to the .ell file.

**Conclusion:** §7 residual 2 is RESOLVED. The CLI already does exactly what
TSDev does at the higher level — same functions, same order, same arguments
for the steps that touch the .ell file or the compiler. The INFOREC count gap
is NOT caused by a missing higher-level TSDev step. The root cause must be
INSIDE `_VerifyModule_New@20` / `CELBasicCompiler::CompileText` — i.e., a
compilation-path issue where the compiler populates the counts for TSDev's
process context but not for the CLI's process context, OR the counts are
populated for both but the CLI's `_SaveModule_New` write path discards them.

### 10.3 M.4 Blocker-Escalation Flag Confirmation

The Phase 1 disassembly **confirms** the AGENTS.md M.4 blocker escalation
(`AGENTS.md` line 131) and the M.4 RE note
(`RE_M4_REVERSE_DEPENDENT_REVALIDATION.md` lines 167-177): TSDev's
`FUN_004D91E0` at `0x004d9278`–`0x004d9299` computes the
`_VerifyModule_New@20` 4th-arg flag as
`(compiler[2]==category && compiler[4]==progId) ? 1 : 0`. For the PRIMARY
(root) verify, the args come straight from `compiler[2]`/`compiler[4]`, so
the flag is **1** — matching the CLI's primary verify. For RECURSIVE
DEPENDENT verifies, the dependent's category/progId differ from the
compiler's saved root values, so the flag is **0** — matching the AGENTS.md
escalation's "primary=0 for a reverse dependent." The CLI's `callVerify()`
hardcodes flag=1 for dependents (`src/tsdev_lib.cpp:178`), which IS a real
divergence from TSDev (dependent flag mismatch) — but that is an
M.4-dependent-verify issue, not the M.3b.2 INFOREC-count issue (the INFOREC
counts are populated during the primary verify's `CompileText`, not the
dependent verify). No correction to the AGENTS.md SSOT is warranted.

---

## 11. Bounded Fix Design for M.3b.2b

Per §6.0, the fallback condition is met: Phase 1 found no higher-level TSDev
mechanism missing from the CLI. The fix must be at the deep level (Phase 2).

### 11.1 Investigation targets for M.3b.2b

1. **`CELBasicCompiler::CompileText` (orlib20 RVA 0x1DD80)** — trace where
   the INFOREC counts (`+0x29`/`+0x1d`/`+0x31`) are written during
   token-list building. The `RecordArrays` function (RVA 0x147F0) increments
   `INFOREC+0x31` (array count) and writes array entries; similar functions
   exist for elements (`+0x29`) and variables (`+0x1d`). The question is
   whether these run in the CLI's compilation path and whether the INFOREC
   they populate is the same one that `WriteTokens` writes to the .ell file.

2. **`InitTokenListNew` (orlib20 RVA 0x1D900)** — confirms it zeros the
   counts. Already decompiled (§3.6.1).

3. **`WriteTokens` (elbardb.dll `FUN_0040FB50`)** — determines whether it
   writes the INFOREC from the in-memory compiler object (which would have
   populated counts) or from a separate buffer.

4. **`DAT_1023aac8` (global "verify in progress" flag)** — referenced by 239
   sites in orlib20 including `CompileText`, `RecordArrays`, `RecordVARs`,
   `RecordInputs`, `RecordFuncInputs`, `RecordAttributes`, `RecordConsts`,
   and `RecordCustField`. `Ordinal_56` zeros it before verify. The CLI does
   NOT call `Ordinal_56`, but `DAT_1023aac8` should be 0 in a fresh CLI
   process. **However**, if a prior CLI operation left it non-zero, the
   Record* functions might skip count population. This is a candidate root
   cause to verify dynamically (Frida trace of `DAT_1023aac8` during CLI
   compilation).

### 11.2 Fix approach (bounded)

The fix must ensure the INFOREC counts are populated by the compiler's
token-list building process during `CompileText`, so that `WriteTokens`
writes non-zero counts to the .ell file's object code. The exact mechanism
will be determined by the Phase 2 investigation above. Two candidate fixes:

**Candidate A (preferred — if `DAT_1023aac8` is the issue):** Call
`Ordinal_56` (clear `DAT_1023aac8`) before `StartCompilingProcess` in the
CLI's `verifyModule`, matching TSDev's pre-verify state exactly. This is
harmless defensive parity even if `DAT_1023aac8` is already 0, and it
matches TSDev's exact pre-verify sequence. If a prior CLI operation left
`DAT_1023aac8` non-zero (e.g., a failed verify or a save without verify),
this would fix the count population.

**Candidate B (if the Record* functions run but counts are lost before
write):** Investigate whether `WriteTokens` writes the INFOREC from the
compiler's in-memory INFOREC (populated) or from a separate buffer (empty).
If the latter, the fix is to ensure the populated INFOREC is the one written.

**Candidate C (if the Record* functions do NOT run in the CLI path):**
Investigate why the token-list building process skips count population for
CLI items. This would require tracing the `CompileText` path with Frida to
identify the branch that skips the Record* calls.

### 11.3 What M.3b.2b must do

1. Frida-trace `DAT_1023aac8` during a CLI compilation to confirm whether it
   is 0 when `CompileText` runs.
2. Frida-trace `RecordArrays` / `RecordVARs` / `RecordInputs` to confirm
   whether they run and whether they increment the INFOREC counts.
3. Frida-trace `WriteTokens` to confirm which INFOREC buffer it writes.
4. Based on the trace results, implement the appropriate fix (Candidate A, B,
   or C).
5. Make the RED test pass.
6. Run the full CLI E2E, dependency-graph, and operation-mode gates.
7. Wire the now-green test into `test_regression` and `$TestDefinitions`.

### 11.4 Optional low-risk CLI hardening (not the root cause)

Add `Ordinal_56` (clear `DAT_1023aac8`) before `StartCompilingProcess` for
defensive parity, even if it should already be 0 in a fresh CLI process.
This is harmless and matches TSDev's exact pre-verify state. Do NOT add
`Ordinal_25`/`Ordinal_17` (the verify-cache) — they are re-entrancy guards
for the UI's recursive dependent verify and have no effect on a single-item
CLI verify.

---

## 12. M.3b.2b Phase 2 Dynamic Investigation Results

**Date:** 2026-08-28
**Scope:** Diagnosis-only. No production code changes. No test-source changes.
No changes to `CMakeLists.txt`, `signatures/`, or `tools/`.
Only transient files under `tmp/` were created, plus this appended section.

**Method:** Frida 17.17.0 directly from the CLI (Python-driven spawn mode)
against the standalone RED test process
`build\Release\test_m3b_object_code_version.exe`.
Raw trace output: `tmp/m3b2b_frida_trace.log`.
Frida script: `tmp/m3b2b_frida_trace.js`. Driver: `tmp/m3b2b_run_frida.py`.

### Q1: What value does `DAT_1023aac8` have when `CompileText` executes?

**Answer: 0 (zero) throughout the entire compilation.**

Raw evidence from the trace:

```
>> _VerifyModule_New onEnter
  DAT_1023aac8 before VerifyModule = 0

========== CompileText onEnter (depth=1) ==========
  Q1: DAT_1023aac8 (verify-in-progress flag) = 0

  >> CodeScanner onEnter
  DAT_1023aac8 before CodeScanner = 0
  >> CodeScanner onLeave, retval = 0x0
  DAT_1023aac8 after CodeScanner = 0

  >> _VerifyModule_New onLeave, retval = 0x1
  DAT_1023aac8 after VerifyModule = 0
```

`DAT_1023aac8` is 0 before, during, and after `CompileText`.
It is NOT stale or nonzero. **Candidate A is ruled out.**

### Q2: Which compiler `Record*` functions execute, and what INFOREC counts exist after compilation?

**Answer: NONE of the `Record*` functions execute. INFOREC counts remain 0.**

The full call chain observed was:

```
_VerifyModule_New(hwnd, cat=4, progId=0x5db, flag=1, compiler)
  → CompileText(this, src, cat=4, progId=0x5db, ..., param_6=0, ...)
    → CodeScanner(this, src, cat=4, progId=0x5db, ..., param_6=0, ...)
      → ScanATSourceCode(this, src, cat=4, progId=0x5db, ...)
        → ScanComponentRegionAndMain(this, src, cat=4, progId=0x5db, ...)
          → InitTokenListNew(this, 0x32, 0x0) → INFOREC=0x15064b8, counts all 0
          → InitTokenListNew(this, 0x4, 0x5db) → INFOREC=0x15062e8, counts all 0
          → ScanSourceCode(this, src, cat=4, progId=0x5db, ...) → retval=0 (success)
        → retval=0 (success)
      → retval=0 (success)
    → UpdateProperties(this, cat=4, progId=0x5db, ...)
  → retval=1 (success)
```

Hooked functions and their execution status:

| Function | RVA | Called? | Notes |
|---|---|---|---|
| `CompileText` | 0x1DD80 | YES | retval=1 (success) |
| `CodeScanner` | 0x1E8E0 | YES | retval=0 (success) |
| `ScanATSourceCode` | 0x1E6E0 | YES | retval=0 (success) |
| `ScanComponentRegionAndMain` | 0x1E4C0 | YES | retval=0 (success), FULL SCAN path taken |
| `InitTokenListNew` | 0x1D900 | YES (×2) | Both returned INFOREC with counts all 0 |
| `ScanSourceCode` | 0x11A70 | YES (called from within ScanComponentRegionAndMain) | retval=0 |
| `UpdateProperties` | 0x1FF60 | YES | INFOREC already freed by this point |
| `RecordArrays` | 0x147F0 | **NO** | Not called |
| `RecordVARs` | 0x153F0 | **NO** | Not called |
| `RecordInputs` | 0x17EE0 | **NO** | Not called |
| `GetTokenOnlyInfo` | 0x1F310 | NO | Token-only path NOT taken |
| `UpdateTokenOnly` | 0x1F140 | NO | Token-only path NOT taken |

**Record* call count: 0.**

`InitTokenListNew` was called twice:
1. `param_1=0x32 (50)`, `param_2=0x0` → INFOREC allocated and zeroed
   (`+0x29=0, +0x1d=0, +0x31=0`)
2. `param_1=0x4 (Function category)`, `param_2=0x5db (ProgId)` → INFOREC
   allocated and zeroed (`+0x29=0, +0x1d=0, +0x31=0`)

The `ScanComponentRegionAndMain` (full scan path) was taken — NOT the
token-only path. `ScanSourceCode` ran and returned 0 (success). But
`RecordArrays`, `RecordVARs`, and `RecordInputs` were never called.

**Why the Record* functions were not called:** Static analysis of
`ScanSourceCode` (RVA 0x11A70) shows it is a token-by-token scanner that
calls `GetWord` and `IdentifySymbol` in a loop, dispatching to `MakeSymbol`
and other handlers based on the identified token type. The `Record*`
functions are called from specific token-type cases (e.g., when an `Input`
declaration is encountered). The RED test source
`TS_AUTO_M3B_VER_FUNC = 1;\r\n` contains no explicit `Input`, `Variable`,
or `Array` declarations, so no token triggers the `Record*` calls.

**Candidate C is confirmed: the Record* functions do not execute, so count
population is skipped.**

### Q3: Which INFOREC buffer does `WriteTokens` write, and with what counts?

**Answer: `WriteTokens` was not observed. `elbardb.dll` was never loaded
during the trace.**

`elbardb.dll` (the WHServer COM server containing `WriteTokens`) was not
loaded by the RED test process. The `WriteTokens` hook at
`elbardb+0xFB50` (speculative, from the findings note's `FUN_0040FB50`)
was never triggered. Since the INFOREC counts were 0 after compilation,
and the `.ell` file was created (and later deleted during cleanup), the
`.ell` file was written with 0 element/variable/array counts.

The elbardb `WriteTokens` address remains unconfirmed. The findings note's
`FUN_0040FB50` (image base 0x00400000, RVA 0xFB50) maps to
`0x1000FB50` with elbardb's actual preferred base 0x10000000, but Ghidra
shows `FID_conflict:_Tidy` at that address, not a WriteTokens
implementation. Since elbardb.dll was not loaded, this could not be
resolved dynamically. However, since the INFOREC counts were 0 before any
write, the WriteTokens buffer question is secondary — the counts were
never populated in the first place.

**Candidate B is ruled out:** the issue is not that `WriteTokens` writes a
different buffer. The counts were never populated by the `Record*`
functions, so there were no counts to write regardless of which buffer
`WriteTokens` uses.

### Selected Candidate

**Candidate C is confirmed.** The `Record*` functions (`RecordArrays`,
`RecordVARs`, `RecordInputs`) do not execute during CLI compilation of the
test source, so INFOREC count population is skipped. The compilation
succeeds (returns 1), but the INFOREC is written with all counts at 0.

Candidate A is ruled out (`DAT_1023aac8 = 0` throughout).
Candidate B is ruled out (counts were never populated, so the WriteTokens
buffer is not the issue).

### Exact M.3b.2c Fix Mechanism

The trace confirms that the CLI takes the full scan path
(`ScanComponentRegionAndMain` → `ScanSourceCode`), not the token-only
path. `ScanSourceCode` runs successfully but does not call any `Record*`
functions because the source `TS_AUTO_M3B_VER_FUNC = 1;\r\n` has no
explicit `Input`, `Variable`, or `Array` declarations.

The fix for M.3b.2c must ensure that the INFOREC element count (`+0x29`)
is populated even for simple function sources with no explicit
declarations. The exact mechanism depends on how TSDev's UI path produces
nonzero element counts for the same source. Two possible approaches:

1. **Post-compilation element injection:** After `ScanSourceCode` returns
   0, call `RecordInputs` (or an equivalent function) with an implicit
   element for the function return value. This would require identifying
   the exact `RecordInputs` calling convention and the implicit element
   data that TSDev creates.

2. **Source preprocessing:** Modify the source before compilation to
   include explicit declarations that trigger the `Record*` functions.
   This would require understanding what implicit declarations TSDev adds.

The trace does not provide direct evidence of which approach TSDev uses,
because the TSDev UI path was not traced (UI operation is prohibited).
M.3b.2c should investigate `ScanSourceCode`'s token dispatch more closely
to determine whether there is a code path that creates implicit elements
for function definitions, or whether the UI adds source preprocessing.

**Key function to investigate for M.3b.2c:** `ScanSourceCode` (RVA
0x11A70) — specifically, the `GetPreCompiledHeader` call at its entry and
the `MakeSymbol` / `IdentifySymbol` dispatch loop, to determine whether
the UI path creates an implicit element that the CLI path does not.

### RED-Test Source Sanity Check

The RED test source is `TS_AUTO_M3B_VER_FUNC = 1;\r\n`. This source
contains:
- No `Input` declarations
- No `Variable` declarations
- No `Array` declarations
- No multiple elements

The test asserts `record+0x29 (elemCount) > 0` and
`record+0x4f (elemArrayPtr) != NULL`. The trace confirms that with this
source, the `Record*` functions are never called and the INFOREC counts
remain 0. The assertion that element count should be nonzero for a
function with no explicit inputs is **questionable** — it depends on
whether TradeStation creates an implicit element for the function's
return value. If TSDev's UI compilation of this same source also produces
0 elements, then the test's assertion is incorrect and the test source
should be modified to include explicit declarations (e.g., `Inputs:
Price (NumericSimple);` or similar). If the UI produces nonzero elements,
then the CLI path is missing an implicit-element creation step.

**M.3b.2c must validate this by either:**
- Confirming through static analysis that TSDev creates an implicit
  element for functions, or
- Modifying the RED test source to include explicit `Input` declarations
  that trigger `RecordInputs`, which would be a more robust test of the
  count-population mechanism.

### Obstacles and Address Ambiguities

1. **Frida 17 API changes:** `Module.findExportByName(moduleName, name)`
   and `Module.findBaseAddress(name)` are removed in Frida 17. Replaced
   with `Module.findGlobalExportByName(name)` and
   `Process.findModuleByName(name).base`. `setInterval`/`setTimeout` are
   not available in Frida's JS runtime. Resolved by using a Python-driven
   RPC approach (`rpc.exports` with `exports_sync`).

2. **elbardb `WriteTokens` address:** The findings note's `FUN_0040FB50`
   (RVA 0xFB50) maps to an unconfirmed function in Ghidra
   (`FID_conflict:_Tidy`). Since `elbardb.dll` was not loaded during the
   trace, this could not be resolved dynamically. This is a secondary
   issue since the INFOREC counts were 0 before any write.

3. **INFOREC lifetime:** The INFOREC buffer is freed before
   `CompileText` returns. Attempts to read counts at `CompileText`
   onLeave and `UpdateProperties` onLeave caused access violations
   (`Error: access violation accessing 0x2a`). The INFOREC is a
   temporary compilation structure, not a persistent buffer.

4. **SIGNAL_RECORD flags at onEnter:** The `SIGNAL_RECORD` flags
   (`+0x79`, `+0xd8`, `+0xcc`) were all 0 when read at
   `ScanATSourceCode` onEnter because they are populated inside the
   function (by `FUN_101cff60`), not before it. The full scan path was
   confirmed by the `ScanComponentRegionAndMain` hook firing.

### Crash Status

**No crash occurred.** `TSCrashReport.exe` was not running before or
after the trace. The test process exited normally with exit code 1
(expected RED failure). The Frida session detached cleanly with
`reason=process-terminated`.

### Files Changed

- **No files under `src/` were modified.**
- **No files under `tests/` were modified.**
- **No changes to `CMakeLists.txt`, `signatures/`, or `tools/`.**
- Transient files created under `tmp/`:
  - `tmp/m3b2b_frida_trace.js` (Frida script)
  - `tmp/m3b2b_run_frida.py` (Python driver)
  - `tmp/m3b2b_frida_trace.log` (raw trace output)
  - `tmp/m3b2b_api_test.js` (API test, transient)
  - `tmp/m3b2b_api_test.py` (API test driver, transient)
  - `tmp/m3b2b_minimal_test.js` (minimal test, transient)
- This findings note section 12 was appended.

---

## 13. M.3b.2b PM Verification — Defect Premise Is a Test Artifact

**Date:** 2026-08-27 (PM direct verification, elevated process)
**Scope:** Diagnosis-only. No production code changed. The RED test source
(`tests/test_m3b_object_code_version.cpp`) was temporarily modified for
verification runs and then restored to its exact original content (SHA-256
`619012C26655AAAADC4533B930E84E9EE612BABF85559131B7C770F6A4DB63DC` confirmed
before and after). No `src/`, `CMakeLists.txt`, `signatures/`, or `tools/`
changes.

### 13.1 The critical finding

The §12 Frida trace confirmed Candidate C (Record* functions don't fire) for
the RED test's declaration-free source (`TS_AUTO_M3B_VER_FUNC = 1;\r\n`). The
§12 "RED-Test Source Sanity Check" flagged the test premise as questionable
but did not have the answer. The PM verification provides the answer:

**The M.3b "defect" (elemCount=0 for CLI-compiled items) is a TEST ARTIFACT
caused by the test using a declaration-free source. For the two source/type
combinations tested (Function with `Inputs:`, Strategy with `Inputs:` +
function call), CLI-compiled items WITH declarations produce non-zero
elemCount and non-NULL elemArrayPtr. The general claim awaits broader testing
across all five supported types and larger sources (see §13.6).**

### 13.2 Verification runs (all from elevated process, live TradeStation)

| # | Source | elemCount | elemArrayPtr | Result | Exit |
|---|--------|-----------|--------------|--------|------|
| 1 | Function, no declarations: `X = 1;\r\n` | 0 | NULL | FAIL (RED pin) | 1 |
| 2 | Function, WITH declarations: `Inputs: x(Numeric);\nVariables: y(0);\nX = x + 1;\r\n` | **1** | **NON-NULL** | **PASS** | **0** |
| 3 | Strategy, only Variables + function call: `Variables: result(0);\nresult = FUNC(Close);\r\n` | 0 | NULL | FAIL | 1 |
| 4 | Strategy, WITH Inputs + Variables + function call: `Inputs: x(10);\nVariables: result(0);\nresult = FUNC(x);\r\n` | **1** | **NON-NULL** | elemCount PASS, elemArrayPtr PASS | 1* |

*Run 4 exit code 1 is due to the dep-list scan failing (see §13.3), not
elemCount. The elemCount and elemArrayPtr assertions PASSED.

### 13.3 Run 4 — the real M.4 use case (Strategy with Inputs + function call)

This is the actual M.4 scenario: a Strategy that declares `Inputs:` and calls
a Function. The CLI-compiled Strategy produced:
- `record+0x29 (elemCount) = 1` → **PASS** (elements ARE populated)
- `record+0x4f (elemArrayPtr) = NON-NULL` → **PASS**
- `elem[0]+0x7b depListPtr = NON-NULL` (0x3B896A68) → a dep-list exists
- `elem[0]+0x7f depListSize = 978827072 (0x3A5400A0)` → **garbage value** (the
  dep-list size offset `+0x7f` is WRONG — this is not a real size)
- No 0xFA3 entry found → **FAIL** (but the scan was corrupted by the wrong
  size offset, so this is inconclusive — the dep-list may contain 0xFA3
  entries that were not scanned due to the bad size)

**Conclusion for M.4:** The element arrays ARE populated for CLI-compiled
Strategies with `Inputs:` declarations. The dep-list pointer is non-NULL. The
dep-list size offset (`+0x7f`) used in this verification is wrong (produces a
garbage value), so the dep-list contents could not be scanned. M.4's
dependency enumeration may actually work — the remaining question is whether
the dep-list at `element+0x7b` contains 0xFA3 entries for called Functions,
which requires the correct dep-list size offset (to be determined via Ghidra
or the existing M.4 diagnostics).

### 13.4 Root cause of the original "empty element arrays" observation

The original M.4 diagnostic (`diag_m4_objcode.cpp`) and the M.3b RED test both
used a declaration-free Function source (`X = 1;\r\n`). This source has no
`Inputs:`, `Variables:`, or `Array:` declarations, so the compiler's
`ScanSourceCode` token scanner never encounters a declaration token, never
calls `RecordInputs`/`RecordVARs`/`RecordArrays`, and the INFOREC counts
remain at 0 (as confirmed by the §12 Frida trace). This is CORRECT behavior —
a declaration-free source legitimately has 0 elements/variables/arrays.

The M.4 note §8.6 observation that "Functions (cat 4): dep-lists are empty
(size=0, ptr=NULL for all elements)" is consistent: the sampled existing
Functions may also have been declaration-free or the dep-list is genuinely
empty for Functions that don't call other Functions.

The M.4 note §8.8 "CLI-compiled items have empty element arrays due to the
separate CLI-created object-code defect (version=0, element count=0)" is
**INCORRECT as a general statement**. CLI-compiled items with declarations
have non-zero element counts. The observation was based on declaration-free
test sources. The `version=0` characterization was already corrected by §8
of this note (the version is at INFOREC `+0x9` = 147, not `+0x24`).

### 13.5 Implications for M.3b and M.4

1. **The .ell INFOREC count-population "defect" is a test artifact.** There
   is no compilation-path defect that prevents .ell INFOREC count population
   for sources with declarations. The CLI correctly populates .ell INFOREC
   counts when the source has declarations (verified for Function and
   Strategy; see §13.6 for untested types). The RED test
   (`test_m3b_object_code_version.cpp`) uses a declaration-free source and
   asserts elemCount > 0, which is an incorrect assertion for that source.
   NOTE: this is scoped to the .ell INFOREC path only; the §3.6.2
   SIGNAL_RECORD/CPrgList secondary finding is a separate storage path and is
   NOT addressed by this verification (see §13.8).

2. **The RED test should be fixed, not the compilation path.** The correct
   fix for M.3b.2c is to modify the RED test source to include declarations
   (e.g., `Inputs: x(Numeric);\nX = x + 1;\r\n`), at which point the test
   will PASS on current code (no production fix needed). This is a test fix,
   not a production code fix.

3. **M.4 unblock status is NOT determined from this verification.** Element
   arrays are populated for CLI-compiled Strategies with `Inputs:` + function
   calls, and the dep-list pointer is non-NULL. Whether the dep-list contains
   the expected 0xFA3 entries is NOT verified — the dep-list size offset used
   in Run 4 (`+0x7f`) is wrong and produced a garbage size. M.4's unblock
   status requires resolving the correct dep-list size offset and confirming
   0xFA3 entries are present.

4. **M.3b.2c should be re-scoped.** Instead of implementing a deep
   compilation-path fix, M.3b.2c should: (a) fix the RED test source to
   include declarations, (b) confirm the test passes, (c) verify the M.4
   dep-list enumeration works with the corrected test, and (d) update the
   M.4 note §8.8 to correct the "empty element arrays" characterization.

### 13.6 What was NOT verified

- **The correct dep-list size offset** (the `+0x7f` offset produced garbage).
  The M.4 note §8.3 says "dep-list at element+0x7b" but does not specify the
  size offset. The existing `diag_m4_deplist_dump.cpp` diagnostic may have
  the correct offset.
- **Whether the dep-list at `element+0x7b`** for a CLI-compiled Strategy that
  calls a Function actually contains 0xFA3 entries with the called Function's
  ProgId. Run 4's scan was corrupted by the wrong size offset.
- **Whether the dep-list size offset difference is the reason M.4's existing
  implementation doesn't find reverse dependents** (a separate investigation).
- **Other component types:** Only Function (cat 4) and Strategy (cat 0) were
  tested. Indicator (cat 2), TradingApp (cat 2 subtype 0x14), and ShowMe (cat
  2 subtype 0x05) were not tested. The §3.2 / §4 evidence shows these have
  different version/subtype paths; the count-population behavior is not
  confirmed for them.
- **The §3.6.2 secondary finding (SIGNAL_RECORD/CPrgList counts):** §3.6.2
  calls this "a real defect in a separate storage path." §13's conclusion
  that the .ell INFOREC count-population is a test artifact does not address
  whether the §3.6.2 secondary finding is also a test artifact or remains a
  real defect. See §13.8 for reconciliation.
- **No TSDev-UI baseline for the same source:** The conclusion "CLI correctly
  populates .ell INFOREC counts when the source has declarations" is based on
  CLI-only runs. There is no TSDev-UI-compiled baseline for the same
  `Inputs:` source to confirm parity. (UI operation may be prohibited per
  AGENTS.md, but the gap is stated here, not silently assumed.)
- **The M.4 note §8.6 observation** (existing Functions with empty dep-lists)
  is dismissed in §13.4 with speculation ("the sampled existing Functions may
  also have been declaration-free") — this is NOT verified. If those existing
  Functions were created WITH declarations (by TSDev UI or import) and still
  have empty dep-lists, the "test artifact" conclusion does not fully explain
  the M.4 blocker.
- **Sample size:** The "test artifact" conclusion is drawn from a 2-run
  sample (Function with `Inputs:`, Strategy with `Inputs:` + function call).
  This is thin for a conclusion that overturns M.3b.1, M.3b.2a, and the §3–§11
  root-cause analysis. Broader testing (more types, larger sources with
  multiple Inputs and nested function calls) is needed before stating the
  conclusion generally.
- **The `WriteTokens` write path (Q3):** §12 Q3 could not observe
  `WriteTokens` because `elbardb.dll` was never loaded during the Function-only
  Frida trace. The §13.5 #2 prescription ("no production fix needed") rests on
  the unverified assumption that `WriteTokens` faithfully writes the populated
  INFOREC to the .ell file. Run 4 (Strategy with function call) did exercise
  the warehouse path (elbardb loaded), but `WriteTokens` was not hooked in
  that run. See §13.9.

### 13.7 Files changed

- **No `src/` files modified.**
- **`tests/test_m3b_object_code_version.cpp`** was temporarily modified for
  verification runs (4 builds + 4 runs) and then restored to its exact
  original content (SHA-256 confirmed: `619012C26655AAAADC4533B930E84E9EE612BABF85559131B7C770F6A4DB63DC`).
- No `CMakeLists.txt`, `signatures/`, or `tools/` changes.
- This findings note section 13 was appended.
- No crash occurred in any run (TSCrashReport.exe not running pre/post).

### 13.8 Reconciliation with §3.6.2 (SIGNAL_RECORD/CPrgList path)

§3.6.2 identifies a separate storage path: the master-record/CPrgList path
with count fields at `SIGNAL_RECORD+0xc6`/`+0xca`/`+0xe4`. §3.6.2 states
these counts are 0 for newly created items because the compilation path
doesn't propagate them from the INFOREC into the SIGNAL_RECORD.

The §13 verification addressed ONLY the .ell INFOREC path (the linker reads
INFOREC data decompressed from the `.ell` object code). The §13 conclusion
("no compilation-path defect that prevents .ell INFOREC count population")
is scoped to the .ell INFOREC path and does NOT claim anything about the
SIGNAL_RECORD/CPrgList path.

The §3.6.2 finding may be:
- (a) Also a test artifact (if the SIGNAL_RECORD counts are populated for
  sources with declarations) — NOT verified by §13.
- (b) A real defect on the CPrgList path that is independent of the .ell
  INFOREC test artifact — NOT ruled out by §13.

The §3.6.2 finding is secondary (§3.6.2 itself says "the master-record path
is secondary and is not what the linker populators consume"). The linker
reads INFOREC data from the .ell, not the SIGNAL_RECORD. Therefore the
§3.6.2 finding does not block the linker's element-array population, and the
§13 conclusion (that the linker gets populated element arrays for sources
with declarations) stands regardless of the §3.6.2 status.

M.3b.2c or a follow-up investigation should verify whether the §3.6.2
SIGNAL_RECORD counts are also populated for sources with declarations (by
reading `+0xc6`/`+0xca`/`+0xe4` from the partial master record for a
declaration-rich source). If they are, §3.6.2 is also a test artifact. If
they are not, §3.6.2 remains a real defect on the secondary CPrgList path
that does not block the linker but may affect other consumers.

### 13.9 Q3 (WriteTokens) closure

The original M.3b.2b task required tracing `WriteTokens` to confirm which
INFOREC buffer it writes (Q3). §12 Q3 could not answer this because
`elbardb.dll` was never loaded during the Function-only Frida trace (the
Function-only compilation path does not exercise the warehouse write path).

The §13 verification provides indirect evidence: Run 2 (Function with
`Inputs:`) produced `elemCount=1` and `elemArrayPtr=NON-NULL` when the
linker read the .ell file. This means the INFOREC counts WERE written to the
.ell file by some write path (presumably `WriteTokens` or equivalent), and
the linker successfully read them back. This is strong indirect evidence
that the write path works correctly for sources with declarations.

However, `WriteTokens` was not directly observed. The §13.5 #2 prescription
("no production fix needed, just fix the test source") rests on this
indirect evidence. A direct `WriteTokens` trace (with a source that forces
`elbardb.dll` to load, such as the Run 4 Strategy with function call) would
provide definitive confirmation but was not performed in this TVS.

**Q3 status:** Indirectly answered (the linker read non-zero counts from the
.ell, proving the write path worked for declaration-rich sources). Not
directly observed. The `FUN_0040FB50` / `FID_conflict:_Tidy` address
ambiguity in Ghidra remains unresolved. A direct trace is recommended for
M.3b.2c or a follow-up but is not blocking for the "test artifact" conclusion.

---

## 14. M.3b.2c — Test fix + sample-size gap closure (2026-08-28)

### 14.1 Scope and re-scoping

M.3b.2c was re-scoped from "fix RED test + wire into regression" to "fix RED
test + close §13.6 sample-size gap + correct false 'defect' narrative." The
"wire into regression" criterion was removed per the 2026-08-28 PM
abstraction-level directive: the "element arrays populated" property is only
accessible via the linker (orlib20 ordinal 85, `FUN_101a3340`) — an internal
mechanism TSDev.exe never calls directly. The warehouse-level path (cmd
`0x0D`, SafeArrays, CPrgList stored record) has 0 counts for newly created
items (§3.6.2 — a separate storage path). There is no higher-level API that
reads the .ell INFOREC counts. The property is only relevant to the
linker-based M.4 approach, which M.4b will abandon in favor of warehouse-level
`CELPrograms` BST APIs. The higher-level property ("CLI items
compile/store/round-trip correctly") is already covered by the existing E2E
regression suite. The test therefore remains a manual admin diagnostic, NOT
wired into `test_regression`/`$TestDefinitions`.

### 14.2 Test changes

`tests/test_m3b_object_code_version.cpp` was fixed and parameterized for all
5 supported component types:

- **Strategy** (category=0, expandedWhType=0): `Inputs: int x(0);\r\nif x > 0 then Buy next bar at market;\r\n` — declaration-rich, asserts elemCount > 0.
- **Indicator** (category=2, expandedWhType=2): `Inputs: int x(0);\r\nPlot1(x, "MyPlot");\r\n` — declaration-rich, asserts elemCount > 0.
- **Function** (category=4, expandedWhType=4): `Inputs: x(Numeric);\r\n<FuncName> = x + 1;\r\n` — declaration-rich, asserts elemCount > 0.
- **TradingApp** (category=2, expandedWhType=5122): `using elsystem;\r\n` — no declarations, asserts elemCount == 0 (expected no-declarations pattern).
- **ShowMe** (category=2, expandedWhType=1282): `Inputs: int x(0);\r\nPlot1(Close, "MyPlot");\r\n` — declaration-rich, asserts elemCount > 0.

The test header comment and `CMakeLists.txt` comment were updated to document
the test-artifact finding and the manual-diagnostic rationale.

### 14.3 Verification run (2026-08-28, elevated process)

Command: `.\build\Release\test_m3b_object_code_version.exe` (from
`D:\tsdev_automation`, elevated process, High Mandatory Level confirmed via
`whoami /groups` S-1-16-12288).

Build: `cmake --build build --config Release --target test_m3b_object_code_version` — succeeded, exit code 0.

Run result: exit code 0 (PASS), no TSCrashReport.exe pre/post.

| Type | ProgId | elemCount | elemArrayPtr | .ell cleanup | Result |
|------|--------|-----------|--------------|--------------|--------|
| Strategy | 8909 | 1 | NON-NULL (36E85490) | gone | PASS |
| Indicator | 2887 | 1 | NON-NULL (36E84890) | gone | PASS |
| Function | 1505 | 1 | NON-NULL (36E85310) | gone | PASS |
| TradingApp | 2888 | 0 | NULL | gone | PASS |
| ShowMe | 2889 | 1 | NON-NULL (36E84F50) | gone | PASS |

All 5 disposable items were deleted; all 5 `.ell` files confirmed gone via
`FindFirstFileA`. No crash reporter process was detected pre or post.

### 14.4 New finding: linker takes expanded whType, not category

The M.3b.2c run discovered that the linker (ordinal 85) and Ordinal_28 take
the **expanded whType** (the enum value: 0, 2, 4, 5122, 1282), NOT the
category (low 8 bits). The first run used `category` (2) for TradingApp and
ShowMe, and the linker returned NULL for both. The second run used
`expandedWhType` (5122, 1282), and the linker returned non-NULL records for
both.

This is a finding for M.4: the production `enumerateReverseDependents()` in
`src/tsdev_lib.cpp` (line ~2010) iterates categories 0-7 and calls the linker
with the category, which fails for composite types (TradingApp/ShowMe). This
is a real limitation of the linker-based approach, but it will be bypassed by
M.4b's warehouse-level approach. This finding does NOT change the M.3b.2c
conclusion (the test-artifact finding stands); it is recorded here for M.4
handoff.

### 14.5 §13.6 sample-size gap — type coverage CLOSED; larger-sources sub-gap not addressed

The §13.6 "sample size" gap ("the 'test artifact' conclusion is drawn from a
2-run sample") is now CLOSED for type coverage. The M.3b.2c run verified all
5 supported component types:

- 4 declaration-rich types (Strategy, Indicator, Function, ShowMe): elemCount > 0, elemArrayPtr != NULL.
- 1 no-declarations type (TradingApp): elemCount == 0, elemArrayPtr == NULL (expected).

The "test artifact" conclusion now generalizes across all 5 supported types.
The §13.6 "larger sources with multiple Inputs and nested function calls"
sub-gap is not addressed by M.3b.2c (all declaration-rich sources use a
single `Inputs:` declaration, yielding elemCount=1). This is not expected to
differ: a single `Inputs:` triggers the `Record*` populators, and more
declarations would yield elemCount=N>1, not a different populated-vs-empty
behavior. This sub-gap is deferred to M.4 (see §14.6).

### 14.6 §13.6 other gaps — status unchanged

The other §13.6 gaps remain unresolved and are M.4 scope:

- The correct dep-list size offset (`+0x7f` produced garbage).
- Whether dep-lists contain `0xFA3` entries for function dependencies.
- The §3.6.2 SIGNAL_RECORD/CPrgList counts (separate storage path).
- No TSDev-UI baseline for the same source.
- The §8.6 observation (existing Functions with empty dep-lists).
- The `WriteTokens` write path (Q3) — indirectly answered in §13.9, not directly observed.
- Larger sources with multiple Inputs and nested function calls (§13.6 sub-gap; not expected to differ — see §14.5).

### 14.7 Files changed

- `tests/test_m3b_object_code_version.cpp` — fixed + parameterized for all 5 types; header comment updated; linker/Ordinal_28 calls use expanded whType.
- `CMakeLists.txt` — comment block for `test_m3b_object_code_version` updated (no longer "expected to fail"; manual diagnostic rationale documented).
- `docs/workstream_tsdev_parity_plan.md` — M.3b.2c scope updated (removed "wire into regression"; added highest-level API assessment).
- `docs/RE/RE_M4_REVERSE_DEPENDENT_REVALIDATION.md` — §8.8 corrected.
- `docs/RE/RE_M3B_OBJECT_CODE_VERSION_DEFECT.md` — §14 appended (this section).
- No `src/` files modified. No `signatures/` or `tools/` changes. No crash occurred.
