# Workstream K — `--serve` Mode (Elevated Named-Pipe Broker)

**Status:** K.1–K.6b COMPLETE (Workstream K fully complete)
**Goal:** Add a `--serve` mode to `tsdev_cli.exe` that starts elevated (one UAC
prompt via the existing `requireAdministrator` manifest), creates a Windows
named pipe, and services multiple commands in a loop. The Python wrapper
connects to the pipe and sends framed JSON requests, receiving framed JSON
responses. One UAC per session, no per-call prompts, native speed, no
third-party dependency.

**Implementation status:**
- **K.1 (elevation guard):** COMPLETE — `isElevated()` helper, exit code 11, "not_elevated" error string, guard in main() before all subcommands including serve. Test: test_elevation_guard.cpp. Review APPROVED.
- **K.2 (cliResultToJson):** COMPLETE — Extracted JSON serialization from emitOutput() into `cliResultToJson(const CliResult&)`. emitOutput() and emitBatchOutput() refactored to use it. Byte-identical output preserved. Test: test_json_serialize.cpp. Review APPROVED.
- **K.3 (serve dispatch):** COMPLETE — `ServeRequest` struct, `parseServeRequest()`, `dispatchServeRequest()`. Test: test_serve_dispatch.cpp. Review APPROVED.
- **K.4 (named pipe server):** COMPLETE — Framing helpers, createServePipe, runServe, cmdServe, `serve` subcommand. Test: test_serve_framing.cpp. Review APPROVED.
- **K.5 (Python serve-broker):** COMPLETE — _pipe.py, _ServeBroker, _detect_elevation(), use_serve=False, routing logic. 115 Python tests pass. Review APPROVED (after fixing 3 MAJOR findings: 64MB cap in recv_response, orphaned broker on start failure, reconnect process leak).
- **K.6a (E2E integration): COMPLETE (2026-08-21)** — Full E2E with serve mode, backwards-compat verification, elevation guard E2E: all 7 phases passed; adversarial review APPROVED; committed as 027e7fe.
- **K.6b (documentation + default flip): COMPLETE (2026-08-21)** — `use_serve` default flipped to `True`; `_ServeBroker.start()` FileNotFoundError guard added; serve-mode docs added to BUILD.md, E2E_TEST_SPEC.md (Part F), README.md; E2E 2/2 serve + 12/12 C++ regression passed; adversarial review APPROVED (17 INFO).

**Crash handling (done alongside K.5, not in original plan):**
A C++ VEH (Vectored Exception Handler) crash handler and Python `TsDevCrashError` exception were added to make serve mode safe. The VEH (`vehCrashHandler()` in anonymous namespace) catches access violations, stack overflows, and heap corruption, then calls `TerminateProcess(GetCurrentProcess(), code)` to prevent TSCrashReport.exe from spawning. This is critical for serve mode where the broker runs elevated and a non-elevated Python client cannot kill an elevated TSCrashReport.exe. The VEH is installed as the FIRST thing in main() via `AddVectoredExceptionHandler(1, ...)` + `SetErrorMode(SEM_NOGPFAULTERRORBOX)`. On the Python side, `TsDevCrashError` (subclass of `TsDevError` with `exit_code` attribute) is raised when crash NT status codes are detected. Tests: test_crash_handler.cpp (normal mode + --crash mode), test_crash_handler.ps1. Review APPROVED.

**Supported component types in serve mode:**
The serve dispatch (`parseServeRequest`) supports all 5 CLI component types: `strategy`, `indicator`, `function`, `tradingapp`, and `showme` (ShowMe was added via TVS-C.8, whType=1282).

**Additional requirement:** The CLI must check whether it has elevated
privileges and warn + exit if it hasn't. This applies to ALL subcommands (not
just serve), since they all access the TSDev Library in
`C:\Program Files (x86)\...`.

---

## Architecture recap (from codebase exploration)

The CLI (`src/tsdev_cli.cpp`, 2115 lines at planning time; 2517 lines now) has 3 layers:

- **`do*` functions** (`doInsert`, `doModify`, `doDelete`, `doExists`,
  `doUpsert`, `doGetSource`): take a `TsDevLib&` + parsed args, return a
  `CliResult`. Do NOT write to stdout. **This is the reuse seam.** (lines
  317–1150)
- **`cmd*` functions**: parse args, spawn a 64MB-stack worker thread,
  construct `TsDevLib`, call `do*`, emit output via `emitOutput()`. Write to
  stdout. (lines 607–1230)
- **`run*` functions**: worker-thread entry points that construct/destruct
  `TsDevLib` on the 64MB-stack thread (thread-affine constraint: the hidden
  EDIT control and the compiler both need the large stack). (lines 581–1175)

Key existing pieces:
- `CliResult` struct (line 201): `op`, `typeStr`, `name`, `success`,
  `exitCode`, `progId`, `ellPath`, `existingProgId`, `compileErrors`,
  `humanStdout`, `humanStderr`, `errorMessage`, `exists`, `storedName`,
  `sourceText`.
- `emitOutput()` (line 251): has a JSON branch that serializes `CliResult` →
  `nlohmann::ordered_json` → stdout. **This serialization logic should be
  extracted into a reusable helper** (`cliResultToJson()`).
- `emitBatchOutput()` (line 1481): duplicates the per-result JSON
  serialization (does NOT include `exists`/`stored_name`/`source` fields — a
  pre-existing minor inconsistency to be aware of).
- `runBatch` (line 1409): the existing precedent for a long-lived `TsDevLib`
  (one thread, one lib, many ops). Constructs `TsDevLib` once, loops over
  items calling `do*`, calls `closeWarehouse()` once at the end, then
  destructs `TsDevLib`.
- `#ifndef TSDEV_CLI_TEST` around `main()` (line 1740) so unit tests can
  `#include` the `.cpp` and access anonymous-namespace internals.
- `nlohmann/json.hpp` already included (line 67). Windows APIs available
  transitively via `tsdev_lib.h` (line 19: `#include <windows.h>`).
- CMakeLists.txt (line 249): `TSDEV_REQUIRE_ADMIN` option embeds
  `requireAdministrator` manifest in `tsdev_cli` and all TS-dependent test
  executables (line 282).
- Python wrapper (`src/tsdev_automation/client.py`): `TsDevClient` shells out
  via `subprocess.run(..., capture_output=True)` → `json.loads(stdout)`.
  `_run_cli()` (line 366) and `_run_batch()` (line 393) are the two execution
  paths. `TsDevResult.from_json()` (results.py line 63) parses the JSON.
- Python tests (`tests/test_tsdev_client.py`): unit tests mock
  `subprocess.run`; E2E tests marked `@pytest.mark.e2e` drive the real CLI.

---

## 1. TVS breakdown

### TVS-K.1 — Elevation guard for all subcommands — **COMPLETE**

**Goal:** Add an elevation check at the top of `main()` that warns and exits
with a clear message if the process is not elevated. Applies to ALL
subcommands (insert/modify/delete/exists/get-source/upsert/batch-*/serve).

**Files to change:**
- `src/tsdev_cli.cpp` — add `isElevated()` helper + guard in `main()`.
- `tests/test_elevation_guard.cpp` — new unit test (includes
  `tsdev_cli.cpp` with `TSDEV_CLI_TEST`).
- `CMakeLists.txt` — add `test_elevation_guard` target (NOT in
  `TSDEV_ADMIN_TARGETS` — it's a pure unit test of the guard logic, no
  orlib20 access; OR add it with a note that it must run elevated to pass
  the "is elevated" branch — see TDD approach below).
- `tests/run_unit_tests.ps1` — add `test_elevation_guard` to `$TestDefinitions` with `RequiresTs = $false`.

**Key implementation details:**
- `isElevated()`: use `OpenProcessToken(GetCurrentProcess(),
  TOKEN_QUERY, &hToken)` → `GetTokenInformation(hToken, TokenElevation,
  &elev, sizeof(elev), &ret)` → return `elev.TokenIsElevated != 0`.
  Fallback if token APIs fail: treat as not-elevated (fail safe).
- Place the guard in `main()` AFTER the global flag pre-scan but BEFORE
  subcommand dispatch (so `--json`/`--ts-path` are still parsed, and the
  error can be emitted in JSON form when `--json` is set).
- Error output: human-readable → `stderr`:
  `"tsdev_cli: ERROR — this tool requires administrator privileges. "
  "Run from an elevated terminal (or let the Python wrapper auto-elevate)."`
  Exit code: a NEW dedicated code. To stay backwards-compatible with the
  existing exit-code scheme (0–10), use **exit 11** for "not elevated".
  JSON form: `{"success":false,"exit_code":11,
  "error":{"code":"not_elevated","message":"..."}}`. Add `"not_elevated"` to
  `errorCodeString()` (line 223) as `case 11`.
- The guard does NOT apply when `TSDEV_CLI_TEST` is defined (unit tests
  include the .cpp and define their own `main()`; the guard is inside
  `#ifndef TSDEV_CLI_TEST`).

**TDD approach:**
- **Red:** `test_elevation_guard.cpp` includes `tsdev_cli.cpp`, calls
  `isElevated()` directly (anonymous-namespace function visible via the
  include), and asserts it returns a `bool`. Also tests
  `errorCodeString(11, "")` returns `"not_elevated"`. Fails to compile
  (neither symbol exists yet).
- **Green:** Add `isElevated()` + `case 11` in `errorCodeString()` + the
  guard in `main()`. Test compiles and passes.
- **Note on E2E:** When run elevated (the normal test-runner path), the
  guard passes silently — existing behavior is preserved. The "not elevated"
  branch is verified by the unit test of `isElevated()` returning false when
  mocked, and by manual inspection of the guard logic.
- **Limitation:** `isElevated()` calls real Windows token APIs and is not
  mockable. The unit test asserts it returns a `bool` and that the value
  matches the test runner's elevation state (true on elevated runner,
  false on non-elevated CI). The guard-in-`main()` path (exit 11) is
  verified only by the E2E test in K.6a, not by the unit test (guard is
  inside `#ifndef TSDEV_CLI_TEST`).

**Dependencies:** None (first TVS).

---

### TVS-K.2 — Extract `cliResultToJson()` reusable serialization helper — **COMPLETE**

**Goal:** Extract the JSON serialization branch of `emitOutput()` (lines
264–297) into a standalone `ordered_json cliResultToJson(const CliResult& r)`
function, and refactor `emitOutput()` and `emitBatchOutput()` to call it.
This eliminates the duplicated serialization logic and gives the serve layer
a single function to serialize a `CliResult` to JSON for the wire.

**Files to change:**
- `src/tsdev_cli.cpp` — extract `cliResultToJson()`, refactor
  `emitOutput()` and `emitBatchOutput()` to use it.
- `tests/test_json_serialize.cpp` — new unit test (includes
  `tsdev_cli.cpp` with `TSDEV_CLI_TEST`).
- `tests/run_unit_tests.ps1` — add `test_json_serialize` to `$TestDefinitions` with `RequiresTs = $false`.

**Key implementation details:**
- `cliResultToJson(const CliResult& r)` returns `ordered_json`. Contains
  the EXACT logic currently in `emitOutput()` lines 264–297 (including the
  `exists`/`stored_name`/`source` op-specific fields).
- `emitOutput()` JSON branch becomes: `ordered_json j = cliResultToJson(r);
  const std::string s = j.dump(); ...`.
- `emitBatchOutput()` per-result loop becomes: `arr.push_back(cliResultToJson(r))`.
  **This fixes the pre-existing inconsistency** where `emitBatchOutput()`
  omitted `exists`/`stored_name`/`source` — now batch results for
  exists/get-source ops will include those fields too. Verify no existing
  test depends on the omission (batch subcommands are insert/modify/delete/
  upsert only, so `exists`/`get-source` never appear in batch output → safe).
- Pure refactor: byte-identical JSON output for all existing paths.

**TDD approach:**
- **Red:** `test_json_serialize.cpp` calls `cliResultToJson()` on a
  hand-built `CliResult` (success insert, compile-error, exists-found,
  get-source) and asserts the exact JSON key/value structure. Fails to
  compile (function doesn't exist).
- **Green:** Extract the function, refactor callers. Test passes. Existing
  `--json` output is byte-identical (verified by E2E).

**Dependencies:** None (independent of K.1, but logically first since serve
builds on it).

---

### TVS-K.3 — Serve request dispatch: JSON request → `do*` call → `CliResult` — **COMPLETE**

**Goal:** Implement the in-process request handler that takes a parsed JSON
request, validates it, calls the appropriate `do*` function on a shared
`TsDevLib&`, and returns a `CliResult`. This is the pure logic layer with NO
pipe I/O — fully unit-testable.

**Files to change:**
- `src/tsdev_cli.cpp` — add `ServeRequest` struct, `parseServeRequest()`,
  `dispatchServeRequest()`.
- `tests/test_serve_dispatch.cpp` — new unit test (includes
  `tsdev_cli.cpp` with `TSDEV_CLI_TEST`).
- `CMakeLists.txt` — add `test_serve_dispatch` target.
- `tests/run_unit_tests.ps1` — add `test_serve_dispatch` to `$TestDefinitions` with `RequiresTs = $false`.

**Key implementation details:**
- `ServeRequest` struct: `std::string op` (insert/modify/delete/exists/
  upsert/get-source), `ComponentType type`, `std::string typeStr`,
  `std::string name`, `std::string source`, `bool caseInsensitive`,
  `std::string outputPath`.
- `parseServeRequest(const ordered_json& req, ServeRequest& out)` →
  `CliResult` (returns a usage-error `CliResult` on malformed request:
  missing `op`, unknown `op`, missing `type`/`name`, unknown type, etc.).
  Maps request fields to the struct. `source` is the raw EL source text
  (NOT a file path — the Python wrapper reads files client-side and sends
  the text; this avoids file-path round-trips through the broker).
- `dispatchServeRequest(TsDevLib& lib, const ServeRequest& req)` →
  `CliResult`: switches on `req.op` and calls the corresponding `do*`
  function. For insert: `doInsert(lib, type, typeStr, name, source,
  /*flushPerItem=*/true)` (persists immediately, matching single-item CLI
  semantics). For modify/upsert: `doModify`/`doUpsert` do NOT flush
  internally (no `flushPerItem` param on `doModify`), so
  `dispatchServeRequest` calls `lib.closeWarehouse()` AFTER every mutating
  op (insert/modify/delete/upsert) to ensure persistence. This matches
  single-item CLI behavior where each op is persisted before the process
  exits. For get-source/exists (read-only): no flush needed. For
  get-source with `outputPath`: `doGetSource` writes to the file (the path
  is broker-side; the Python wrapper normally omits it and reads
  `sourceText` from the response).
- **MVP scope:** single-item ops only (insert/modify/delete/exists/upsert/
  get-source). Batch ops (`batch-insert`, etc.) are NOT served over the pipe
  in the MVP — the Python wrapper sends N individual requests. Batch-over-
  pipe is deferred (see §6).

**TDD approach:**
- **Red:** `test_serve_dispatch.cpp` calls `parseServeRequest()` with
  valid and malformed JSON, asserts the parsed struct / error `CliResult`.
  Calls `dispatchServeRequest()` with a mock/fake `TsDevLib` — BUT `TsDevLib`
  is not mockable (concrete class, thread-affine). **Resolution:** test
  `parseServeRequest()` thoroughly (pure logic, no `TsDevLib`); test
  `dispatchServeRequest()` only via the integration path (K.5/K.6a E2E with
  the real `TsDevLib`). The dispatch switch is trivial (one line per op), so
  the risk is low; the real validation is E2E.
- **Green:** Implement `ServeRequest`, `parseServeRequest()`,
  `dispatchServeRequest()`. Unit test for `parseServeRequest()` passes.

**Dependencies:** K.2 (uses `cliResultToJson` indirectly via the response
framing in K.4; but `dispatchServeRequest` itself only returns `CliResult`).

---

### TVS-K.4 — Named pipe server: create, accept, frame I/O, serve loop — **COMPLETE**

**Goal:** Implement the `--serve` subcommand: create a named pipe with the
correct security descriptor (Medium-IL + user DACL), accept connections,
read length-prefixed JSON requests, dispatch them via K.3 on a long-lived
64MB-stack worker thread, write length-prefixed JSON responses, loop until
the pipe is closed / `quit` op / process killed.

**Files to change:**
- `src/tsdev_cli.cpp` — add `cmdServe()`, `runServe()` (worker thread),
  pipe-creation + framing helpers.
- `tests/test_serve_framing.cpp` — new unit test for the framing
  read/write helpers (includes `tsdev_cli.cpp` with `TSDEV_CLI_TEST`).
- `CMakeLists.txt` — add `test_serve_framing` target.
- `tests/run_unit_tests.ps1` — add `test_serve_framing` to `$TestDefinitions` with `RequiresTs = $false`.

**Key implementation details:**
- **Pipe name:** `\\.\pipe\tsdev_cli_<PID>` where `<PID>` is the broker's
  process ID (unique per broker instance, avoids collisions). The broker
  prints the pipe name to stdout on startup so the Python wrapper can parse
  it: `{"pipe":"\\\\.\\pipe\\tsdev_cli_12345"}` followed by newline. (The
  Python wrapper launched the broker via `ShellExecute("runas")` and reads
  the broker's stdout — but `ShellExecute` does NOT give a stdout pipe. See
  §4 for the handshake: the wrapper probes `\\.\pipe\tsdev_cli_<PID>` using
  the PID it captures from the launched process, OR the broker writes the
  pipe name to a temp file whose path is passed as `--serve-pipe-file`.)
  **Chosen approach:** `--serve-pipe-file <path>` — the broker writes the
  full pipe name to that file once the pipe is created; the Python wrapper
  polls that file. This is robust and avoids PID-guessing. The pipe name
  still embeds the PID for uniqueness. The broker writes the full pipe name
  AND its own PID to that file (format: first line = pipe name, second line
  = PID as decimal string).
- **Security descriptor (SDDL):** `D:P(A;;GA;;;BA)(A;;GWGR;;;SY)(A;;GWGR;;;`
  `OW)` + `S:(ML;;NW;;;ME)` — owner + admins + system full control, owner
  (the calling user) read/write, mandatory label Medium No-Write-Up. Built
  via `ConvertStringSecurityDescriptorToSecurityDescriptorW()` and passed
  to `CreateNamedPipeW()` via `SECURITY_ATTRIBUTES`. See §3 for details.
- **Pipe creation flags:** `CreateNamedPipeW` called with
  `dwOpenMode = PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE`,
  `dwPipeMode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT`
  (blocking mode; length-prefix framing handles message boundaries),
  `nMaxInstances = 1` (single instance; MVP is single-broker).
- **Framing:** 4-byte little-endian length prefix + UTF-8 JSON payload.
  `writeFramed(HANDLE pipe, const std::string& json)` and
  `readFramed(HANDLE pipe, std::string& out)` helpers. Length capped at
  64MB (generous; EL source can be large but not THAT large). On
  `readFramed` failure (broken pipe, length > cap, short read) → return
  false → close that client connection.
- **Serve loop (`runServe`, the 64MB-stack worker thread):**
  1. Construct ONE `TsDevLib` (lives for the whole serve session, like
     `runBatch`).
  2. `while (!g_serveQuit)`: `ConnectNamedPipe()` (blocking, one client at a
     time — MVP is single-client; see §6 for multi-client).
  3. `while (readFramed())`: parse JSON → `parseServeRequest()` → if
     `op == "quit"` → set `g_serveQuit = true`, break inner loop. Else
     `dispatchServeRequest(lib, req)` → `cliResultToJson(result)` →
     `writeFramed()`.
  4. `FlushFileBuffers()` + `DisconnectNamedPipe()` → check `g_serveQuit`:
     if true, break outer loop; else back to step 2.
  5. On outer loop exit: `closeWarehouse()` + `delete lib`.
- **`cmdServe()`:** parses `--serve-pipe-file <path>` (optional; if absent,
  the pipe name is printed to stdout but no file is written — for manual
  use). Creates the pipe with the security descriptor. Launches `runServe`
  on a 64MB-stack thread. Waits. `g_serveQuit` is a `std::atomic<bool>`
  set by the WORKER THREAD (in `runServe`) when it receives the `quit` op.
  The main thread waits on the worker thread handle.
- **`--ts-path` handling:** `cmdServe` receives `tsPathWidePtr` from the
  global pre-scan (same as all other subcommands) and passes it to
  `runServe`, which forwards it to the `TsDevLib` constructor. The Python
  wrapper passes `--ts-path` when launching the broker via
  `ShellExecuteExW` if `TsDevClient(ts_path=...)` was set.
- **`main()` dispatch:** add `if (iequals(subcommand, "serve"))` branch
  BEFORE the existing subcommand dispatch. The elevation guard (K.1) runs
  first. `serve` takes optional `--serve-pipe-file <path>`.

**TDD approach:**
- **Red:** `test_serve_framing.cpp` tests `writeFramed`/`readFramed` using
  an in-memory buffer (or a pair of anonymous pipes via `CreatePipe`).
  Asserts: round-trip of a JSON string, length-prefix is little-endian,
  empty payload works, oversized payload is rejected. Fails to compile
  (helpers don't exist).
- **Green:** Implement framing helpers + `cmdServe` + `runServe`. Unit
  test passes. Full E2E (K.6a) validates the pipe end-to-end.
- **Adversarial review focus:** thread-affine correctness (TsDevLib on the
  64MB thread only), pipe security (SDDL correctness), framing robustness
  (partial reads, broken pipe), shutdown semantics.

**Dependencies:** K.1 (elevation guard), K.2 (cliResultToJson), K.3
(dispatch).

---

### TVS-K.5 — Python wrapper: serve-broker lifecycle + pipe client — **COMPLETE**

**Goal:** Extend `TsDevClient` to detect elevation, auto-start the serve
broker (via `ShellExecute("runas")`) when not elevated, connect to the named
pipe, send/receive framed JSON, and route all operations through the broker.
Fall back to direct `subprocess.run` when already elevated (zero overhead in
the common elevated-session case).

**Files to change:**
- `src/tsdev_automation/client.py` — add `_ServeBroker` class, elevation
  detection, pipe client, route `_single`/`exists`/`get_source` through the
  broker when active.
- `src/tsdev_automation/_pipe.py` — new module: framed-JSON pipe I/O
  (pure-Python, stdlib only: `ctypes` for `CreateFileW`/`WriteFile`/
  `ReadFile`, `struct` for length prefix).
- `tests/test_tsdev_client.py` — add unit tests for the pipe client and
  broker lifecycle (mocked).
- `tests/test_serve_e2e.py` — new E2E test (marked `@pytest.mark.e2e`).

**Key implementation details:**
- **Elevation detection:** `ctypes.windll.advapi32.OpenProcessToken` +
  `GetTokenInformation(TokenElevation)` — mirrors the C++ `isElevated()`.
  Cached on the `TsDevClient` instance.
- **Broker start (`_ServeBroker`):**
  1. Create a temp file path for the pipe-name handshake.
  2. `ShellExecuteExW` with `SEE_MASK_NOCLOSEPROCESS` + `lpVerb="runas"` +
     `lpFile=cli_path` + `lpParameters=f"--serve --serve-pipe-file {tmp}"`
     + `nShow=SW_HIDE` → triggers ONE UAC prompt. `SHELLEXECUTEINFOW.hProcess`
     is a real HANDLE usable with `WaitForSingleObject`/`TerminateProcess`.
     If the user declines (hInstApp ≤ 32), raise `PermissionError("UAC declined")`.
  3. Poll the temp file (up to ~30s) for the pipe name line. The broker
     writes the pipe-file atomically: writes to `<path>.tmp` then
     `MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`
     for atomic replacement. The Python wrapper polls for the file to
     exist AND be non-empty.
  4. Connect to the pipe via `CreateFileW(pipe_name, GENERIC_READ |
     GENERIC_WRITE, 0, None, OPEN_EXISTING, 0, None)`.
- **Pipe client (`_pipe.py`):**
  - `send_request(handle, obj)`: `json.dumps(obj).encode("utf-8")` →
    `struct.pack("<I", len)` + payload → `WriteFile`.
  - `recv_response(handle)`: `ReadFile` 4 bytes → `struct.unpack("<I")` →
    `ReadFile` N bytes → `json.loads`.
  - `call(handle, obj)`: send + recv → `dict`.
- **Routing in `TsDevClient`:**
  - `__init__`: detect elevation. If not elevated AND `use_serve=True`
    (new opt-in flag, default `False` for backwards compat in this TVS;
    flipped to `True` by default in K.6b after validation), start the
    broker lazily on first operation.
  - `_single`/`exists`/`get_source`: if broker is active, build the request
    JSON `{"op":..., "type":..., "name":..., "source":...}` (source read
    client-side from the file/string), `broker.call()`, parse the response
    via `TsDevResult.from_json()`.
  - Batch ops (`_batch`/`_batch_dir`): NOT routed through the broker in
    MVP — they continue to use `subprocess.run` (which works because the
    broker's elevation is inherited by... no, `subprocess.run` from a
    non-elevated Python won't inherit the broker's token). **MVP decision:**
    batch ops raise `NotImplementedError("batch ops require an elevated
    session; use serve mode for single ops or run elevated")` when not
    elevated and serve is active. Batch-over-serve is deferred (§6).
- **Lifecycle:** `_ServeBroker.__del__` sends `{"op":"quit"}` and waits for
  `hProcess` to exit (or `TerminateProcess(hProcess, 1)` after a 5s
  timeout). `atexit` registration as a safety net.

**TDD approach:**
- **Red:** Unit tests mock `ShellExecuteExW` and the pipe I/O; assert
  `_ServeBroker` builds the correct command line, polls the pipe-file,
  connects, and sends/receives framed JSON. Assert `TsDevClient` routes
  through the broker when not elevated. Fails (classes don't exist).
- **Green:** Implement `_pipe.py`, `_ServeBroker`, routing. Unit tests
  pass (mocked). E2E validation in K.6a.

**Dependencies:** K.4 (the broker must exist to test against).

---

### TVS-K.6a — E2E integration + backwards-compat verification (no docs, no default flip) — **COMPLETE (2026-08-21)**

**Goal:** End-to-end verification that the serve mode works with the real
elevated broker and the real TSDev Library, AND that all existing
subcommands behave byte-identically when `--serve` is not used. No docs,
no `use_serve` default flip (that is K.6b, gated on K.6a passing).

**Files to change:**
- `tests/test_serve_e2e.py` — E2E test (real broker, real orlib20).
- `tests/run_regression.ps1` — update test count (19 → 23 C++ test executables).

**Key implementation details:**
- E2E test: from a NON-elevated Python process, construct `TsDevClient(
  use_serve=True)`, perform insert → exists → get-source → modify → delete
  on a test strategy. Assert each result matches the direct-subprocess
  results. Verify only ONE UAC prompt was triggered (the broker start).
  Clean up the test entry.
- Backwards-compat: run the full existing E2E suite
  (`docs/E2E_TEST_SPEC.md`) WITHOUT `use_serve` (elevated session, direct
  `subprocess.run`). Assert byte-identical JSON output vs. the pre-serve
  baseline.
- Elevation guard E2E: run `tsdev_cli insert ...` from a non-elevated
  process (with `TSDEV_REQUIRE_ADMIN=OFF` build so Windows doesn't block
  startup) → assert exit 11 + the not-elevated message.

**TDD approach:**
- **Red:** E2E test fails (serve mode not yet wired through Python
  end-to-end).
- **Green:** Run E2E, fix any integration bugs. All existing E2E tests
  still pass (backwards compat). `use_serve` default remains `False`.

**Dependencies:** K.1–K.5 all complete.

---

### TVS-K.6b — Documentation + `use_serve` default flip — **COMPLETE (2026-08-21)**

**Completion summary:**
- `use_serve` default flipped to `True` in `TsDevClient.__init__`.
- Critical bug fixed: `_ServeBroker.start()` now validates the CLI path exists before `ShellExecuteExW` (raises `FileNotFoundError` instead of a Windows popup on a non-existent CLI path).
- Serve-mode docs added to `docs/BUILD.md`, `docs/E2E_TEST_SPEC.md` (Part F), `README.md`.
- `tests/test_serve_e2e.py` updated to use the default (no explicit `use_serve=True`).
- E2E: 2/2 serve E2E passed (default-flip path), 12/12 C++ regression suites / 135 cases passed.
- Adversarial review: APPROVED (17 INFO findings).

**Goal:** Document the serve mode and flip the `use_serve` default to
`True` (auto-elevate when not elevated) once K.6a has verified the
end-to-end path.

**Files to change:**
- `docs/BUILD.md` — document `--serve` mode + the `use_serve` Python flag.
- `docs/E2E_TEST_SPEC.md` — add serve-mode E2E procedure.
- `README.md` — brief mention of serve mode for the one-UAC UX.
- `src/tsdev_automation/client.py` — flip `use_serve` default to `True`
  (auto-elevate when not elevated) after K.6a passes.

**Key implementation details:**
- Flip `use_serve` default to `True` in `TsDevClient.__init__`.
- Re-run the full E2E suite from a NON-elevated session to confirm the
  default-flip path works (broker auto-starts, one UAC, all ops pass).
- Update docs to reflect the one-UAC UX and the `use_serve` flag.

**TDD approach:**
- **Red:** Docs absent; `use_serve` default still `False`.
- **Green:** Docs written; default flipped; E2E from non-elevated session
  passes with no explicit `use_serve=True`.

**Dependencies:** K.6a passing (E2E verified before flipping the default).

---

## 2. Protocol specification

### Pipe name
```
\\.\pipe\tsdev_cli_<PID>
```
- `<PID>` = broker process ID (decimal). Ensures uniqueness across
  concurrent brokers.
- Broker writes the full pipe name (as a UTF-8 line, no BOM, trailing
  `\n`) to the file specified by `--serve-pipe-file <path>` immediately
  after `CreateNamedPipeW` succeeds and before `ConnectNamedPipe`. The
  broker writes the full pipe name AND its own PID to that file (format:
  first line = pipe name, second line = PID as decimal string).

### Message framing
Every message (request and response) is:
```
[4 bytes: payload length N, little-endian uint32]
[N bytes: UTF-8 JSON payload]
```
- No trailing newline in the payload (the length prefix delimits messages).
- `N` must be in `[0, 67108864]` (64 MiB cap). Messages with `N` outside
  this range → connection closed by the receiver.
- A zero-length payload (`N=0`) is accepted by the framing layer but
  fails JSON parsing (empty string is not valid JSON); the broker
  responds with a usage-error `CliResult`.

### Request JSON schema
```json
{
  "op": "insert" | "modify" | "delete" | "exists" | "upsert" | "get-source" | "quit",
  "type": "strategy" | "function" | "indicator" | "tradingapp" | "showme",
  "name": "<component name>",
  "source": "<EasyLanguage source text>",     // insert/modify/upsert only
  "case_insensitive": false,                  // exists only (optional, default false)
  "output_path": "<broker-local file path>"   // get-source only (optional)
}
```
- `op` (required): the operation.
- `type` (required for all ops except `quit`): component type,
  case-insensitive (same as CLI).
- `name` (required for all ops except `quit`): component name.
- `source` (insert/modify/upsert): the raw EL source TEXT (not a file
  path). The Python wrapper reads the file client-side and sends the text.
  This avoids file-path round-trips and permission issues.
- `case_insensitive` (exists): optional boolean, default `false`.
- `output_path` (get-source): optional; if present, the broker writes the
  source to that path (broker-local) in addition to returning it in the
  response. The Python wrapper normally omits this and reads `source`
  from the response.
- `quit`: special op that signals the broker to shut down gracefully. No
  other fields required. The broker responds with a success `CliResult`
  (op="quit", success=true, exit_code=0) before closing.

### Response JSON schema
The response is the EXACT same JSON object that `--json` mode emits today
(produced by `cliResultToJson()`), with one addition for the `quit` op:
```json
{"op":"quit","success":true,"exit_code":0}
```
For all other ops, the response is identical to the existing `--json`
output schema (see `src/tsdev_automation/results.py` docstring):
```json
// success
{"op":"insert","type":"strategy","name":"MyStrat","success":true,
 "exit_code":0,"prog_id":42,"ell_path":"C:\\...\\MyStrat.ell"}
// failure
{"op":"insert","type":"strategy","name":"MyStrat","success":false,
 "exit_code":8,"error":{"code":"compile_error","message":"...",
 "prog_id":43,"compile_errors":"..."}}
```

### Error handling
| Condition | Broker behavior | Response |
|-----------|----------------|----------|
| Malformed JSON (parse error) | Catch exception, respond with usage-error `CliResult` | `{"success":false,"exit_code":1,"error":{"code":"usage_error","message":"malformed JSON request"}}` |
| Missing `op` field | Respond with usage-error | `{"success":false,"exit_code":1,"error":{"code":"usage_error","message":"missing 'op' field"}}` |
| Unknown `op` | Respond with usage-error | `{"success":false,"exit_code":1,"error":{"code":"usage_error","message":"unknown op 'foo'"}}` |
| Missing required field (`type`/`name`/`source`) | Respond with usage-error | `{"success":false,"exit_code":1,"error":{"code":"usage_error","message":"op 'insert' requires 'type','name','source'"}}` |
| Unknown `type` | Respond with unknown-type | `{"success":false,"exit_code":2,"error":{"code":"unknown_type","message":"..."}}` |
| `do*` failure (compile error, not found, etc.) | Return the `CliResult` from `do*` as-is | Normal failure response (same as `--json`) |
| `do*` throws (unexpected exception) | Catch, respond with entry-op-failure | `{"success":false,"exit_code":6,"error":{"code":"entry_op_failure","message":"<exception what()>"}}` |
| `readFramed` fails (broken pipe) | Close client connection, loop to `ConnectNamedPipe` | (no response — connection gone) |
| `writeFramed` fails | Close client connection, loop | (no response) |

The broker NEVER crashes on a bad request — every error is a JSON response
or a connection close.

### Connection lifecycle
1. **Connect:** client `CreateFileW(pipe_name, ...)` → broker's
   `ConnectNamedPipe` returns.
2. **Multiple requests:** client sends N framed requests on the SAME
   connection; broker reads + responds in a loop. One `TsDevLib` instance
   serves all requests on all connections for the broker's lifetime.
3. **Disconnect:** client closes its pipe handle (or sends `quit`).
   Broker calls `FlushFileBuffers` + `DisconnectNamedPipe`, then loops
   back to `ConnectNamedPipe` for the next client.
4. **Reconnect:** a new client can connect after the previous disconnect.
   The `TsDevLib` instance persists across connections.
5. **Broker shutdown:** `quit` op (graceful) OR process killed (the pipe
   handle is closed by the OS; the Python wrapper's `atexit` sends `quit`
   first, then `TerminateProcess(hProcess, 1)` after a 5s timeout).

---

## 3. Security design

### Pipe DACL construction
The broker builds a `SECURITY_ATTRIBUTES` with an explicit security
descriptor via SDDL string:
```
D:P(A;;GA;;;BA)(A;;GWGR;;;SY)(A;;GWGR;;;OW)S:(ML;;NW;;;ME)
```
- `D:P` — DACL, protected (no inherited ACEs).
- `(A;;GA;;;BA)` — Administrators: full control (GENERIC_ALL).
- `(A;;GWGR;;;SY)` — LOCAL SYSTEM: read/write.
- `(A;;GWGR;;;OW)` — Owner (the elevated broker's user SID): read/write.
- `S:(ML;;NW;;;ME)` — SACL mandatory integrity label: Medium (ME),
  No-Write-Up (NW). This lowers the pipe from the broker's High-IL to
  Medium-IL so a non-elevated (Medium-IL) client can write to it. Without
  this, Windows No-Write-Up blocks the non-elevated client's
  `CreateFileW(GENERIC_WRITE)`.

Built via `ConvertStringSecurityDescriptorToSecurityDescriptorW(
  sddl, SDDL_REVISION_1, &pSD, NULL)` → set
  `sa.lpSecurityDescriptor = pSD` → pass `&sa` to `CreateNamedPipeW`.

### Integrity label lowering
The `S:(ML;;NW;;;ME)` SACL entry is the critical piece. The broker runs
elevated (High-IL); its objects default to High-IL. A non-elevated client
(Medium-IL) cannot write to a High-IL object. The explicit Medium label
lowers the pipe's IL to Medium, permitting the cross-IL write. This is the
standard documented pattern for elevated-broker / non-elevated-client IPC.

### Client PID/SID verification (MVP vs. hardening)
- **MVP:** NO client verification. The pipe is secured by the DACL (only
  the owning user + admins can connect) and the Medium-IL label. Any
  process running as the same user can connect. This is acceptable for the
  MVP because the threat model is "another process running as the same
  user issues TSDev Library operations" — which is the same trust level as
  the existing `subprocess.run` model (any same-user process can already
  invoke `tsdev_cli.exe`).
- **Hardening (later workstream):** `GetNamedPipeClientProcessId()` →
  `OpenProcess()` → `OpenProcessToken()` → `GetTokenInformation(
  TokenUser)` → compare the client's SID against the broker user's SID.
  Optionally verify the client PID matches the expected Python process.
  This defends against a same-user rogue process that connects after the
  legitimate client disconnects. Deferred — see §6.

### What's MVP vs. hardening
| Concern | MVP | Hardening (later) |
|---------|-----|-------------------|
| DACL (owner + admins only) | ✅ | ✅ |
| Medium-IL label (No-Write-Up bypass) | ✅ | ✅ |
| Client PID verification | ❌ | ✅ |
| Client SID verification | ❌ | ✅ |
| Per-request authentication token | ❌ | ✅ |
| Pipe name obfuscation (GUID instead of PID) | ❌ | ✅ |
| Encryption (TLS over pipe) | ❌ | N/A (local IPC) |

---

## 4. Python wrapper integration design

### Elevation detection
`TsDevClient._detect_elevation()` (cached):
```python
import ctypes
from ctypes import wintypes
token = wintypes.HANDLE()
ctypes.windll.advapi32.OpenProcessToken(
    ctypes.windll.kernel32.GetCurrentProcess(),
    0x0020,  # TOKEN_QUERY
    ctypes.byref(token))
elevated = wintypes.DWORD()
ctypes.windll.advapi32.GetTokenInformation(
    token, 20,  # TokenElevation
    ctypes.byref(elevated), ctypes.sizeof(elevated),
    ctypes.byref(wintypes.DWORD()))
return bool(elevated.value)
```

### Broker start (ShellExecuteExW RunAs)
`_ServeBroker.start()`:
1. `tmp = tempfile.NamedTemporaryFile(delete=False, suffix=".pipe")` →
   note the path, close it (the broker will create/overwrite it).
2. `ShellExecuteExW` with `SEE_MASK_NOCLOSEPROCESS` + `lpVerb="runas"` +
   `lpFile=str(cli_path)` + `lpParameters=f"--serve --serve-pipe-file
   {tmp_path}"` + `nShow=SW_HIDE` → triggers the UAC prompt.
   `SHELLEXECUTEINFOW.hProcess` is a real HANDLE usable with
   `WaitForSingleObject`/`TerminateProcess`. If the user declines
   (hInstApp ≤ 32), raise `PermissionError("UAC declined")`.
3. Poll `tmp_path` (read, check non-empty) every 100ms up to 30s. Read the
   pipe name line. Delete the temp file. The broker writes the pipe-file
   atomically: writes to `<path>.tmp` then
   `MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`
   for atomic replacement. The Python wrapper polls for the file to exist
   AND be non-empty.
4. `CreateFileW(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, None,
   OPEN_EXISTING, 0, None)` → pipe handle. Retry for ~5s if the pipe
   isn't ready yet (broker may not have called `ConnectNamedPipe`).

### Send / receive framed JSON
`_pipe.py`:
```python
import struct, ctypes, json
from ctypes import wintypes

def send_request(handle: int, obj: dict) -> None:
    payload = json.dumps(obj).encode("utf-8")
    hdr = struct.pack("<I", len(payload))
    _write_all(handle, hdr)
    _write_all(handle, payload)

def recv_response(handle: int) -> dict:
    hdr = _read_exact(handle, 4)
    (n,) = struct.unpack("<I", hdr)
    payload = _read_exact(handle, n)
    return json.loads(payload.decode("utf-8"))
```
`_write_all` / `_read_exact` use `WriteFile` / `ReadFile` via ctypes with
looping for partial writes/reads.

### Lifecycle management
- **Keep alive:** the broker stays running across multiple `TsDevClient`
  operations (one broker per `TsDevClient` instance). The pipe handle is
  held open for the instance's lifetime.
- **Kill on exit:** `_ServeBroker.__del__` sends `{"op":"quit"}`, waits
  up to 5s for `hProcess` to exit (`WaitForSingleObject(hProcess, 5000)`),
  then `TerminateProcess(hProcess, 1)` if still alive, then
  `CloseHandle(hProcess)`. `atexit.register(broker.shutdown)` as a safety
  net so the broker doesn't outlive the Python process.
- **Reconnect:** if the pipe handle breaks mid-session (broker restarted),
  `_ServeBroker.call()` catches the I/O error, re-starts the broker, and
  retries once.

### Fallback to subprocess.run if already elevated
If `TsDevClient._is_elevated` is `True` (the Python session is already
elevated), the wrapper uses the EXISTING `subprocess.run` path directly —
no broker, no pipe. This is the optimal path (zero overhead, no extra UAC).
The broker path is ONLY used when the Python session is NOT elevated AND
`use_serve=True`.

**Decision matrix:**
| Python elevated? | `use_serve` | Path |
|------------------|-------------|------|
| Yes | True/False | `subprocess.run` (direct, no broker) |
| No | True | Serve broker (one UAC, then pipe) |
| No | False | `subprocess.run` (fails — each child triggers UAC or is blocked by manifest) |

---

## 5. Backwards compatibility verification

### How to verify existing subcommands work identically
1. **Unit tests:** all existing C++ unit tests (`tests/test_*.cpp`) must
   compile and pass unchanged. The `TSDEV_CLI_TEST` guard and the
   `#include "../src/tsdev_cli.cpp"` mechanism are preserved. New code
   (serve, framing, dispatch) lives in the anonymous namespace and is
   testable via the same mechanism.
2. **JSON output byte-identical:** K.2 is a pure refactor of
   `cliResultToJson()`. Run the existing `--json` E2E for insert/modify/
   delete/exists/get-source/upsert and diff the JSON output against a
   pre-K baseline. Must be byte-identical.
3. **Human-readable output byte-identical:** run the same ops WITHOUT
   `--json` and diff against baseline. The serve layer never touches the
   human-readable path.
4. **Batch output:** `emitBatchOutput()` now uses `cliResultToJson()` —
   verify batch-insert/modify/delete/upsert JSON array output is identical
   (the `exists`/`source` fields are absent in batch ops by construction,
   so no change).
5. **Exit codes:** all existing exit codes (0–10) unchanged. New exit
   code 11 (`not_elevated`) only fires when NOT elevated — never in the
   existing elevated-session path.
6. **CMake manifest:** `TSDEV_REQUIRE_ADMIN` still embeds
   `requireAdministrator`. The elevation guard (K.1) is a defense-in-depth
   check; when the manifest is present, Windows blocks non-elevated
   startup BEFORE the guard runs (so the guard is for `TSDEV_REQUIRE_ADMIN=
   OFF` builds and for serve mode where the broker is explicitly elevated).

### Tests to run
- Full C++ test suite: `ctest` (all `test_*` targets).
- Full Python unit suite: `pytest tests/` (mocked, fast).
- E2E CLI suite: `docs/E2E_TEST_SPEC.md` (all 5 component types, all
  subcommands) from an ELEVATED session — must pass identically.
- Elevation guard test: `tsdev_cli insert ...` from non-elevated (with
  `TSDEV_REQUIRE_ADMIN=OFF` build) → exit 11.
- Serve E2E: `tests/test_serve_e2e.py` from a NON-elevated session.

---

## 6. Risk assessment

### What could go wrong
1. **Pipe security SDDL is wrong** → non-elevated client can't connect
   (No-Write-Up not bypassed) OR the pipe is world-accessible (too
   permissive). Mitigation: K.4 adversarial review + E2E test from a
   non-elevated process. Test both "can connect" (correct user) and
   "reasonable permissions" (DACL inspection).
2. **TsDevLib thread-affinity violation** → the serve loop must construct
   AND use `TsDevLib` on the SAME 64MB-stack thread. If the pipe I/O is
   accidentally on the main thread while `do*` is on the worker thread,
   the EDIT control breaks. Mitigation: `runServe` does EVERYTHING (pipe
   I/O + `do*` calls) on the worker thread; the main thread only creates
   the pipe and launches the worker. Review carefully.
3. **`closeWarehouse()` cadence** → in batch mode, `closeWarehouse()` is
   called once at the end. In serve mode, the broker runs indefinitely.
   `doModify`/`doUpsert` do NOT flush internally (no `flushPerItem` param).
   If the broker is `TerminateProcess`ed, `OrlibExit` never runs and
   unflushed modifies would be lost. **Decision:**
   `dispatchServeRequest` calls `lib.closeWarehouse()` after EVERY mutating
   op (insert/modify/delete/upsert) in serve mode. This matches single-item
   CLI semantics (each op persisted before exit). `doInsert` is also called
   with `flushPerItem=true`. `doDelete` flushes internally but the explicit
   `closeWarehouse()` is harmless. Read-only ops (exists/get-source) do not
   flush.
4. **Broker zombie processes** → if the Python process crashes without
   sending `quit`, the broker stays alive. Mitigation: `atexit` +
   `TerminateProcess` in `__del__` + the broker could also self-terminate
   on stdin EOF (but serve mode doesn't use stdin). Hardening: broker
   checks a "heartbeat" file timestamp and self-exits if stale (deferred).
5. **UAC prompt UX** → `ShellExecute("runas")` shows a prompt that may
   steal focus or confuse automated pipelines. Mitigation: this is
   inherent to the one-UAC-per-session design and is the explicit goal.
   Document it clearly. The prompt appears ONCE per `TsDevClient`
   instance.
6. **Large source payloads** → EL source > 64MB would be rejected by the
   framing cap. Real EL source is never that large (KB range). Mitigation:
   64MB cap is generous; document the limit.
7. **Broker blocked in `ConnectNamedPipe` with no client** → if the
   Python client crashes without sending `quit`, the broker loops back to
   `ConnectNamedPipe` and blocks forever. The Python wrapper holds
   `hProcess` (via F3 fix) and writes the broker PID to the pipe-file
   alongside the pipe name, so a recovery path can
   `OpenProcess(PROCESS_TERMINATE, pid)` + `TerminateProcess`. **MVP
   decision:** the Python wrapper's `atexit` + `__del__` kill the broker
   via `hProcess`; if the Python process itself is killed (not
   `atexit`-cleanable), the broker becomes a zombie until the user kills
   it via Task Manager or a PID-based recovery script. A heartbeat-based
   auto-shutdown is deferred (§6 deferred list). Document this limitation.
8. **Broker crash mid-operation / warehouse inconsistency** → if the
   broker process crashes (access violation in orlib20, OS kill) during a
   `doInsert`/`doModify`, the warehouse may be left in an inconsistent
   state (entry created but not compiled, .ell missing). The Python
   client's `ReadFile` would fail, triggering reconnect-retry. **MVP
   decision:** the Python wrapper's retry-once should ONLY retry
   read-only ops (exists/get-source). For mutating ops (insert/modify/
   delete/upsert), a mid-op crash returns an error to the caller WITHOUT
   automatic retry — the caller must decide whether to retry (e.g., call
   `exists` first to detect a partial entry). This matches the existing
   single-item CLI risk profile (a crash mid-op in the current CLI has the
   same consequence). Document this limitation.

### What's the hardest part
- **The SDDL security descriptor + Medium-IL label.** Getting this wrong
  means either the non-elevated client can't connect (the whole feature
  fails) or the pipe is insecure. This requires careful testing from a
  genuinely non-elevated process (not just an elevated test runner).
- **TsDevLib lifetime on the worker thread.** The serve loop must keep
  `TsDevLib` alive across many requests on the 64MB-stack thread, with
  correct `closeWarehouse()` cadence. `runBatch` is the precedent but it
  has a finite loop; serve is open-ended. The shutdown path (quit / kill)
  must cleanly destruct `TsDevLib` on the worker thread.
- **Python ↔ broker handshake without a stdout pipe.** `ShellExecuteExW`
  doesn't give the parent a stdout handle, so the `--serve-pipe-file`
  temp-file handshake is the workaround. Polling a temp file is slightly
  fragile (timing, permissions); the E2E test must validate it reliably.

### What should be deferred to a later workstream
- **Batch ops over the pipe** (`batch-insert`, `batch-modify`, etc.): the
  MVP routes only single-item ops through the broker. Batch ops from a
  non-elevated session will need either (a) the broker to accept a batch
  request (array of items) and loop internally, or (b) the Python wrapper
  to send N individual requests. Option (b) works today but loses the
  single-`TsDevLib`-init amortization of batch mode. Defer the batch-
  over-pipe protocol to a follow-up workstream.
- **Multi-client concurrency:** the MVP is single-client (one
  `ConnectNamedPipe` + loop). Supporting multiple simultaneous Python
  processes sharing one broker requires overlapped I/O + a thread pool
  (but all `do*` calls must serialize on the one `TsDevLib`-owning
  thread). Defer.
- **Client PID/SID verification** (see §3 hardening table).
- **Directory-batch ops over the pipe** (`batch-insert-dir`, etc.).
- **Heartbeat / auto-shutdown** for zombie broker prevention.
- **`TSDEV_REQUIRE_ADMIN=OFF` + serve** interaction: when the manifest is
  OFF, `tsdev_cli --serve` launched non-elevated would fail the K.1 guard.
  The Python wrapper's `ShellExecute("runas")` elevates it regardless of
  the manifest. This works but is an edge case to document, not block on.
- **Concurrent broker instances:** two `TsDevClient` objects in the same
  process each start their own broker (pipe names embed PID, no collision).
  Both brokers access the same orlib20 — this is safe to the same extent
  as concurrent `subprocess.run` invocations are today (orlib20 does not
  serialize cross-process access). Not recommended but not blocked.
