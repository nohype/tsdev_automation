# TVS-M.2 Foundation Research — `_SaveModule_New@20` Flags, CELProperties, and Save-Side Encryption

> **Status:** READ-ONLY foundation research. No production code modified. No `tsdev_cli`
> or warehouse-mutating operation run. No new Frida sessions executed (existing traces
> only). One new findings document created (this file).
>
> **Method:** Existing Frida traces + RE notes examined FIRST, then Ghidra static
> analysis via the `ghidra-mcp` MCP server against the `TSDevAnalysis` project
> (`/orlib20.dll` and `/TSDev.exe` loaded, preferred bases 0x10000000 / 0x00400000).
>
> **Date:** 2026-08-24
> **Author:** TVS-M.2 foundation-research subagent

---

## UPDATE 2026-08-25 — Targeted static analysis corrections

A targeted static analysis of `TSDev.exe` resolved two of the four EVIDENCE NOT
FOUND statements in this document and corrected one substantive error. The
analysis located the unique TSDev.exe caller of `_SaveModule_New@20` and
decompiled its flags/password buffer construction. No production code was
modified; no new Frida sessions were run. All corrections below are
**superseding** — the original findings are preserved (with explicit
"SUPERSEDED" markers) for provenance.

### Correction 1 — R-M.2.1 exact flag bit values: RESOLVED (was EVIDENCE NOT FOUND)

The flags word (arg3) is **NOT a constant**. It is computed by the TSDev.exe
caller `FUN_00452290` (at `0x00452290`) as a per-item bitfield:

```c
flags = (type_word & 0x7)                        // bits 0-2: component category
      | ((FUN_0044f5b0() & 1) << 3)              // bit 3: has expiration date
      | ((FUN_0044f720() & 1) << 4)              // bit 4: is verified
      | ((IsProtected() & 1) << 5)               // bit 5: is protected
      | ((IsProtectedPlus() & 1) << 6)           // bit 6: is protected-plus
      // bits 7-15: cleared to 0 by the 0x3f mask applied in step 5
```

**CRITICAL — TSDev ALWAYS forces protection.** When `IsProtected()` returns
false, `FUN_00452290` enters an `else` branch that:
1. Sets the password string to `"!p$Kn@aSus"` (the internal default at
   `0x00567b40`, 10 bytes — see Correction 2);
2. Calls `SetProtectedPlus(true)`;
3. Calls `SetProtected(true)`.

Therefore **bits 5 and 6 are ALWAYS set to 1** for items saved through TSDev,
regardless of whether the user opted into protection. The only items that would
have bits 5/6 clear are those saved by a path that does not force protection —
not observed in TSDev.

For a **new item being saved before verify** (not yet verified, no expiration
date — the case captured in the Frida trace at line 20412):

| Component | whType | category (`type_word & 0x7`) | flags |
|---|---|---|---|
| Strategy | 0 | 0 | `0x60` (bits 5,6) |
| Indicator | 2 | 2 | `0x62` (bits 1,5,6) |
| Function | 4 | 4 | `0x64` (bits 2,5,6) |
| ShowMe | 1282 (`0x502`) | 2 | `0x62` (bits 1,5,6) |
| TradingApp | 5122 (`0x1402`) | 2 | `0x62` (bits 1,5,6) |

For an **existing verified item being re-saved** (bit 4 = 1, e.g. a modify
operation on a previously-verified item):

| Component | flags |
|---|---|
| Strategy | `0x70` (bits 4,5,6) |
| Indicator / ShowMe / TradingApp | `0x72` (bits 1,4,5,6) |
| Function | `0x74` (bits 2,4,5,6) |

**Reconciliation with §2d (Ghidra bitfield decomposition of `FUN_10159770`):**
the per-bit semantics are consistent — bit 5 (0x20) → master-record bit 0x400
("internally encrypted"), bit 4 (0x10) → metadata bit 2, bit 6 (0x40) →
metadata bit 3. The inferential `0x20` value used in §7a was correct in
identifying bit 5 as set, but **incomplete** — bits 5 and 6 are both always set
by TSDev, and the category bits (0-2) are also populated from the component
type, not zeroed. The pre-verify Strategy value `0x60` (bits 5,6) is the
closest match to the captured trace (Strategy, progId 6686, pre-verify).

**Evidence:** `FUN_00452290` decompilation; disassembly at
`0x004523a4`-`0x00452422`; IAT slot `0x005602c4` xref (the
`GetProcAddress`-resolved `_SaveModule_New@20` slot used by the caller). See
§8d for the new evidence-index entries.

### Correction 2 — R-M.2.3 password buffer: CORRECTED (was "zeroed buffer")

The password buffer (arg5) is **NOT purely zeroed**. `FUN_00452290` constructs
it as:

1. `memset(buffer, 0, 0x40)` — 64 bytes zeroed (matches the §4e / §6
   "zeroed 64-byte buffer" observation at the buffer level);
2. `memcpy(buffer, password_string, length)` — the password string is then
   copied in, overwriting the leading bytes.

For **unprotected items** (the TSDev-forced-protection `else` branch), the
password string is the **internal default `"!p$Kn@aSus"`** (10 bytes at
`0x00567b40`). For protected items, the password is the user-set password from
CELProperties.

**The foundation research's conclusion that "the password buffer is a
fixed/zeroed internal buffer" was WRONG.** The buffer contains the fixed
internal default password `"!p$Kn@aSus"`, not zeroes. This is consistent with,
and explains, three previously-mysterious observations:

- The constant salt `0xe1ca1015` returned by `_GetPasswordSalt@8` across three
  items (lines 20436, 182819, 266495) is the salt **of the fixed password
  `"!p$Kn@aSus"`**, not of a zeroed buffer.
- The `_DoEncryption2@16` call with length `0xa = 10` (Frida trace line 20437:
  `args=[0x8fdef0, 0xa, 0x5, 0x2, ...]`) matches the **10-byte length of
  `"!p$Kn@aSus"`** exactly — a zeroed buffer would not produce a 10-byte
  encryption length.
- `_IsUnicodePassword→0` is consistent: `"!p$Kn@aSus"` is an ANSI string.

**`"!p$Kn@aSus"` is an internal TSDev default, NOT a user secret.** It is a
hardcoded constant baked into `TSDev.exe` at `0x00567b40`, used as the
encryption key material for items TSDev force-protects. It is not a password
the user chooses or knows. The R-M.2.5 conclusion ("no user-facing password
parameter is needed or appropriate") still holds — the CLI must replicate this
**fixed internal default string**, not accept user input.

### New finding — TSDev.exe caller identified

`FUN_00452290` at `0x00452290` is the **unique caller** of
`_SaveModule_New@20` in `TSDev.exe`, via IAT slot `0x005602c4`
(`GetProcAddress`-resolved). This was previously "EVIDENCE NOT FOUND"
(foundation research §2e and §9 item 4). The caller builds both the flags word
(Correction 1) and the password buffer (Correction 2) on its stack before the
call. See §8d for the new evidence-index entries.

### Summary of impact on this document

| Section | Change |
|---|---|
| §2e | Marked SUPERSEDED; resolved flag values added. |
| §4e | Corrected: password buffer contains `"!p$Kn@aSus"`, not zeroes. |
| §7a | Code example updated to use the correct flags computation and password string. |
| §7c | Limitation #1 (flags) RESOLVED; limitation #3 (caller) RESOLVED. |
| §8d | New evidence entries added (`FUN_00452290`, IAT `0x005602c4`, string `0x00567b40`). |
| §9 | Items 1 and 4 marked RESOLVED. |

---

## 1. Executive summary

- **R-M.2.1 (flags/arg5):** TSDev passes **non-NULL** for both arg3 (flags pointer =
  `0x8fdf34`, a stack address) and arg5 (password pointer = `0x8fdef0`, a stack
  address) at the single captured pre-verify `_SaveModule_New@20` call
  (`tmp/frida_trace_verify.log` line 20412). The CLI passes NULL for both
  (`src/tsdev_lib.cpp:1109-1110`). Ghidra decompilation of the implementation
  `FUN_10159770` (reached via the `_SaveModule_New@20` wrapper at `0x10159A70`)
  confirms arg3 is `uint *flags` and arg5 is `void *password`, and gives the
  **bitfield decomposition of `*flags`**: bit 4 (0x10) → metadata bit 2; bit 5
  (0x20) → metadata bit 10 (0x400, the "internally encrypted" master-record flag);
  bit 6 (0x40) → metadata bit 3. **~~The exact bit VALUES TSDev writes into
  `*0x8fdf34` are EVIDENCE NOT FOUND~~** — **RESOLVED 2026-08-25 (UPDATE
  Correction 1):** the flags word is computed by `FUN_00452290` as
  `(type_word & 0x7) | (has_expdate << 3) | (is_verified << 4) | (is_protected
  << 5) | (is_protected_plus << 6)`, with TSDev always forcing bits 5 and 6 on.
  Pre-verify values: Strategy=`0x60`, Indicator/ShowMe/TradingApp=`0x62`,
  Function=`0x64`. See §2e and the UPDATE block at the top of this document.
- **R-M.2.2 (CELProperties):** CELProperties is **NOT an orlib20.dll export** (0
  functions match in `/orlib20.dll`). `CELProperties::GetNotificationWindow` /
  `SetNotificationWindow` are **TSDev.exe imports** (C++ decorated names
  `?GetNotificationWindow@CELProperties@@QAEPAUHWND__@@XZ` /
  `?SetNotificationWindow@CELProperties@@QAEPAUHWND__@@XZ` confirmed in the TSDev.exe
  HINT/NAME table at `0x005ba3ca` / `0x005ba404`) from a **non-orlib20 DLL**.
  Critically, `_SaveModule_New` / `FUN_10159770` **does not reference CELProperties at
  all** — it takes the editor HWND directly as arg4. CELProperties is only used in the
  **verify** path (`FUN_00451800`) to populate the CELCompiler object's `+0x18`
  notification HWND (M.1 finding). **Implementation implication: the standalone CLI
  cannot access CELProperties (not an orlib20 export), but the save path does not need
  it — it only needs the editor HWND, which the CLI already has (`m_editorHwnd`).**
- **R-M.2.3 (save-side encryption):** Save-side internal encryption is triggered by a
  **non-NULL password pointer (arg5)**, NOT by the 0x400 flag alone. In `FUN_101a08b0`
  (the real save+encrypt routine): `if (param_5 != NULL)` → encrypt path
  (`_IsUnicodePassword`, `_GetPasswordSalt`, `_DoEncryption2` on the password then on
  the source body, embed the password into the data header, set master-record `+0x79`
  bit 1 and `+0xde` encryption-type bits); `else` → plain-text save. This **mirrors the
  get-side Branch C** in `FUN_101a0f90` (`_LoadTextFileEX` impl), where non-NULL
  password → extract embedded password + decrypt, NULL → plain copy. ~~The password
  buffer content is **fixed/zeroed** (the salt `_GetPasswordSalt(pw,2)` returns the
  constant `0xe1ca1015` across three different items), i.e. an internal empty
  password, never a user secret.~~ **CORRECTED 2026-08-25 (UPDATE Correction 2):**
  the password buffer is **NOT zeroed in content** — it is zeroed by `memset(buf, 0,
  0x40)` then overwritten by `memcpy(buf, "!p$Kn@aSus", 10)`, where `"!p$Kn@aSus"` is
  the internal TSDev default password string at `0x00567b40` (NOT a user secret). The
  constant salt `0xe1ca1015` and the `_DoEncryption2` length `0xa = 10` both derive
  from this fixed 10-byte string. See §4e and the UPDATE block.
- **R-M.2.4 (pre/post-verify save):** The trace contains **3 verify operations**
  (marker generations 2/3/4). Each has **exactly one `_SaveModule_New` call
  (pre-verify) followed by exactly one `_VerifyModule_New` call** — there is **no
  post-verify save**. Lines: 20412→21682 (progId 6686), 182793→184093 (progId 6687),
  266469→267763 (progId 6688). The CLI's single `saveSource()` before `verifyModule()`
  matches this ordering; a second save is NOT required.
- **R-M.2.5 (no user password):** Confirmed. Any password-shaped buffer passed on save
  must be an **internal fixed buffer mirroring TSDev**, never a user-supplied secret.
  The constant salt across items, `_IsUnicodePassword→0`, and the root-cause doc
  (`RE_UNKNOWN_LINK_ROOT_CAUSE.md` L256: "This is not a user password feature") all
  confirm this is internal encryption, not password protection. **2026-08-25
  correction (UPDATE Correction 2):** the buffer is not "zeroed" in content — it
  contains the internal TSDev default `"!p$Kn@aSus"` (10 bytes at `0x00567b40`), which
  IS the fixed internal material (not a user secret) this bullet asserts. The
  "no user password" conclusion stands; only the "zeroed" characterization was wrong.

---

## 2. R-M.2.1 — Exact arg3 flag values and arg5 value

### 2a. Parameter mapping (1-based arg numbering, matching the task and the CLI call)

The CLI call (`src/tsdev_lib.cpp:1109-1110`):
```cpp
result = m_pSaveModuleNew(getCategory(type), progId, NULL, m_editorHwnd, NULL);
//                           arg1          arg2  arg3   arg4        arg5
```
So **arg3 = flags pointer**, **arg5 = password pointer** (1-based). The Frida script
reads 6 slots (0-based `args[0..5]`); the 6th slot (`args[5]`) is an over-read past the
`@20` frame and is NOT a declared parameter (`tmp/m1_trace_reanalysis.md` §1.1, item 8).

### 2b. Frida trace — the captured TSDev call

`tmp/frida_trace_verify.log` line 20412 (single pre-verify call for progId 6686):
```
00020412 ts=2026-08-22T17:56:52.029Z tid=38376 ENTER call=8630 name="EXPORT:_SaveModule_New@20" args=[0x0,0x1a1e,0x8fdf34,0x162bb6,0x8fdef0,0x1b5f2038]
```
| Arg (1-based) | 0-based slot | Value | Meaning |
|---|---|---|---|
| arg1 category | args[0] | `0x0` | whType 0 (Strategy) |
| arg2 progId | args[1] | `0x1a1e` (6686) | ProgId |
| arg3 flags ptr | args[2] | `0x8fdf34` | **NON-NULL** stack pointer |
| arg4 editorHwnd | args[3] | `0x162bb6` | live editor window (confirmed by `SendMessageW` at line 20422) |
| arg5 password ptr | args[4] | `0x8fdef0` | **NON-NULL** stack pointer |
| (over-read) | args[5] | `0x1b5f2038` | not a parameter |

Two further calls in the same trace confirm the pattern (different progIds, same
buffer addresses for calls #1/#2):
- Line 182793: `args=[0x0,0x1a1f,0x8fdf34,0x132f6e,0x8fdef0,0x1b5f0f30]` (progId 6687)
- Line 266469: `args=[0x0,0x1a20,0x8fdf7c,0x4d2c20,0x8fdf38,0x1b5f0bc8]` (progId 6688)

All three: **arg3 non-NULL, arg5 non-NULL**. The CLI passes NULL for both.

### 2c. Ghidra — wrapper ABI and implementation

`_SaveModule_New@20` at `0x10159A70` (orlib20 ordinal 634, per
`tmp/m1_ghidra_findings.md` §4c) is a thin stdcall `@20` wrapper (disassembly
`0x10159a70`-`0x10159a8c`):
```asm
MOV EDX,[EBP+0xc]    ; EDX = arg2 (progId)
PUSH ECX             ; local 4-byte alloc
PUSH [EBP+0x14]      ; push arg4 (hwnd)
MOV ECX,[EBP+0x8]    ; ECX = arg1 (category)
PUSH [EBP+0x18]      ; push arg5 (password)
PUSH [EBP+0x10]      ; push arg3 (flags)
CALL 0x10159770      ; FUN_10159770(ECX=category, EDX=progId, stack: flags, password, hwnd)
ADD ESP,0x10
RET 0x14             ; stdcall, 20 bytes / 5 args
```
Stack frame: `[EBP+8]`=arg1 category, `[EBP+0xc]`=arg2 progId, `[EBP+0x10]`=arg3 flags,
`[EBP+0x14]`=arg4 hwnd, `[EBP+0x18]`=arg5 password. `RET 0x14` confirms 5 stdcall args
(Ghidra's `__thiscall`/6-param typing is incorrect; the real ABI is stdcall `@20`).

`FUN_10159770` decompiled signature (Ghidra):
```c
undefined4 __fastcall
FUN_10159770(int param_1, undefined4 param_2, uint *param_3, undefined4 param_4, HWND param_5)
//                     category(ECX)  progId(EDX)  flags*(stack)  password(stack)  hwnd(stack)
```
This confirms **param_3 = `uint *flags` (arg3)** and **param_4 = password (arg5)** and
**param_5 = HWND (arg4)**.

### 2d. Flags bitfield decomposition (Ghidra, `FUN_10159770` body)

```c
local_bb  = local_bb  ^ (uVar1 >> 2 ^ local_bb)  & 4;      // uVar1 = (ushort)*param_3
uStack_b9 = uStack_b9 ^ (uVar1 << 5 ^ uStack_b9) & 0x400;
local_2b  = local_2b  ^ ((char)(uVar1 >> 6) << 3 ^ local_2b) & 8;
```
where `uVar1 = (ushort)*param_3` (the low 16 bits of the flags word). Careful
bit-extraction (C precedence: `>>` and `<<` bind tighter than `^`, `&` binds tighter
than `^`):

| Flag bit | Mask | Target | Evidence |
|---|---|---|---|
| bit 4 | `0x10` | `local_bb` bit 2 (`0x4`) | `(uVar1 >> 2) & 4` → uVar1 bit 4 → bit 2 |
| bit 5 | `0x20` | `uStack_b9` bit 10 (`0x400`) — the "internally encrypted" master-record flag | `(uVar1 << 5) & 0x400` → uVar1 bit 5 → bit 10 |
| bit 6 | `0x40` | `local_2b` bit 3 (`0x8`) | `((char)(uVar1 >> 6) << 3) & 8` → uVar1 bit 6 → bit 3 |

The NULL-flags fallback (`FUN_10159770`):
```c
local_14 = 0;
if (param_3 == (uint *)0x0) {
  param_3 = &local_14;
  FUN_10107bf0();
  local_14 = local_14 | 8;   // local_14 = 0x8 (bit 3)
  FUN_10107b70();
}
uVar1 = (ushort)*param_3;    // = 0x8 if NULL was passed
```
When NULL is passed, the effective flags word is `0x8` (bit 3), whose bits 4/5/6 are
all 0 → **none of the three metadata-bit XORs fire**. Bit 3 itself is not consumed by
these extractions. So passing NULL leaves the encryption master-record bit (0x400)
untoggled by the flags path.

### 2e. Exact flag BIT VALUES TSDev passes — ~~EVIDENCE NOT FOUND~~ **RESOLVED 2026-08-25 (see UPDATE block, Correction 1)**

> **SUPERSEDED 2026-08-25.** The flags word is NOT a constant and is NOT
> missing evidence. The TSDev.exe caller `FUN_00452290` (`0x00452290`) was
> located and decompiled; it computes the flags word as a per-item bitfield
> `(type_word & 0x7) | (has_expdate << 3) | (is_verified << 4) |
> (is_protected << 5) | (is_protected_plus << 6)`, with TSDev always forcing
> bits 5 and 6 on. See the **UPDATE 2026-08-25** block at the top of this
> document (Correction 1) for the resolved values per component type. The
> original inferential reasoning below is preserved for provenance — note that
> it correctly identified bit 5 as set, but was incomplete (bit 6 is also
> always set, and the category bits 0-2 are populated, not zeroed).

**Original finding (preserved, SUPERSEDED):**

The Frida trace logs the pointer values but **never dumps the memory at `*0x8fdf34`**
(`tmp/m1_trace_reanalysis.md` §7 item 1: "No compiler-object bytes were dumped"; the
same applies to the flags/password buffers — neither script reads object/buffer
memory). The TSDev.exe caller of `_SaveModule_New@20` was **not located** in this
scope: TSDev.exe does **not statically import** `_SaveModule_New@20`
(`tmp/m1_ghidra_findings.md` §4c), and `get_xrefs_to(0x10159A70)` on `/orlib20.dll`
returns only the export Entry Point + a DATA ref to the export directory — **no
internal orlib20 caller**. The string `_SaveModule_New@20` is present in TSDev.exe at
`0x005bbfa4` inside a `GetProcAddress` HINT/NAME table (alongside
`_IsUnicodePassword@4`, `_GetNumofMasterRecsEX@4`, `ConvertToPowerType`, etc.),
confirming TSDev resolves it dynamically by name via `GetProcAddress`. The TSDev.exe
function that walks this table and builds the flags/password stack buffers was not
identified within this research scope.

**Inferential reasoning (NOT direct evidence):** Because TSDev is saving items that the
get-side confirms are internally encrypted (the 0x400 master-record bit), and because
flag bit 5 (0x20) is the input that sets the 0x400 master-record bit via
`uStack_b9`, TSDev very likely sets flag bit 5 (0x20). Bits 4 and 6 are plausible
additional metadata flags but undetermined. **This must be confirmed by a future Frida
trace that dumps `*(uint*)0x8fdf34` (4 bytes) at the moment `_SaveModule_New@20` is
entered.**

> **2026-08-25 resolution note:** The "future Frida trace" recommended above is no
> longer required — static analysis of `FUN_00452290` resolved the value
> definitively. For a pre-verify Strategy save (the line-20412 case), the value is
> `0x60` (bits 5,6), not the inferred `0x20`.

### 2f. arg5 (password) value — non-NULL, ~~fixed/zeroed buffer~~ **CORRECTED 2026-08-25: contains `"!p$Kn@aSus"`**

> **SUPERSEDED 2026-08-25.** The password buffer is NOT zeroed in content. It
> is zeroed by `memset(buf, 0, 0x40)` and then immediately overwritten by
> `memcpy(buf, "!p$Kn@aSus", 10)` (the internal TSDev default at
> `0x00567b40`). The constant salt `0xe1ca1015` is the salt of this fixed
> password string, not of a zeroed buffer. See the **UPDATE 2026-08-25** block
> (Correction 2) and §4e for the corrected description. The original reasoning
> below is preserved for provenance — its conclusion ("fixed, deterministic,
> not a user secret") is correct; only the "zeroed/empty" characterization of
> the buffer content was wrong.

**Original finding (preserved, partially SUPERSEDED):**

The password pointer `0x8fdef0` is non-NULL. Its content is not dumped, but the
**constant salt** returned by `_GetPasswordSalt@8(0x8fdef0, 2)` across three different
items — `0xe1ca1015` at lines 20436, 182819, and 266495 — proves the password buffer
content is **fixed** (deterministic), i.e. a zeroed/empty internal buffer, not a
per-item or user-supplied secret. See §4 for the full encryption-path evidence.

---

## 3. R-M.2.2 — CELProperties metadata and standalone-CLI accessibility

### 3a. CELProperties is NOT an orlib20 export

`search_functions` on `/orlib20.dll` with `name_pattern="CELProperties"` → **0
functions**. `search_functions` on `/orlib20.dll` with `name_pattern="NotificationWindow"`
→ 0 functions. There is no CELProperties class, vtable, or method in orlib20.dll.

### 3b. CELProperties is a TSDev.exe import from a non-orlib20 DLL

The IAT entry `0x0055ea6c` (which M.1 identified as
`CELProperties::GetNotificationWindow`, `tmp/m1_ghidra_findings.md` §2a/§4a) holds RVA
`0x001ba3ca` → VA `0x005ba3ca`, which points into a HINT/NAME structure. Reading that
memory (`0x005ba3ca`, 80 bytes) yields the decorated import name:
```
?GetNotificationWindow@CELProperties@@QAEPAUHWND__@@XZ
```
= `public: struct HWND__ * __thiscall CELProperties::GetNotificationWindow(void)`.
The adjacent IAT entry `0x0055ea70` → RVA `0x001ba404` → VA `0x005ba404` yields:
```
?SetNotificationWindow@CELProperties@@QAEPAUHWND__@@XZ
```
= `public: struct HWND__ * __thiscall CELProperties::SetNotificationWindow(...)`.
These are **imports** (HINT/NAME table entries), i.e. CELProperties is implemented in a
DLL that TSDev.exe links against. The providing DLL is **not orlib20** (§3a) and was
not positively identified — `list_imports` on `/TSDev.exe` did not surface these
entries (they appear to belong to a delay-load or manually-constructed pointer table
not enumerated by that tool). `search_functions` for `CELProperties`/`NotificationWindow`
in `/receivers/tselc.dll`, `/receivers/ELMDCore.dll`, `/receivers/ECL.dll`,
`/receivers/ELRte.dll`, and `/TSDev.exe` all returned 0. **The exact providing DLL is
EVIDENCE NOT FOUND, but it is definitively NOT orlib20.dll.**

### 3c. `_SaveModule_New` does NOT use CELProperties

Decompilation of `FUN_10159770` (the `_SaveModule_New` implementation) shows it takes
the editor HWND directly as `param_5` and uses it only to: (1) `IsWindow(param_5)`
guard, (2) pass to `FUN_101081c0(param_5=hwnd, ...)` which reads the source text via
`SendMessageW(hwnd, WM_GETTEXTLENGTH/WM_GETTEXT)`, and (3) a possible `MessageBoxA`
warning. **There is no CELProperties reference anywhere in the save path.**

### 3d. CELProperties is only in the verify path (M.1, confirmed)

Per `tmp/m1_ghidra_findings.md` §2a and §4a: in `FUN_00451800` (the verify chain),
TSDev calls `CELProperties::SetNotificationWindow(activeEditView->HWND)` then
`CELProperties::GetNotificationWindow()` to populate the CELCompiler object's `+0x18`
field. This is the **only** CELProperties interaction in the verify chain. No
CELProperties metadata is "set before/after verify" in the save path — the save path
has no CELProperties involvement at all.

### 3e. Answer to the CRITICAL sub-question

**CELProperties is NOT accessible from a standalone CLI that loads only orlib20.dll.**
It is a TSDev.exe-side object (imported from a non-orlib20 TS framework DLL). The
standalone CLI cannot construct or query a CELProperties instance.

**However, this is NOT a blocker for `_SaveModule_New` parity:** the save path does not
use CELProperties. It only needs the editor HWND (arg4), which the CLI already owns
(`m_editorHwnd`, the hidden editor window it creates). The CLI's current arg4
(`m_editorHwnd`) is already correct and matches TSDev's usage
(`tmp/m1_trace_reanalysis.md` §5(a): TSDev's `_SaveModule_New` editorHwnd `0x162bb6`
equals its `_VerifyModule_New` hwnd — the same editor window).

**The only CELProperties-related parity gap is in the VERIFY path** (compiler object
`+0x18` = notification HWND), which is M.1's domain, not M.2's. M.1 already
implemented `+0x18 = m_editorHwnd` (`docs/M1_HANDOFF.md` §2), which is the
standalone-CLI-equivalent of what TSDev obtains via CELProperties::GetNotificationWindow.

---

## 4. R-M.2.3 — Save-side encryption semantics

### 4a. The save chain (Ghidra-confirmed)

```
_SaveModule_New@20(category, progId, flags, hwnd, password)        ; 0x10159A70, stdcall @20
  → FUN_10159770(ECX=category, EDX=progId, flags, password, hwnd)  ; 0x10159770, fastcall
      → FUN_101081c0(hwnd, ..., category, progId, password)        ; 0x101081c0
          SendMessageW(hwnd, 0xe=WM_GETTEXTLENGTH, 0, 0)           ; get source length
          GlobalAlloc(wide + multibyte buffers)
          SendMessageW(hwnd, 0xd=WM_GETTEXT, len+1, lpWideCharStr) ; get source text
          WideCharToMultiByte(...)                                  ; → lpMultiByteStr (ANSI source)
          FUN_101a08b0(warehouse, lpMultiByteStr, category, progId, password)  ; 0x101a08b0
            if (password != NULL)  → ENCRYPT path
            else                   → PLAIN-TEXT path
```
The `FUN_101081c0` → `FUN_101a08b0` call site disassembly (`0x1010824c`-`0x10108259`)
confirms the mapping: `ECX=warehouse`, `EDX=lpMultiByteStr(source)`, stack pushes
`[EBP+0x10]=category`, `[EBP+0x14]=progId`, `[EBP+0x18]=password`; `ADD ESP,0xc`
cleans 3 stack args. So `FUN_101a08b0.param_5 = password = _SaveModule_New.arg5`.

The `SendMessageW(hwnd, 0xe)` / `SendMessageW(hwnd, 0xd)` calls inside
`FUN_101081c0` match the Frida trace exactly: line 20422
(`SendMessageW hwnd=0x162bb6 msg=MSG_0xe` = WM_GETTEXTLENGTH) and line 20425
(`SendMessageW hwnd=0x162bb6 msg=MSG_0xd` = WM_GETTEXT). This is why the CLI must
`setSourceText(m_editorHwnd, source)` before calling `_SaveModule_New` — the save
routine reads the source FROM the editor window.

### 4b. The encrypt path (`FUN_101a08b0`, `if (param_5 != NULL)`)

```c
if (param_5 != (void *)0x0) {
  uVar5 = 2; local_c = 0; local_18 = 2; local_8 = 10;        // ANSI defaults
  iVar2 = _IsUnicodePassword_4(param_5);
  if (iVar2 != 0) { uVar5 = 3; local_c = 1; local_18 = 3; local_8 = 0x3c; }  // unicode
  uVar5 = _GetPasswordSalt_8(param_5, uVar5);                 // derive salt from password
  uVar6 = local_10 + 2 + local_c + local_8;
  pcVar3 = operator_new__(uVar6);                             // alloc output blob
  _DoEncryption2_16(param_5, uVar4, 5, local_18);             // encrypt/derive from password
  *pcVar3 = (char)local_8 + '\x01';                           // header byte
  memcpy(pcVar3 + local_c, param_5, local_8 + 1);             // embed password in header
  _DoEncryption2_16(local_20, local_10, uVar5, 3);            // encrypt source body with salt
  memcpy(pcVar3 + local_c + 1 + local_8, _Src, local_10 + 1); // append encrypted source
  *(ushort*)((int)local_24 + 0x79) |= 2;                      // master record: set bit 1 at +0x79
  *(ushort*)((int)local_24 + 0xde) = ... | (ushort)(local_18 << 0xd); // encryption type at +0xde
  FUN_101d3c00(local_1c, param_4, local_24);                  // write master record
  FUN_101d3ea0(local_1c, param_4, pcVar3, uVar6);             // write encrypted blob
  ...
  return 1;
}
// else (param_5 == NULL): plain-text save
if ((*(ushort*)((int)pvVar1 + 0x79) & 2) == 0) {
  *(ushort*)((int)pvVar1 + 0x79) |= 2;
  FUN_101d3c00(local_1c, param_4, pvVar1);
}
FUN_101d3ea0(local_1c, param_4, local_20, local_10 + 1);      // write PLAIN source
return 1;
```

### 4c. Frida trace confirmation (lines 20433-20440, inside the line-20412 call)

```
00020433 ENTER call=8640 name="EXPORT:_IsUnicodePassword@4" args=[0x8fdef0,...]      → EXIT retval=0x0
00020435 ENTER call=8641 name="EXPORT:_GetPasswordSalt@8"  args=[0x8fdef0,0x2,...]   → EXIT retval=0xe1ca1015
00020437 ENTER call=8642 name="EXPORT:_DoEncryption2@16"   args=[0x8fdef0,0xa,0x5,0x2,...]  → 0x8fdef6
00020439 ENTER call=8643 name="EXPORT:_DoEncryption2@16"   args=[0x19bc7978,0x63,0xe1ca1015,0x3,...] → 0x19bc79d7
```
This is an **exact match** to the Ghidra encrypt path: `_IsUnicodePassword(0x8fdef0)`
→ 0 (ANSI), `_GetPasswordSalt(0x8fdef0, 2)` → `0xe1ca1015`,
`_DoEncryption2(password=0x8fdef0, 0xa=10, 5, 2)`, then
`_DoEncryption2(source=0x19bc7978, len=0x63, salt=0xe1ca1015, 3)`. The identical
sequence recurs at lines 182816-182823 (progId 6687) and 266492-266499 (progId 6688),
each with the **same salt `0xe1ca1015`**.

### 4d. The get-side parallel (Branch C, `FUN_101a0f90`)

`FUN_101a0f90` (the `_LoadTextFileEX` implementation referenced by the CLI
`getSource` comment at `src/tsdev_lib.cpp:1126-1146`):
```c
if (param_5 == (void *)0x0) {
  memcpy(_Dst, _Src, local_8);                       // NULL password → plain copy
} else {
  // Branch C: non-NULL password → decrypt
  memcpy(param_5, (void*)(uVar5 + (int)_Src), local_10 + 1);  // EXTRACT embedded password into param_5
  ... _GetPasswordSalt_8(param_5, ...) ...                     // derive key from extracted password
  FUN_10151880(_Dst, ..., param_4, ...);                       // decrypt source body
}
```
**Mirror symmetry:** On SAVE, a non-NULL password buffer supplies the (empty) password
that gets embedded in the header and used to encrypt. On GET, a non-NULL password
buffer is **overwritten** with the embedded password extracted from the header, then
used to decrypt. This is why the get-side CLI comment is correct that "any non-NULL
pointer works" for decryption (the buffer is overwritten), while on the save-side the
buffer content **is** used (it becomes the embedded password) — hence it must be the
same fixed/zeroed buffer TSDev uses, to produce a decryptable artifact.

### 4e. Answer to R-M.2.3

> **CORRECTED 2026-08-25.** The original answer below stated the password
> buffer is "a fixed/zeroed internal buffer." That characterization of the
> buffer **content** was WRONG. The buffer is zeroed by `memset(buf, 0, 0x40)`
> and then immediately overwritten by `memcpy(buf, password_string, length)`.
> For unprotected items (the TSDev-forced-protection path), the password
> string is the **internal TSDev default `"!p$Kn@aSus"`** (10 bytes at
> `0x00567b40` in `TSDev.exe`). The constant salt `0xe1ca1015` is the salt of
> this fixed password string, and the `_DoEncryption2` length `0xa = 10`
> (Frida trace line 20437) matches the 10-byte length of `"!p$Kn@aSus"`
> exactly. The original answer's structural claims (encryption is triggered by
> non-NULL arg5; both non-NULL arg3 and non-NULL arg5 are needed for full
> parity; the buffer is fixed/deterministic, not a user secret) remain
> correct. Only the "zeroed" characterization of the buffer content was
> wrong. See the **UPDATE 2026-08-25** block (Correction 2) for full details.

**Corrected answer:**

Save-side internal encryption is triggered by a **non-NULL password pointer (arg5)**.
It is NOT controlled by the 0x400 flag alone: the 0x400 master-record bit is set by
flag bit 5 (arg3) in `FUN_10159770` (metadata path), while the actual
encryption/ciphertext production is triggered by `param_5 != NULL` in `FUN_101a08b0`.
**Both are needed for full parity:** a non-NULL password buffer to perform the
encryption, and the correct flags word (with bit 5 set — see §2e / UPDATE Correction
1) to mark the master record as encrypted. TSDev passes both non-NULL.

The password buffer is a **fixed internal buffer mirroring TSDev**, but its content is
**NOT zeroes** — it is the internal TSDev default password string `"!p$Kn@aSus"` (10
bytes), written into a 64-byte stack buffer that is first zeroed then overwritten by
`memcpy`. The constant salt `0xe1ca1015` is the salt of this fixed password string.
This mirrors the get-side `FUN_10108630` 64-byte buffer pattern in **size and
stack-allocation shape**, but the save-side buffer's leading 10 bytes hold
`"!p$Kn@aSus"`, not zeroes. **`"!p$Kn@aSus"` is an internal TSDev default, NOT a user
secret** — it is hardcoded in `TSDev.exe` at `0x00567b40` and used as the encryption
key material for items TSDev force-protects. The CLI must replicate this fixed
internal default string, never accept user input as the password.

**Original answer (preserved, partially SUPERSEDED as noted above):**

Save-side internal encryption is triggered by a **non-NULL password pointer (arg5)**.
It is NOT controlled by the 0x400 flag alone: the 0x400 master-record bit is set by
flag bit 5 (arg3) in `FUN_10159770` (metadata path), while the actual
encryption/ciphertext production is triggered by `param_5 != NULL` in `FUN_101a08b0`.
**Both are needed for full parity:** a non-NULL zeroed password buffer to perform the
encryption, and (inferentially) flag bit 5 set to mark the master record as encrypted.
TSDev passes both non-NULL. The password buffer is a fixed/zeroed internal buffer
(constant salt `0xe1ca1015`), mirroring the get-side `FUN_10108630` zeroed 64-byte
buffer pattern.

---

## 5. R-M.2.4 — Pre-verify vs post-verify save

The trace contains 3 verify operations (marker generations 2, 3, 4;
`tmp/frida_trace_verify.log` MARKER lines 12900, 173923, 247376). For each, the
`_SaveModule_New` ENTER precedes the `_VerifyModule_New` ENTER, and there is exactly
one of each per operation:

| Gen | SaveModule ENTER | VerifyModule ENTER | progId | Save→Verify |
|---|---|---|---|---|
| 2 (BEGIN_VERIFY) | line 20412 | line 21682 | 6686 (`0x1a1e`) | pre-verify only |
| 3 (BEGIN_REVERIFY_CLI) | line 182793 | line 184093 | 6687 (`0x1a1f`) | pre-verify only |
| 4 (BEGIN_ARTIFACT_VERIFY) | line 266469 | line 267763 | 6688 (`0x1a20`) | pre-verify only |

There is **no second `_SaveModule_New` call after any `_VerifyModule_New` call** for
the same item. The root-cause doc confirms the ordering
(`RE_UNKNOWN_LINK_ROOT_CAUSE.md` L47): "The UI path saves before compiling:
`_SaveModule_New@20` enters/returns success at trace L20412... `StartCompilingProcess`
follows at L21662, then `_VerifyModule_New@20` at L21682."

**Answer:** Only a pre-verify save is performed. The CLI's single `saveSource()` before
`verifyModule()` matches TSDev. **No post-verify save is required.**

---

## 6. R-M.2.5 — No user-facing password parameter

Evidence that the password buffer is an internal zeroed buffer, never a user secret:

1. **Constant salt across items:** `_GetPasswordSalt@8(passwordBuf, 2)` returns
   `0xe1ca1015` for progIds 6686, 6687, and 6688 (lines 20436, 182819, 266495). A
   user-supplied or per-item password would produce varying salts. Constant salt ⇒
   fixed (zeroed/empty) password content.
2. **`_IsUnicodePassword` returns 0** in all three calls (lines 20434, 182817, 266493)
   ⇒ the buffer is an empty/zeroed ANSI buffer, not a unicode password string.
3. **Get-side parallel:** the CLI `getSource` (`src/tsdev_lib.cpp:1147`) uses a zeroed
   `char decryptBuffer[64] = {};` and the comment (L1144-1146) states "TSDev does the
   same (`FUN_10108630` uses a zeroed 64-byte buffer)."
4. **Root-cause doc:** `RE_UNKNOWN_LINK_ROOT_CAUSE.md` L256 — "SaveModule parity:
   reproduce TSDev's non-NULL flags, internal metadata/encryption buffer, and
   CELProperties state after parameter semantics are confirmed. **This is not a user
   password feature** and is not the link fix."
5. **Save-side semantics (§4b):** the password buffer content is embedded into the
   encrypted data header and used to derive the encryption key. For the artifact to be
   decryptable by the same internal path on get-side, the password must be the same
   fixed empty buffer TSDev uses — a user secret would break the internal
   encrypt/decrypt round-trip.

**Answer:** Confirmed. No user-facing password parameter is needed or appropriate. Any
password-shaped buffer passed to `_SaveModule_New` arg5 must be an internal zeroed
buffer mirroring TSDev, never a user-supplied secret.

---

## 7. Implementation recommendations

### 7a. Changes indicated for `src/tsdev_lib.cpp:1109-1110` (`saveSource`)

> **UPDATED 2026-08-25.** The code example below reflects the resolved flags
> computation (UPDATE Correction 1) and the corrected password buffer content
> (UPDATE Correction 2). The earlier inferential `0x20` flags value and the
> "zeroed password buffer" characterization are both superseded.

Current:
```cpp
result = m_pSaveModuleNew(getCategory(type), progId, NULL, m_editorHwnd, NULL);
```
Indicated (resolved 2026-08-25 — no longer inferential):
```cpp
// arg3 (flags): non-NULL pointer to a flags word computed exactly as TSDev's
//   FUN_00452290 does. Bitfield (UPDATE 2026-08-25, Correction 1):
//     bits 0-2 = (type_word & 0x7)            // component category
//     bit 3    = has expiration date          // 0 for new/pre-verify saves
//     bit 4    = is verified                  // 0 for new saves, 1 for re-saves
//     bit 5    = is protected                 // TSDev ALWAYS forces this on
//     bit 6    = is protected-plus            // TSDev ALWAYS forces this on
//     bits 7-15 = 0 (cleared by 0x3f mask)
//   TSDev forces bits 5 and 6 on unconditionally (FUN_00452290 else-branch
//   calls SetProtected(true) + SetProtectedPlus(true)). For a new pre-verify
//   save: Strategy=0x60, Indicator/ShowMe/TradingApp=0x62, Function=0x64.
//   For a re-save of an already-verified item (bit 4 set): add 0x10.
unsigned int saveFlags =
    (static_cast<unsigned int>(getCategory(type)) & 0x7u)  // category bits 0-2
  | 0x60u;                                                  // bits 5,6 always on
//   (bit 4 = is_verified is NOT set for a brand-new pre-verify save; set it
//   only when re-saving an already-verified item — see UPDATE Correction 1.)
// arg5 (password): non-NULL 64-byte buffer mirroring TSDev. The buffer is
//   first zeroed (memset 0, 0x40) then overwritten with the internal TSDev
//   default password string "!p$Kn@aSus" (10 bytes, at TSDev.exe 0x00567b40).
//   This fixed string is the encryption key material TSDev uses for items it
//   force-protects; it is NOT a user secret and NOT user input. The constant
//   salt 0xe1ca1015 and the _DoEncryption2 length 0xa=10 both derive from
//   this string. NEVER accept user input as this password.
char savePasswordBuf[64] = {};
static const char kTsdevDefaultPassword[] = "!p$Kn@aSus";  // internal default, NOT a user secret
std::memcpy(savePasswordBuf, kTsdevDefaultPassword,
            sizeof(kTsdevDefaultPassword) - 1);  // copy 10 bytes, no NUL
result = m_pSaveModuleNew(getCategory(type), progId, &saveFlags, m_editorHwnd, savePasswordBuf);
```
**arg4 (editorHwnd) is already correct** (`m_editorHwnd`) — no change.

> **Note on `getCategory(type) & 0x7`:** the category value passed as arg1 is
> the same value used for the flags bits 0-2. The CLI's existing
> `getCategory(type)` already returns the correct category for arg1; reusing it
> for the flags bits 0-2 matches `FUN_00452290`'s `(type_word & 0x7)`. Confirm
> at implementation time that `getCategory(type)` for ShowMe (whType 1282) and
> TradingApp (whType 5122) returns 2 (the category bits TSDev uses for those
> types), not the raw whType — see UPDATE Correction 1 table.

### 7b. Achievable from the standalone CLI vs. TSDev.exe-internal (limitations)

| Concern | Achievable from CLI? | Evidence |
|---|---|---|
| Non-NULL arg5 (password buffer with `"!p$Kn@aSus"`) → trigger internal encryption | **YES** | §4 — `FUN_101a08b0` encrypt path is gated only on `param_5 != NULL`; buffer is a local 64-byte array initialized with the fixed internal default string (UPDATE Correction 2) |
| Non-NULL arg3 (flags) → set master-record metadata bits | **YES (fully resolved 2026-08-25)** | §2d + UPDATE Correction 1 — both the bit decomposition AND the exact value are now known (computed by `FUN_00452290`); pre-verify values: Strategy=0x60, Indicator/ShowMe/TradingApp=0x62, Function=0x64 |
| Editor HWND (arg4) | **YES (already done)** | §3e — CLI already passes `m_editorHwnd` |
| Reading source from the editor HWND via WM_GETTEXT | **YES (already done)** | §4a — CLI already `setSourceText(m_editorHwnd, source)` before save |
| CELProperties metadata | **NO — TSDev.exe-internal, NOT a blocker** | §3 — CELProperties is not an orlib20 export; the save path does not use it |

### 7c. Documented limitations (must be carried into the implementation TVS)

> **UPDATED 2026-08-25.** Limitations #1 and #3 are now RESOLVED by the
> targeted static analysis of `FUN_00452290` (see UPDATE block at the top of
> this document). They are preserved below as historical context, marked
> RESOLVED. Limitation #2 (CELProperties) is unchanged — it was always a
> non-blocker for the save path.

1. **~~Exact flags bit values: EVIDENCE NOT FOUND.~~** **RESOLVED 2026-08-25
   (UPDATE Correction 1).** The flags word is computed by `FUN_00452290` as
   `(type_word & 0x7) | (has_expdate << 3) | (is_verified << 4) |
   (is_protected << 5) | (is_protected_plus << 6)`, with TSDev always forcing
   bits 5 and 6 on. Pre-verify values per component type are tabulated in the
   UPDATE block. The implementation should use the resolved computation
   (§7a), not the earlier inferential `0x20`. **Original (preserved,
   RESOLVED):** The implementation must either (a) confirm the value via a
   new Frida trace that dumps `*(uint*)flagsBuf` at `_SaveModule_New` entry
   (recommended; let the PM decide per the no-new-Frida rule), or (b) use the
   inferential `0x20` and document it as an unconfirmed-but-reasoned value
   with a code comment naming the source of the inference. **Neither (a) nor
   (b) is needed anymore — static analysis resolved the value.**
2. **CELProperties is not accessible from the standalone CLI.** This is documented as a
   non-blocker for the save path (which does not use CELProperties). The only
   CELProperties-related parity is in the verify path (compiler `+0x18`), already
   handled by M.1 via `m_editorHwnd`.
3. **~~The TSDev.exe caller of `_SaveModule_New` (GetProcAddress-based) was not
   located~~** — **RESOLVED 2026-08-25 (UPDATE, "New finding").** The unique
   caller is `FUN_00452290` at `0x00452290`, via IAT slot `0x005602c4`. Its
   decompilation resolved limitations #1 and the password-buffer content
   question (Correction 2). **Original (preserved, RESOLVED):** this was the
   root cause of limitation #1.

### 7d. Additional Frida data that would close the residual gap (for PM decision)

> **UPDATED 2026-08-25.** All three bullets below are now **RESOLVED by static
> analysis of `FUN_00452290`** (UPDATE block). The flags word value, the
> password buffer content (`"!p$Kn@aSus"`), and the TSDev.exe caller
> (`FUN_00452290` at `0x00452290`, via IAT slot `0x005602c4`) are all known. A
> dynamic Frida confirmation is no longer required for parity, though it
> remains available as an optional cross-check if the PM desires runtime
> confirmation of the static findings. The original recommendation is
> preserved below for provenance.

**Original recommendation (preserved, RESOLVED):**

A targeted Frida hook on `_SaveModule_New@20` entry that reads, for one TSDev UI save:
- `*(uint*)args[2]` (4 bytes at the flags buffer) — to get the exact flags word;
- `args[4]` and the first ~16 bytes at the password buffer — to confirm it is zeroed;
- a backtrace of the `_SaveModule_New` caller — to locate the TSDev.exe save function
  that builds these buffers.

Per the task rules, no new Frida sessions were run; this is recorded as a
recommendation for the PM to decide on.

---

## 8. Evidence index

### 8a. Frida trace (`tmp/frida_trace_verify.log`)
- L20412 — `_SaveModule_New@20` ENTER, args (progId 6686): the primary captured call.
- L20422, L20425 — `SendMessageW(hwnd=0x162bb6, 0xe/0xd)` inside the save (WM_GETTEXT).
- L20433-20440 — `_IsUnicodePassword`/`_GetPasswordSalt`/`_DoEncryption2`×2 (encrypt path).
- L20436 — `_GetPasswordSalt` retval `0xe1ca1015` (constant salt).
- L20611 — `_SaveModule_New@20` EXIT retval `0x1`.
- L21682 — `_VerifyModule_New@20` ENTER (progId 6686, same hwnd `0x162bb6`).
- L182793, L182816-182823 — 2nd save (progId 6687) + its encrypt path (same salt).
- L266469, L266492-266499 — 3rd save (progId 6688) + its encrypt path (same salt).
- L12900, L173923, L247376 — MARKER BEGIN for gens 2/3/4.
- L184093, L267763 — VerifyModule ENTER for progIds 6687/6688.

### 8b. Frida script (`tmp/frida_trace_verify.js`)
- L14-22 — INTERNALS hooked (does NOT include `_SaveModule_New`; it is hooked as an
  EXPORT via `enumerateExports`, L142-146).
- L87-93 — `formatArgs(args, 6)` always reads 6 slots (the 6th is an over-read).
- L114-131 — `hookOrlibFunction` (ENTER/EXIT logging; backtrace only for first
  VerifyModule, L120-124 — NOT for SaveModule).

### 8c. Existing RE notes
- `tmp/m1_ghidra_findings.md` §2a/§4a — CELProperties::Get/SetNotificationWindow in
  `FUN_00451800` (verify path); IAT `[0x0055ea6c]`.
- `tmp/m1_ghidra_findings.md` §4c — `_SaveModule_New@20` (ordinal 634, RVA 0x159A70)
  is NOT statically imported by TSDev.exe.
- `tmp/m1_trace_reanalysis.md` §2 — single SaveModule call table; §5(a) editorHwnd ==
  VerifyModule hwnd; §7 item 1 — no buffer memory dumped.
- `docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md` L47 — save-before-compile ordering; L98 —
  save ordering parity; L256 — "SaveModule parity... not a user password feature";
  L279 — SaveModule flags/CELProperties listed as deferred parity item.
- `docs/M1_HANDOFF.md` §1 (R-M.1.4) — `+0x18` = notification HWND; TSDev does not
  import `_SaveModule_New` into the automation path; CLI uses `m_editorHwnd`.
- `src/tsdev_lib.cpp:1103-1115` — current `saveSource` (NULL arg3/arg5).
- `src/tsdev_lib.cpp:1122-1165` — get-side Branch C (zeroed 64-byte buffer).

### 8d. Ghidra static evidence (`ghidra-mcp`, `TSDevAnalysis` project)
- `/orlib20.dll` `0x10159A70` — `_SaveModule_New@20` wrapper (disasm + decompile);
  `RET 0x14` = stdcall @20 (5 args).
- `/orlib20.dll` `0x10159770` — `FUN_10159770` (impl): `uint *param_3` flags,
  `param_4` password, `HWND param_5`; flags bitfield (bit4→meta2, bit5→meta0x400,
  bit6→meta3); NULL-flags fallback `local_14=0x8`.
- `/orlib20.dll` `0x101081c0` — `FUN_101081c0`: reads source via
  `SendMessageW(hwnd,0xe/0xd)` + `WideCharToMultiByte`; calls `FUN_101a08b0`
  (disasm `0x1010824c`-`0x10108259` confirms ECX=warehouse, EDX=source, stack:
  category/progId/password).
- `/orlib20.dll` `0x101a08b0` — `FUN_101a08b0`: `if (param_5 != NULL)` encrypt path
  (`_IsUnicodePassword`/`_GetPasswordSalt`/`_DoEncryption2`×2, embed password, set
  master `+0x79`/`+0xde`); else plain-text save.
- `/orlib20.dll` `0x101a0f90` — `FUN_101a0f90` (get-side `_LoadTextFileEX` impl):
  `param_5 == NULL` → plain copy; else Branch C (extract embedded password + decrypt).
- `/orlib20.dll` `0x10107bf0` — `FUN_10107bf0` (NULL-flags-path callee; `IsWindow` +
  `SendMessageW(0x40f)`; not security-relevant).
- `get_xrefs_to(0x10159A70)` on `/orlib20.dll` — only Entry Point + DATA ref
  (`0x1021d978`); **no internal orlib20 caller**.
- `/TSDev.exe` `0x005bbfa4` — string `_SaveModule_New@20` (inside a GetProcAddress
  HINT/NAME table at `~0x005bbf80`, alongside `_IsUnicodePassword@4`,
  `_GetNumofMasterRecsEX@4`, `ConvertToPowerType`, `GetSymbolFromReferenceTable`).
- `/TSDev.exe` `0x0055ea6c`/`0x0055ea70` — IAT entries → HINT/NAME at `0x005ba3ca`/
  `0x005ba404` = `?GetNotificationWindow@CELProperties@@QAEPAUHWND__@@XZ` /
  `?SetNotificationWindow@CELProperties@@QAEPAUHWND__@@XZ` (imports, non-orlib20).
- `search_functions` `/orlib20.dll` `CELProperties` → 0; `NotificationWindow` → 0.
- `search_functions` `/TSDev.exe`, `/receivers/tselc.dll`, `/receivers/ELMDCore.dll`,
  `/receivers/ECL.dll`, `/receivers/ELRte.dll` `CELProperties`/`NotificationWindow` → 0.

**New evidence (2026-08-25 targeted static analysis — UPDATE block):**
- `/TSDev.exe` `0x00452290` — `FUN_00452290`: the **unique TSDev.exe caller** of
  `_SaveModule_New@20`. Builds the flags word (arg3) and the password buffer (arg5)
  on its stack before the call. Decompilation resolves R-M.2.1 §2e (flags value) and
  R-M.2.3 §4e (password buffer content). Disassembly of the flags-computation and
  forced-protection sequence: `0x004523a4`-`0x00452422`.
- `/TSDev.exe` `0x005602c4` — IAT slot holding the `GetProcAddress`-resolved address
  of `_SaveModule_New@20`. `get_xrefs_to` on this slot identifies `FUN_00452290` as
  the sole caller (resolves §9 item 4).
- `/TSDev.exe` `0x00567b40` — the **internal default password string `"!p$Kn@aSus"`**
  (10 ANSI bytes, NUL-terminated). Referenced by `FUN_00452290`'s forced-protection
  `else` branch (when `IsProtected()` returns false) and copied into the password
  buffer via `memcpy`. This is the source of the constant salt `0xe1ca1015` and the
  `_DoEncryption2` length `0xa = 10` observed in the Frida trace. **Internal TSDev
  default, NOT a user secret.**
- `/TSDev.exe` `FUN_0044f5b0` — callee used to compute flags bit 3 (has expiration
  date); returns 0 for new pre-verify saves.
- `/TSDev.exe` `FUN_0044f720` — callee used to compute flags bit 4 (is verified);
  returns 0 for new pre-verify saves, 1 for re-saves of verified items.
- `/TSDev.exe` `IsProtected()` / `SetProtected(bool)` / `IsProtectedPlus()` /
  `SetProtectedPlus(bool)` — CELProperties methods invoked by `FUN_00452290`'s
  forced-protection branch. TSDev unconditionally calls `SetProtected(true)` +
  `SetProtectedPlus(true)` when `IsProtected()` is false, so flags bits 5 and 6 are
  always set for items saved through TSDev.

### 8e. L8 trace (`tmp/l8_fault_a_trace/`)
- `frida_trace_l8_tsdev.log` L16-20, L176-181 — `IAT_SaveModule` =
  `elmdcore:MDVault_SaveModule` (a DIFFERENT function from `_SaveModule_New@20`;
  deduped). `_SaveModule_New@20` was NOT hooked in the L8 script
  (`tmp/m1_trace_reanalysis.md` §6.1). No additional `_SaveModule_New` data.

---

## 9. Explicit EVIDENCE NOT FOUND statements

> **UPDATED 2026-08-25.** Items 1 and 4 are now **RESOLVED** by the targeted
> static analysis of `FUN_00452290` (see UPDATE block at the top of this
> document). They are preserved below as historical context, marked RESOLVED,
> for provenance. Items 2 and 3 remain as stated (item 2's "zeroed" inference
> is corrected by UPDATE Correction 2 — the buffer contains `"!p$Kn@aSus"`,
> not zeroes, so the buffer content is no longer "not found" but rather "was
> inferred incorrectly"; see §4e. Item 3 on the CELProperties providing DLL
> remains genuinely unresolved, but is a non-blocker per §3).

1. **~~Exact bit values of the TSDev flags word `*0x8fdf34` (arg3).~~**
   **RESOLVED 2026-08-25 (UPDATE Correction 1).** The flags word is computed
   by `FUN_00452290` (`0x00452290`) as
   `(type_word & 0x7) | (has_expdate << 3) | (is_verified << 4) |
   (is_protected << 5) | (is_protected_plus << 6)`, with TSDev always forcing
   bits 5 and 6 on. Pre-verify values: Strategy=`0x60`,
   Indicator/ShowMe/TradingApp=`0x62`, Function=`0x64`. Re-verify values add
   `0x10` (bit 4). Evidence: `FUN_00452290` decompilation, disassembly
   `0x004523a4`-`0x00452422`, IAT slot `0x005602c4` xref. **Original
   (preserved, RESOLVED):** The trace does not dump that memory; the TSDev.exe
   caller was not located. Only the bitfield decomposition (bits 4/5/6) is
   confirmed from Ghidra; the value is inferentially `0x20` (bit 5) but NOT
   directly observed. **The inferred `0x20` was incomplete — the resolved
   pre-verify Strategy value is `0x60` (bits 5,6), not `0x20`.**
2. **Exact byte content of the TSDev password buffer `*0x8fdef0` (arg5).** Not
   dumped. Inferred zeroed/fixed from the constant salt `0xe1ca1015` and
   `_IsUnicodePassword→0`. **CORRECTED 2026-08-25 (UPDATE Correction 2):** the
   inference "zeroed" was wrong — the buffer contains the internal TSDev
   default password `"!p$Kn@aSus"` (10 bytes at `0x00567b40`), copied in after
   a `memset(buf, 0, 0x40)`. The salt `0xe1ca1015` and the `_DoEncryption2`
   length `0xa = 10` both derive from this string. See §4e.
3. **The specific DLL that exports `CELProperties::Get/SetNotificationWindow`.**
   Confirmed NOT orlib20; not positively identified among the loaded receivers
   (`list_imports` did not surface the entry; likely delay-load/manual table).
4. **~~The TSDev.exe function that calls `_SaveModule_New@20` (the
   GetProcAddress-based caller that builds the flags/password buffers).~~**
   **RESOLVED 2026-08-25 (UPDATE, "New finding").** The unique caller is
   `FUN_00452290` at `0x00452290`, via IAT slot `0x005602c4`. Its
   decompilation resolved items 1 and 2 above. **Original (preserved,
   RESOLVED):** Not located in this scope.
