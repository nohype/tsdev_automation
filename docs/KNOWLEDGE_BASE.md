# TSDev.exe Knowledge Base

Authoritative knowledge base of discovered facts about **TSDev.exe** only. Current proven knowledge only: every statement cites its evidence; no assumptions, conclusions, history, or unevidenced claims. Scope is TSDev.exe exclusively — per `AGENTS.md`, no other binary or process is in scope, and facts about other binaries do not belong here. All addresses are from TSDev 9.5 and are version-specific.

The UI save path below is the definition of "the same verify (compile + save) code path the UI triggers" that the automation drives.

---

## TSDev.exe UI save path (the verify/compile + save code path)

Entry point: `FUN_00451800` @ `0x00451800`. Ordered sequence (TSDev-internal addresses; steps named only semantically — pending-save queue add/remove, save, compiler alloc, notify — are calls whose callees lie outside TSDev.exe, and per `AGENTS.md` their internals are out of scope):

1. **Pending-save queue add** — `(category, progId, name)` added to the pending-save queue. The save is bracketed by this add and the queue remove in step 6.
2. **Save** — `FUN_00452290` @ `0x00452290` performs the save via an external save call, passing the document's category (see category facts below).
3. **Compiler alloc** — external compiler-allocation call.
4. **Compile process Start/End** — `FUN_004D9480` → `StartCompilingProcess` / `FUN_004D91E0` / `EndCompilingProcess`.
5. **Category-gated dependent-recalc block** (runs only for categories 0/1/4 — see gate below): `FUN_004D7140` + worker cmd `0x0D`; `FUN_004c9670` builds the dependent list; recursive `FUN_004D91E0` per dependent.
6. **Pending-save queue remove**.
7. **Property/cache persistence** — `FUN_004531A0`.
8. **Notify** — external notify call `(category, progId)` via `FUN_00453f70`.

Evidence: TSDev.exe Ghidra decompilation @ `0x00451800` (static), plus runtime Frida traces of the UI save path.

### Category gate in `FUN_004D91E0`

The post-verify dependent-recalc block (step 5) runs only for categories `0` (Strategy), `1` (Function), and `4` (built-in Function). Indicator/ShowMe/TradingApp (category `2`) skip it — the shared-warehouse path is sufficient because the modified item IS the applied item.

Decompiled gate condition:

```c
if ((uVar6 == 1) && (((uVar2 == 1 || (uVar2 == 4)) || (uVar2 == 0))))
```

Evidence: TSDev.exe Ghidra decompilation (`FUN_004D91E0`).

### Per-type category mapping

| Type | Category |
|------|----------|
| Strategy | `0` |
| Function | `1` (user) / `4` (built-in) |
| Indicator | `2` |
| ShowMe | `2`, subType `0x05` |
| TradingApp | `2`, subType `0x14` |

Evidence: decompilation-derived.

### Category assignment facts

| Fact | Address | Evidence |
|------|---------|----------|
| TSDev.exe creates user functions with warehouse category `1` | `CELFunctionDoc` ctor @ `0x00457eb0` | TSDev.exe Ghidra decompilation |
| TSDev.exe creates built-in functions with category `4` (read-only built-ins) | `CELBIFunctionDoc` ctor @ `0x004140c0` | TSDev.exe Ghidra decompilation |
| TSDev.exe's save path passes the document's category (`param_1[0x7a]`) from `FUN_00452290` into the external save call (callee outside TSDev.exe; internals out of scope) | `FUN_00452290` @ `0x00452290` | TSDev.exe Ghidra decompilation |

---

## TSDev.exe UI characteristics relevant to automation

- TSDev.exe has NO "Find Dependents" / "Find References" / "Where Used" / "Used By" UI feature for regular functions/indicators. Evidence: `strings` analysis of TSDev.exe (narrow + wide) found no such menu text; runtime menu exploration found no such feature.
- The TSDev.exe main window has NO standard Win32 HMENU: `win32gui.GetMenu` on the main window returns `hmenu=0x0`; TSDev uses a BCGSoft owner-drawn `BCGPToolBar` menu bar, not a standard Win32 HMENU. pywinauto `menu()` returns None; the UIA backend finds only the system MenuBar; Alt+key walks produce no `#32768` popup windows. Evidence: runtime UI inspection.
- The strings "Dependencies"/"Callers" in TSDev.exe exist ONLY as TradingApp project resource-tree node names (`TradingApp, UserFunction, SystemFunction, Icon, Form, Dependencies, Strings, Callers, Image`) — not a function/indicator feature. Evidence: `strings` analysis of TSDev.exe.
- The function editor Outline pane is a `SysTreeView32` showing only a `Document` node (children: `using`, `inputs`, and the input names — e.g. `Price`, `Length`) — no `Callers` or `Dependencies` nodes. Evidence: runtime UI inspection (built-in `Average` and user `E2eFn_202608291052` opened in the editor; Outline pane dumped).

---

## Expected observable behavior after modify/delete (E2E expectations)

The dependent recalc itself executes outside TSDev.exe; these are distilled end-to-end expectations, not TSDev.exe-internal mechanisms.

- An applied Indicator re-plots immediately after modify; deleting it while applied auto-removes it from the chart and from the Insert Analysis Techniques list.
- ShowMe markers move immediately after modify.
- Strategy signal labels update immediately after modify.
- An open TradingApp WinForms form reinitializes immediately when the TradingApp itself or a caller Function is modified.

Evidence: user-confirmed runtime observation.
