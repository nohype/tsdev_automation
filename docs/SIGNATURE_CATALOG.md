# Signature Catalog — orlib20.dll Code-Section RVA Targets

| Field | Value |
|---|---|
| **TVS Identifier** | TVS-B.1 |
| **Date** | 2026-08-17 |
| **Status** | APPROVED-pending-review |
| **Purpose** | Primary deliverable of TVS-B.1. Catalogs the 12 code-section RVA targets in `orlib20.dll`, providing for each: a candidate byte signature (hex with `??` wildcard notation), signature length, uniqueness-verification result (scan count = 1 within `.text`), expected original bytes at the patch site (for patches), calling convention, and structural assumptions. Also documents the DLL baseline metadata (version, PE timestamp, SHA-256), the 7 export names, 3 data-section RVA targets, and structural constants. This catalog is the baseline for signature-based RVA resolution and version detection in later TVSs. |

> **WORKSTREAM E NOTICE (2026-08-17):** The signature data source of truth is being migrated from `tmp/re_artifacts/b1_signatures.h` (gitignored, not regenerable) to version-controlled `signatures/v9.50.01.3344.json` with build-time C++ code generation. See `docs/ARCHITECTURE_REDESIGN.md` for the complete design and TVS-E.1 through TVS-E.7 migration plan. The signature patterns and metadata in this document remain valid; only the storage format and build integration change. This migration is **DESIGNED but NOT YET IMPLEMENTED** — the current state (`b1_signatures.h` as the build dependency) remains the actual state until Workstream E's TVSs are executed.

**Source artifacts (all in `tmp/re_artifacts/`):**

| Artifact | Content |
|---|---|
| `b1_dll_baseline.md` | DLL baseline metadata (version, SHA-256, PE timestamp, .text section info) |
| `b1_rva_inventory.md` | Full inventory of 46 entries (12 RVA targets + 3 data offsets + 7 export names + structural constants) |
| `b1_ghidra_evidence.md` | Ghidra disassembly, raw bytes, calling conventions for all 12 targets |
| `b1_signatures.h` | The 12 finalized signatures (hex patterns with `??` wildcards, patch metadata) |
| `b1_export_confirmation.md` | Export table confirmation for the 7 export names |
| `b1_sig_scan_run.md` | Uniqueness scan results (12/12 PASS, exit 0) |

---

## 1. DLL Baseline

*Source: `tmp/re_artifacts/b1_dll_baseline.md`*

All measurements taken from the copied baseline DLL, not the read-only install original.

| Field | Value |
|---|---|
| **Source (read-only install)** | `C:\Program Files (x86)\TradeStation 9.5\Program\orlib20.dll` |
| **Copied artifact** | `tests/fixtures/orlib20_baseline.dll` (committed to repo) |
| **FileVersion** | 9.50.01.3344 |
| **ProductVersion** | 9.50.01.3344 |
| **FileDescription** | Omega EasyLanguage |
| **CompanyName** | TradeStation Technologies, Inc. |
| **File size** | 4,924,400 bytes |
| **SHA-256** | `318DB24A4F41BDB9FF69E2E647026B788706904CF887CBB0A953E003DB22EC18` |
| **PE timestamp (TimeDateStamp)** | `0x5C01735E` = Fri Nov 30 18:29:02 2018 UTC |
| **Machine** | 0x014C (x86) |
| **Characteristics** | 0x2102 (Executable, 32-bit word machine, DLL) |
| **Magic** | 0x010B (PE32) |
| **Linker version** | 14.00 |
| **Entry point (RVA)** | 0x1EB235 (VA 0x101EB235) |
| **Image base** | `0x10000000` (range 0x10000000–0x104BAFFF) |
| **.text RVA (virtual address)** | `0x00001000` |
| **.text virtual size** | `0x001F72FC` (2,061,052 bytes) |
| **.text raw size (size of raw data)** | `0x001F7400` (2,061,312 bytes) |
| **.text file pointer to raw data** | `0x00000400` |

**Summary:** Version 9.50.01.3344 (Omega EasyLanguage, TradeStation Technologies, Inc.), 4,924,400 bytes, SHA-256 `318DB24A...`, PE timestamp `0x5C01735E` (Fri Nov 30 18:29:02 2018 UTC), image base `0x10000000`, .text RVA `0x00001000`, virtual size `0x001F72FC`, raw size `0x001F7400`.

---

## 2. Wildcard Notation Convention

*Source: `tmp/re_artifacts/b1_ghidra_evidence.md` (§Wildcard Notation Convention), `tmp/re_artifacts/b1_signatures.h` (header comment)*

In all signatures in this catalog, `??` represents a **wildcard byte** (matches any single byte at that position). All other bytes are **fixed** and must match exactly.

### What is wildcarded (`??`)

| Category | Bytes | Rationale |
|---|---|---|
| **SEH handler addresses** | 4 bytes at prologue offset 6–9 (or 13–16 for GetWarehouse) | Absolute, image-base-relative addresses that change if the DLL is relocated or rebuilt |
| **Data-section references** | 4 bytes in `MOV reg, [imm32]` / `MOV [imm32], reg` | Absolute, image-base-relative pointers into `.data` (e.g., `DAT_1023cea0`) |
| **IAT indirect call targets** | 4 bytes in `CALL [imm32]` (`FF 15 imm32`) | IAT slot addresses are image-base-relative |
| **Relative call/jump targets** | 4 bytes in `CALL rel32` (`E8 imm32`) and `JZ rel32` (`0F 8x imm32`) | Relative displacements change if code shifts |

### What is fixed (NOT wildcarded)

| Category | Examples |
|---|---|
| **Opcode bytes** | `55` (PUSH EBP), `8B EC` (MOV EBP,ESP), `E8` (CALL rel32), `FF 15` (CALL [imm32]), etc. |
| **ModR/M bytes** | Register encodings, addressing mode bytes |
| **Register encodings** | e.g., `8B F9` (MOV EDI,ECX), `8B F2` (MOV ESI,EDX) |
| **Structural immediates** | Stack frame sizes (`81 EC 68 01 00 00` = SUB ESP,0x168), error record stride (`69` = IMUL prefix with 0x129), error record size (`68 B1 00 00 00` = PUSH 0xB1), allocation sizes (`6A 2C` = PUSH 0x2C) |
| **Short jump displacements** | `74 15` (JZ +0x15), `74 01` (JZ +1), `75 19` (JNZ +0x19), `74 62` (JZ +0x62) — 1-byte displacements kept fixed |
| **FS:[0] encoding zeros** | The `00 00 00 00` in `64 A1 00 00 00 00` (MOV EAX,FS:[0]) and `64 89 25 00 00 00 00` (MOV FS:[0],ESP) — always zero in this encoding |

---

## 3. Signature Catalog Table

*Source: `tmp/re_artifacts/b1_signatures.h` (exact hex patterns), `tmp/re_artifacts/b1_sig_scan_run.md` (scan results)*

One row per RVA target (12 rows). For patch sites (targets 9–12), additional patch metadata columns are populated.

> **Note on targets 10–12:** The signature matches the **containing function** entry point, not the patch byte itself. The "RVA" column shows the **patch-site RVA** (the production target). The "Sig Match RVA" column shows where the signature actually matches (the containing function entry). The patch byte is at `Sig Match RVA + Patch Offset`.

| # | Name | RVA (target) | Calling Convention | Length | Unique? | Patch Site? |
|---|---|---|---|---|---|---|
| 1 | StartCompilingProcess | 0x165DE0 | `__cdecl`, no args | 64 | Yes (1/1) | No |
| 2 | EndCompilingProcess | 0x165ED0 | `__fastcall`, 1 arg (int param) | 55 | Yes (1/1) | No |
| 3 | CELCompiler_ctor | 0x165A00 | `__fastcall` (ecx=this) | 64 | Yes (1/1) | No |
| 4 | CELCompiler_dtor | 0x165AC0 | `__fastcall` (ecx=this) | 38 | Yes (1/1) | No |
| 5 | InitGlobalLookup | 0x1A9460 | `__cdecl`, no args | 66 | Yes (1/1) | No |
| 6 | DeleteFromTree | 0x1E4030 | `__thiscall` (ecx=globalLookup; stack: whType, progId, flag); RET 12 | 66 | Yes (1/1) | No |
| 7 | GetWarehouse | 0x1A9650 | `__cdecl`, 1 arg (path; 0=default) | 45 | Yes (1/1) | No |
| 8 | FUN_101a8360_delete_orchestrator | 0x1A8360 | `__fastcall` (ECX=whType, EDX=progId, no stack args) | 64 | Yes (1/1) | No |
| 9 | FUN_10011230_persistence_patch | 0x11230 | `__fastcall` (ECX=arg1, EDX=arg2), 12 bytes stack args (RET 0x0C) | 72 | Yes (1/1) | Yes |
| 10 | FUN_101d0950_thread_check_INT3 | 0x1D0993 | `__thiscall` (ecx=this) | 36 | Yes (1/1) | Yes |
| 11 | FUN_101cf5b0_thread_check_INT3 | 0x1CF5D4 | `__thiscall` (ecx=this) | 37 | Yes (1/1) | Yes |
| 12 | FUN_101d1fa0_thread_check_INT3 | 0x1D1FC8 | `__thiscall` (ecx=this) | 57 | Yes (1/1) | Yes |

### Patch-site metadata

| # | Name | Patch-Site RVA | Sig Match RVA (containing fn) | Patch Offset | Original Bytes | Replacement Bytes |
|---|---|---|---|---|---|---|
| 9 | FUN_10011230_persistence_patch | 0x11230 | 0x11230 | 0x00 (0) | `55 8B EC 6A FF 68 CD D3` | `B8 01 00 00 00 C2 0C 00` |
| 10 | FUN_101d0950_thread_check_INT3 | 0x1D0993 | 0x1D0950 | 0x43 (67) | `CC` | `90` |
| 11 | FUN_101cf5b0_thread_check_INT3 | 0x1CF5D4 | 0x1CF5B0 | 0x24 (36) | `CC` | `90` |
| 12 | FUN_101d1fa0_thread_check_INT3 | 0x1D1FC8 | 0x1D1FA0 | 0x28 (40) | `CC` | `90` |

---

## 4. Per-Target Detail Sections

*Source: `tmp/re_artifacts/b1_ghidra_evidence.md` (disassembly, raw bytes, calling conventions), `tmp/re_artifacts/b1_signatures.h` (exact signatures), `tmp/re_artifacts/b1_sig_scan_run.md` (scan results)*

**Export status note (applies to all 12 targets):** None of the 12 RVA targets are exported functions. They are **internal functions** resolved by RVA (`m_orlibBase + RVA`), not by `GetProcAddress`. The 7 exported functions used in production code are documented separately in §5.

---

### 4.1 Target 1: StartCompilingProcess (RVA 0x165DE0)

| Field | Value |
|---|---|
| **RVA** | 0x165DE0 |
| **VA** | 0x10165DE0 |
| **Name** | StartCompilingProcess |
| **Calling convention** | `__cdecl`, no args |
| **Signature length** | 64 bytes |
| **Patch site?** | No |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature:**
```
55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 51 8B 0D ?? ?? ?? ?? 33 C0 A3 ?? ?? ?? ?? 85 C9 74 15 51 FF 15 ?? ?? ?? ?? A3 ?? ?? ?? ?? 85 C0 75 19 A1 ?? ?? ?? ?? 69
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x00165DE0 (expected RVA). PASS. *(Source: `b1_sig_scan_run.md`, final run, target [1])*

**Structural assumptions:**
- Assumes the standard 24-byte SEH frame prologue (`55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00`), with the SEH handler address (bytes 6–9) wildcarded.
- Assumes `DAT_1023cea0` is the global error-array pointer (accessed via `MOV ECX, [0x1023cea0]` at instruction RVA 0x165DF9 — the 4-byte absolute address is wildcarded).
- Assumes `DAT_1023cea4` is the global error-record count (accessed via `MOV [0x1023cea4], EAX` — wildcarded).
- Assumes error record stride 0x129 (297 bytes) — embedded in the signature as the `69` opcode prefix of `IMUL EAX, EAX, 0x129` (the 0x129 immediate is part of the next instruction, beyond the signature boundary, but the `69` opcode is fixed).
- Distinctive feature: `51 8B 0D` (PUSH ECX; MOV ECX, [imm32]) followed by error array reset and IMUL with 0x129 stride.

---

### 4.2 Target 2: EndCompilingProcess (RVA 0x165ED0)

| Field | Value |
|---|---|
| **RVA** | 0x165ED0 |
| **VA** | 0x10165ED0 |
| **Name** | EndCompilingProcess |
| **Calling convention** | `__fastcall`, 1 arg (int param) |
| **Signature length** | 55 bytes |
| **Patch site?** | No |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature:**
```
55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 51 56 8B 35 ?? ?? ?? ?? 85 F6 74 62 C7 45 FC 00 00 00 00 8B CE 8B 06 FF 70 04 E8 ?? ?? ?? ??
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x00165ED0 (expected RVA). PASS. *(Source: `b1_sig_scan_run.md`, final run, target [2])*

**Structural assumptions:**
- Assumes the standard 24-byte SEH frame prologue (SEH handler at bytes 6–9 wildcarded).
- Assumes `DAT_1023ceb0` is a global pointer to some list/object (accessed via `MOV ESI, [0x1023ceb0]` — wildcarded).
- Assumes the SEH handler address (0x101EC887) is the same as Target 8 (FUN_101a8360) — both functions share the same handler but diverge immediately after the prologue, so the post-prologue bytes distinguish them.
- Distinctive feature: `51 56 8B 35` (PUSH ECX; PUSH ESI; MOV ESI, [imm32]) — differs from Target 1's `51 8B 0D`.

---

### 4.3 Target 3: CELCompiler constructor (RVA 0x165A00)

| Field | Value |
|---|---|
| **RVA** | 0x165A00 |
| **VA** | 0x10165A00 |
| **Name** | CELCompiler_ctor |
| **Calling convention** | `__fastcall` (ecx=this) |
| **Signature length** | 64 bytes |
| **Patch site?** | No |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature:**
```
55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 83 EC 08 56 57 8B F9 89 7D F0 8D 47 1C C7 47 04 00 00 00 00 C7 47 08 00 00 00 00 C7 47 0C 00 00 00 00 C7 47 10 00 00 00
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x00165A00 (expected RVA). PASS. *(Source: `b1_sig_scan_run.md`, final run, target [3])*

**Structural assumptions:**
- Assumes the standard 24-byte SEH frame prologue (SEH handler at bytes 6–9 wildcarded).
- Assumes the constructor initializes 6 DWORD fields at `[EDI+4]` through `[EDI+0x18]` to zero (the `C7 47 04/08/0C/10 00 00 00 00` sequence).
- Assumes `LEA EAX, [EDI+0x1C]` (`8D 47 1C`) precedes the field zeroing — part of the constructor's object layout.
- Assumes SSO (small string optimization) capacity = 7 is set at `[EAX+0x14]` (beyond the signature boundary, but the zeroing pattern is distinctive).
- Distinctive feature: `83 EC 08 56 57 8B F9` (SUB ESP,8; PUSH ESI; PUSH EDI; MOV EDI,ECX) — constructor prologue capturing `this` pointer.

---

### 4.4 Target 4: CELCompiler destructor (RVA 0x165AC0)

| Field | Value |
|---|---|
| **RVA** | 0x165AC0 |
| **VA** | 0x10165AC0 |
| **Name** | CELCompiler_dtor |
| **Calling convention** | `__fastcall` (ecx=this) |
| **Signature length** | 38 bytes |
| **Patch site?** | No |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature:**
```
55 8B EC 51 56 57 8B F9 8B 47 4C 8D 4F 4C 50 FF 30 8D 45 FC 50 E8 ?? ?? ?? ?? 6A 20 6A 01 FF 77 4C E8 ?? ?? ?? ??
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x00165AC0 (expected RVA). PASS. *(Source: `b1_sig_scan_run.md`, final run, target [4])*

**Structural assumptions:**
- Assumes **NO SEH frame** — this function uses a minimal prologue (`55 8B EC 51 56 57 8B F9`), distinct from the 24-byte SEH prologue used by most other functions.
- Assumes access to `[EDI+0x4C]` (destructor cleanup of some field — `MOV EAX, [EDI+0x4C]` and `LEA ECX, [EDI+0x4C]`).
- Assumes two relative calls (`E8 ?? ?? ?? ??`) to cleanup helper functions — wildcarded.
- Distinctive feature: `55 8B EC 51 56 57 8B F9 8B 47 4C 8D 4F 4C` — very distinctive (no SEH frame; PUSH ECX; PUSH ESI; PUSH EDI; MOV EDI,ECX; MOV EAX,[EDI+0x4C]; LEA ECX,[EDI+0x4C]).

---

### 4.5 Target 5: InitGlobalLookup (RVA 0x1A9460)

| Field | Value |
|---|---|
| **RVA** | 0x1A9460 |
| **VA** | 0x101A9460 |
| **Name** | InitGlobalLookup |
| **Calling convention** | `__cdecl`, no args |
| **Signature length** | 66 bytes |
| **Patch site?** | No |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature:**
```
55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 51 A1 ?? ?? ?? ?? 85 C0 75 3A 6A 2C FF 15 ?? ?? ?? ?? 83 C4 04 89 45 F0 C7 45 FC 00 00 00 00 85 C0 74 1A 8B C8 E8 ?? ?? ?? ??
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x001A9460 (expected RVA). PASS. *(Source: `b1_sig_scan_run.md`, final run, target [5])*

**Structural assumptions:**
- Assumes the standard 24-byte SEH frame prologue (SEH handler at bytes 6–9 wildcarded).
- Assumes `DAT_1023ced4` is a global singleton pointer for the lookup object (accessed via `MOV EAX, [0x1023ced4]` — wildcarded).
- Assumes allocation of 0x2C (44) bytes for the lookup object (`6A 2C` = PUSH 0x2C before IAT call).
- Assumes the IAT indirect call (`FF 15 ?? ?? ?? ??`) is to an allocator (e.g., `malloc`/`operator new`).
- Distinctive feature: `51 A1` (PUSH ECX; MOV EAX, [imm32]) — accesses DAT_1023ced4, allocates 0x2C bytes.

---

### 4.6 Target 6: DeleteFromTree (RVA 0x1E4030)

| Field | Value |
|---|---|
| **RVA** | 0x1E4030 |
| **VA** | 0x101E4030 |
| **Name** | DeleteFromTree |
| **Calling convention** | `__thiscall` (ecx=globalLookup; stack: whType, progId, flag); RET 12 |
| **Signature length** | 66 bytes |
| **Patch site?** | No |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature:**
```
55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 83 EC 28 83 7D 10 00 53 56 8B 75 0C 8B D9 57 89 5D F0 74 0C 56 FF 75 08 E8 ?? ?? ?? ?? 83 C4 08 8D 43 08 50 FF 15 ?? ?? ?? ??
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x001E4030 (expected RVA). PASS. *(Source: `b1_sig_scan_run.md`, final run, target [6])*

**Structural assumptions:**
- Assumes the standard 24-byte SEH frame prologue (SEH handler at bytes 6–9 wildcarded).
- Assumes `__thiscall` convention: ECX = `globalLookup` object pointer; 3 stack args at `[ebp+0x8]` (whType), `[ebp+0xc]` (progId), `[ebp+0x10]` (flag).
- Assumes the flag at `[ebp+0x10]` controls a conditional path (`CMP [EBP+0x10], 0` → `JZ`).
- Assumes a relative call (`E8 ?? ?? ?? ??`) to a helper function when flag != 0, and an IAT indirect call (`FF 15 ?? ?? ?? ??`) for tree manipulation.
- Distinctive feature: `83 EC 28 83 7D 10 00 53 56 8B 75 0C 8B D9 57` — SUB ESP,0x28; CMP [EBP+0x10],0; PUSH EBX; PUSH ESI; MOV ESI,[EBP+0xc]; MOV EBX,ECX; PUSH EDI.

---

### 4.7 Target 7: GetWarehouse (RVA 0x1A9650)

| Field | Value |
|---|---|
| **RVA** | 0x1A9650 |
| **VA** | 0x101A9650 |
| **Name** | GetWarehouse |
| **Calling convention** | `__cdecl`, 1 arg (path; 0=default) |
| **Signature length** | 45 bytes |
| **Patch site?** | No |
| **Exported?** | No (internal function, resolved by RVA). Note: resolved and stored (`m_pGetWarehouse`) but NOT actually called in the production flow — the delete orchestrator (FUN_101a8360) calls it internally. Retained as a resolved-but-unused CALL_TARGET. |

**Signature:**
```
55 8B EC 64 A1 00 00 00 00 6A FF 68 ?? ?? ?? ?? 50 64 89 25 00 00 00 00 81 EC 94 01 00 00 A1 ?? ?? ?? ?? 53 56 57 A8 01 75 66 83 C8 01
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x001A9650 (expected RVA). PASS. *(Source: `b1_sig_scan_run.md`, final run, target [7])*

**Structural assumptions:**
- Assumes a **DIFFERENT SEH prologue order**: `MOV EAX, FS:[0]` (`64 A1 00 00 00 00`) occurs **BEFORE** `PUSH -1` / `PUSH handler` (`6A FF 68 ?? ?? ?? ??`). This is unique among the 12 targets. The SEH handler address is at bytes 13–16 (wildcarded), not 6–9.
- Assumes `DAT_102408f8` is a one-time-init guard flag (accessed via `MOV EAX, [0x102408f8]` — wildcarded; `TEST AL, 0x1` checks if already initialized).
- Assumes a 0x194 (404) byte stack frame (`81 EC 94 01 00 00` = SUB ESP, 0x194).
- Distinctive feature: `55 8B EC 64 A1` (MOV EAX,FS:[0] before PUSH -1) + `81 EC 94 01 00 00` (large frame) + `A8 01` (TEST AL,1 — one-time-init guard).

---

### 4.8 Target 8: FUN_101a8360 delete orchestrator (RVA 0x1A8360)

| Field | Value |
|---|---|
| **RVA** | 0x1A8360 |
| **VA** | 0x101A8360 |
| **Name** | FUN_101a8360_delete_orchestrator |
| **Calling convention** | `__fastcall` (ECX=whType, EDX=progId, no stack args); invoked via inline asm |
| **Signature length** | 64 bytes |
| **Patch site?** | No |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature:**
```
55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 81 EC DC 02 00 00 53 56 8B F2 8B D9 57 8D 4D CC 89 75 EC E8 ?? ?? ?? ?? 6A 00 E8 ?? ?? ?? ?? 83 C4 04 8B F8 8D 45 90 89
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x001A8360 (expected RVA). PASS. *(Source: `b1_sig_scan_run.md`, final run, target [8])*

**Structural assumptions:**
- Assumes the standard 24-byte SEH frame prologue (SEH handler at bytes 6–9 wildcarded).
- Assumes `__fastcall` convention: ECX = whType, EDX = progId (captured via `MOV ESI,EDX` / `MOV EBX,ECX`).
- Assumes a 0x2DC (732) byte stack frame (`81 EC DC 02 00 00` = SUB ESP, 0x2DC) — very large frame.
- Assumes the SEH handler address (0x101EC887) is the same as Target 2 (EndCompilingProcess) — both functions share the same handler but diverge immediately after the prologue.
- Assumes a relative call to GetWarehouse (`E8 ?? ?? ?? ??` after `6A 00` = PUSH 0) — the call target is wildcarded.
- Distinctive feature: `81 EC DC 02 00 00` (732-byte frame) + `8B F2 8B D9` (MOV ESI,EDX; MOV EBX,ECX — fastcall arg capture).

---

### 4.9 Target 9: FUN_10011230 persistence patch (RVA 0x11230)

| Field | Value |
|---|---|
| **RVA (patch site)** | 0x11230 |
| **VA (patch site)** | 0x10011230 |
| **Name** | FUN_10011230_persistence_patch |
| **Calling convention** | `__fastcall` (ECX=arg1, EDX=arg2), 12 bytes stack args (RET 0x0C in patch) |
| **Signature length** | 72 bytes |
| **Patch site?** | Yes |
| **Sig match RVA** | 0x11230 (patch at function entry, offset 0) |
| **Patch offset** | 0x00 (0) from function entry |
| **Original bytes at patch site** | `55 8B EC 6A FF 68 CD D3` (8 bytes) |
| **Replacement bytes** | `B8 01 00 00 00 C2 0C 00` (8 bytes = `MOV EAX,1; RET 0x0C`) |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature (matches ORIGINAL unpatched bytes):**
```
55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 81 EC 68 01 00 00 56 57 8D 45 EC 89 4D E8 8B FA C7 45 EC 00 00 00 00 50 57 8D 8D 8C FE FF FF E8 ?? ?? ?? ?? C7 45 FC 00 00 00 00 68 B1 00 00 00
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x00011230 (expected RVA). PASS. Patch byte check: OK (byte at patch offset = 0x55, matches original first byte). *(Source: `b1_sig_scan_run.md`, final run, target [9])*

**Patch details:**
- The patch overwrites the first 8 bytes of the function with `MOV EAX, 1; RET 0x0C` (`B8 01 00 00 00 C2 0C 00`), making `SymWriter::Save` return success so the `.ell` file is written. This bypasses a .NET `XmlDocument.Save` crash in `ELDebugger.dll`.
- The signature matches the **original unpatched** bytes. `patchOffset = 0` because the patch overwrites the function entry itself.
- Original first 8 bytes: `55 8B EC 6A FF 68 CD D3` (PUSH EBP; MOV EBP,ESP; PUSH -1; PUSH 0x101ED3CD).
- The production code does NOT store/verify original bytes before patching — it overwrites unconditionally after `VirtualProtect`. This is a known fragility (no rollback if the 8-byte window overlaps live code differently in another DLL build).

**Structural assumptions:**
- Assumes the standard 24-byte SEH frame prologue (SEH handler at bytes 6–9 wildcarded; actual handler = 0x101ED3CD).
- Assumes `__fastcall` convention (ECX=arg1, EDX=arg2) with 12 bytes of stack args (the patch's `RET 0x0C` cleans 12 bytes).
- Assumes a 0x168 (360) byte stack frame (`81 EC 68 01 00 00`).
- Assumes error record size 0xB1 = 177 (`68 B1 00 00 00` = PUSH 0xB1) — embedded in the signature as a fixed structural immediate.
- Distinctive feature: `81 EC 68 01 00 00` (SUB ESP,0x168) + `8B FA` (MOV EDI,EDX — fastcall 2nd arg) + `68 B1 00 00 00` (PUSH 0xB1 = 177 — error record size, very distinctive).

---

### 4.10 Target 10: FUN_101d0950 thread-check INT3 patch (RVA 0x1D0993)

| Field | Value |
|---|---|
| **RVA (patch site)** | 0x1D0993 |
| **VA (patch site)** | 0x101D0993 |
| **Containing function** | FUN_101d0950 (RVA 0x1D0950, VA 0x101D0950) |
| **Name** | FUN_101d0950_thread_check_INT3 |
| **Calling convention** | `__thiscall` (ecx=this) |
| **Signature length** | 36 bytes |
| **Patch site?** | Yes |
| **Sig match RVA** | 0x1D0950 (containing function entry) |
| **Patch offset** | 0x43 (67) from function entry |
| **Original byte at patch site** | `CC` (INT3) |
| **Replacement byte** | `90` (NOP) |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature (for the containing function FUN_101d0950):**
```
55 8B EC 56 8B F1 83 BE AE 00 00 00 00 0F 84 ?? ?? ?? ?? FF 75 08 B8 00 20 00 00 66 89 46 1E E8 ?? ?? ?? ??
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x001D0950 (containing function entry). PASS. Patch byte check: OK (byte at offset 0x43 = 0xCC, matches original). *(Source: `b1_sig_scan_run.md`, final run, target [10])*

**Patch details:**
- Single-byte patch: `0xCC` (INT3) → `0x90` (NOP) at function entry + 0x43.
- This NOP's the thread-check INT3 in the command 0x0F (load program) dispatch helper, allowing it to be called from any thread.
- Context bytes at patch site: `3B 46 76 74 01 CC FF 76 6E C7 46 04 0F 00 00 00` (CMP EAX,[ESI+0x76]; JZ +1; INT3; PUSH [ESI+0x6e]; MOV [ESI+4], 0x0F).
- The production code verifies the byte is either `0xCC` or `0x90` before patching; skips idempotently if already `0x90`.

**Structural assumptions:**
- Assumes `__thiscall` convention (ecx=this) — `8B F1` (MOV ESI,ECX) captures the this pointer.
- Assumes **NO SEH frame** — minimal prologue (`55 8B EC 56 8B F1`).
- Assumes `[ESI+0xAE]` field check (`83 BE AE 00 00 00 00` = CMP [ESI+0xAE], 0 — 7-byte encoding with 32-bit displacement) controls the command dispatch path.
- Assumes `MOV EAX, 0x2000` (`B8 00 20 00 00`) sets a window style or flag.
- Assumes the INT3 at offset 0x43 is the thread-check breakpoint (command 0x0F — load program).

---

### 4.11 Target 11: FUN_101cf5b0 thread-check INT3 patch (RVA 0x1CF5D4)

| Field | Value |
|---|---|
| **RVA (patch site)** | 0x1CF5D4 |
| **VA (patch site)** | 0x101CF5D4 |
| **Containing function** | FUN_101cf5b0 (RVA 0x1CF5B0, VA 0x101CF5B0) |
| **Name** | FUN_101cf5b0_thread_check_INT3 |
| **Calling convention** | `__thiscall` (ecx=this) |
| **Signature length** | 37 bytes |
| **Patch site?** | Yes |
| **Sig match RVA** | 0x1CF5B0 (containing function entry) |
| **Patch offset** | 0x24 (36) from function entry |
| **Original byte at patch site** | `CC` (INT3) |
| **Replacement byte** | `90` (NOP) |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature (for the containing function FUN_101cf5b0):**
```
55 8B EC 56 8B F1 6A FF FF 76 72 FF 15 ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 3B 46 76 8B 45 08 74 06 83 F8 1E 74 01 CC
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x001CF5B0 (containing function entry). PASS. Patch byte check: OK (byte at offset 0x24 = 0xCC, matches original). *(Source: `b1_sig_scan_run.md`, final run, target [11])*

**Patch details:**
- Single-byte patch: `0xCC` (INT3) → `0x90` (NOP) at function entry + 0x24.
- This NOP's the thread-check INT3 in the general dispatch helper, allowing it to be called from any thread.
- Context bytes at patch site: `83 F8 1E 74 01 CC FF 76 6E 89 46 04 FF 15 ?? ?? ?? ??` (CMP EAX,0x1E; JZ +1; INT3; PUSH [ESI+0x6e]; MOV [ESI+4], EAX; CALL [IAT]).
- The production code verifies the byte is either `0xCC` or `0x90` before patching; skips idempotently if already `0x90`.

**Structural assumptions:**
- Assumes `__thiscall` convention (ecx=this) — `8B F1` (MOV ESI,ECX) captures the this pointer.
- Assumes **NO SEH frame** — minimal prologue (`55 8B EC 56 8B F1`).
- Assumes `PUSH [ESI+0x72]` (`FF 76 72`) accesses a field before two IAT indirect calls.
- Assumes `CMP EAX, 0x1E` (`83 F8 1E`) checks for command 0x1E before the thread-check INT3.
- Assumes the INT3 at offset 0x24 is the thread-check breakpoint (general dispatch helper).

---

### 4.12 Target 12: FUN_101d1fa0 thread-check INT3 patch (RVA 0x1D1FC8)

| Field | Value |
|---|---|
| **RVA (patch site)** | 0x1D1FC8 |
| **VA (patch site)** | 0x101D1FC8 |
| **Containing function** | FUN_101d1fa0 (RVA 0x1D1FA0, VA 0x101D1FA0) |
| **Name** | FUN_101d1fa0_thread_check_INT3 |
| **Calling convention** | `__thiscall` (ecx=this) |
| **Signature length** | 57 bytes |
| **Patch site?** | Yes |
| **Sig match RVA** | 0x1D1FA0 (containing function entry) |
| **Patch offset** | 0x28 (40) from function entry |
| **Original byte at patch site** | `CC` (INT3) |
| **Replacement byte** | `90` (NOP) |
| **Exported?** | No (internal function, resolved by RVA) |

**Signature (for the containing function FUN_101d1fa0):**
```
55 8B EC 8B 45 08 56 8B F1 6A FF FF 76 72 89 46 12 8B 45 0C 89 46 16 FF 15 ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 3B 46 76 74 01 CC FF 76 6E C7 46 04 17 00 00 00 FF 15 ?? ?? ?? ??
```

**Uniqueness verification:** Scan count = 1, matched at RVA 0x001D1FA0 (containing function entry). PASS. Patch byte check: OK (byte at offset 0x28 = 0xCC, matches original). *(Source: `b1_sig_scan_run.md`, final run, target [12])*

**Note on signature evolution:** This signature was initially 41 bytes and matched twice in `.text` (a spurious match at RVA 0x001CFCE0 in addition to the expected 0x001D1FA0). The signature was extended to 57 bytes by appending the post-INT3 context (`FF 76 6E C7 46 04 17 00 00 00 FF 15 ?? ?? ?? ??`), which disambiguated the duplicate. The final scan confirms uniqueness (count = 1). *(Source: `b1_sig_scan_run.md`, §Comparison with previous run)*

**Patch details:**
- Single-byte patch: `0xCC` (INT3) → `0x90` (NOP) at function entry + 0x28.
- This NOP's the thread-check INT3 in the command 0x17 (save/commit) dispatch helper, allowing it to be called from any thread.
- Context bytes at patch site: `3B 46 76 74 01 CC FF 76 6E C7 46 04 17 00 00 00 FF 15 ?? ?? ?? ??` (CMP EAX,[ESI+0x76]; JZ +1; INT3; PUSH [ESI+0x6e]; MOV [ESI+4], 0x17; CALL [IAT]).
- The production code verifies the byte is either `0xCC` or `0x90` before patching; skips idempotently if already `0x90`.

**Structural assumptions:**
- Assumes `__thiscall` convention (ecx=this) — `8B F1` (MOV ESI,ECX) captures the this pointer.
- Assumes **NO SEH frame** — minimal prologue (`55 8B EC 8B 45 08 56 8B F1`).
- Assumes both stack args are read into struct fields before the thread check: `[EBP+0x8]` → `[ESI+0x12]` (`89 46 12`) and `[EBP+0xc]` → `[ESI+0x16]` (`89 46 16`).
- Assumes `PUSH [ESI+0x72]` (`FF 76 72`) — same pattern as Target 11.
- Assumes command 0x17 (save/commit) is embedded as `C7 46 04 17 00 00 00` (MOV [ESI+4], 0x17) — this immediate distinguishes Target 12 from Target 10 (which uses 0x0F).
- Assumes the INT3 at offset 0x28 is the thread-check breakpoint (command 0x17 — save/commit).

---

## 5. Export Name Confirmation

*Source: `tmp/re_artifacts/b1_export_confirmation.md`*

The 7 export names used in production code (`src/tsdev_lib.cpp` lines 357–367), confirmed present in `orlib20_baseline.dll`'s export table via `dumpbin /exports`. Export table: 699 functions total, 636 named, 63 ordinal-only (`[NONAME]`). Ordinal base = 10.

| # | Export name (as used in code) | Decorated? | Ordinal | Hint | Export RVA | Found in export table? |
|---|---|---|---|---|---|---|
| 1 | `_OrlibInit@8` | Yes (stdcall-decorated) | 594 | 24F | 0x0011B4F0 | Yes |
| 2 | `_OrlibExit@0` | Yes (stdcall-decorated) | 593 | 24E | 0x0011C080 | Yes |
| 3 | `FindExistPos` | **No — undecorated** | 453 | 12A | 0x001A4230 | Yes |
| 4 | `_CreateEntryEX@24` | Yes (stdcall-decorated) | 389 | 200 | 0x00159610 | Yes |
| 5 | `_SaveModule_New@20` | Yes (stdcall-decorated) | 634 | 25D | 0x00159A70 | Yes |
| 6 | `_VerifyModule_New@20` | Yes (stdcall-decorated) | 700 | 274 | 0x00169630 | Yes |
| 7 | `_CloseWarehouseForCurrentThreadAndPath@4` | Yes (stdcall-decorated) | 377 | 1F9 | 0x001A9940 | Yes |

**All 7 export names are present in the export table.** None are ordinal-only; all 7 are exported by name.

### `FindExistPos` decorated-form check

The production code resolves `FindExistPos` by its **undecorated** name (not `_FindExistPos@8`). Both forms were checked:

- `FindExistPos` (undecorated) → **FOUND** (ordinal 453, RVA 0x001A4230).
- `_FindExistPos@8` (stdcall-decorated) → **NOT FOUND.** No line in the export table matches `_FindExistPos@8`.

Note: a *different* function `_FindExistPosSubType@12` exists (ordinal 454, RVA 0x001A41B0), but that is a separate export, not the decorated form of `FindExistPos`. The production code's claim that `FindExistPos` is exported undecorated is **CONFIRMED** — the decorated form does not exist.

### Calling conventions for the 7 exports

*Source: `tmp/re_artifacts/b1_rva_inventory.md` (§Calling-convention summary)*

| Export | Convention | Signature |
|---|---|---|
| `_OrlibInit@8` | `__stdcall` | (const char* workDirPath, int contextId) → BOOL |
| `_OrlibExit@0` | `__stdcall` | () → BOOL |
| `FindExistPos` | `__stdcall` | (const char* name, int warehouseType) → int |
| `_CreateEntryEX@24` | `__stdcall` | (int whType, int secondary, const char* name, int param4, char param5, int* errCode) → int |
| `_SaveModule_New@20` | `__stdcall` | (int warehouseType, int progId, void* arg3, HWND editorHwnd, void* arg5) → int |
| `_VerifyModule_New@20` | `__stdcall` | (HWND hwnd, int warehouseType, int progId, int flag, void* compilerObj) → int |
| `_CloseWarehouseForCurrentThreadAndPath@4` | `__stdcall` | (int param) → void |

---

## 6. Data-Section RVA Targets

*Source: `tmp/re_artifacts/b1_rva_inventory.md` (entries 13–15), `tmp/re_artifacts/b1_ghidra_evidence.md` (§3 Data-Section RVA Targets)*

These 3 data-section RVAs are **NOT code signatures** — they are data globals read directly by the production code. They are documented here for completeness but are NOT part of the 12 code-section signature targets.

| # | RVA | VA | Name | Purpose | Access method |
|---|---|---|---|---|---|
| 13 | 0x23CEA0 | 0x1023CEA0 | DAT_1023cea0 | Global pointer to error-record array | `*(uint8_t**)(base + RVA)` — direct memory read |
| 14 | 0x23CEA4 | 0x1023CEA4 | DAT_1023cea4 | Global uint32 count of error records | `*(uint32_t*)(base + RVA)` — direct memory read |
| 15 | 0x23AAB8 | 0x1023AAB8 | DAT_1023aab8 | HINSTANCE for LoadStringA fallback (string table) | `*(HINSTANCE*)(base + RVA)` — direct memory read |

### Resolution strategy (deferred)

Data-section RVAs cannot be matched by instruction-pattern signatures (there are no instruction patterns in data). For robustness across DLL versions, these should be resolved by:

1. Finding the **referencing code function** by its code signature (e.g., `StartCompilingProcess` at RVA 0x165DE0 reads `DAT_1023cea0` at instruction RVA 0x165DF9 via `MOV ECX, [0x1023cea0]`).
2. Extracting the **data reference** from the disassembled instruction (the 4-byte absolute address operand).
3. Converting the absolute address to an RVA by subtracting the image base.

This approach ties data-offset resolution to code-signature resolution, making both robust to DLL version changes (as long as the referencing function's signature still matches). Implementation of this strategy is **deferred to TVS-B.2/B.3**.

---

## 7. Structural Constants

*Source: `tmp/re_artifacts/b1_rva_inventory.md` (entries 16–29)*

These constants are used by the production code and/or embedded in signatures. They are documented here as part of the RE baseline.

### Constants embedded in signatures (fixed bytes, NOT wildcarded)

| Constant | Value | Name | Where embedded |
|---|---|---|---|
| Error record stride | 0x129 (297) | kErrRecordStride | Target 1 (StartCompilingProcess): `69` opcode prefix of `IMUL EAX, EAX, 0x129` |
| Error record size | 0xB1 (177) | — | Target 9 (FUN_10011230): `68 B1 00 00 00` (PUSH 0xB1) |
| Stack frame size | 0x168 (360) | — | Target 9: `81 EC 68 01 00 00` (SUB ESP, 0x168) |
| Stack frame size | 0x194 (404) | — | Target 7 (GetWarehouse): `81 EC 94 01 00 00` (SUB ESP, 0x194) |
| Stack frame size | 0x2DC (732) | — | Target 8 (FUN_101a8360): `81 EC DC 02 00 00` (SUB ESP, 0x2DC) |
| Stack frame size | 0x28 (40) | — | Target 6 (DeleteFromTree): `83 EC 28` (SUB ESP, 0x28) |
| Stack frame size | 0x08 (8) | — | Target 3 (CELCompiler ctor): `83 EC 08` (SUB ESP, 8) |
| Allocation size | 0x2C (44) | — | Target 5 (InitGlobalLookup): `6A 2C` (PUSH 0x2C) |
| Command dispatch | 0x0F | — | Target 10 (FUN_101d0950): `C7 46 04 0F 00 00 00` (MOV [ESI+4], 0x0F) |
| Command dispatch | 0x17 | — | Target 12 (FUN_101d1fa0): `C7 46 04 17 00 00 00` (MOV [ESI+4], 0x17) |
| Window style/flag | 0x2000 | — | Target 10: `B8 00 20 00 00` (MOV EAX, 0x2000) |
| Command check | 0x1E | — | Target 11 (FUN_101cf5b0): `83 F8 1E` (CMP EAX, 0x1E) |
| One-time-init guard | 0x01 | — | Target 7: `A8 01` (TEST AL, 1) + `83 C8 01` (OR EAX, 1) |

### Constants used in production logic (NOT in signatures)

| Constant | Value | Name | Source |
|---|---|---|---|
| CELCompiler object size | 0x58 (88) | kCELCompilerSize | `src/tsdev_lib.cpp:48` — malloc'd + zeroed before ctor |
| Error record stride | 0x129 (297) | kErrRecordStride | `src/tsdev_lib.cpp:141` — bytes per error record in global array |
| Error text offset | 0x29 | kErrTextOffset | `src/tsdev_lib.cpp:142` — offset of null-terminated error text within record |
| Error resource ID offset | 0x12 | kErrResIdOffset | `src/tsdev_lib.cpp:143` — offset of `short` string-table resource ID |
| Error char offset | 0x0A | kErrCharOffset | `src/tsdev_lib.cpp:144` — offset of `int` character offset for line/col computation |
| CELCompiler +0x10 field offset | 0x10 | — | `src/tsdev_lib.cpp:474` — set to 0xA02 after construction |
| CELCompiler +0x18 field offset | 0x18 | — | `src/tsdev_lib.cpp:475` — set to m_editorHwnd |
| CELCompiler +0x28 field offset | 0x28 | — | `src/tsdev_lib.cpp:477` — set to 6 after construction |
| CELCompiler +0x10 field value | 0xA02 | — | `src/tsdev_lib.cpp:474` |
| CELCompiler +0x28 field value | 6 | — | `src/tsdev_lib.cpp:477` |
| Warehouse type: Strategy | 0 | — | `src/tsdev_lib.h:26-28` |
| Warehouse type: Indicator | 2 | — | `src/tsdev_lib.h:26-28` |
| Warehouse type: Function | 4 | — | `src/tsdev_lib.h:26-28` |
| Error count sanity bound | 1000 | — | `src/tsdev_lib.cpp:153` — guards against corrupt count |
| LoadStringA buffer size | 1024 | — | `src/tsdev_lib.cpp:187` — `char buf[1024]` |
| Error level (error) | 0x7775 | — | Referenced in docs/AGENTS only; NOT present as a literal in production source. Error-record level field at +0x00 (0x7775 = error). Recorded for completeness. |

---

## 8. Uniqueness Verification Methodology

*Source: `tmp/re_artifacts/b1_sig_scan_run.md`, `src/sig_scan.cpp`*

### Probe description

The standalone probe `src/sig_scan.cpp` verifies that each of the 12 candidate signatures matches exactly once within the `.text` section of `orlib20_baseline.dll`.

**How it works:**

1. **Reads the DLL from disk** (does NOT load it into memory). The entire file is read into a `std::vector<uint8_t>`.
2. **Parses PE headers**: DOS header → `e_lfanew` → NT headers (PE32). Validates DOS signature (`MZ`), PE signature (`PE\0\0`), and optional header magic (`0x010B` = PE32). Asserts `sizeof(void*) == 4` (32-bit build).
3. **Locates the `.text` section** by iterating section headers (`IMAGE_FIRST_SECTION` macro) and matching the name `.text`. Extracts: virtual address (RVA), virtual size, raw file offset, raw size.
4. **Parses each signature** from `b1_signatures.h` into a parallel byte array + wildcard mask (`Pattern` struct). `??` tokens become wildcards; `XX` tokens are fixed bytes.
5. **Scans the `.text` raw bytes** with wildcard-aware byte matching: for each position `i` in `[0, textRawSize - patLen]`, checks if all non-wildcard bytes match. Collects all match offsets.
6. **Converts match offsets to RVAs**: `RVA = textRVA + offsetInText`. Reports file offset and RVA for each match.
7. **PASS criteria**: exactly 1 match AND the match RVA equals the expected RVA from `b1_signatures.h`.
8. **Patch-site verification**: for patch sites, reads the byte at `matchOffset + patchOffset` and compares it to the first byte of `originalBytes`. Reports OK if they match, WARNING if they differ (may already be patched).
9. **Exit code**: 0 = all 12 PASS; 1 = at least one FAIL.

### Build and run

```
cmake -S . -B build -A Win32
cmake --build build --config Release --target sig_scan
build\Release\sig_scan.exe [path_to_orlib20.dll]
```

Defaults to `tests/fixtures/orlib20_baseline.dll` if no argument is given.

### Result

**12 / 12 signatures PASS. Exit code 0.**

All 12 signatures matched exactly once at their expected RVAs within the `.text` section. All 4 patch-site byte checks passed (the byte at each patch offset matches the expected original byte).

| # | Name | Expected RVA | Match Count | Matched RVA | Result |
|---|---|---|---|---|---|
| 1 | StartCompilingProcess | 0x00165DE0 | 1 | 0x00165DE0 | PASS |
| 2 | EndCompilingProcess | 0x00165ED0 | 1 | 0x00165ED0 | PASS |
| 3 | CELCompiler_ctor | 0x00165A00 | 1 | 0x00165A00 | PASS |
| 4 | CELCompiler_dtor | 0x00165AC0 | 1 | 0x00165AC0 | PASS |
| 5 | InitGlobalLookup | 0x001A9460 | 1 | 0x001A9460 | PASS |
| 6 | DeleteFromTree | 0x001E4030 | 1 | 0x001E4030 | PASS |
| 7 | GetWarehouse | 0x001A9650 | 1 | 0x001A9650 | PASS |
| 8 | FUN_101a8360_delete_orchestrator | 0x001A8360 | 1 | 0x001A8360 | PASS |
| 9 | FUN_10011230_persistence_patch | 0x00011230 | 1 | 0x00011230 | PASS (patch byte 0x55 OK) |
| 10 | FUN_101d0950_thread_check_INT3 | 0x001D0950 | 1 | 0x001D0950 | PASS (patch byte 0xCC OK) |
| 11 | FUN_101cf5b0_thread_check_INT3 | 0x001CF5B0 | 1 | 0x001CF5B0 | PASS (patch byte 0xCC OK) |
| 12 | FUN_101d1fa0_thread_check_INT3 | 0x001D1FA0 | 1 | 0x001D1FA0 | PASS (patch byte 0xCC OK) |

*(Source: `b1_sig_scan_run.md`, final run, exit code 0, "ALL SIGNATURES UNIQUE AND VERIFIED.")*

**Note on signature #12 evolution:** During the first scan run, signature #12 (FUN_101d1fa0) was 41 bytes and matched twice (a spurious match at RVA 0x001CFCE0 in addition to the expected 0x001D1FA0). The signature was extended to 57 bytes by appending the post-INT3 context bytes (`FF 76 6E C7 46 04 17 00 00 00 FF 15 ?? ?? ?? ??`), which disambiguated the duplicate. The re-run confirmed uniqueness (count = 1, exit 0). The 57-byte version is the finalized signature in `b1_signatures.h`.

---

## 9. Open Items / Deferred

The following items are **out of scope for TVS-B.1** and deferred to later TVSs:

| Item | Deferred to | Description |
|---|---|---|
| Data-section RVA resolution | TVS-B.2/B.3 | The 3 data-section RVAs (0x23CEA0, 0x23CEA4, 0x23AAB8) are currently hardcoded. Robust resolution requires finding the referencing code function by signature, then extracting the data reference from the disassembled instruction. See §6. |
| Signature-based RVA resolution integration | TVS-B.4/B.5 | The 12 signatures in this catalog are verified unique but NOT yet integrated into the production code. The production code still uses hardcoded RVAs. Integration will replace hardcoded RVAs with signature-scanned RVAs at runtime. |
| Version detection logic | TVS-B.6 | The DLL baseline metadata (FileVersion, SHA-256, PE timestamp) documented in §1 is the baseline for version detection. Logic to compare the loaded DLL against this baseline (and abort/warn on mismatch) is deferred. |
| SEH protection on data-array reads | Future robustness TVS | `readErrorsFromArray` reads the global error array without SEH protection. Adding SEH guards around the array pointer dereference and record iteration is a future robustness improvement (noted in TVS-A.4 adversarial review). |
| Warehouse type resolution | Future TVS | Only Strategy=0, Indicator=2, Function=4 are supported. ActivityBar, PaintBar, ProbabilityMap, ShowMe, TradingApp warehouse types remain unresolved (`src/tsdev_lib.h:22-24`). |

---

## Appendix A: Acceptance Criteria Traceability

| Criterion (HANDOFF_B §4) | Where satisfied |
|---|---|
| 1. Document lists, for each of 12 RVA targets: RVA, function/patch name, candidate signature (hex with wildcard), signature length, uniqueness-verification result (count = 1 in .text), expected original bytes at patch site (for patches), calling convention, structural assumptions | §3 (catalog table + patch metadata table), §4 (per-target detail sections with all fields) |
| 2. Each signature verified unique by scanning actual orlib20.dll .text section | §8 (methodology + results: 12/12 PASS, exit 0, using `src/sig_scan.cpp` standalone probe) |
| 3. For exported functions (§2.4), catalog notes decorated/undecorated name and confirms it appears in export table | §5 (7 export names with decorated/undecorated status, ordinals, RVAs, all confirmed FOUND) |
| 4. Catalog notes orlib20.dll file version, PE timestamp, file size, SHA-256 hash | §1 (DLL Baseline: FileVersion 9.50.01.3344, PE timestamp 0x5C01735E, file size 4,924,400 bytes, SHA-256 318DB24A...) |
