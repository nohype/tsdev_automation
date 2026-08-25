# RE_M3 — `0x40A` Post-Success Library-Change Notification

> **Status:** Research-only findings note for Workstream M, slice M.3.
> **Scope:** Reverse-engineering of the `0x40A` notification emitted after successful
> TradeStation library mutations. No production or test code was changed for this task.
> **Method:** Ghidra static analysis of `TSDev.exe`, `orlib20.dll`, `tslib.dll` (import
> tables), and `orchart.exe`; plus an existing Frida trace at
> `D:\tsdev_automation\tmp\frida_trace_verify.log` (captured 2026-08-22, hooks on
> `SendMessageW`/`PostMessageW`/`SendMessageA`/`PostMessageA`).
> **Binaries analyzed (read-only; copies only where required):**
> `C:\Program Files (x86)\TradeStation 9.5\Program\TSDev.exe`,
> `...\orlib20.dll`, `...\tslib.dll`, `...\orchart.exe`.

---

## 0. Executive Summary

`0x40A` is a **private Win32 window message** used by `orlib20.dll` to notify live
TradeStation consumers that a library module changed. It is **not** a durable queue and
**not** the unknown-link repair (that is Workstream L).

Key conclusions, all confirmed by both static and dynamic evidence:

1. **Sender = `orlib20.dll`** (the library layer), **not** `TSDev.exe`. `TSDev.exe`
   contains **no** `PUSH 0x40A` instruction; it is a *receiver* of `0x40A`.
2. **Delivery = `PostMessageW` to a registered set of client HWNDs** (the
   `tslib::EventsViaHandles` client set). It is **not** a broadcast and **not** directed
   to a single hard-coded HWND.
3. **Payload:** `msg = 0x40A`, `wParam = 0`, `lParam = (category << 16) | (ProgId & 0xFFFF)`.
   The **low word of `lParam` IS the ProgId** (a.k.a. warehouse record index — the two are
   the same identifier in the orlib20 warehouse). The Frida trace confirms `wParam=0` and a
   packed `lParam` whose low word equals the affected module's ProgId: `0x1a1e`=6686,
   `0x1a1f`=6687, `0x1a20`=6688 (cross-referenced with `docs/PM_WORKING_MEMORY.md` line 217,
   "TS_AUTO_CLI_LINK_REPRO_01 ProgId 6687"). The same value `0x1a1e` is the record-index
   argument to `InitGlobalLookup` and the return value of `_CreateEntryEX@24` in the trace
   (line 783), proving the low word is the record index = ProgId, not the subtype.
4. **Verify-success emits `0x40A`** — confirmed by Frida: immediately after
   `_VerifyModule_New@20` returns success, `EndCompilingProcess` and
   `ShowRecalculationWarning` run, then a burst of `PostMessageW(0x40A)` is sent to every
   registered client HWND.
5. **Delete emits `0x40A`** after successful deletion (gated on category).
6. The M.3 plan wording "category/ProgId" is **correct**: the payload packs category (high
   word) and ProgId (low word). The earlier draft of this note incorrectly labeled the low
   word "subtype" and claimed ProgId was absent; both are retracted (see §1.1 and the
   Evidence Index).

---

## 1. R-M.3.1 — Message Format and Target Mechanism

### 1.1 Message ID and payload (CONFIRMED)

| Field    | Value                                  | Evidence |
|----------|----------------------------------------|----------|
| `msg`    | `0x040A` (1034)                        | `PUSH 0x40A` at 4 sites in `orlib20.dll`; Frida `msg_id=0x40a` |
| `wParam` | `0`                                    | `PUSH 0` before `PUSH 0x40A`; Frida `wParam=0x0` |
| `lParam` | `(category << 16) \| (ProgId & 0xFFFF)` | `MOVZX ECX,word[EBP+8]; MOVZX EAX,word[EBP+0xC]; SHL ECX,0x10; OR ECX,EAX; PUSH ECX`; Frida `lParam=0x1a1e/0x1a1f/0x1a20` |

**The low word of `lParam` IS the ProgId** (a.k.a. warehouse record index — "record index"
and "ProgId" refer to the same warehouse identifier; `_CreateEntryEX@24` returns it and
`InitGlobalLookup` accepts it as its record-index argument). The payload packs two 16-bit
fields: **category** (high word) and **ProgId** (low word). This is confirmed by three
converging evidence lines:

1. **Frida trace** (`tmp/frida_trace_verify.log`): the `0x40A` posts carry
   `lParam=0x1a1e/0x1a1f/0x1a20`. Decimal: 6686/6687/6688. `docs/PM_WORKING_MEMORY.md`
   line 217 records "TS_AUTO_CLI_LINK_REPRO_01 ProgId 6687" (= `0x1a1f`), and line 243
   lists the retained test ProgIds 6686/6687/6688/6689. Strategy subtype is 0 (per
   `docs/TS_COMPONENT_TYPES.md`: Strategy = category 0, subtype 0), so if the low word
   were subtype it would be 0, not 6687. The low word is therefore the ProgId.
2. **Frida trace identity**: the same value `0x1a1e` is (a) the return value of
   `_CreateEntryEX@24` (trace line 783, `retval=0x1a1e` — the record index assigned by
   CreateEntry), (b) the record-index argument to `InitGlobalLookup` (trace lines 751,
   753, 767, 769, …), and (c) the `lParam` low word of the `0x40A` posts. The record
   index = ProgId, so the low word = ProgId.
3. **Ghidra static evidence**: `FUN_00453f70` (the TSDev verify-success handler) calls
   `Ordinal_57(*(ushort*)(param_1+0x1e8), *(ushort*)(param_1+0x1ec))` (decompilation of
   `FUN_00453f70`). In the Build > Verify command handler `FUN_00451800`, the CELCompiler
   object (`Ordinal_66()`) is configured as `piVar10[2]=*(ushort*)(param_1+0x1e8)`
   (category), `piVar10[3]=*(ushort*)(param_1+0x1ea)` (analysis subtype),
   `piVar10[4]=*(ushort*)(param_1+0x1ec)` (ProgId). This matches the CELCompiler field
   layout recorded in `docs/PM_WORKING_MEMORY.md` line 222 ("+0x08=category, +0x0C=subtype,
   +0x10=current ProgId"). Therefore `param_1+0x1ec` is the **ProgId** field, and it is
   the value passed as the second (low-word) argument to `Ordinal_57`. The earlier draft's
   "subtype" label for `+0x1ec` is retracted.

### 1.2 Canonical sender — `FUN_101526f0` (asynchronous, the one TSDev uses)

`orlib20.dll` export, **ordinal 57** (ordinal-only export, no friendly name). Disassembly
at `0x101526f0`:

```asm
PUSH EBP
MOV  EBP, ESP
MOVZX ECX, word ptr [EBP+8]    ; param_1 = category (16-bit)
MOVZX EAX, word ptr [EBP+0xC]  ; param_2 = ProgId (16-bit, warehouse record index)
SHL  ECX, 0x10
OR   ECX, EAX                  ; lParam = (category<<16)|ProgId
PUSH ECX                       ; lParam
PUSH 0x0                       ; wParam
PUSH 0x40A                     ; msg
MOV  ECX, 0x10231050           ; this = EventsViaHandles singleton (PTR_DAT_10231050)
CALL [0x101fa054]              ; tslib!PosMessageToClients
MOV  EAX, 1                    ; always returns 1
POP  EBP
RET  8                         ; stdcall, 2 dword args
```

**Ordinal mapping (Ghidra-confirmed):** Ghidra's decompilation of `FUN_101526f0` carries
the export-ordinal annotation `/* 0x1526f0  57   */` (read from the PE export directory
ordinal field; the export is ordinal-only and auto-named `FUN_101526f0`). The export entry
point at `0x101526f0` is confirmed as an EXTERNAL Entry Point xref, and the export-address
table slot at `0x1021d074` (DATA xref) points to `0x101526f0`. The TSDev IAT thunk at
`0x0053f56e` is named `Ordinal_57` (Ghidra `get_function_by_address` on `0x0053f56e` →
`name="Ordinal_57"`), i.e. TSDev imports orlib20 ordinal 57, which resolves to `0x101526f0`.
The two TSDev call sites of this thunk are `FUN_00453f70` (verify-success, xref from
`0x00453f89`) and `FUN_0044cab0` (rename, xref from `0x0044cd8a`).

The call target `[0x101fa054]` resolves to the tslib import
`?PosMessageToClients@EventsViaHandles@tslib@@QAEXIIJ@Z`
(`void EventsViaHandles::PosMessageToClients(unsigned, unsigned, long)`).

### 1.3 Synchronous variant — `FUN_101526c0` (ordinal 65)

Identical packing, but calls `?SendMessageToClients@EventsViaHandles@tslib@@...`
(`[0x101fa058]`) — the **synchronous** `SendMessage`-based variant. **Ordinal 65**
(Ghidra-confirmed: decompilation of `FUN_101526c0` carries the export-ordinal annotation
`/* 0x1526c0  65   */`; export entry point at `0x101526c0` is an EXTERNAL Entry Point xref;
export-address table slot at `0x1021d094` (DATA xref) points to `0x101526c0`).
**TSDev.exe does not import ordinal 65**, so TSDev never uses the synchronous sender.
The synchronous variant exists for other orlib20 consumers.

### 1.4 Delivery target — registered HWND set (CONFIRMED)

Delivery is to the **registered client HWND set** maintained by
`tslib::EventsViaHandles`, **not** a broadcast and **not** a single fixed HWND.

Registration API chain:

- `orlib20.dll` exports `_OrlibRegisterNotificationHandle@4` (Ghidra-confirmed: export at
  `0x1011c130`, **ordinal 595**; decompilation carries the annotation
  `/* 0x11c130  595  _OrlibRegisterNotificationHandle@4 */` and calls
  `tslib::EventsViaHandles::RegisterHandle((EventsViaHandles *)&PTR_DAT_10231050, param_1)`,
  i.e. `bool EventsViaHandles::RegisterHandle(HWND*)`).
- The matching unregister is `_OrlibUnregisterNotificationHandle@4` (Ghidra-confirmed:
  export at `0x1011c140`, **ordinal 596**; calls
  `tslib::EventsViaHandles::UnregisterHandle`).
- `orchart.exe` imports `_OrlibRegisterNotificationHandle@4`, confirming at least
  orchart registers a receiver HWND. (Import verified via orchart IAT; orchart also
  contains `0x040A` as a data constant at `0x006f7df0` and `0x00710180`, consistent with
  a message-map / dispatch entry for the registered message.)

The `EventsViaHandles` singleton pointer is stored at `orlib20!0x10231050`
(`PTR_DAT_10231050`) and is loaded into `ECX` as the `this` pointer for every
`PosMessageToClients` / `SendMessageToClients` call.

### 1.5 Frida confirmation of the delivery model

From `tmp/frida_trace_verify.log`, filtering `msg_id=0x40a`:

- **57 total `0x40A` sends**, **all via `PostMessageW`** (asynchronous), **0 via
  `SendMessageW`**.
- **19 distinct receiver HWNDs** per operation:
  `0x100d4e, 0x150a8e, 0x180a9c, 0x234f2, 0x23550, 0x235b8, 0x2360c, 0x23660,
  0x50e96, 0x50eb8, 0x50f6a, 0x61376, 0x61400, 0x63190, 0x70a40, 0x710a8, 0x81280,
  0xc09c6, 0xe11be`.
- **3 distinct `lParam` values**: `0x1a1e, 0x1a1f, 0x1a20` (one per verify operation,
  marker generations 2/3/4). These are the affected modules' **ProgIds** (warehouse record
  indices): 6686/6687/6688 (cross-referenced with `docs/PM_WORKING_MEMORY.md` lines 217,
  243). Each operation posts the same `(msg,wParam,lParam)` to all
  19 HWNDs (57 = 3 × 19).
- Every `PostMessageW` returned `retval=0x1` (TRUE).

This is exactly the behavior of `PosMessageToClients` iterating the registered HWND set
and calling `PostMessageW` once per registered handle.

### 1.6 `0x40A` is not a registered (string-named) message

No `RegisterWindowMessage` call is associated with the `0x40A` send sites. `0x40A` is a
fixed private message ID (in the WM_USER+ range, `0x400`–`0x4FF`), dispatched by
receivers via static message-map / dispatch tables, not by atom name.

---

## 2. R-M.3.2 — Operation Set and Lifetime Behavior

### 2.1 Send sites in `orlib20.dll` (exhaustive — `PUSH 0x40A` scan)

A byte-pattern search for `68 0A 04 00 00` (`PUSH 0x40A`) in `orlib20.dll` returns
exactly four sites:

| Address      | Function       | Export / ordinal            | Variant | Gating |
|--------------|----------------|-----------------------------|---------|--------|
| `0x101526d3` | `FUN_101526c0` | Ordinal **65**              | `SendMessageToClients` (sync)  | none |
| `0x10152703` | `FUN_101526f0` | Ordinal **57**              | `PosMessageToClients` (async)  | none |
| `0x1016b33e` | `FUN_1016b2f0` | internal (import/export)    | `PosMessageToClients` (async)  | `category != 1 && category != 4` |
| `0x10159d76` | `FUN_10159ae0` | internal (delete; wrapped by export `FUN_10159dd0`) | `PosMessageToClients` (async) | `category != 4` |

### 2.2 TSDev callers of the async sender (Ordinal 57)

`TSDev.exe` imports `ORLIB20.DLL` **Ordinal_57** (IAT thunk at `0x0053f56e`). Xrefs to
that thunk — i.e. the only places TSDev posts `0x40A`:

1. **`FUN_00453f70`** — the verify/recalculate-success handler:
   ```c
   void FUN_00453f70(int param_1) {
       ShowRecalculationWarning();
       Ordinal_57(*(ushort*)(param_1 + 0x1e8),   // category
                  *(ushort*)(param_1 + 0x1ec));  // ProgId (warehouse record index)
   }
   ```
   The `+0x1ec` field is the ProgId, confirmed by `FUN_00451800` which loads it into
   CELCompiler `+0x10` (the "current ProgId" field; see `docs/PM_WORKING_MEMORY.md`
   line 222) and passes it as the ProgId argument to `Ordinal_17`, `FUN_004d7a30`-related
   calls, etc. Referenced from **18 DATA locations** (`0x56374c`…`0x56d800`) — MFC
   command-handler / vtable dispatch entries. This is the post-verify-success notification
   path.

2. **`FUN_0044cab0`** — the rename / change-analysis-technique-type flow. After a
   confirmed name change (`SetNameEx` + `SetReturnNotesEx` + vtable save), it calls:
   ```c
   Ordinal_57((short)param_1[0x7a], (short)param_1[0x7b]);  // category, ProgId
   ```
   Called from `FUN_0044c950`. (`param_1[0x7b]` is the same record's ProgId field at the
   dword-indexed offset equivalent to `+0x1ec`.)

**TSDev does NOT import Ordinal_65** (the synchronous sender) — confirmed by the TSDev
ORLIB20 import list (ordinals 10,13,15,17,18,20,24,25,28,30,37,38,39,56,57,66,67,68,69,
70,71; no 65). TSDev therefore always uses the **asynchronous** Post path.

### 2.3 Operation matrix

| Operation | Sends `0x40A`? | Sender / path | Evidence |
|-----------|----------------|---------------|----------|
| **Verify (successful)** | **YES (CONFIRMED)** (async Post) | `FUN_00453f70` → Ordinal_57 → `PosMessageToClients` | Frida: `_VerifyModule_New@20` retval=1 → `EndCompilingProcess` → `ShowRecalculationWarning` → `PostMessageW(0x40A)` burst to 19 HWNDs (`tmp/frida_trace_verify.log` lines ~105941+, marker_generation 2) |
| **Insert** | **YES (CONFIRMED)** (async Post) | Same verify-success path; insert compiles+verifies then notifies | Evidence chain: (a) TSDev UI workflow "File > New > Strategy > name > OK > source > Build > Verify (F3)" (`docs/PM_WORKING_MEMORY.md` line 213) — INSERT goes through Build > Verify; (b) Build > Verify is WM_COMMAND 0x296A → `FUN_0047b0e0` → `FUN_00451800` → `FUN_004d9480` → `FUN_004d91e0` → `_VerifyModule_New@20` (`docs/PM_WORKING_MEMORY.md` line 215) — the **same** command handler fires for new and existing items; (c) on verify success the dispatch reaches `FUN_00453f70` (the 0x40A sender), referenced from 18 MFC command-handler/vtable DATA locations; (d) Frida confirms 0x40A is sent after verify success (retval=1). New item → Build > Verify → verify success → `FUN_00453f70` → 0x40A. |
| **Modify** | **YES (CONFIRMED)** (async Post) | Same verify-success path | Same evidence chain as Insert: an existing item is edited then Build > Verify (same WM_COMMAND 0x296A handler `FUN_00451800`) → verify success → `FUN_00453f70` → 0x40A. The command handler is identical for new and existing items; only the verify-success branch sends 0x40A. `docs/PM_WORKING_MEMORY.md` line 217 confirms a CLI-inserted item was "harmlessly edited, and successfully re-verified" through this path. |
| **Upsert** | **YES (by composition)** (async Post) | Resolves to insert or modify, both CONFIRMED | Upsert is a CLI composition concept (insert-or-modify), NOT a TSDev UI operation — TSDev has no "upsert" command. It resolves to insert (CONFIRMED above) when the item is absent, or modify (CONFIRMED above) when present. Both branches verify-on-success and emit 0x40A via `FUN_00453f70`. |
| **Delete** | **YES (CONFIRMED)** (async Post) | `FUN_10159ae0` (delete) → `PosMessageToClients(0x40A,0, cat<<16\|ProgId)` | Static: post-delete send gated on `category != 4` (decompilation `FUN_10159ae0`; export wrapper `FUN_10159dd0`). |
| **Failed compile / verify** | **NO (CONFIRMED)** | — | `_VerifyModule_New@20` returns failure; `FUN_00453f70` is only reached on the success branch (Frida shows the burst only after `retval=1`). No `0x40A` send on failure paths. (No dedicated failed-compile Frida capture; see §5 item 4 — static evidence is conclusive but a confirming capture remains open.) |
| **Identical-source no-op modify** | **UNRESOLVED** (gap recorded) | — | Not captured in the available Frida trace; no evidence either way. Static analysis: the verify-success path is gated on verify success, not on source-difference detection; a no-op modify that still compiles+verifies successfully would still reach `FUN_00453f70`. Whether orlib20 short-circuits identical-source recompiles is NOT confirmed. Per R-M.3.2 (no-guessing rule, `docs/workstream_tsdev_parity_plan.md` M.3 section): this operation does NOT get a confirmed 0x40A answer and the gap is recorded here. **Needs a dedicated Frida capture.** |
| **Import/Export batch** | **YES (CONFIRMED)** (async Post, per module) | `EndImportExport` → `FUN_1016bc20` → `FUN_101743b0` (tree walk) → `FUN_1016b2f0` per node | One `0x40A` per affected module; gated `category != 1 && category != 4`. |

### 2.4 Category gating

- **Delete** (`FUN_10159ae0`): sends `0x40A` only when `category != 4`. Before/during
  deletion it also sends a *different* message, `0x40C`, with `wParam` = 1 (pre-delete),
  2 (during), 3 (cancel). `0x40C` is out of scope for M.3 but noted for completeness.
- **Import/Export** (`FUN_1016b2f0`): sends `0x40A` only when
  `category != 1 && category != 4`.
- **Verify-success** (`FUN_00453f70` via Ordinal_57): **no category gate** in the sender
  itself; the gate is simply "verify succeeded". (The category and ProgId are still packed
  into `lParam`.)

### 2.5 Frida verify-success sequence (verbatim ordering)

From `tmp/frida_trace_verify.log` (marker_generation 2; the `0x40A` posts begin at trace
line 105941):

```
EXIT  _VerifyModule_New@20            retval=0x1            ; verify SUCCESS
ENTER GetWarehouse@RVA_0x1a9650       args=[...,0x1a1e,...] ; record index = 0x1a1e
ENTER InitGlobalLookup@RVA_0x1a9460   args=[0x0,0x1a1e,0x0,...]
EXIT  EndCompilingProcess             retval=0x1
EXIT  ShowRecalculationWarning        retval=0x0
ENTER UI:PostMessageW  hwnd=0x100d4e  msg=MSG_0x40a wParam=0x0 lParam=0x1a1e  retval=0x1
ENTER UI:PostMessageW  hwnd=0x50e96   msg=MSG_0x40a wParam=0x0 lParam=0x1a1e  retval=0x1
... (19 HWNDs total, all lParam=0x1a1e, all retval=0x1)
```

The `lParam` low word (`0x1a1e`) is the same identifier used in the immediately preceding
`GetWarehouse`/`InitGlobalLookup` calls — i.e. the affected module's **ProgId** (warehouse
record index). It is packed into the low 16 bits with `category = 0` (high 16 bits) for
the captured Strategy verifies (Strategy category = 0 per
`docs/TS_COMPONENT_TYPES.md`). The same value `0x1a1e` is the return value of
`_CreateEntryEX@24` earlier in the trace (line 783, `retval=0x1a1e`), confirming it is the
record index assigned by CreateEntry = the ProgId. Successive verifies produced `0x1a1f`
(=6687) then `0x1a20` (=6688), consistent with incrementing ProgIds (cross-referenced with
`docs/PM_WORKING_MEMORY.md` lines 217 and 243: ProgIds 6686/6687/6688).

### 2.6 `orlib20` save/verify internals do NOT themselves send `0x40A`

Decompilation confirms:
- `_SaveModule_New@20` (`0x10159a70`, Ordinal_634) → `FUN_10159770`: saves the module;
  **no** `0x40A` send.
- `_VerifyModule_New@20` (`0x10169630`, Ordinal_700): compiles via
  `CELBasicCompiler::CompileText` and finalizes via `FUN_101698f5`; **no** `0x40A` send.

The `0x40A` for verify/insert/modify is therefore emitted by the **TSDev success
handler** (`FUN_00453f70`) calling orlib20 Ordinal_57, **after** orlib20's verify
returns success. This is the TSDev parity point: a CLI replacement must call
`_OrlibRegisterNotificationHandle`-equivalent delivery (i.e. post `0x40A` via the same
orlib20 export) after a successful verify, exactly as TSDev does.

---

## 3. R-M.3.3 — Error / Failure Behavior

### 3.1 Sender return value

`FUN_101526f0` (Ordinal_57) **unconditionally returns 1** (`MOV EAX,1` before `RET 8`)
regardless of how many HWNDs received the message or whether any `PostMessageW`
succeeded. The wrapper is effectively `void`-semantics from the caller's perspective:
**TSDev cannot detect a delivery failure from the return value.**

### 3.2 Per-HWND Win32 result (Frida-observed)

Every `PostMessageW(0x40A)` in the trace returned `retval=0x1` (TRUE). `PostMessageW`
returns BOOL (success of posting, not of processing), so `0x1` means the message was
accepted into the target's queue — it does **not** mean the receiver processed it.

### 3.3 No registered receivers

If the `EventsViaHandles` registered set is empty, `PosMessageToClients` iterates zero
handles: no `PostMessageW` calls are made, no error is raised, and Ordinal_57 still
returns 1. The notification is silently dropped. (Static inference from the
iterate-and-post structure; consistent with the void-wrapper return.)

### 3.4 Invalid / stale HWND

`PostMessageW` to an invalid HWND returns `0` (FALSE). Because Ordinal_57 ignores the
per-call result and returns 1 regardless, a stale HWND causes a silent per-HWND failure
with **no logging, no propagation, no surfaced error**. Stale-handle cleanup (if any)
resides inside `tslib::EventsViaHandles` and was **not** statically confirmed in this
task (see §5 unresolved). The `UnregisterHandle` import exists, implying receivers are
expected to unregister on shutdown; a crashed receiver would leave a stale HWND that
`PostMessageW` would fail on silently.

### 3.5 Synchronous variant error semantics

`FUN_101526c0` (Ordinal_65, `SendMessageToClients`) also returns 1 unconditionally.
`SendMessageW` to an invalid HWND returns 0, but again the wrapper ignores it. TSDev
does not use this variant, so this is informational only.

### 3.6 Summary of failure semantics

| Condition | Behavior |
|-----------|----------|
| No registered receivers | Silent no-op; Ordinal_57 returns 1 |
| Invalid/stale HWND | `PostMessageW` returns 0; silently ignored; Ordinal_57 still returns 1 |
| All posts succeed | `PostMessageW` returns 1 each; Ordinal_57 returns 1 |
| Receiver crashes mid-processing | Not detectable by sender (Post is fire-and-forget) |

**Errors are neither logged, propagated, nor surfaced.** A CLI replacement must not
treat Ordinal_57's return value as a delivery guarantee.

---

## 4. R-M.3.4 — Test-Only Receiver

A test-only receiver can observe and validate `0x40A` without modifying production or
test repository code by acting as a **registered client** of `EventsViaHandles`.

### 4.1 Registration (coordinate with M.1 HWND findings)

Per M.1, the notification HWND is the active edit view's HWND, and `CELCompiler+0x18`
holds the notification HWND. For a *test receiver*, the cleanest parity approach is to
register a dedicated hidden message-only window via the same orlib20 API TSDev's peers
use:

- Load `orlib20.dll` and resolve `_OrlibRegisterNotificationHandle@4` (Ghidra-confirmed
  orlib20 export at `0x1011c130`, **ordinal 595**; `orchart.exe` imports it).
- Create a message-only window (`CreateWindowExW(WS_EX_NOACTIVATE, "Static", ...)` or a
  registered class with `HWND_MESSAGE` parent) — **not** a TSDev edit view, to avoid
  touching production HWNDs.
- Call `_OrlibRegisterNotificationHandle(hwnd)` to add it to the
  `EventsViaHandles` client set.
- On test teardown, call the matching unregister `_OrlibUnregisterNotificationHandle@4`
  (Ghidra-confirmed orlib20 export at `0x1011c140`, **ordinal 596**) or simply destroy
  the window.

> **Caveat:** `_OrlibRegisterNotificationHandle@4` (ordinal 595) is an orlib20 export but
> is **not** imported by `TSDev.exe` (TSDev registers its own frame via
> `_RegisterPowerEditorFrameWnd@4`, Ordinal_611, which is a *different* function that
> cleans masters — it is **not** the EventsViaHandles registration). The
> EventsViaHandles registration is used by peer consumers (orchart, etc.). A test
> receiver should use `_OrlibRegisterNotificationHandle@4` (ordinal 595) to match the
> peer-consumer registration path. The export name and ordinal are now resolved
> (Ghidra-confirmed, see Evidence Index); no build-time re-resolution is required.

### 4.2 Receiver window and message loop

- Run a standard `GetMessage`/`DispatchMessage` loop on the test thread.
- Handle `0x40A` in the window proc: capture `msg`, `wParam`, `lParam`, plus
  `GetCurrentProcessId()`/`GetCurrentThreadId()` of the receiving thread, and the sender
  context (the sender is in the TSDev/orlib20 process; for cross-process posts the
  receiver thread is the test process).
- Decode `lParam`: `category = HIWORD(lParam)`, `ProgId = LOWORD(lParam)`.
- Assert `msg == 0x40A`, `wParam == 0`, `category == HIWORD(lParam)`, and
  `ProgId == LOWORD(lParam)` matches the ProgId of the module just
  inserted/modified/deleted/verified. (The ProgId is carried directly in the low word —
  no separate warehouse lookup is needed to validate the ProgId field.)

### 4.3 What to capture and validate

| Field | Source | Expected |
|-------|--------|----------|
| `msg` | window proc `uMsg` | `0x040A` |
| `wParam` | window proc `wParam` | `0` |
| `lParam` | window proc `lParam` | `(category<<16)\|(ProgId&0xFFFF)`; high word = category, low word = **ProgId** (warehouse record index) |
| ProgId | `LOWORD(lParam)` | equals the operated module's ProgId directly (no separate warehouse lookup needed for the ProgId field); Frida confirms `0x1a1e/0x1a1f/0x1a20` = ProgIds 6686/6687/6688 |
| Sender | inferred from which process posted | orlib20 in the TSDev (or CLI) process |
| Delivery | one post per registered HWND | receiver gets exactly one per mutation |
| Timing | async (posted) | arrives after the verify-success sequence completes |

### 4.4 Coordination with M.1

M.1 established that TSDev obtains the notification HWND from the active edit view and
that `CELCompiler+0x18` is the notification HWND. For `0x40A` specifically, the
*sender* does not target that HWND directly — it targets the **EventsViaHandles
registered set**. The M.1 HWND is relevant to the *receiver-side* dispatch (TSDev's own
`0x40A` handler). A test receiver must therefore register its own HWND with
`EventsViaHandles` rather than reuse the M.1 edit-view HWND.

### 4.5 TSDev receiver-side handler (for reference)

TSDev's own `0x40A` handling is a static dispatch-table entry, not a window-proc switch:

- Table base `0x005d372c`, written by a static initializer at `0x00440565`.
- The `0x40A` entry (`0x005d3778`) maps to handler `FUN_004430a0`.
- `FUN_004430a0` refreshes string resources / CEL program state (receive-side refresh),
  confirming TSDev treats `0x40A` as "library changed → refresh in-memory state".

This confirms the architectural intent: `0x40A` is the live-consumer refresh signal.

---

## 5. Unresolved / Open Items

1. **`tslib.dll` internals not statically confirmed.** The exact Win32 primitive inside
   `EventsViaHandles::PosMessageToClients` / `SendMessageToClients` was inferred
   (Frida observes `PostMessageW` at the Win32 boundary, so `PosMessageToClients` calls
   `PostMessageW`; `SendMessageToClients` presumably calls `SendMessageW`). Stale-HWND
   removal and iteration order inside `tslib.dll` were not disassembled in this task.
2. **Identical-source no-op modify.** Not captured in the available Frida trace. Needs a
   dedicated trace: modify an item with byte-identical source and observe whether the
   verify-success path (and thus `0x40A`) still fires. Static evidence suggests it would
   (the gate is verify-success, not source-diff), but this is unconfirmed.
3. **Pure-insert Frida capture.** The available trace captured verify-success
   (marker_generations 2/3/4). A pure insert (new ProgId) capture would directly confirm
   the insert path emits `0x40A` with the same packing. (Insert is already upgraded to
   CONFIRMED in §2.3 via the TSDev UI workflow + shared command-handler + verify-success
   evidence chain; a pure-insert trace would be additional direct confirmation.)
4. **Failed-compile Frida capture.** Not in the trace; static evidence says no `0x40A`
   on failure, but a capture with a deliberately broken source would confirm zero
   `0x40A` sends.
5. **Complete receiver inventory.** Only `orchart.exe` was confirmed to import
   `_OrlibRegisterNotificationHandle@4` (ordinal 595). Other potential receivers
   (`tselc.dll`, `ELIS.exe`, `ORPlat.exe`, `ORDllHst.exe`) were not exhaustively
   checked. The 19 HWNDs observed in Frida belong to whatever TradeStation processes
   were running at capture time.
6. **`_OrlibRegisterNotificationHandle@4` ordinal — RESOLVED.** Ghidra-confirmed: export
   at `0x1011c130`, **ordinal 595** (decompilation annotation
   `/* 0x11c130  595  _OrlibRegisterNotificationHandle@4 */`; calls
   `tslib::EventsViaHandles::RegisterHandle`). The matching unregister
   `_OrlibUnregisterNotificationHandle@4` is at `0x1011c140`, **ordinal 596**. No longer
   open; recorded here for provenance. (See §1.4 and the Evidence Index.)
7. **`0x40C` (delete lifecycle message).** Out of scope for M.3 but observed in the
   delete path (`wParam` 1/2/3). Documented for context; a future workstream may need
   it for full delete parity.

---

## 6. Implications for TSDev Parity (Workstream M)

To emulate TSDev behavior for automated library management:

1. **After a successful verify (insert/modify/upsert), post `0x40A`** via orlib20
   Ordinal_57 (`FUN_101526f0`), packing `lParam = (category<<16)|(ProgId&0xFFFF)`,
   `wParam = 0`. This matches TSDev's `FUN_00453f70` path. Insert and modify are
   CONFIRMED (§2.3): both go through the same Build > Verify command handler
   (`FUN_00451800`, WM_COMMAND 0x296A) and emit `0x40A` on verify success via
   `FUN_00453f70`; Frida confirms the verify-success → 0x40A burst. Upsert is CONFIRMED
   by composition (it resolves to insert or modify, both confirmed).
2. **After a successful delete, post `0x40A`** via the same export, respecting the
   `category != 4` gate (or use the orlib20 delete export `FUN_10159dd0` which already
   contains the gated send).
3. **Do not emit `0x40A` on compile/verify failure.**
4. **Do not treat the return value as a delivery guarantee** — Ordinal_57 always returns
   1. Delivery to live consumers is best-effort, fire-and-forget, exactly as in TSDev.
5. **Pack the ProgId into the low word of `lParam`.** The low word IS the ProgId
   (warehouse record index), confirmed by Frida (`0x1a1e/0x1a1f/0x1a20` = ProgIds
   6686/6687/6688) and by Ghidra (`FUN_00453f70` passes `+0x1ec` = the ProgId field per
   `FUN_00451800`'s CELCompiler `+0x10` assignment). Do NOT pack the subtype; the subtype
   is not carried in the `0x40A` payload.
6. **`0x40A` is live-only and has no durable queue.** It cannot repair the unknown-link
   defect (Workstream L). Restarting Desktop does not replay it.

---

## 6a. Desktop Runtime Observation (2026-08-25, user-supervised)

This section records the user-supervised Desktop runtime observation required by plan L218
(`docs/workstream_tsdev_parity_plan.md`, M.3 Desktop observation requirement). The
observation verifies that an already-applied study in a running TradeStation Desktop
refreshes without restart after a CLI operation that the CLI confirms sends `0x40A`.

The observation was performed under the explicit M.3 Desktop-observation exception to the
general "do NOT run `tsdev_cli` insert/modify/delete while TradeStation Desktop is open"
rule (see `docs/PM_WORKING_MEMORY.md`, "DURABLE CONCURRENCY CAVEAT — 2026-08-24",
operational rule amended 2026-08-25). The 2026-08-24 concurrency caveat's original claim
that the affected items were applied to charts was corrected on 2026-08-25: they were
NOT applied. The M.3 observation is an explicit, user-authorized exception to the general
rule.

### 6a.1 EventsViaHandles locality — CROSS-PROCESS (CONFIRMED)

The `EventsViaHandles` notification HWND set is **cross-process**, stored in a PE shared
memory section (`SHARE_WO`, marked `IMAGE_SCN_MEM_SHARED`). The CLI's `0x40A` posts
therefore CAN reach a registered receiver window in a different process (e.g.,
TradeStation Desktop / `ORPlat.exe`), because:

1. The HWND array lives in `SHARE_WO` (shared across all processes that load
   `orlib20.dll`).
2. Each process's `EventsViaHandles` object (in process-local `.data`) holds a pointer
   statically initialized to the same shared array address.
3. `orlib20.dll` has no ASLR (`DllCharacteristics = 0x0100`, no `DYNAMIC_BASE`), so it
   loads at `0x10000000` in every process — the shared section is at the same VA
   everywhere.
4. Registration uses `LOCK CMPXCHG` (hardware-atomic), the correct primitive for
   cross-process shared-memory concurrency.
5. `PostMessageW` delivers to any HWND system-wide (HWNDs are global within a window
   session/desktop).

Full PE section analysis, registration/send-path decompilation, and the cross-process
data-flow walkthrough are in `docs/RE/RE_M3_EVENTSVIAHANDLES_LOCALITY.md`. This finding
establishes that the Desktop observation is a valid verification target: the CLI's
`0x40A` is not process-local and is not silently dropped simply because the sender is a
different process from the receiver.

### 6a.2 Modify observation (two CLI modifies, both refreshed automatically)

Indicator `TS_AUTO_M3_DESK_IND_20260825` (ProgId 2757) was inserted via CLI while
TradeStation Desktop was closed. The user then opened Desktop and applied the indicator
to a chart. Initial plot behavior: v1 plotted `Close`, v2 plotted `High`, v3 plotted a
constant `100.00`.

Two CLI `modify` operations were then run while Desktop was open with the indicator
applied to a chart:

- **Modify 1 (Close → High):** the chart refreshed automatically; no Desktop restart was
  needed. The user initially misread the change direction but subsequently confirmed the
  modify worked correctly.
- **Modify 2 (High → 100.00 constant):** the chart refreshed automatically to a flat
  line at `100.00`; no Desktop restart was needed. The user confirmed: "it worked.
  perfect."

Both modifies produced an automatic chart refresh, consistent with the CLI's `0x40A`
post reaching Desktop's registered receiver windows via the cross-process
`EventsViaHandles` shared array (§6a.1) and Desktop's `0x40A` handler refreshing
in-memory library/runtime state (§4.5).

### 6a.3 Delete observation (indicator disappeared with warning, matching TSDev UI)

A CLI `delete` was run while Desktop was open with the indicator applied to a chart.
The indicator disappeared from the chart, accompanied by a warning message: "the
indicator has been deleted." The user confirmed: "this is 100% correct and the same
behavior as if the indicator had been removed via the TSDev UI."

This matches the expected `0x40A`-driven refresh behavior: the delete-success
`0x40A` post (gated `category != 4`; Indicator category = 2, so the gate passes) reaches
Desktop, which removes the now-deleted indicator from the live chart and surfaces a
warning consistent with TSDev's own delete-from-library UI flow.

### 6a.4 No crashes

No crashes occurred during any of the three CLI operations (two modifies + one delete)
while TradeStation Desktop was open with the affected indicator applied to a chart. This
contrasts with the 2026-08-24 concurrent-write incident (see `docs/PM_WORKING_MEMORY.md`,
"DURABLE CONCURRENCY CAVEAT — 2026-08-24"), which involved a concurrent warehouse write
while Desktop was open but did NOT involve items applied to charts. The M.3 observation
operations were performed under the user-authorized M.3 Desktop-observation exception
and did not reproduce the earlier crash.

### 6a.5 Plan L218 satisfaction

This observation satisfies plan L218's Desktop observation requirement for M.3: a CLI
operation was run while Desktop was open with an affected study applied to a chart, and
the study refreshed without restart. Both the modify path (verify-success → `0x40A` →
refresh) and the delete path (delete-success → `0x40A` gated `category != 4` → refresh
+ warning) were observed end-to-end in the live Desktop runtime. The cross-process
delivery mechanism is established by `docs/RE/RE_M3_EVENTSVIAHANDLES_LOCALITY.md`.

---

## 7. Evidence Index

| Claim | Source |
|-------|--------|
| `PUSH 0x40A` sites in orlib20 | Ghidra byte search `68 0A 04 00 00` → `0x101526d3, 0x10152703, 0x10159d76, 0x1016b33e` |
| Packing `(cat<<16)\|ProgId` (low word = ProgId, NOT subtype) | Disasm at `0x101526f0` (`MOVZX/SHL/OR/PUSH`); Ghidra decompile `FUN_101526f0` → `CONCAT22(param_1,param_2)`; Frida `lParam=0x1a1e/0x1a1f/0x1a20` = ProgIds 6686/6687/6688 |
| Low word = ProgId (record index), confirmed three ways | (1) Frida: `_CreateEntryEX@24` retval=`0x1a1e` (trace line 783) = same value used by `InitGlobalLookup` (lines 751+) = `lParam` low word (lines 105941+); (2) `docs/PM_WORKING_MEMORY.md` line 217 "ProgId 6687" = `0x1a1f`; (3) Ghidra: `FUN_00453f70` passes `+0x1ec` to Ordinal_57, and `FUN_00451800` loads `+0x1ec` into CELCompiler `+0x10` = "current ProgId" (`docs/PM_WORKING_MEMORY.md` line 222) |
| `+0x1ec` is the ProgId field (not subtype) | Ghidra decompile `FUN_00451800`: `piVar10[4]=*(ushort*)(param_1+0x1ec)` (CELCompiler +0x10 = ProgId); `piVar10[2]=+0x1e8` (category), `piVar10[3]=+0x1ea` (subtype); matches `docs/PM_WORKING_MEMORY.md` line 222 |
| `wParam=0` | `PUSH 0x0` at `0x10152701` |
| EventsViaHandles singleton | `MOV ECX, 0x10231050` at `0x10152708` |
| `PosMessageToClients` import | `[0x101fa054]` → `?PosMessageToClients@EventsViaHandles@tslib@@QAEXIIJ@Z` |
| `SendMessageToClients` import | `[0x101fa058]` → `?SendMessageToClients@EventsViaHandles@tslib@@...` |
| `RegisterHandle`/`UnregisterHandle` imports | orlib20 IAT → tslib mangled names |
| Ordinal 57 → `0x101526f0` (async sender) | Ghidra decompile `FUN_101526f0` annotation `/* 0x1526f0  57   */`; EXTERNAL Entry Point xref; export-address slot `0x1021d074` (DATA) → `0x101526f0` |
| Ordinal 65 → `0x101526c0` (sync sender) | Ghidra decompile `FUN_101526c0` annotation `/* 0x1526c0  65   */`; EXTERNAL Entry Point xref; export-address slot `0x1021d094` (DATA) → `0x101526c0` |
| `_OrlibRegisterNotificationHandle@4` = ordinal 595 @ `0x1011c130` | Ghidra `search_functions` "RegisterNotification" → `_OrlibRegisterNotificationHandle@4 @ 1011c130`; decompile annotation `/* 0x11c130  595  _OrlibRegisterNotificationHandle@4 */`; calls `tslib::EventsViaHandles::RegisterHandle` |
| `_OrlibUnregisterNotificationHandle@4` = ordinal 596 @ `0x1011c140` | Ghidra `search_functions` → `_OrlibUnregisterNotificationHandle@4 @ 1011c140`; decompile annotation `/* 0x11c140  596  _OrlibUnregisterNotificationHandle@4 */`; calls `tslib::EventsViaHandles::UnregisterHandle` |
| `_OrlibRegisterNotificationHandle@4` | orlib20 export (ordinal 595); orchart IAT import |
| TSDev imports Ordinal_57 | TSDev ORLIB20 import list; thunk `0x0053f56e` (Ghidra `get_function_by_address` → `name="Ordinal_57"`); xrefs from `0x00453f89` (`FUN_00453f70`) and `0x0044cd8a` (`FUN_0044cab0`) |
| TSDev does NOT import Ordinal_65 | TSDev ORLIB20 import list (no 65) |
| TSDev has no `PUSH 0x40A` | Ghidra byte search `68 0A 04 00 00` in TSDev.exe → no matches |
| TSDev receiver table | `0x005d372c` (init `0x00440565`); `0x40A` entry `0x005d3778` → `FUN_004430a0` |
| `FUN_00453f70` posts `0x40A` after ShowRecalculationWarning | Decompilation `0x00453f70` (`Ordinal_57(*(ushort*)(param_1+0x1e8), *(ushort*)(param_1+0x1ec))`); 18 vtable DATA xrefs |
| `FUN_0044cab0` posts `0x40A` after rename | Decompilation `0x0044cab0` (`Ordinal_57((short)param_1[0x7a], (short)param_1[0x7b])`); caller `FUN_0044c950` |
| Build > Verify command handler = `FUN_00451800` (WM_COMMAND 0x296A) | `docs/PM_WORKING_MEMORY.md` line 215; Ghidra decompile `FUN_00451800` configures CELCompiler (+0x08 cat, +0x0C subtype, +0x10 ProgId from `+0x1e8/+0x1ea/+0x1ec`) and dispatches to `FUN_004d9480` → `_VerifyModule_New@20`; same handler for new and existing items |
| Insert CONFIRMED via Build > Verify | `docs/PM_WORKING_MEMORY.md` line 213 (UI workflow "File > New > Strategy > … > Build > Verify (F3)"); line 215 (command chain); `FUN_00453f70` is verify-success 0x40A sender (18 DATA xrefs); Frida verify-success → 0x40A |
| Modify CONFIRMED via Build > Verify | Same command handler `FUN_00451800` (WM_COMMAND 0x296A) for existing items; `docs/PM_WORKING_MEMORY.md` line 217 (CLI item "harmlessly edited, and successfully re-verified"); verify success → `FUN_00453f70` → 0x40A |
| Upsert = composition of insert/modify (both CONFIRMED) | TSDev has no "upsert" UI command; CLI upsert resolves to insert (CONFIRMED) or modify (CONFIRMED) |
| Identical-source no-op modify = UNRESOLVED (gap recorded) | No Frida capture; no static evidence either way; R-M.3.2 no-guessing rule (`docs/workstream_tsdev_parity_plan.md` M.3) → gap recorded, not guessed |
| Delete sends `0x40A` gated `cat!=4` | Decompilation `FUN_10159ae0`; export wrapper `FUN_10159dd0` |
| Import/export sends `0x40A` gated `cat!=1 && !=4` | Decompilation `FUN_1016b2f0`; callers `FUN_101743b0`←`FUN_1016bc20`←`EndImportExport` |
| `_SaveModule_New@20` / `_VerifyModule_New@20` do NOT send `0x40A` | Decompilation `0x10159a70`/`0x10169630` |
| Frida: 57 posts, all PostMessageW, 19 HWNDs, lParam 0x1a1e/1a1f/1a20, retval=1 | `tmp/frida_trace_verify.log` filter `msg_id=0x40a` (posts begin at line 105941, marker_generation 2) |
| Frida: `_CreateEntryEX@24` retval=0x1a1e (=ProgId 6686) | `tmp/frida_trace_verify.log` line 783 |
| Frida: `InitGlobalLookup` record-index arg = 0x1a1e | `tmp/frida_trace_verify.log` lines 751, 753, 767, 769, … |
| Frida: verify-success ordering | `tmp/frida_trace_verify.log` lines ~105941+ (marker_generation 2) |
| Strategy subtype = 0 (so low word cannot be subtype) | `docs/TS_COMPONENT_TYPES.md`: Strategy = category 0, subtype 0 |
| orchart contains `0x40A` data constant | orchart `0x006f7df0`, `0x00710180` = `0A 04 00 00` |
