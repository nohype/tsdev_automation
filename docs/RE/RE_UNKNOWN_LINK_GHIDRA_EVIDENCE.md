# Unknown-link persistence evidence appendix

> **Document status:** Durable tracked RE evidence (promoted from
> `tmp/unknown_link_ghidra_evidence.md`). Research complete and adversarially
> approved (Round 2 APPROVED). **Production fix not implemented** — this
> document records static evidence only; no production source, signatures,
> tests, or documentation were changed as part of this research deliverable.
> **Final PM review:** Hands-on adversarial PM review APPROVED on 2026-08-23; all load-bearing control-flow, ABI, loader, and COM-ordering claims were rechecked against this appendix.
> **Relationship to workstreams:** This appendix is the durable static
> evidence base for **Workstream L** (`docs/workstream_unknown_link_fix_plan.md`,
> the immediate runtime-link root-cause fix) and a cited source authority for
> **Workstream M** (`docs/workstream_tsdev_parity_plan.md`, TSDev verify/library
> parity follow-ups). It is referenced by
> `docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md`. Promotion of this artifact satisfies
> Workstream M's durable-evidence prerequisite gate for the static-evidence
> citations it carries; it does **not** weaken Workstream M's hard dependency
> on Workstream L being approved and merged.

## Scope, programs, and address convention

This appendix records instructions and decompiler output obtained from the installed baseline programs in the Ghidra project `TSDevAnalysis`. No Ghidra annotations or production-file changes are required to reproduce it.

| Ghidra program | Preferred image base | Relevant role |
|---|---:|---|
| `/orlib20.dll` (`orlib20.dll`) | `0x10000000` | compiler, symbol writer call, ELL save |
| `/receivers/ELMDCore.dll` (`ELMDCore.dll`) | `0x10000000` | `.ell` path construction and metadata reader |
| `/receivers/ELRte.dll` (`ELRte.dll`) | `0x10000000` | runtime load/link and GUID validation |
| `/receivers/ELDebugger.dll` (`ELDebugger.dll`) | `0x10000000` | debugger symbol XML reader |
| `/TSDev.exe` (`TSDev.exe`) | `0x00400000` | UI command chain and COM initialization |

All addresses below are preferred virtual addresses, not ASLR runtime addresses. `orlib20.dll` is the committed version `9.50.01.3344`; `../../signatures/v9.50.01.3344.json` (lines 3-13) records SHA-256 `318DB24A4F41BDB9FF69E2E647026B788706904CF887CBB0A953E003DB22EC18` and image base `0x10000000`.

## 1. `CompileText` -> `FUN_10011230`: actual cleanup contract

### Complete caller sequence

Ghidra identifies `CELBasicCompiler::CompileText` at `orlib20.dll:0x1001DD80`. Its only direct call to `FUN_10011230` is at `0x1001E0BC`:

```asm
1001E098  PUSH 0xFDE9
1001E09D  LEA  EAX,[EBP-0x64]
1001E0A0  PUSH EAX
1001E0A1  LEA  EAX,[EBP-0x7C]
1001E0A4  PUSH EAX
1001E0A5  CALL 0x1002ABB0
1001E0AA  ADD  ESP,0x0C
1001E0AD  MOV  ESI,[EBP-0x18]
1001E0B0  MOV  EDX,EDI                 ; register argument 2
1001E0B2  PUSH EAX                     ; stack argument 3
1001E0B3  PUSH ECX                     ; stack argument 2
1001E0B4  MOV  byte ptr [EBP-4],0x05
1001E0B8  MOV  ECX,[EBP-0x30]          ; register argument 1
1001E0BB  PUSH ESI                     ; stack argument 1
1001E0BC  CALL 0x10011230
1001E0C1  ADD  ESP,0x0C                ; caller removes all three pushes
1001E0C4  MOV  byte ptr [EBP-4],0x03
1001E0C8  CMP  EAX,0x01
1001E0CB  LEA  ECX,[EBP-0x7C]
1001E0CE  SETZ BL
1001E0D1  CALL 0x10009780
1001E0D6  TEST BL,BL
1001E0D8  JZ   0x1001E24F             ; failure when return != 1
```

The decompiler renders the core operation as:

```c
iVar3 = FUN_10011230(local_1c, extraout_ECX_01, uVar6);
FUN_10009780();
if (iVar3 != 1) {
    FUN_10156ad0(0, 0, 0x7532);
    FUN_1000e640();
    goto LAB_1001e2b2;
}
```

### Original callee epilogue

The unpatched `FUN_10011230` entry is `0x10011230`. Its original epilogue is:

```asm
10011443  MOV ECX,[EBP-0x0C]
10011446  MOV EAX,EDI
10011448  POP EDI
10011449  MOV dword ptr FS:[0],ECX
10011450  POP ESI
10011451  MOV ESP,EBP
10011453  POP EBP
10011454  RET                         ; bytes C3; no immediate
```

Thus the original function does **not** execute `RET 0x0C`.

### Current project replacement bytes

The authoritative signature entry is `../../signatures/v9.50.01.3344.json` (lines 156-168):

```text
expectedRva:      0x11230
originalBytes:    55 8B EC 6A FF 68 CD D3
replacementBytes: B8 01 00 00 00 C2 0C 00
```

The same replacement is present in source at `../../src/tsdev_lib.cpp` (lines 44-59):

```asm
B8 01 00 00 00    MOV EAX,1
C2 0C 00          RET 0x0C
```

### Calling-convention conclusion and stale contradiction

The instruction-level contract is unambiguous: ECX and EDX carry two register arguments, three values are pushed, the original callee returns with plain `RET`, and the caller immediately executes `ADD ESP,0x0C`. Therefore the original ABI is fastcall-like register passing with **caller cleanup for these 12 stack bytes**. The current replacement's `RET 0x0C` changes that cleanup contract; on return, `CompileText` still adds another `0x0C`.

`../SIGNATURE_CATALOG.md` (lines 106, 380, 386, 404) is stale where it says the function/patch uses `RET 0x0C` because the callee cleans 12 bytes. The same stale assumption appears in `../FEASIBILITY.md` (lines 338-340). Those statements contradict both `0x10011454 RET` and `0x1001E0C1 ADD ESP,0x0C`.

## 2. Complete direct-call inventory and the skipped work

### Direct-call inventory

Ghidra `get_function_xrefs(0x10011230)` returns exactly one code reference:

| Caller | Call instruction | Type |
|---|---:|---|
| `CELBasicCompiler::CompileText` (`0x1001DD80`) | `0x1001E0BC` | unconditional direct call |

No other direct call is present in the analyzed `orlib20.dll` program.

### `FUN_10011230` construction/save order

A concise decompiled excerpt, preserving order, is:

```c
FUN_10010ca0(param_2, local_18);
pSVar1 = operator_new(0xb1);
SymInfo::SymInfo(pSVar1);
SymInfo::SetVersion(pSVar1, ...);
SymInfo::SetFileName(pSVar1, ...);
pSVar3 = operator_new(0x5d);
SymMethod::SymMethod(pSVar3, 0, 0, 0, 0, 0x52a, 0);
SymMethod::SetMethodRef(pSVar3, SymInfo::GetMethodRef(pSVar1));
SymMethod::SetMethodName(pSVar3, L"Main");
SymInfo::AddBlock(pSVar1, (SymBlock *)pSVar3, ...);
uVar5 = FUN_1008e640(local_1c, param_3, extraout_ECX);
SymInfo::AdjustOffsets(pSVar1);
SymWriter::SymWriter(local_44, pSVar1);
SymWriter::Save(local_44);
SymWriter::~SymWriter(local_44);
...
return uVar5;
```

The exact direct call and argument setup are:

```asm
100113C2  PUSH ECX
100113C3  PUSH dword ptr [EBP+0x08]
100113C6  LEA  ECX,[EBP-0x174]
100113CC  MOV  [EBP-0x80],ESI
100113CF  PUSH dword ptr [EBP-0x18]
100113D2  CALL 0x1008E640
100113D7  MOV  ECX,ESI
100113D9  MOV  EDI,EAX                ; preserve return from FUN_1008E640
100113DB  CALL dword ptr [0x101F928C] ; SymInfo::AdjustOffsets
100113E1  PUSH ESI
100113E2  LEA  ECX,[EBP-0x40]
100113E5  CALL dword ptr [0x101F9288] ; SymWriter constructor
100113EB  LEA  ECX,[EBP-0x40]
100113EE  MOV  byte ptr [EBP-4],0x06
100113F2  CALL dword ptr [0x101F9280] ; SymWriter::Save
100113F8  LEA  ECX,[EBP-0x40]
100113FB  CALL dword ptr [0x101F9284] ; SymWriter destructor
...
10011446  MOV  EAX,EDI                ; function result is saved callee result
```

Ghidra `get_function_xrefs(0x1008E640)` likewise reports exactly one direct caller, `FUN_10011230` at `0x100113D2`.

### What `FUN_1008E640` establishes, conservatively

`FUN_1008E640` spans `0x1008E640-0x1008F777`. Its decompilation is a large compiler/link-finalization routine: it initializes compiler-side tables/state, examines module flags/type, appends generated instruction bytes in applicable branches, invokes numerous finalization helpers, and returns success according to the global compile status. Its final return sequence is:

```asm
1008F749  MOV  EAX,[EBP-0x30]
1008F74C  PUSH EAX
1008F74D  MOV  ECX,0x1023E844
1008F752  CALL 0x100FBF90
1008F757  XOR  EAX,EAX
1008F759  CMP  dword ptr [0x1023AAC8],EAX
1008F75F  SETZ AL                     ; true iff compile-status global == 0
...
1008F775  RET  0x0C
```

This supports describing `FUN_1008E640` as compiler/link/finalization behavior and identifies the source of the boolean later returned by `FUN_10011230`. It does **not** support attributing construction of the `SymInfo`/`SymMethod` XML tree to `FUN_1008E640`: that construction is visibly performed by `FUN_10011230` before the call. It also does **not** establish that `FUN_1008E640` writes any particular RNAM section. No such section-specific claim is made without section-writer xrefs.

## 3. `CompileText` after `FUN_10011230`: failure versus ELL persistence

The failure target at `0x1001E24F` is:

```asm
1001E24F  MOV  EBX,[EBP+0x10]
1001E252  MOV  ECX,ESI
1001E254  PUSH 0x7532
1001E259  PUSH 0x0
1001E25B  PUSH 0x0
1001E25D  MOV  EDX,EBX
1001E25F  CALL 0x10156AD0
1001E264  ADD  ESP,0x0C
1001E267  MOV  byte ptr [EBP-4],0x01
1001E26B  LEA  ECX,[EBP-0x64]
1001E26E  CALL 0x1000E640
1001E273  JMP  0x1001E2B2
```

On the success edge, the next persistence operations are:

```asm
1001E0DE  MOV  EAX,[EBP+0x24]
1001E0E1  MOV  EAX,[EAX+0x142]
1001E0E7  MOV  ESI,[EAX+0xDA]         ; ModuleInfo *
1001E0ED  LEA  EAX,[EBP-0x8C]         ; generated GUID
1001E0F3  PUSH EAX
1001E0F4  MOV  ECX,ESI
1001E0F6  CALL dword ptr [0x101F9340] ; ELMeta::ModuleInfo::SetGuid
1001E0FC  PUSH dword ptr [EBP+0x28]
1001E0FF  MOV  ECX,[ESI+0x214]        ; MDVault *
1001E105  PUSH ESI
1001E106  CALL dword ptr [0x101F9344] ; ELMeta::MDVault::SaveModule
```

The decompiler agrees:

```c
if (iVar3 != 1) { ... goto LAB_1001e2b2; }
this_00 = *(ModuleInfo **)(*(int *)(param_8 + 0x142) + 0xda);
ELMeta::ModuleInfo::SetGuid(this_00, &local_90);
ELMeta::MDVault::SaveModule(*(MDVault **)(this_00 + 0x214), this_00, param_9);
```

Therefore `FUN_10011230`'s symbol-tree construction and `SymWriter::Save` occur before the ELL-side `ModuleInfo::SetGuid`/`MDVault::SaveModule` sequence. A non-1 return takes the failure branch and skips those later calls. This proves ordering only. It does **not** prove that the runtime consumes the XML.

## 4. Runtime loader: ELL, GUID check, null return; XML is debugger-only

### `.ell` construction and ELMDCore load

In `ELMDCore.dll`, `FUN_10020310` appends only `.ell`:

```c
piVar1 = FUN_10020a70(param_2, (int *)L".ell", 4);
FUN_10011670(param_1, piVar1);
```

```asm
10020320  PUSH 0x4
10020322  PUSH 0x10024AD0             ; ASCII ".ell"
10020327  MOV  ECX,EDX
1002032C  CALL 0x10020A70
10020346  PUSH EAX
10020347  CALL 0x10011670
```

`ELMeta::MDVault::LoadModule` at `ELMDCore.dll:0x1001EE30` builds that path (`CALL 0x10020310` at `0x1001EF1B`) and invokes the file overload at `0x1001EF59`. The file overload at `0x1001EDD0` constructs `MDReader` (`CALL 0x10019520` at `0x1001EDFC`) if the module is not already cached. `MDReader::ReadFromFile` at `0x10019A60` checks/opens the selected path, reads its bytes, calls `ReadFromBuff`, and then `ModuleInfo::Load`. No `.xml` string or XML API occurs in this ELMDCore load chain.

### ELRte load and expected-GUID validation

`ELRte::Loader::LoadAndLinkELProgram` is at `ELRte.dll:0x1000D9D0`. Its control flow is:

```asm
1000DA88  PUSH EBX                    ; module id / selector
1000DA89  PUSH [EBP+0x10]
1000DA8C  LEA  EAX,[EBP-0x28]         ; module name
1000DA8F  MOV  ECX,0x1002D898         ; MDVault
1000DA94  PUSH EAX
1000DA95  CALL dword ptr [0x1001E0E8] ; MDVault::GetModule
1000DA9B  MOV  ESI,EAX
1000DA9D  TEST ESI,ESI
1000DA9F  JZ   0x1000DB75             ; load failure -> null result path
1000DAA5  CMP  EBX,-1
1000DAA8  JZ   0x1000DABE
1000DAAA  PUSH [EBP+0x18]             ; caller's expected GUID string
1000DAAD  MOV  ECX,[EBP-0x10]         ; Loader this
1000DAB0  PUSH ESI                    ; loaded ELL ModuleInfo
1000DAB1  CALL 0x1000D920             ; ValidateVersion
1000DAB6  CMP  AL,0x1
1000DAB8  JNZ 0x1000DB75             ; GUID mismatch -> null result path
```

`ELRte::Loader::ValidateVersion` at `0x1000D920` obtains the GUID from the loaded `ModuleInfo`, converts it to text, and compares it case-insensitively with the caller-provided expected string:

```c
ELMeta::ModuleInfo::GetGuid(param_1, &local_14);
StringFromGUID2(&local_14, &local_214, 0xff);
...
iVar3 = _stricmp(param_2, _Str2);
return iVar3 == 0;
```

```asm
1000D923  MOV  ECX,[EBP+0x08]         ; loaded ModuleInfo
1000D926  LEA  EAX,[EBP-0x10]
1000D931  PUSH EAX
1000D932  CALL dword ptr [0x1001E0E4] ; ModuleInfo::GetGuid
...
1000D9A6  PUSH EAX                    ; converted ELL GUID text
1000D9A7  PUSH [EBP+0x0C]             ; expected GUID text
1000D9AA  CALL dword ptr [0x1001E444] ; _stricmp
1000D9B3  NEG  EAX
1000D9B5  SBB  EAX,EAX
1000D9B7  INC  EAX                    ; 1 only when equal
```

The function initializes its eventual `ModuleNode *` result to null and reaches `0x1000DB75` both when `GetModule` returns null and when `ValidateVersion` returns false. A relocated `ModuleNode` is assigned only later on the success path. Thus the observable failure contract is a null return. Static evidence proves expected-GUID-versus-loaded-ELL-GUID validation; it does not identify which upstream producer is wrong in a failing run.

### XML is not in the runtime chain; `SymReader` is debugger-side

The `.xml` path is instead in `ELDebugger.dll`. `SymReader::Load` at `0x100124B0` calls `FUN_10014E60` at `0x100124E2`; that helper appends `.xml` at `0x10014F10`. It then calls `elsystem::xml::XmlDocument::LoadWithoutException` at `0x10012522` and calls `FillSymInfo` only if XML load succeeds:

```c
FUN_10014e60(local_7f8, ...);                  // append .xml
...
bVar1 = XmlDocument::LoadWithoutException(...);
if (bVar1) FillSymInfo(this);
```

`SymReader::Load` is directly called by `CELDebugger::LoadDependencies` (`0x1000596A`) and debugger setup (`SetModuleName`, `0x10005ADD`) inside the program named `ELDebugger.dll`. By contrast, the ELRte/ELMDCore runtime path above uses MDVault/MDReader and `.ell`, with no XML API or `.xml` path. This supports “debugger-only symbol reader path” and “XML absent from runtime load”; it does not claim that XML is never used by any non-runtime tool.

## 5. TSDev COM initialization and Orlib initialization order

### Main/UI initialization routine

`TSDev.exe:0x00491960` is the large application initialization routine (its sole reference is the application vtable/data entry at `0x0057166C`). It calls `CoInitialize(NULL)` here:

```asm
00491F71  CALL dword ptr [0x0055E748]
00491F77  PUSH 0x0
00491F79  MOV  ESI,EAX
00491F7B  CALL dword ptr [0x0055FF5C] ; OLE32!CoInitialize
00491F81  MOV  ECX,EDI
00491F83  CALL 0x00490A20
```

Ghidra's external xrefs identify `[0x0055FF5C]` as `OLE32!CoInitialize` (`EXTERNAL:0000068B`). The literal null argument is visible at `0x00491F77`.

Later in the **same routine**, after intervening synchronous initialization calls, control reaches:

```asm
00492A3A  MOV  ECX,EDI
00492A3C  CALL 0x00492DC0
00492A41  MOV  ECX,EDI
00492A43  CALL 0x00492E20
00492A48  MOV  ECX,EDI
00492A4A  CALL 0x004918F0
```

`FUN_00492E20` directly calls `_OrlibInit@8`:

```asm
00492E3E  CMP  dword ptr [ESI+0x220],0
00492E45  JNZ  0x00492E92
00492E47  PUSH 0x1
00492E49  PUSH 0x0
00492E4B  CALL dword ptr [0x00560238] ; orlib20!_OrlibInit@8
00492E51  MOV  ECX,ESI
00492E53  CALL 0x00493290
00492E58  PUSH 0x3
00492E5A  CALL dword ptr [0x0056023C] ; _DisableELDebugger@4
```

Ghidra external xrefs identify `[0x00560238]` as `orlib20!_OrlibInit@8` (`EXTERNAL:00000198`). Therefore static control flow proves order `CoInitialize(NULL)` at `0x00491F7B` -> direct call to `FUN_00492E20` at `0x00492A43` -> `_OrlibInit(NULL,1)` at `0x00492E4B`. Both first and second steps are ordinary calls in the same application-initialization routine; no worker dispatch occurs in this chain. This is the static basis for saying TSDev initializes COM before Orlib on its UI/main initialization thread.

### Confirmed COM-bracketed TSDev worker

`TSDev.exe:FUN_00495E90` is a separate callback/worker-style routine (referenced through data at `0x0057207C`) that brackets its work:

```asm
00495E90  PUSH ESI
00495E91  MOV  ESI,[ECX+4]
00495E94  TEST ESI,ESI
00495E96  JZ   0x00495ED8
00495E98  PUSH 0x0
00495E9A  CALL dword ptr [0x0055FF5C] ; CoInitialize(NULL)
00495EA0  LEA  ECX,[ESI+0xC32C]
00495EA6  CALL 0x0046C590              ; worker operation
...
00495ED2  JMP  dword ptr [0x0055FF64] ; tail-call CoUninitialize
```

Ghidra external xrefs identify `[0x0055FF64]` as `OLE32!CoUninitialize` (`EXTERNAL:0000068D`). The decompiler renders the same bracket as `CoInitialize(NULL); FUN_0046c590(); ...; CoUninitialize();`.

## 6. UI Verify command chain and the omitted trace frame

### Message map

Raw data at `TSDev.exe:0x0056C500` is:

```text
6A 29 00 00  6A 29 00 00  3A 00 00 00  E0 B0 47 00
```

Interpreted as the MFC message-map entry fields, this is first ID `0x296A`, last ID `0x296A`, notification/signature field `0x3A`, and handler pointer `0x0047B0E0`. This directly binds command ID `0x296A` to `FUN_0047B0E0`.

### Exact direct-call chain

The requested chain, including every direct-call instruction address, is:

1. Handler `FUN_0047B0E0`:

```asm
0047B0FE  PUSH ESI
0047B0FF  MOV  ESI,ECX
0047B101  CALL 0x00451800
```

2. `FUN_00451800` calls `FUN_004D9480`:

```asm
00451AEC  MOV  ECX,EBX
00451AEE  CALL dword ptr [0x0055EA6C]
00451AF4  MOV  ECX,ESI
00451AF6  MOV  [ESI+0x18],EAX
00451AF9  CALL 0x004D9480
```

Ghidra xrefs show `0x00451AF9` is the only direct caller of `FUN_004D9480`.

3. `FUN_004D9480` calls `FUN_004D91E0`:

```asm
004D9488  PUSH dword ptr [ESI+0x10]
004D948B  MOV  ECX,ESI
004D948D  MOV  dword ptr [ESI],0
004D9493  PUSH dword ptr [ESI+0x0C]
004D9496  MOV  dword ptr [ESI+4],0
004D949D  PUSH dword ptr [ESI+8]
004D94A0  CALL 0x004D91E0
```

4. `FUN_004D91E0` calls the imported verifier:

```asm
004D928B  PUSH EBX
004D928C  PUSH ECX
004D928D  PUSH EAX
004D928E  PUSH EDI
004D928F  PUSH dword ptr [EBX+0x18]
004D9292  MOV  [EBP+8],ECX
004D9295  MOV  byte ptr [EBP-4],1
004D9299  CALL 0x0053F5BC
```

The target thunk is:

```asm
0053F5BC  JMP dword ptr [0x00560220]
```

Ghidra names `0x0053F5BC` `_VerifyModule_New@20`, and the import table names it `orlib20.dll!_VerifyModule_New@20`. Consequently the full static path is:

```text
message-map ID 0x296A
 -> FUN_0047B0E0
 -> [CALL 0x0047B101] FUN_00451800
 -> [CALL 0x00451AF9] FUN_004D9480
 -> [CALL 0x004D94A0] FUN_004D91E0
 -> [CALL 0x004D9299] thunk 0x0053F5BC
 -> orlib20!_VerifyModule_New@20
```

The `0x004D94A0` frame is therefore statically proven even if a dynamic trace omitted it.

## 7. Evidentiary conclusions and limits

1. **Dynamic causal conclusion.** The reported dynamic A/B result establishes that bypassing the `FUN_10011230` path is causally responsible for the runtime failure, while running the path in an STA-enabled context allows it to succeed. This is the dynamic conclusion; static disassembly alone cannot substitute for that A/B.
2. **Static mechanism.** Static analysis proves exactly what the bypass skips: `FUN_10011230`, its sole direct callee `FUN_1008E640`, symbol-tree construction, `SymInfo::AdjustOffsets`, and `SymWriter::Save`; it also proves that a non-1 result prevents the later `ModuleInfo::SetGuid` and `MDVault::SaveModule` calls. Independently, ELRte/ELMDCore prove `.ell` loading, expected-GUID versus loaded-ELL-GUID validation, and the null-return path on load/validation failure.
3. **Threading/COM.** TSDev's application initialization statically calls `CoInitialize(NULL)` before `_OrlibInit(NULL,1)` on the same synchronous UI/main initialization path, and a separate TSDev worker explicitly brackets work with `CoInitialize(NULL)`/`CoUninitialize`.
4. **XML limit.** XML creation/save is ordered before ELL persistence in the compiler path, but ELRte/ELMDCore runtime loading uses `.ell`, not XML. XML loading shown here belongs to `ELDebugger.dll!SymReader`. No claim is made that runtime consumes XML.
5. **Byte-comparison limit.** A byte comparison between working and failing `.ell` files can locate differences, but it does **not** establish that method bodies, class tables, or any other particular table bodies are missing. Such an interpretation requires identified section/table writers and xrefs; this appendix does not claim it.
6. **Mismatch limit.** The static loader proves a GUID comparison and rejection edge. It does not by itself prove that a specific observed failure is a “warehouse GUID mismatch,” identify which GUID producer is wrong, or exclude another earlier load failure. No exact internal mismatch is asserted without direct runtime values.
7. **Patch safety finding.** Independently of the persistence hypothesis, the current `RET 0x0C` replacement is inconsistent with the original callee's plain `RET` plus the caller's `ADD ESP,0x0C`. That stale cleanup assumption should not be used as evidence for the original ABI.
