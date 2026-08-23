# Integration Approach Evaluation — 64-bit Python ↔ orlib20.dll

**Document type:** Decision document (research-only evaluation)
**Date:** 2026-07-15
**Author:** Systems architecture evaluation (subagent)
**Status:** DECISION MADE — Option A+ (batch CLI + JSON I/O, no daemon) chosen and IMPLEMENTED. See `src/tsdev_automation/` for the Python wrapper (`TsDevClient`) and `docs/BUILD.md` for build instructions.

> **DECISION (2026-08):** Option A+ was chosen — 64-bit Python drives the 32-bit `tsdev_cli.exe` as a subprocess via JSON I/O. No daemon (Option B) was pursued. The `TsDevClient` wrapper is implemented at `src/tsdev_automation/`. All 6 functions (insert/modify/delete/exists/get-source/upsert) + batch operations work via the Python wrapper. 60 Python tests (50 unit + 10 E2E) passed at decision time; as of 2026-08-21 pytest collects 131 tests, of which 116 run non-E2E (15 E2E tests deselected without a live TradeStation install).

> **ADDENDUM (2026-08-21):** Workstream K subsequently added an opt-in persistent elevated serve broker — a `serve` subcommand in `tsdev_cli.exe` plus a Python `_ServeBroker` communicating over a named pipe. This is architecturally adjacent to the rejected Option B, but its motivation is UAC/elevation UX (one elevation prompt per session rather than per CLI invocation), not performance. The core Option A+ architecture (batch CLI subprocess + JSON I/O) is unchanged; `use_serve` now defaults to `True` (flipped in TVS-K.6b, 2026-08-21).

> **SUPERSESSION NOTE (2026-08-23):** The pytest counts in the 2026-08-21 DECISION block above (131 collected / 116 non-E2E / 15 E2E deselected) are a **historical snapshot** and are NOT current. Verified ground truth as of 2026-08-23: pytest collects **154** tests; with `-m "not e2e"`, **139** are selected and **15** e2e tests are deselected (the e2e count is unchanged; the non-e2e suite has grown). For the current authoritative test/build status, see `docs/BUILD.md` and `AGENTS.md` (Completed Work Summary). The 2026-08-21 figures are retained above unchanged for historical traceability of the Option A+ decision.

---

## 1. Problem Statement & Hard Constraints

### 1.1 Requirement (verbatim intent)

The project requires exposing the SAME functionality as the existing 32-bit C++ CLI
(`tsdev_cli.exe`) — INSERT / DELETE / MODIFY of TradeStation EasyLanguage components in
the TSDev Library, with proper error handling and compiler error messages (e.g.
`Line X, Col Y:` source locations) — to **64-bit Python**, so it can be integrated into
other 64-bit Python applications.

The user's stated preferences:
- "We would prefer to avoid interacting with an external process (the CLI executable)
  and, for example, use a DLL — but we did not find a way to call 32-bit DLLs in 64-bit
  Python without disproportionate complexity and brittle setups."
- "Calling the CLI is one option."
- "The exact approach to realize these requirements is yet to be determined."
- "If there is no clear winner, summarize the options and their pros/cons for the user
  to decide."

### 1.2 Hard constraint: 64-bit process cannot load 32-bit DLL (WOW64 boundary)

This is a fundamental Windows architectural limitation, not a tooling deficiency.

**Evidence — Microsoft Learn documentation:**

> "On 64-bit Windows, a 64-bit process cannot load a 32-bit dynamic-link library (DLL).
> Additionally, a 32-bit process cannot load a 64-bit DLL. However, 64-bit Windows
> supports remote procedure calls (RPC) between 64-bit and 32-bit processes (both on the
> same computer and across computers). On 64-bit Windows, an out-of-process 32-bit COM
> server can communicate with a 64-bit client, and an out-of-process 64-bit COM server
> can communicate with a 32-bit client. Therefore, if you have a 32-bit DLL that is not
> COM-aware, you can wrap it in an out-of-process COM server and use COM to marshal calls
> to and from a 64-bit process."

— Source: [Process Interoperability (Microsoft Learn)](https://learn.microsoft.com/en-us/windows/win32/winprog64/process-interoperability)

> "The system isolates 32-bit applications from 64-bit applications, which includes
> preventing file and registry collisions. Console, GUI, and service applications are
> supported. The system provides interoperability across the 32/64 boundary for scenarios
> such as cut and paste and COM. However, 32-bit processes cannot load 64-bit DLLs for
> execution, and 64-bit processes cannot load 32-bit DLLs for execution."

— Source: [Running 32-bit Applications (Microsoft Learn)](https://learn.microsoft.com/en-us/windows/win32/winprog64/running-32-bit-applications)

> "32-bit binaries cannot be loaded into a 64-bit process and vice versa."

— Source: [Common Issues with 32-bit Applications Failing on 64-bit (Microsoft Learn)](https://learn.microsoft.com/en-us/archive/blogs/patricka/common-issues-with-32-bit-applications-failing-on-64-bit)

The WOW64 emulator itself runs in user mode and provides an interface between the 32-bit
version of `Ntdll.dll` and the kernel. The only 64-bit binaries that can be loaded into a
32-bit process are the WOW64 infrastructure DLLs themselves (`Wow64.dll`, `Wow64Win.dll`,
`Wow64Cpu.dll`, and the 64-bit `Ntdll.dll`). There is no reverse path: a 64-bit process
cannot host 32-bit code execution.

— Source: [WOW64 Implementation Details (Microsoft Learn)](https://learn.microsoft.com/en-us/windows/win32/winprog64/wow64-implementation-details)

### 1.3 Evidence: orlib20.dll is 32-bit (PE header inspection)

**Path discovered:** `C:\Program Files (x86)\TradeStation 9.5\Program\orlib20.dll`
**File size:** 4,924,400 bytes (4.7 MB)

PE header inspected by reading raw bytes via PowerShell
(`[System.IO.File]::ReadAllBytes` + `[BitConverter]`):

| Field | Offset | Raw bytes (little-endian) | Value | Interpretation |
|-------|--------|---------------------------|-------|----------------|
| DOS e_lfanew | 0x3C | `38 01 00 00` | 0x0138 (312) | PE header offset |
| PE signature | 0x0138 | `50 45 00 00` | 0x00004550 | `"PE\0\0"` — valid PE |
| **Machine** | **0x013C** | **`4C 01`** | **0x014C** | **IMAGE_FILE_MACHINE_I386 (32-bit x86)** |
| NumberOfSections | 0x013E | `09 00` | 9 (0x0009) | |
| SizeOfOptionalHeader | 0x014C | `E0 00` | 0x00E0 | 224 bytes = PE32 (not PE32+) |
| Characteristics | 0x014E | `02 21` | 0x2102 | IMAGE_FILE_EXECUTABLE_IMAGE (0x0002) \| IMAGE_FILE_32BIT_MACHINE (0x0100) \| IMAGE_FILE_DLL (0x2000) |

Full 24-byte dump at PE header offset (0x0138):
```
50 45 00 00 4C 01 09 00 5E 73 01 5C 00 00 00 00 00 00 00 00 E0 00 02 21
```

The Machine field `0x014C` is unambiguous: **IMAGE_FILE_MACHINE_I386**. This is a 32-bit
x86 PE binary. It is NOT `0x8664` (IMAGE_FILE_MACHINE_AMD64). There is no 64-bit build of
orlib20.dll — it is part of TradeStation 9.5, a 32-bit application, and we have no source
code or ability to recompile it.

This is corroborated by the project's own `docs/FEASIBILITY.md` §3, which independently
records: "orlib20.dll | Program\orlib20.dll | 32-bit PE32, image base 0x10000000, 699
exports."

### 1.4 Implication (unambiguous)

**Every viable solution requires a 32-bit process or 32-bit environment somewhere.**
There is no pure in-process 64-bit-Python solution. The user's stated preference to "use
a DLL" directly from 64-bit Python is **technically impossible** given that orlib20.dll
is 32-bit and cannot be recompiled. The real architectural choice is not "external
process vs. in-process DLL" — it is **"which 32-bit process, and how heavy is the IPC
between it and 64-bit Python?"**

The MS documentation explicitly identifies two cross-bitness mechanisms:
1. **RPC** between 32-bit and 64-bit processes (general IPC).
2. **Out-of-process COM** (32-bit LocalServer32 COM server ↔ 64-bit COM client).

All viable options are variants of one of these two mechanisms, or a simpler
stdin/stdout pipe (which is a form of RPC).

---

## 2. Option Enumeration

### 2.1 Option A — CLI subprocess wrapper (per-call spawn)

**Architecture:** 64-bit Python spawns `tsdev_cli.exe` (32-bit) as a subprocess for each
operation. The wrapper writes EasyLanguage source to a temporary file (the CLI takes a
file path, not stdin), invokes the CLI with the appropriate subcommand, captures
stdout/stderr, and maps the exit code to a result.

**Exit-code contract (from `src/tsdev_cli.cpp`):**

| Exit code | Meaning |
|-----------|---------|
| 0 | Success (component inserted/modified/deleted) |
| 1 | Usage error / unknown subcommand |
| 2 | Unknown component type / empty name |
| 3 | Source file read failure |
| 4 | orlib20 init failure |
| 5 | Component not found (delete/modify) or already exists (insert) |
| 6 | createEntry / deleteEntry failure |
| 7 | saveSource failure |
| 8 | Compilation error (stderr contains `Line X, Col Y:` messages) |
| 9 | .ell file not found after successful compile |
| 10 | CreateThread failure |

**Output format:** stdout contains `SUCCESS: <type> "<name>" inserted/modified/deleted,
ProgId=N, .ell=<path>`. stderr contains error messages including compiler errors with
`Line X, Col Y:` prefixes.

**Implementation:** Pure Python — a wrapper class using `subprocess.run()`. No new C++
code. The CLI already exists, is tested (34 E2E CLI test cases (Part A-C) + Workstream F E2E tests (exists/get-source/upsert/batch-stdin/exists-ci) — see `docs/E2E_TEST_SPEC.md`),
and is production-approved.

### 2.2 Option B — CLI daemon / `--serve` mode (persistent 32-bit server)

**Architecture:** A new 32-bit executable (`tsdev_serve.exe`) or an enhanced `tsdev_cli`
with a `--serve` flag runs as a long-lived process. It links against the existing
`tsdev_lib` static library (no duplication of orlib20 integration logic). It speaks a
structured JSON protocol over stdin/stdout (newline-delimited JSON, one request/response
per line). The 64-bit Python client spawns this server once, sends requests via the
pipe, and receives structured JSON responses.

**Protocol design (concrete):**

Transport: stdin/stdout pipe (inherited by the child process). Newline-delimited JSON
(NDJSON). One JSON object per line. This is the simplest reliable framing for
pipe-based IPC — no need for length prefixes, named pipes, or TCP sockets.

Request format:
```json
{"id": 1, "method": "insert", "params": {"type": "strategy", "name": "MyStrat", "source": "..."}}
{"id": 2, "method": "delete", "params": {"type": "function", "name": "MyFunc"}}
{"id": 3, "method": "modify", "params": {"type": "indicator", "name": "MyInd", "source": "..."}}
```

Response format (success):
```json
{"id": 1, "ok": true, "progId": 42, "ellPath": "C:\\...\\elmodule_0_0042.ell"}
```

Response format (failure):
```json
{"id": 1, "ok": false, "errorCode": "compile_error", "exitCode": 8, "message": "Line 3, Col 5: ...", "errors": ["Line 3, Col 5: ...", "Line 7, Col 1: ..."]}
```

Error codes map to CLI exit codes: `not_found` (5), `already_exists` (5), `compile_error`
(8), `init_failed` (4), `create_failed` (6), `save_failed` (7), `internal_error` (10).

**Thread architecture:** The server constructs a single `TsDevLib` instance on a
persistent worker thread with a 64 MB stack (the compiler requires a large stack, and
the hidden EDIT control is thread-affine — see `src/tsdev_cli.cpp` lines 29, 229). The
main thread reads requests from stdin, enqueues them to the worker thread via a
message queue, and writes responses to stdout. This amortizes all DLL initialization
(OrlibInit, signature scan, patch application, support DLL pre-loading, EDIT control
creation) across all requests.

**Lifecycle:** Python spawns the server, sends requests, and terminates it by closing
stdin (EOF) or sending a `{"method": "shutdown"}` request. The server exits cleanly
(TsDevLib destructor calls OrlibExit, restores patches, destroys EDIT control).

**Crash detection & recovery:** The Python client detects a daemon crash via a broken
pipe (write/read on the stdin/stdout pipe raises `BrokenPipeError` / `OSError`) or by
monitoring the subprocess exit code (`Popen.poll()` returning non-None unexpectedly).
In-flight requests fail fast — the client raises a `TsDevDaemonError` (a subclass of
the wrapper's exception hierarchy) so the caller can react immediately rather than
hang. Reconnection / restart of the daemon is the client's responsibility: the wrapper
exposes a `restart()` method that re-spawns the server process, but does not auto-retry
the failed request (the caller decides whether to retry). A lightweight health-check
(e.g. a `{"method": "ping"}` request on start-up and optionally before each call) can
be added if liveness probing beyond pipe-error detection is needed.

**Implementation effort:** ~200–400 lines of new C++ (JSON parsing with the already-
vendored nlohmann/json, the serve loop, the message queue, the protocol handler). All
orlib20 integration logic is reused from `tsdev_lib`. The Python client is a thin
wrapper class (~100–150 lines) that manages the subprocess and sends/receives NDJSON.

### 2.3 Option C — msl-loadlib bridge (previously rejected)

**Architecture:** msl-loadlib is a Python package that provides a client-server IPC
framework for calling 32-bit libraries from 64-bit Python. It spawns a frozen 32-bit
Python server process that loads the 32-bit DLL via ctypes, and the 64-bit Python
client communicates with it via pickle-serialized messages over TCP.

— Source: [msl-loadlib documentation](https://msl-loadlib.readthedocs.io/en/stable/)

**Previous rejection rationale (from `docs/archive/HANDOFF_D_PYTHON_INTEGRATION.md`):**
"high complexity (32-bit server subprocess, frozen-executable dependency, custom C ABI
DLL layer, cross-process IPC), fragile dependencies, and unnecessary given that the CLI
already solves the hard problems."

**Re-evaluation — does a leaner variant change the calculus? No.**

The fundamental problem is that orlib20.dll is NOT a simple ctypes-callable library.
The existing `tsdev_lib.cpp` (1,120 lines) exists precisely because orlib20 requires:
- Pre-loading 7 support DLLs (`ECL.dll`, `ORELUI20.dll`, `orUI20.dll`, `elobjmod.dll`,
  `elwinforms.dll`, `tschartxelobjmdl.dll`, `ELDebugger.dll`) — see `src/tsdev_lib.cpp`
  lines 31–39.
- Applying runtime patches to 3 thread-check INT3 sites and 1 persistence patch site
  (signature-scanned RVAs, validated against original byte patterns).
- Calling `_OrlibInit@8` with a NULL sentinel (not a path string) and context ID 1.
- Creating a hidden Win32 EDIT control (thread-affine, requires a message pump).
- Constructing a `CELCompiler` object (88 bytes, `__fastcall` constructor) with
  specific field initialization before calling `VerifyModule_New`.
- Calling `StartCompilingProcess` / `VerifyModule_New` / `EndCompilingProcess` in
  sequence, wrapped in SEH (`__try`/`__except`) handlers.
- Reading compiler errors from a global error array via direct memory access (the
  `GetCompilingErrors` export uses a `std::vector` ABI that is impractical across DLL
  boundaries — see `src/tsdev_lib.cpp` lines 153–163).
- Running all operations on a 64 MB-stack worker thread.
- A delete orchestrator calling an unexported internal function (`FUN_101a8360`) via
  `__fastcall`, followed by `InitGlobalLookup` + `DeleteFromTree` via `__thiscall`
  (emulated with inline asm).

Reimplementing all of this in Python via ctypes would be a massive undertaking —
ctypes cannot express `__fastcall`, `__thiscall`, SEH handlers, inline asm, or the
CELCompiler object construction. A "leaner" msl-loadlib variant would still need a
custom C ABI wrapper DLL (a thin C-callable shim around the orlib20 primitives), which
is itself substantial new code, plus the frozen 32-bit Python server, plus the
pickle-over-TCP IPC. This is strictly more complex than Option B (which reuses the
existing tested C++ `tsdev_lib` and uses simpler stdin/stdout JSON IPC).

**Verdict: Rejection confirmed.** msl-loadlib is designed for simple ctypes-callable
DLLs, not for DLLs with complex ABIs requiring SEH, thread-affine UI controls, runtime
patching, and inline asm. Using it here would require building a custom C wrapper DLL
anyway, at which point the msl-loadlib layer adds complexity without value.

### 2.4 Option D — Custom minimal 32-bit surrogate + lean IPC

**Architecture:** A tiny 32-bit executable (not the existing CLI) that loads orlib20
directly and exposes a minimal RPC over named pipes or stdin/stdout.

**Comparison to B:** This is architecturally identical to Option B but built from
scratch instead of enhancing the existing CLI / linking against `tsdev_lib`. The
distinction is:

- If the surrogate reimplements orlib20 integration from scratch: it duplicates 1,120+
  lines of hard-won, tested, reverse-engineered logic (`tsdev_lib.cpp` +
  `orlib_resolver.cpp`). This is strictly worse than B.
- If the surrogate links against `tsdev_lib` (the static library): it IS Option B with
  a different name and a potentially different transport (named pipes instead of
stdin/stdout). The only difference is the IPC transport choice.

**Named pipes vs. stdin/stdout:** Named pipes (`\\.\pipe\tsdev`) offer the same
semantics as stdin/stdout pipes but require the server to create and listen on a named
pipe, and the client to connect to it. This adds complexity (pipe name management,
security descriptors, connection handling) with no benefit for a single-client,
local-machine use case. stdin/stdout pipes are inherited automatically, require no
name management, and are the standard pattern for subprocess IPC in Python
(`subprocess.Popen` with `stdin=PIPE, stdout=PIPE`).

**Verdict:** Subsumed by Option B. A from-scratch surrogate that doesn't reuse
`tsdev_lib` is strictly worse (duplication). A surrogate that does reuse `tsdev_lib`
is Option B with a cosmetic difference. No independent merit.

### 2.5 Option E — COM out-of-process (LocalServer32) server

**Architecture:** A 32-bit COM server (EXE) wrapping orlib20, registered with
`LocalServer32`, called from 64-bit Python via `pywin32` (`win32com.client.Dispatch`).
COM handles all cross-process marshaling automatically.

**Feasibility of cross-bitness COM:** Confirmed by Microsoft documentation:
> "On 64-bit Windows, an out-of-process 32-bit COM server can communicate with a
> 64-bit client."

— Source: [Process Interoperability (Microsoft Learn)](https://learn.microsoft.com/en-us/windows/win32/winprog64/process-interoperability)

This is also confirmed by real-world practice: MS Office (32-bit) communicates with
64-bit clients via out-of-process COM routinely.

**pywin32 maturity:** pywin32 is actively maintained. Latest release: Build 312
(released 2026-06-04). It has ~54.6M monthly PyPI downloads, 5,583 GitHub stars, and
supports Python 3.9+. The `win32com.client.Dispatch` API for out-of-process COM is
mature and widely used.

— Sources: [pywin32 PyPI](https://pypi.org/project/pywin32/),
[pywin32 GitHub releases](https://github.com/mhammond/pywin32/releases),
[pywin32 CHANGES.md](https://github.com/mhammond/pywin32/blob/main/CHANGES.md)

**Existing COM server (ELIS.exe) — NOT viable:** TradeStation ships `ELIS.exe`, a
32-bit ATL/C++ COM LocalServer wrapping orlib20 (ProgID `ELIsolatedServices.ELServices`,
CLSID `{44D304D4-55F4-455C-B159-FB88DD525B47}`). However, per `docs/FEASIBILITY.md` §4,
it has critical functional gaps:
- No name-to-ProgId resolution (all methods require `(WHType, ProgId)` — no lookup).
- `ImportSourceCode` takes an XML file path, not raw EasyLanguage source.
- `DeleteAnalysisTechnique` returns void (no not-found/error signal).
- `VerifyAll` verifies the entire library, not a targeted item (no per-item compile
  error retrieval).
- No modify primitive.
- Possible TradeStation Desktop dependency (not confirmed headless).

**Verdict: ELIS.exe cannot be used as-is.** A custom COM server would need to be built.

**Custom COM server costs:**
- Define a COM interface (IDL or Python `win32com.server`): methods for insert/delete/
  modify with structured parameters and return values.
- Implement the server in 32-bit Python (using `win32com.server`) or C++ (ATL).
  If Python: the server would need to load orlib20 via the same complex ABI as
  `tsdev_lib` — but ctypes cannot handle `__fastcall`, `__thiscall`, SEH, or the
  CELCompiler construction (same problem as Option C). If C++ (ATL): link against
  `tsdev_lib`, but ATL COM server development is significantly more complex than a
  stdin/stdout JSON server.
- Register the COM server in the registry (`HKEY_CLASSES_ROOT\CLSID\{...}\LocalServer32`).
  This requires administrator privileges (or per-user HKCU registration, which is less
  robust).
- Handle COM threading: orlib20 uses a thread-affine EDIT control and requires a 64 MB
  stack. COM STA (Single-Threaded Apartment) would serialize calls on one thread, but
  the thread stack size is controlled by COM, not easily set to 64 MB. This is a real
  technical risk.
- The 64-bit Python client uses `win32com.client.Dispatch` — clean API, but adds a
  pywin32 dependency.

**Specific failure modes:**
- COM registration can fail silently if the registry is locked, if a previous
  registration is stale, or if the executable path changes (e.g., after an update).
  Debugging COM registration issues is notoriously painful.
- The 64-bit client may pick up a stale `InprocServer32` key (pointing to a 32-bit DLL)
  instead of `LocalServer32`, causing a "class not registered" or "BadImageFormatException"
  error. This is a known issue documented in Stack Overflow discussions of cross-bitness
  COM.
  — Source: [SO: 64 bit python is building a 32 bit COM server](https://stackoverflow.com/questions/67462529/64-bit-python-is-building-a-32-bit-com-server)
- COM surrogate (`dllhost.exe`) timeouts: if the server doesn't register its class object
  within 60 seconds, COM times out.
  — Source: [LocalServer32 (Microsoft Learn)](https://learn.microsoft.com/en-us/windows/win32/com/localserver32)

### 2.6 Option F — Require 32-bit Python (baseline/contrast)

**Architecture:** The consumer runs 32-bit Python and loads orlib20 via ctypes directly
(in-process). No IPC, no external process.

**Why this violates the requirement:** The user explicitly requires "64-bit Python (to
integrate in other Python applications)." Most modern Python applications are 64-bit;
many key libraries (numpy, pandas, etc.) have limited or no 32-bit wheel support.
Forcing 32-bit Python on consumers would severely limit integration.

**Additional technical barrier:** Even in 32-bit Python, calling orlib20 via ctypes
directly is impractical for the same reasons as Option C: orlib20 requires `__fastcall`,
`__thiscall`, SEH, inline asm, runtime patching, a thread-affine EDIT control, a 64 MB
stack, and CELCompiler object construction. ctypes cannot express these. A C extension
module (`.pyd`) would be needed — essentially porting `tsdev_lib.cpp` to a Python C
extension, which is substantial new code.

**Verdict: Rejected as a primary option** (violates the 64-bit Python requirement).
Included only as a baseline contrast to quantify what the "in-process" ideal would cost:
it would require a 32-bit Python C extension reimplementing all of `tsdev_lib`, and
would force consumers into 32-bit Python.

### 2.7 Option G — 32-bit Python child with IPC (variant of B/D)

**Architecture:** 64-bit Python spawns a 32-bit Python child process. The child loads
orlib20 via a 32-bit C extension module (`.pyd`) that wraps `tsdev_lib`, and
communicates with the parent via stdin/stdout JSON or a pipe.

**Comparison to B:** This is functionally identical to Option B but uses 32-bit Python
as the surrogate host instead of a 32-bit C++ executable. The differences:

- **Pro:** The 32-bit Python child can use Python-level JSON parsing, error handling,
  and protocol logic (no need for nlohmann/json in C++).
- **Con:** Requires building a 32-bit C extension module (`.pyd`) that wraps
  `tsdev_lib`. This is a new build artifact with its own toolchain requirements
  (32-bit Python dev headers, 32-bit compiler). The `.pyd` must be compiled for the
  exact 32-bit Python version the consumer uses.
- **Con:** Requires distributing a 32-bit Python runtime (or assuming one is installed).
  The frozen 32-bit Python server approach (as used by msl-loadlib) adds a
  ~30 MB dependency.
- **Con:** Python startup time (~100–200 ms) adds to the server startup latency,
  though this is amortized in daemon mode.

**Verdict:** Viable but strictly more complex than Option B.** Option B uses a single
self-contained 32-bit C++ executable (already buildable via the existing CMake setup)
with no Python runtime dependency on the 32-bit side. Option G introduces a 32-bit
Python runtime dependency, a 32-bit C extension build, and Python version coupling —
all for the marginal benefit of writing protocol logic in Python instead of C++.

---

## 3. Evaluation Matrix

Scoring scale: **1–5** where **5 = best** (lowest complexity, lowest brittleness,
highest performance, best ergonomics, highest fidelity, lowest dependencies, lowest
install friction, lowest maintenance).

| Axis | A: CLI subprocess | B: Daemon/serve | C: msl-loadlib | D: Custom surrogate | E: COM LocalServer | F: 32-bit Python | G: 32-bit Py child |
|------|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
| **Implementation complexity** | **5** — zero new C++; ~100 LoC Python | **3** — ~200–400 LoC C++ + ~150 LoC Python | **1** — custom C ABI DLL + frozen server + Python wrapper | **2** — duplicates orlib20 logic or == B | **2** — COM server + IDL + registration | **2** — C extension port of tsdev_lib | **2** — 32-bit .pyd + 32-bit Python runtime |
| **Brittleness / fragility** | **3** — string parsing of stdout/stderr; temp file for source | **4** — structured JSON; no string parsing | **1** — frozen exe, pickle, TCP, C ABI | **3** — same as B if reusing tsdev_lib | **2** — COM registration, stale keys, threading model | **2** — ctypes ABI mismatch risks | **3** — Python version coupling |
| **Performance** | **2** — per-call: process spawn (~50ms) + DLL init (~200–500ms) + signature scan + patches | **5** — DLL init once; per-call: ~ms (pipe + orlib op) | **3** — init amortized but pickle/TCP overhead | **4** — same as B | **4** — init amortized; COM marshaling overhead | **5** — in-process (fastest possible) | **4** — init amortized; Python startup overhead |
| **Ergonomics for Python consumer** | **3** — subprocess.run + parse strings; temp file for source | **5** — structured JSON in/out; source passed inline | **3** — Client64 subclass pattern | **4** — same as B | **4** — win32com.client.Dispatch (clean but COM-specific) | **2** — forced 32-bit Python | **4** — same as B |
| **Error handling fidelity** | **3** — exit codes + stderr string parsing; compiler errors in stderr text | **5** — structured JSON with error codes, messages, line/col arrays | **2** — pickle of Python objects; error mapping needed | **4** — same as B | **4** — COM HRESULT + structured return | **3** — depends on .pyd wrapper quality | **4** — same as B |
| **Dependency footprint** | **5** — only the existing CLI exe; stdlib Python | **4** — new exe + stdlib Python | **1** — msl-loadlib + frozen 32-bit Python + custom C DLL | **4** — new exe (same as B) | **2** — pywin32 + COM server + registry entries | **3** — 32-bit Python + .pyd | **2** — 32-bit Python + .pyd + pywin32/IPC lib |
| **Distribution / install friction** | **5** — ship tsdev_cli.exe; user runs Python wrapper | **4** — ship tsdev_serve.exe; user runs Python wrapper | **1** — frozen 32-bit Python + custom DLL + msl-loadlib | **4** — same as B | **2** — COM registration (admin/elevated) + server exe | **1** — user must install 32-bit Python | **2** — user needs 32-bit Python + .pyd |
| **Maintenance burden** | **5** — no new C++ to maintain; CLI already tested | **4** — small C++ serve loop + protocol; tsdev_lib unchanged | **1** — 3 artifacts to maintain (C DLL, server, wrapper) | **3** — new surrogate to maintain | **2** — COM server + IDL + registration scripts | **2** — C extension port to maintain | **2** — .pyd + 32-bit Python compat |

### 3.1 Score totals (higher = better)

| Option | Total (out of 40) | Rank |
|--------|:-:|:-:|
| **B: Daemon/serve** | **34** | **1** |
| A: CLI subprocess | 31 | 2 |
| D: Custom surrogate | 28 | 3 |
| E: COM LocalServer | 24 | 4 |
| G: 32-bit Py child | 21 | 5 |
| F: 32-bit Python | 20 | 6 |
| C: msl-loadlib | 12 | 7 |

### 3.2 Qualitative axis discussion

**Implementation complexity:** A wins (zero new C++). B is moderate (new serve loop +
JSON, but reuses all orlib20 logic). C, D, E, F, G all require substantial new code or
build artifacts.

**Brittleness:** A's main weakness is stdout/stderr string parsing — if the CLI output
format changes (e.g., a new prefix, a different success message format), the Python
wrapper breaks silently. However, this risk is materially lower than parsing a
third-party tool's output: the CLI output format is under this project's own control
(`src/tsdev_cli.cpp`), is deterministic, and will not change unless this project itself
changes it — so any format change would be intentional and visible in the same
repository as the Python wrapper. B eliminates this risk entirely with structured JSON.
E (COM) has registration-related fragility (stale registry keys, InprocServer32 vs
LocalServer32 conflicts) that is notoriously hard to debug.

**Performance:** A pays full DLL init cost per call. Based on the CLI's architecture
(`src/tsdev_lib.cpp` constructor: load orlib20, pre-load 7 support DLLs, signature-scan
for ~15 function RVAs, validate + apply 4 patch sites, call OrlibInit, create EDIT
control), initialization is estimated at 200–500 ms per invocation. Process spawn adds
~50 ms. **Measured (Option A, real CLI):** `tsdev_cli.exe delete strategy
__NonExistentBenchmark__` (exercises full DLL init + orlib init + not-found lookup,
exit code 5, side-effect-free — no warehouse modification) — **measured median = 616 ms
over 5 runs** (raw: 837, 610, 616, 619, 570 ms). This confirms the per-call cost is at
the upper end of / slightly above the estimate (the estimate's 200–500 ms DLL-init +
~50 ms spawn = 250–550 ms; measured ~616 ms median, first run ~837 ms likely due to
cold OS file-cache effects). For a batch of 10 operations, A pays this 10 times
(~6.2 s measured overhead); B pays it once (~250–550 ms overhead, estimated — not yet
measured since the daemon does not exist). For single-call use, the difference is
~616 ms (measured, A) vs. ~250–550 ms (estimated, B) — likely acceptable. For batch
use, B is significantly better.

**Error handling fidelity:** A can surface compiler errors (they're in stderr with
`Line X, Col Y:` prefixes) but requires parsing free-text output. B encodes errors as
structured JSON with an `errors` array — each error is a separate string, preserving
the `Line X, Col Y:` prefix. Both can faithfully surface compiler errors; B does so
more robustly.

---

## 4. Recommendation

### 4.1 Honest reconciliation of the user's preference

The user's stated preference is to "avoid interacting with an external process" and
"use a DLL." **This is technically impossible.** A 64-bit Python process cannot load
the 32-bit orlib20.dll in-process (§1.2–1.4). There is no workaround, no clever
trick, and no library that changes this — it is a fundamental Windows architectural
constraint confirmed by Microsoft documentation and verified by PE header inspection.

**An external 32-bit process is unavoidable.** The real choice is: which 32-bit
process, and how heavy is the IPC between it and 64-bit Python?

### 4.2 Top 2 finalists (no single clear winner)

The evaluation identifies **Option A (CLI subprocess wrapper)** and **Option B
(daemon/serve mode)** as the clear top 2. They score 31 and 34 out of 40 respectively.
The other options are dominated:

- **C (msl-loadlib)** is rejected (§2.3) — orlib20's ABI is too complex for ctypes.
- **D (custom surrogate)** is subsumed by B (§2.4) — no independent merit.
- **E (COM)** scores lower due to registration fragility and threading complexity.
- **F (32-bit Python)** violates the core requirement.
- **G (32-bit Python child)** is strictly more complex than B with no benefit.

#### Finalist 1: Option A — CLI subprocess wrapper

| Pros | Cons |
|------|------|
| Zero new C++ code — uses the existing, tested, production-approved CLI | Per-call cost: ~250–550 ms (process spawn + DLL init + signature scan + patches) |
| Simplest possible Python wrapper (~100 LoC) | Source must be written to a temp file (CLI takes a file path, not stdin) |
| No daemon lifecycle management (fire-and-forget) | Stdout/stderr string parsing is brittle if output format changes |
| Trivial to distribute (one exe + one .py) | No amortized init — every call reinitializes orlib20 from scratch |
| Easy to test (the CLI is already E2E-tested with 34 cases) | Exit codes are structured but error details are free-text in stderr |
| No long-running process to manage/monitor/clean-up | Compiler errors are in stderr text (parseable but not structured) |

#### Finalist 2: Option B — Daemon / `--serve` mode

| Pros | Cons |
|------|------|
| Amortizes DLL init — ~250–550 ms once, then ~ms per call | Requires ~200–400 LoC new C++ (serve loop + JSON + message queue) |
| Structured JSON I/O — no stdout/stderr string parsing | Daemon lifecycle management (start, health, shutdown, crash recovery) |
| Source passed inline in JSON — no temp file | New build artifact (`tsdev_serve.exe`) to build, test, and distribute |
| Error codes + structured error arrays with Line/Col | Must handle worker-thread message queue (thread-affine EDIT control) |
| Best performance for batch operations | More complex to test than A (protocol tests in addition to orlib tests) |
| Clean Python API (JSON in, JSON out) | Slightly more maintenance burden than A |

### 4.3 Decision criteria for the user

The choice between A and B depends on two questions the user must answer:

1. **Is per-call latency of ~250–550 ms acceptable?** If operations are infrequent
   (e.g., a user-triggered action in a UI), A's latency is likely fine. If operations
   are batched (e.g., inserting 50 strategies programmatically), B's amortized init
   saves ~12–27 seconds.

2. **Is the investment of ~200–400 LoC of new C++ justified?** B requires building and
   testing a new serve mode. If the project values minimal new code and fast time-to-
   delivery, A is the pragmatic choice. If structured I/O and performance matter for
   the downstream Python applications, B is the better long-term architecture.

**If the user values simplicity and fast delivery above all:** Choose **A**. It is the
lowest-risk, lowest-effort option that fully satisfies the functional requirements. The
per-call latency is the main trade-off.

**If the user values performance, structured I/O, and long-term ergonomics:** Choose
**B**. It is the better architecture for frequent/batched use and provides structured
error handling without string parsing. The cost is moderate new C++ code.

**A pragmatic hybrid path:** Start with A (immediate value, zero new C++), and if
latency or string-parsing brittleness becomes a problem, upgrade to B. The Python
wrapper API can be designed to be identical for both (the wrapper internally chooses
subprocess-per-call or daemon mode), so downstream Python applications see no change.

---

## 5. Risks & Open Questions for PM/User

### 5.1 Questions requiring a user decision

| # | Question | Context |
|---|----------|---------|
| Q1 | **Is per-call latency of ~250–550 ms acceptable?** | This is A's main trade-off. Determines whether A is sufficient or B is needed. |
| Q2 | **Are operations typically single or batched?** | Batched operations (e.g., inserting 50 strategies) favor B. Single operations favor A. |
| Q3 | **Is a long-running daemon acceptable?** | B requires a persistent 32-bit process. Some environments may restrict long-running subprocesses. |
| Q4 | **Is ~200–400 LoC of new C++ acceptable?** | B's implementation cost. A requires zero new C++. |
| Q5 | **Is COM registration acceptable?** | Only relevant if E is reconsidered. Requires admin/elevated privileges. (Not recommended.) |
| Q6 | **Is requiring 32-bit Python acceptable?** | Only relevant for F. (Violates the stated requirement — not recommended.) |
| Q7 | **Should the Python wrapper support both A and B?** | The hybrid path: design the wrapper API to work with both, defaulting to A, with an option to use B. |

### 5.2 Technical risks (all options)

| Risk | Options affected | Mitigation |
|------|------------------|------------|
| orlib20.dll version change (TradeStation update) breaks signature scan | A, B, D, E, G | The existing OrlibResolver signature system handles known versions. A new TS version requires updating `signatures/*.json`. This is an existing risk, not new to Python integration. |
| `MWRKArea.wrk` is not process-safe (concurrent operations corrupt the warehouse) | All | The existing test suite runs tests sequentially for this reason. The Python wrapper must serialize operations. A daemon (B) naturally serializes via its single worker thread. |
| CLI output format change breaks string parsing | A only | B eliminates this risk with structured JSON. |
| Daemon crash leaves orphaned process | B only | Python wrapper monitors the process; restarts on crash; or uses a watchdog. |
| Temp file cleanup failure (source file left behind) | A only | Use `tempfile.NamedTemporaryFile` with `delete=True` or try/finally cleanup. B passes source inline — no temp file. |
| 64 MB stack requirement for worker thread | B only | The serve mode must create the worker thread with a 64 MB stack (same as the CLI does). This is a proven pattern. |

---

## 6. Next-Step Proposal

### 6.1 If the user selects Option A (CLI subprocess wrapper)

Implementation TVSs (TDD, atomic):

| TVS | Description | Test |
|-----|-------------|------|
| TVS-D.1 | Python wrapper class `TsDevClient` with `insert()`, `delete()`, `modify()` methods. Each method: writes source to temp file (insert/modify), spawns `tsdev_cli.exe`, captures stdout/stderr/exit code, and returns a result object (type defined in TVS-D.2); errors are raised (mapped per TVS-D.2). (Note: TVS-D.1 and TVS-D.2 are merged in practice, but D.1 is listed first to reflect the call-orchestration concern.) | Unit tests with mocked subprocess; E2E test against real CLI. |
| TVS-D.2 | Result model + exit-code mapping (atomic): define the `TsDevResult` dataclass (`success: bool`, `exit_code: int`, `message: str`, `prog_id: int\|None`, `ell_path: str\|None`, `compiler_errors: list[str]`) and the exception hierarchy (`TsDevError` base; `TsDevInitError` for init failure, `TsDevCompileError` for compile errors, `TsDevNotFoundError` for not-found) together with the exit-code-to-result mapping (0→success, 5→not_found/already_exists, 8→compile_error with parsed stderr, others→error). Parse `SUCCESS:` line for ProgId and .ell path. Parse `Line X, Col Y:` from stderr for compiler errors. This TVS is atomic because the dataclass, exceptions, and mapping are mutually dependent and must be defined together. | Unit tests for each exit code + error mapping; E2E test with real compile error; E2E negative tests. |
| TVS-D.3 | Packaging: `pyproject.toml` or setup script. Documentation. | Manual verification. |

### 6.2 If the user selects Option B (daemon/serve mode)

Implementation TVSs (TDD, atomic):

| TVS | Description | Test |
|-----|-------------|------|
| TVS-D.1 | C++ serve mode: `tsdev_serve.cpp` — main loop reading NDJSON from stdin, dispatching to a worker thread (64 MB stack) that owns a `TsDevLib` instance, writing NDJSON responses to stdout. Link against `tsdev_lib` + nlohmann/json. | Unit test: send JSON request via pipe, verify JSON response. |
| TVS-D.2 | Protocol implementation: request parsing (`id`, `method`, `params`), response encoding (`id`, `ok`, `progId`, `ellPath`, `errorCode`, `message`, `errors`). Error code mapping. | Unit tests for each method + error case. |
| TVS-D.3 | Python client class `TsDevClient` — spawns `tsdev_serve.exe`, sends NDJSON requests via stdin, reads NDJSON responses from stdout. Lifecycle management (start, shutdown, crash detection). | Unit tests with mocked server; E2E test against real server. |
| TVS-D.4 | E2E integration test: insert → modify → delete lifecycle via Python client against real orlib20. Compiler error surfacing test. | E2E test following `docs/E2E_TEST_SPEC.md` patterns. |
| TVS-D.5 | Packaging: build `tsdev_serve.exe` via CMake, distribute with Python wrapper. Documentation. | Manual verification. |

### 6.3 If the user selects the hybrid path (A first, then B)

Start with TVS-D.1–D.3 from §6.1 (Option A). Design the `TsDevClient` API to be
transport-agnostic (an internal `_call()` method that can be backed by either
subprocess-per-call or a persistent daemon). When B is needed, implement TVS-D.1–D.5
from §6.2 and swap the internal `_call()` implementation. Downstream Python
applications see no API change.

### 6.4 If no clear winner is chosen

Implementation TVSs will be defined after the user selects an approach (A or B). The
evaluation is complete; no further research is needed. The user has sufficient
information to decide.

---

## Appendix A: Evidence Summary

| Evidence | Source | URL / Path |
|----------|--------|------------|
| 64-bit process cannot load 32-bit DLL | Microsoft Learn — Process Interoperability | https://learn.microsoft.com/en-us/windows/win32/winprog64/process-interoperability |
| 64-bit process cannot load 32-bit DLL (corroborating) | Microsoft Learn — Running 32-bit Applications | https://learn.microsoft.com/en-us/windows/win32/winprog64/running-32-bit-applications |
| 64-bit process cannot load 32-bit DLL (corroborating) | Microsoft Learn — Common Issues with 32-bit Applications | https://learn.microsoft.com/en-us/archive/blogs/patricka/common-issues-with-32-bit-applications-failing-on-64-bit |
| WOW64 implementation details | Microsoft Learn — WOW64 Implementation Details | https://learn.microsoft.com/en-us/windows/win32/winprog64/wow64-implementation-details |
| Out-of-process COM works across bitness | Microsoft Learn — Process Interoperability | https://learn.microsoft.com/en-us/windows/win32/winprog64/process-interoperability |
| LocalServer32 registration | Microsoft Learn — LocalServer32 | https://learn.microsoft.com/en-us/windows/win32/com/localserver32 |
| orlib20.dll PE header: Machine = 0x014C (I386) | PowerShell PE header inspection | `C:\Program Files (x86)\TradeStation 9.5\Program\orlib20.dll` |
| orlib20.dll is 32-bit PE32 (corroborating) | Project FEASIBILITY.md §3 | `docs/FEASIBILITY.md` |
| pywin32 actively maintained (Build 312, Jun 2026) | pywin32 GitHub releases | https://github.com/mhammond/pywin32/releases |
| pywin32 54.6M monthly downloads | PyPI / PyRank | https://pypi.org/project/pywin32/ |
| msl-loadlib IPC mechanism (pickle over TCP) | msl-loadlib documentation | https://msl-loadlib.readthedocs.io/en/stable/ |
| ELIS.exe COM server gaps | Project FEASIBILITY.md §4 | `docs/FEASIBILITY.md` |
| CLI exit codes | Project source `src/tsdev_cli.cpp` | `src/tsdev_cli.cpp` |
| CLI worker thread (64 MB stack, thread-affine EDIT) | Project source `src/tsdev_cli.cpp` lines 29, 229 | `src/tsdev_cli.cpp` |
| tsdev_lib API (orlib20 wrapper) | Project source `src/tsdev_lib.h` | `src/tsdev_lib.h` |
| orlib20 integration complexity (7 support DLLs, patches, SEH) | Project source `src/tsdev_lib.cpp` lines 31–39, 135–211 | `src/tsdev_lib.cpp` |
| CMake build system (32-bit guard, tsdev_lib static lib) | Project `CMakeLists.txt` | `CMakeLists.txt` |
| E2E test spec (34 test cases) | Project `docs/E2E_TEST_SPEC.md` | `docs/E2E_TEST_SPEC.md` |
| Previous msl-loadlib rejection | Project `docs/archive/HANDOFF_D_PYTHON_INTEGRATION.md` | `docs/archive/HANDOFF_D_PYTHON_INTEGRATION.md` |

## Appendix B: orlib20.dll PE Header Raw Bytes

```
File: C:\Program Files (x86)\TradeStation 9.5\Program\orlib20.dll
Size: 4,924,400 bytes (4.7 MB)

DOS Header:
  Offset 0x3C (e_lfanew): 38 01 00 00  → 0x00000138 (PE header at byte 312)

PE Header (at offset 0x0138):
  Offset 0x0138: 50 45 00 00           → PE signature (0x00004550 = "PE\0\0")
  Offset 0x013C: 4C 01                 → Machine = 0x014C (IMAGE_FILE_MACHINE_I386)
  Offset 0x013E: 09 00                 → NumberOfSections = 9
  Offset 0x0140: 5E 73 01 5C           → TimeDateStamp
  Offset 0x0144: 00 00 00 00           → PointerToSymbolTable
  Offset 0x0148: 00 00 00 00           → NumberOfSymbols
  Offset 0x014C: E0 00                 → SizeOfOptionalHeader = 0x00E0 (224 bytes = PE32)
  Offset 0x014E: 02 21                 → Characteristics

24-byte dump: 50 45 00 00 4C 01 09 00 5E 73 01 5C 00 00 00 00 00 00 00 00 E0 00 02 21

Conclusion: IMAGE_FILE_MACHINE_I386 (0x014C) — unambiguously 32-bit x86.
```
