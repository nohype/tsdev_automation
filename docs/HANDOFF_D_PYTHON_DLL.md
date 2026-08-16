# Handoff D — Python DLL Bridge via msl-loadlib

**Priority (d):** Provide a DLL library that can be easily used in 64-bit Python via [msl-loadlib](https://github.com/MSLNZ/msl-loadlib).

**Why this matters:** The user wants to drive TradeStation component management (INSERT / DELETE / MODIFY of EasyLanguage strategies, indicators, and functions) from **64-bit Python**. The core engine (`orlib20.dll`) is a **32-bit** DLL that cannot be loaded into a 64-bit process. msl-loadlib is the chosen bridge: it provides a `Server32`/`Client64` architecture that hosts a 32-bit library in a 32-bit Python subprocess and communicates with 64-bit Python via inter-process communication (pickle over HTTP).

---

## 1. Current State

- The core engine is `src/tsdev_lib.cpp` / `src/tsdev_lib.h` — a C++ class (`tsdev::TsDevLib`) that wraps the confirmed orlib20 operation set (init, name lookup, create entry, save source, compile/verify, error retrieval, warehouse close, delete orchestrator, modify).
- `TsDevLib` is compiled as a **32-bit static library** (`add_library(tsdev_lib STATIC ...)` in `CMakeLists.txt`).
- It is consumed by 32-bit test executables (`test_insert`, `test_delete`, `test_modify`, `tsdev_cli`) and probes (`elevated_test20`, etc.).
- All three core functions (INSERT, DELETE, MODIFY) are **DONE and APPROVED** at the library level (see AGENTS.md TODO, TVS-2.3).
- There is **no shared DLL** (only a static `.lib`).
- There is **no C ABI export layer** — the `TsDevLib` API uses C++ types (`std::string`, `Result` struct, `ComponentType` enum) that cannot cross a ctypes boundary directly.
- There is **no Python layer** — no Server32 module, no Client64 module, no Python tests.
- The `TsDevLib` constructor is expensive (loads orlib20.dll, pre-loads 7 support DLLs, applies 1 persistence patch + 3 thread-check patches, resolves 8 exports + 6 internal RVAs, calls `OrlibInit`, creates a hidden EDIT control). It should not be reconstructed per call.
- The hidden EDIT control is **thread-affine** (created via `CreateWindowW` on the constructor's thread; `WM_SETTEXT`/`WM_GETTEXT` work cross-thread via `SendMessage` but the control itself must remain alive on its creating thread).
- Thread-check INT3 instructions in three orlib20 dispatch helpers are already NOP-patched in the constructor, allowing orlib20 functions to be called from any thread (not just TSDev.exe's worker thread).

---

## 2. Goal

A 64-bit Python package (`tsdev_python`) that can INSERT, DELETE, and MODIFY TradeStation EasyLanguage components (strategies, indicators, functions) via a 32-bit DLL bridge, using msl-loadlib's `Server32`/`Client64` architecture. The user imports a Python class in their 64-bit Python environment, calls `insert(name, component_type, source_code)`, `delete(name, component_type)`, or `modify(name, component_type, new_source_code)`, and the operation is executed in the 32-bit orlib20 engine with results returned to 64-bit Python.

### Architecture overview

```
64-bit Python process                        32-bit Python subprocess (frozen server32-windows.exe)
┌──────────────────────────┐                ┌───────────────────────────────────────┐
│  TsDevClient64           │                │  TsDevServer32(Server32)              │
│  (Client64 subclass)     │  HTTP/pickle   │    ├─ ctypes loads tsdev_bridge32.dll │
│    .insert(name,type,src)│ ──────────►    │    │    └─ wraps TsDevLib (static)   │
│    .delete(name,type)    │                │    │         └─ loads orlib20.dll    │
│    .modify(name,type,src)│ ◄──────────    │    .insert(name,type,src) → DLL call  │
│    .find(name,type)      │   response     │    .delete(name,type)    → DLL call  │
└──────────────────────────┘                │    .modify(name,type,src)→ DLL call  │
                                            │    .find(name,type)      → DLL call  │
                                            │    shutdown_handler() → tsdev_shutdown│
                                            └───────────────────────────────────────┘
```

### Key msl-loadlib findings (from research, see TVS-D.1 for full details)

- **msl-loadlib v1.1.0** (2026-06-30) is the latest release. The 32-bit server is a **frozen executable** (`server32-windows.exe`, Python 3.14.6) shipped with the package — no separate 32-bit Python installation is needed for the server itself.
- `Server32` subclasses load a DLL via ctypes (`cdll` for `__cdecl`, `windll` for `__stdcall`). The `.lib` property is the ctypes library object.
- `Client64` subclasses call `self.request32('method_name', *args, **kwargs)` to invoke methods on the `Server32` subclass. Arguments and return values are pickled/unpickled automatically.
- **kwargs limitation:** keyword arguments passed to the `Client64` constructor are forwarded to the `Server32` constructor but their data types are **not preserved** — they arrive as `str`. Complex data must be passed via method arguments (through `request32`), not via constructor kwargs.
- `Server32` is based on `http.server.HTTPServer` — it is **single-threaded** by default (processes one request at a time on the server's main thread). This is favorable for the thread-affine EDIT control: the `TsDevLib` instance is created in the `Server32` constructor (main thread) and all method calls also execute on the main thread.
- `Server32.shutdown_handler()` is called immediately before the server shuts down — the intended place for cleanup (calling `tsdev_shutdown` to destroy the `TsDevLib` instance and call `OrlibExit`).
- `Client64` accepts `add_dll_directory` (adds paths to the 32-bit server's DLL search path) and `append_sys_path` (adds paths to the 32-bit server's `sys.path` for importing the server module).
- There is a `freeze32` console script to create a **custom frozen 32-bit server** if additional Python packages are needed in the 32-bit server (not needed if we only use ctypes, which is built-in).

---

## 3. TVS Decomposition

### TVS-D.1 — RESEARCH: Study msl-loadlib API & architecture

**Goal:** Confirm the exact msl-loadlib API surface, the Server32/Client64 communication model, and the constraints that affect the DLL and Python module design.

**Type:** RESEARCH

**Effort:** S

**Scope:**
- Fetch and study the msl-loadlib documentation:
  - README: https://github.com/MSLNZ/msl-loadlib
  - Server32 API: https://msl-loadlib.readthedocs.io/en/latest/_api/msl.loadlib.server32.html
  - Client64 API: https://msl-loadlib.readthedocs.io/en/latest/_api/msl.loadlib.client64.html
  - Inter-process communication examples: https://msl-loadlib.readthedocs.io/en/latest/interprocess_communication.html
  - C++ example (cpp32/cpp64): https://msl-loadlib.readthedocs.io/en/latest/_api/msl.examples.loadlib.cpp32.html
  - C++ source: https://msl-loadlib.readthedocs.io/en/latest/cpp_source.html
  - CHANGELOG: https://raw.githubusercontent.com/MSLNZ/msl-loadlib/main/CHANGELOG.md
  - Refreezing a custom 32-bit server: https://msl-loadlib.readthedocs.io/en/latest/refreeze.html
- Confirm and document:
  - The frozen 32-bit server executable (`server32-windows.exe`) — version, Python version, how it's located.
  - How `Server32.__init__(path, libtype, host, port, **kwargs)` loads a DLL via ctypes (the `libtype` parameter: `'cdll'` for `__cdecl`, `'windll'` for `__stdcall`).
  - How `Client64.__init__(module32, *, add_dll_directory=..., append_sys_path=..., ...)` starts the 32-bit server and passes the module path.
  - The `request32(name, *args, **kwargs)` method — how args/return values are pickled.
  - The kwargs-become-str limitation for constructor arguments.
  - The `shutdown_handler()` lifecycle hook.
  - The `add_dll_directory` parameter — can it point to the TS Program directory so orlib20.dll is findable?
  - The single-threaded nature of `HTTPServer` and its implications for thread affinity.
  - Whether `host=None` mocking is useful for testing.
  - Error handling: `Server32Error` raised on the client when the server raises an exception.
- Install msl-loadlib in the 64-bit Python environment and run the built-in cpp32/cpp64 example to confirm the bridge works end-to-end.

**NOT in scope:** Designing the C ABI (TVS-D.2), building the DLL (TVS-D.4), writing Python modules (TVS-D.5/D.6).

**Acceptance criteria:**
- A research notes document (`docs/D1_msl_loadlib_research.md`) is written containing:
  - The confirmed Server32/Client64 API surface with exact constructor signatures.
  - The confirmed IPC mechanism (pickle over HTTP).
  - The confirmed threading model (single-threaded HTTPServer).
  - The confirmed kwargs-become-str limitation.
  - The confirmed frozen-server approach (no separate 32-bit Python needed).
  - The confirmed `add_dll_directory` / `append_sys_path` / `server32_dir` parameters.
  - A working example of the built-in cpp32/cpp64 bridge (evidence: transcript or screenshot).
  - Any version-specific notes (v1.1.0, Python 3.14.6 in the frozen server).

**Dependencies:** None.

**Adversarial review:** YES — the research notes must be reviewed for completeness and accuracy (did the researcher actually run the example? did they miss any API constraint?).

---

### TVS-D.2 — RESEARCH: Design the C ABI export layer

**Goal:** Define the exact `extern "C"` function signatures for the shared DLL that wraps `TsDevLib`, suitable for ctypes consumption.

**Type:** RESEARCH

**Effort:** S

**Scope:**
- Design the complete C ABI specification. The DLL must export `extern "C"` functions with only C-compatible types (no `std::string`, no C++ classes, no exceptions across the boundary).
- Determine the calling convention: `__cdecl` (loaded via ctypes `cdll`) is recommended for simplicity and consistency with the msl-loadlib C++ example. Document the choice.
- Design the function set:
  - `int tsdev_init(void)` — create the singleton `TsDevLib` instance. Returns 0 on success, non-zero error code on failure. The instance persists across calls.
  - `int tsdev_shutdown(void)` — destroy the singleton instance (calls `OrlibExit`, destroys EDIT control). Returns 0 on success.
  - `int tsdev_insert(const char* name, int component_type, const char* source, int* out_prog_id, char* out_error, int error_buf_size)` — insert a new component and compile/verify. Returns 0 on success, non-zero on failure. `out_error` is filled with the compile error string (if any). `out_prog_id` receives the new ProgId.
  - `int tsdev_delete(const char* name, int component_type, char* out_error, int error_buf_size)` — delete an existing component. Returns 0 on success (deleted), 1 if not found, negative on error.
  - `int tsdev_modify(const char* name, int component_type, const char* new_source, int* out_prog_id, char* out_error, int error_buf_size)` — modify an existing component's source and recompile. Returns 0 on success, 1 if not found, negative on error (including compile failure).
  - `int tsdev_find(const char* name, int component_type)` — look up a component by name. Returns ProgId (>0) if found, 0 if not found, negative on error.
  - `int tsdev_get_last_error(char* out_error, int error_buf_size)` — retrieve the last error message (for errors that don't have an inline out_error param). Returns 0 on success.
  - `int tsdev_is_alive(void)` — check if the singleton instance is initialized. Returns 1 if alive, 0 if not.
- Define the `component_type` integer mapping: 0 = Strategy, 2 = Indicator, 4 = Function (matching `ComponentType` enum values).
- Define the return code convention: 0 = success, positive = logical outcome (e.g., not-found), negative = error (with error message in `out_error` or via `tsdev_get_last_error`).
- Address string handling:
  - All strings are null-terminated `const char*` (ANSI/UTF-8) on input.
  - Output strings are written into caller-allocated buffers with a size parameter to prevent overflow.
  - `std::string` returns from `TsDevLib` methods (e.g., `verifyModule`) are copied into the caller's buffer via `strncpy`/`snprintf`.
- Address the `Result` struct: split into out-parameters (`int* out_prog_id`, `char* out_error`, return code for success/failure).
- Address singleton instance management:
  - A static `TsDevLib*` pointer in the DLL, protected by a critical section (in case of re-entrancy, though the single-threaded server makes this unlikely).
  - `tsdev_init` creates the instance; `tsdev_shutdown` destroys it.
  - If `tsdev_init` is called when already alive, return success (idempotent) or an error — document the choice.
  - If an operation is called when not alive, return an error code.
- Address SEH exceptions: the `TsDevLib` methods already use SEH-protected wrappers internally. The C ABI functions should catch any C++ exceptions and convert them to error codes (no exception may cross the C boundary).
- Consider whether a worker thread with a large stack is needed:
  - The current implementation calls orlib20 directly from the calling thread (thread-check INT3s are NOP-patched). The existing 32-bit test exes work with the default 1MB stack.
  - The frozen 32-bit Python server's main thread also has a default stack (likely 1MB).
  - Risk: complex EasyLanguage compilations may need more stack. If so, the DLL can spawn a worker thread with a configurable stack size (e.g., 64MB) for compilation operations. This is a fallback, not the initial design.
  - Document this as a risk and a contingency plan.

**NOT in scope:** Building the DLL (TVS-D.4), writing Python modules (TVS-D.5/D.6), modifying CMake (TVS-D.3).

**Acceptance criteria:**
- A C ABI specification document (`docs/D2_c_abi_design.md`) is written containing:
  - The complete list of exported function signatures with parameter descriptions.
  - The calling convention choice and rationale.
  - The return code convention.
  - The `component_type` integer mapping.
  - The string handling strategy (caller-allocated buffers, size params).
  - The singleton instance management strategy.
  - The error handling strategy (SEH → error codes, no exceptions across boundary).
  - The stack-size contingency plan.
  - A draft `tsdev_bridge.h` header file (as a code block in the document).

**Dependencies:** TVS-D.1 (need to know the ctypes libtype and string-passing conventions).

**Adversarial review:** YES — the ABI design must be reviewed for correctness (does it cover all three operations? are the string buffers safe? is the singleton lifecycle correct? are there any C++ types leaking across the boundary?).

---

### TVS-D.3 — RESEARCH: Verify build system & runtime environment

**Goal:** Confirm that the CMake build can produce a 32-bit shared DLL, and verify the runtime environment (msl-loadlib installation, frozen server availability, DLL search paths).

**Type:** RESEARCH

**Effort:** S

**Scope:**
- Verify CMake changes needed:
  - The current `CMakeLists.txt` has `add_library(tsdev_lib STATIC src/tsdev_lib.cpp)`. A new `add_library(tsdev_bridge32 SHARED src/tsdev_bridge.cpp)` target is needed.
  - The shared DLL must link against the static `tsdev_lib` (or compile `tsdev_lib.cpp` directly into the shared target).
  - Confirm that the Win32 (x86) generator produces a 32-bit DLL (the existing 32-bit guard in CMakeLists.txt applies).
  - Confirm export macros: `__declspec(dllexport)` for the extern "C" functions.
  - Determine the output location of the DLL (e.g., `build/Release/tsdev_bridge32.dll`).
- Verify the 64-bit Python environment:
  - Is Python 64-bit installed? What version?
  - Can `pip install msl-loadlib` succeed?
  - Does the frozen `server32-windows.exe` get installed with the package?
  - Where is the frozen server located (for the `server32_dir` parameter)?
- Verify DLL search paths for the 32-bit server:
  - The `tsdev_bridge32.dll` must be findable by the 32-bit Python server. Options: pass its directory via `add_dll_directory`, or place it in a known location.
  - `orlib20.dll` and its dependencies are in `C:\Program Files (x86)\TradeStation 9.5\Program`. The `TsDevLib` constructor calls `SetDllDirectoryW` to this path, so orlib20 should be findable once the bridge DLL is loaded. Confirm this works when loaded via ctypes in the frozen server.
  - Alternatively, pass the TS Program dir via `add_dll_directory` to the `Client64` constructor.
- Verify the stack size of the frozen 32-bit server:
  - Check the default stack size of `server32-windows.exe` (using `dumpbin /headers` or Process Explorer).
  - Compare with the stack usage of the existing test exes (which work with 1MB).
  - Determine if a worker thread with a larger stack is needed as a contingency.
- Verify that `shutdown_handler()` is called when the `Client64` instance is destroyed (for cleanup).

**NOT in scope:** Actually building the DLL (TVS-D.4), writing Python code (TVS-D.5/D.6).

**Acceptance criteria:**
- A research notes document (`docs/D3_build_env_research.md`) is written containing:
  - The exact CMake changes needed (new target, link dependencies, export macro).
  - The confirmed 64-bit Python version and msl-loadlib installation status.
  - The confirmed location of the frozen `server32-windows.exe`.
  - The confirmed DLL search path strategy (how the 32-bit server finds `tsdev_bridge32.dll` and `orlib20.dll`).
  - The confirmed stack size of the frozen server and whether it's sufficient.
  - Any environment setup steps needed (pip install, path configuration).

**Dependencies:** TVS-D.1 (need to know the frozen server location and `add_dll_directory` semantics).

**Adversarial review:** YES — the build/env research must be reviewed for completeness (did the researcher actually install msl-loadlib? did they verify the frozen server? did they check the stack size?).

---

### TVS-D.4 — IMPLEMENTATION: Build the shared DLL with C ABI exports

**Goal:** Create the 32-bit shared DLL (`tsdev_bridge32.dll`) that wraps `TsDevLib` with `extern "C"` exports, and verify it with a 32-bit C++ test executable.

**Type:** IMPLEMENTATION

**Effort:** M

**Scope:**
- Create `src/tsdev_bridge.h` — the C ABI header (extern "C" function declarations with `__declspec(dllexport)`), based on the TVS-D.2 design.
- Create `src/tsdev_bridge.cpp` — the implementation:
  - Singleton `TsDevLib*` instance with a critical section for thread safety.
  - `tsdev_init()` — construct the `TsDevLib` instance (catches `std::runtime_error`, returns error code).
  - `tsdev_shutdown()` — destroy the instance (calls `OrlibExit` via destructor).
  - `tsdev_insert(name, type, source, out_prog_id, out_error, error_buf_size)` — calls `findExistPos` (check not exists) → `createEntry` → `saveSource` → `verifyModule` → `closeWarehouse`. Fills `out_error` with compile errors if any. Returns 0 on success, negative on error.
  - `tsdev_delete(name, type, out_error, error_buf_size)` — calls `deleteEntry`. Returns 0 if deleted, 1 if not found, negative on error.
  - `tsdev_modify(name, type, new_source, out_prog_id, out_error, error_buf_size)` — calls `modifyEntry`. Returns 0 on success, 1 if not found, negative on error (including compile failure).
  - `tsdev_find(name, type)` — calls `findExistPos`. Returns ProgId if found, 0 if not found, negative on error.
  - `tsdev_get_last_error(out_error, error_buf_size)` — retrieves the last error message.
  - `tsdev_is_alive()` — returns 1 if the singleton is initialized.
  - All functions catch C++ exceptions and SEH, converting them to error codes. No exception crosses the C boundary.
  - All output string writes use `snprintf`/`strncpy` with the provided buffer size.
- Update `CMakeLists.txt`:
  - Add `add_library(tsdev_bridge32 SHARED src/tsdev_bridge.cpp)`.
  - Link against `tsdev_lib` (static): `target_link_libraries(tsdev_bridge32 tsdev_lib)`.
  - The DLL output goes to `build/Release/tsdev_bridge32.dll` (Win32 config).
- Write a 32-bit C++ test executable (`src/test_bridge.cpp`) using TDD red-green:
  - Load `tsdev_bridge32.dll` via `LoadLibrary`.
  - Resolve all exports via `GetProcAddress`.
  - Call `tsdev_init` → verify success.
  - Call `tsdev_insert` with a test strategy → verify success, check `out_prog_id > 0`.
  - Call `tsdev_find` with the same name → verify same ProgId.
  - Call `tsdev_modify` with new source → verify success.
  - Call `tsdev_delete` → verify success.
  - Call `tsdev_find` again → verify returns 0 (not found).
  - Call `tsdev_shutdown` → verify success.
  - Negative tests: insert with empty source, delete nonexistent, modify nonexistent.
  - Add to CMakeLists.txt: `add_executable(test_bridge src/test_bridge.cpp)`.
- **TDD approach:** Write the test first (red), then implement the bridge (green).

**NOT in scope:** Python modules (TVS-D.5/D.6), Python integration tests (TVS-D.7).

**Acceptance criteria:**
- `tsdev_bridge32.dll` builds successfully as a 32-bit shared library (`cmake --build build --config Release`).
- `dumpbin /exports build/Release/tsdev_bridge32.dll` shows all 8 exported functions with undecorated C names.
- `test_bridge.exe` runs successfully: init → insert → find → modify → delete → find (not found) → shutdown, all returning expected codes.
- Negative tests pass (nonexistent component, compile error).
- No crashes, no WerFault, no exceptions crossing the C boundary.
- The DLL is verified to be 32-bit (PE32/x86).

**Dependencies:** TVS-D.2 (C ABI design), TVS-D.3 (CMake changes confirmed).

**Adversarial review:** YES — review the DLL exports, the singleton lifecycle, the error handling, the string buffer safety, and the test coverage.

---

### TVS-D.5 — IMPLEMENTATION: Write the 32-bit Python Server32 module

**Goal:** Create the Python module that runs in the 32-bit frozen server, loads `tsdev_bridge32.dll` via ctypes, and exposes INSERT/DELETE/MODIFY/FIND operations.

**Type:** IMPLEMENTATION

**Effort:** S

**Scope:**
- Create `python/tsdev_server32.py` containing `class TsDevServer32(Server32)`:
  - `__init__(self, host, port, **kwargs)`:
    - Call `super().__init__(path_to_tsdev_bridge32_dll, 'cdll', host, port)` (or `'windll'` if `__stdcall` is chosen — confirm from TVS-D.2).
    - The DLL path: accept via kwargs (e.g., `dll_path` kwarg, which arrives as `str`) or hardcode a default relative to the module location.
    - Configure ctypes function signatures (argtypes, restype) for all 8 exports:
      - `tsdev_init` → `CFUNCTYPE(c_int)` (or `WINFUNCTYPE` for stdcall).
      - `tsdev_shutdown` → `CFUNCTYPE(c_int)`.
      - `tsdev_insert` → `CFUNCTYPE(c_int, c_char_p, c_int, c_char_p, POINTER(c_int), c_char_p, c_int)`.
      - `tsdev_delete` → `CFUNCTYPE(c_int, c_char_p, c_int, c_char_p, c_int)`.
      - `tsdev_modify` → `CFUNCTYPE(c_int, c_char_p, c_int, c_char_p, POINTER(c_int), c_char_p, c_int)`.
      - `tsdev_find` → `CFUNCTYPE(c_int, c_char_p, c_int)`.
      - `tsdev_get_last_error` → `CFUNCTYPE(c_int, c_char_p, c_int)`.
      - `tsdev_is_alive` → `CFUNCTYPE(c_int)`.
    - Call `tsdev_init()` in the constructor. If it fails, raise an exception (the client will see `Server32Error`).
  - `insert(self, name, component_type, source)`:
    - Encode strings to bytes (`name.encode('utf-8')`, `source.encode('utf-8')`).
    - Allocate output buffers: `prog_id = c_int(0)`, `error_buf = create_string_buffer(4096)`.
    - Call `self.lib.tsdev_insert(name_bytes, component_type, source_bytes, byref(prog_id), error_buf, 4096)`.
    - Return a dict: `{'success': result == 0, 'prog_id': prog_id.value, 'error': error_buf.value.decode('utf-8', errors='replace')}`.
  - `delete(self, name, component_type)`:
    - Similar pattern. Return `{'success': result == 0, 'not_found': result == 1, 'error': ...}`.
  - `modify(self, name, component_type, new_source)`:
    - Similar pattern. Return `{'success': result == 0, 'not_found': result == 1, 'prog_id': ..., 'error': ...}`.
  - `find(self, name, component_type)`:
    - Call `tsdev_find`. Return `{'prog_id': result}` (0 = not found).
  - `shutdown_handler(self)`:
    - Call `self.lib.tsdev_shutdown()` to destroy the `TsDevLib` instance and call `OrlibExit`.
- Write a minimal test that can be run in 32-bit Python (or via the frozen server) to verify the module loads and `tsdev_init` succeeds. This can be a simple script that instantiates `TsDevServer32` with a dummy host/port and checks `tsdev_is_alive()`.

**NOT in scope:** The 64-bit client (TVS-D.6), end-to-end integration tests (TVS-D.7).

**Acceptance criteria:**
- `python/tsdev_server32.py` is written and importable.
- The `TsDevServer32` class correctly subclasses `Server32`.
- All ctypes function signatures are configured (argtypes/restype).
- `tsdev_init()` is called in the constructor and succeeds (the `TsDevLib` instance is created).
- `shutdown_handler()` calls `tsdev_shutdown()`.
- A minimal smoke test confirms the server module can load the DLL and init succeeds.
- String encoding/decoding is handled correctly (UTF-8, with buffer size limits).

**Dependencies:** TVS-D.1 (msl-loadlib API), TVS-D.2 (C ABI design), TVS-D.4 (DLL must exist).

**Adversarial review:** YES — review the ctypes signatures, string handling, error propagation, and the shutdown_handler implementation.

---

### TVS-D.6 — IMPLEMENTATION: Write the 64-bit Python Client64 module

**Goal:** Create the Python module that runs in 64-bit Python, starts the 32-bit server, and exposes a clean API for INSERT/DELETE/MODIFY/FIND operations.

**Type:** IMPLEMENTATION

**Effort:** S

**Scope:**
- Create `python/tsdev_client64.py` containing `class TsDevClient64(Client64)`:
  - `__init__(self, dll_dir=None, ts_program_dir=None, **kwargs)`:
    - Call `super().__init__('tsdev_server32', append_sys_path=os.path.dirname(__file__), add_dll_directory=dll_dir, **kwargs)`.
    - The `module32` parameter is `'tsdev_server32'` (the server module name).
    - `append_sys_path` points to the directory containing `tsdev_server32.py` so the 32-bit server can import it.
    - `add_dll_directory` points to the directory containing `tsdev_bridge32.dll` so the 32-bit server's ctypes can find it.
    - If `ts_program_dir` is provided, also add it to `add_dll_directory` so orlib20.dll is findable (though `TsDevLib`'s `SetDllDirectoryW` should handle this).
  - `insert(self, name, component_type, source)`:
    - Call `self.request32('insert', name, component_type, source)`.
    - Return the result dict (or a typed wrapper / dataclass).
  - `delete(self, name, component_type)`:
    - Call `self.request32('delete', name, component_type)`.
  - `modify(self, name, component_type, new_source)`:
    - Call `self.request32('modify', name, component_type, new_source)`.
  - `find(self, name, component_type)`:
    - Call `self.request32('find', name, component_type)`.
  - Optionally, provide a context manager (`__enter__`/`__exit__`) that calls `shutdown_server32()` on exit.
- Create a convenience wrapper class or function for ergonomic usage:
  - `TsDev` class that wraps `TsDevClient64` and provides typed methods (e.g., `insert_strategy(name, source)`, `insert_function(name, source)`, `insert_indicator(name, source)`).
  - Or simply document the `component_type` integer mapping (0=Strategy, 2=Indicator, 4=Function) and let the user pass integers.
- Write a minimal smoke test (64-bit Python) that creates a `TsDevClient64`, calls `find` on an existing component, and verifies the response.

**NOT in scope:** End-to-end integration tests for all three operations (TVS-D.7).

**Acceptance criteria:**
- `python/tsdev_client64.py` is written and importable in 64-bit Python.
- The `TsDevClient64` class correctly subclasses `Client64`.
- The constructor starts the 32-bit server and the server successfully loads the DLL and calls `tsdev_init`.
- `insert`, `delete`, `modify`, `find` methods correctly call `request32` and return the results.
- A minimal smoke test from 64-bit Python confirms the bridge works (server starts, a `find` call returns a result).

**Dependencies:** TVS-D.1 (Client64 API), TVS-D.5 (Server32 module must exist).

**Adversarial review:** YES — review the Client64 subclass, the request32 calls, the error handling, and the constructor parameters.

---

### TVS-D.7 — IMPLEMENTATION: End-to-end integration tests from 64-bit Python

**Goal:** Write pytest integration tests that exercise the full INSERT/DELETE/MODIFY cycle from 64-bit Python through the msl-loadlib bridge to orlib20.dll.

**Type:** IMPLEMENTATION

**Effort:** M

**Scope:**
- Create `python/tests/test_tsdev_integration.py` (pytest tests, run from 64-bit Python):
  - **Test INSERT:** Create a test strategy with simple EasyLanguage source → verify success, `prog_id > 0` → verify `.ell` file exists on disk → clean up (delete).
  - **Test DELETE:** Insert a test strategy → delete it → verify success → verify `.ell` file removed → verify `find` returns 0.
  - **Test DELETE not-found:** Delete a nonexistent component → verify `not_found` is true.
  - **Test MODIFY:** Insert a test strategy → modify it with new source → verify success → verify `prog_id` is preserved → verify `.ell` timestamp changed → clean up.
  - **Test MODIFY not-found:** Modify a nonexistent component → verify `not_found` is true.
  - **Test MODIFY compile failure:** Insert a strategy → modify with intentionally broken EasyLanguage → verify `success` is false and `error` is non-empty → clean up.
  - **Test FIND:** Insert a strategy → find it → verify `prog_id` matches → clean up.
  - **Test FIND not-found:** Find a nonexistent component → verify `prog_id` is 0.
  - **Test INSERT duplicate:** Insert a strategy → insert the same name again → verify failure (or appropriate error).
- Use unique test component names (e.g., `PyTestStrategy_<timestamp>`) to avoid collisions with real components.
- Ensure each test cleans up after itself (delete test components, verify removal).
- Tests must be runnable with: `pytest python/tests/test_tsdev_integration.py` from 64-bit Python.
- Tests must be skipped or error clearly if the TS installation or the DLL is not found.
- Consider using `host=None` mocking for unit tests that don't need the real DLL (if applicable).

**NOT in scope:** Performance testing, concurrent access testing, testing with unresolved component types (ActivityBar, PaintBar, etc.).

**Acceptance criteria:**
- All integration tests pass from 64-bit Python via the msl-loadlib bridge.
- INSERT, DELETE, MODIFY, and FIND all work end-to-end.
- Error cases (not-found, compile failure, duplicate) are handled correctly.
- Test components are cleaned up (no leftover `.ell` files).
- No crashes, no WerFault, no orphaned 32-bit server processes.
- The test suite can be run repeatably (idempotent — no state leakage between runs).

**Dependencies:** TVS-D.4 (DLL), TVS-D.5 (Server32), TVS-D.6 (Client64).

**Adversarial review:** YES — review the test coverage, cleanup logic, idempotency, and error case handling.

---

## 4. Research Interleaving Summary

| TVS | Type | Effort | Depends on |
|-----|------|--------|------------|
| TVS-D.1 | RESEARCH | S | — |
| TVS-D.2 | RESEARCH | S | TVS-D.1 |
| TVS-D.3 | RESEARCH | S | TVS-D.1 |
| TVS-D.4 | IMPLEMENTATION | M | TVS-D.2, TVS-D.3 |
| TVS-D.5 | IMPLEMENTATION | S | TVS-D.1, TVS-D.2, TVS-D.4 |
| TVS-D.6 | IMPLEMENTATION | S | TVS-D.1, TVS-D.5 |
| TVS-D.7 | IMPLEMENTATION | M | TVS-D.4, TVS-D.5, TVS-D.6 |

**Execution order:** D.1 → D.2 + D.3 (parallel) → D.4 → D.5 → D.6 → D.7.

The first three TVSs are RESEARCH. No implementation begins until the msl-loadlib architecture is confirmed (D.1), the C ABI is designed (D.2), and the build/environment is verified (D.3). Only then do implementation TVSs follow: build the DLL (D.4), write the server (D.5), write the client (D.6), write integration tests (D.7).

---

## 5. Risk Register

| # | Risk | Likelihood | Impact | Mitigation |
|---|------|-----------|--------|------------|
| R1 | **msl-loadlib version/API changes** — the frozen server or API may differ from what was researched. | Low | High | Pin msl-loadlib version in requirements. TVS-D.1 confirms the exact version and API. |
| R2 | **32-bit Python dependency** — the frozen `server32-windows.exe` is shipped with msl-loadlib, but if a custom 32-bit server is needed (e.g., for additional packages), a 32-bit Python installation would be required. | Low | Medium | TVS-D.1/D.3 confirm whether the frozen server suffices. The ctypes-only approach should work with the frozen server. |
| R3 | **Threading model mismatch** — `Server32` is single-threaded (HTTPServer), which is favorable. But if the HTTP server ever becomes multi-threaded (e.g., `ThreadingHTTPServer`), the thread-affine EDIT control and orlib20 state could break. | Low | High | Document the single-threaded requirement. Do not use `ThreadingHTTPServer`. The `TsDevLib` singleton + critical section provides a safety net. |
| R4 | **std::string across ctypes boundary** — `TsDevLib` methods return `std::string` (e.g., `verifyModule`). This cannot cross the ctypes boundary. | N/A (designed out) | N/A | The C ABI (TVS-D.2) uses caller-allocated `char*` buffers with size parameters. No `std::string` crosses the boundary. |
| R5 | **64MB-stack worker thread requirement** — TSDev.exe uses a 64MB-stack worker thread for compilation. The current implementation patches thread checks and calls orlib20 directly with a 1MB stack (works for simple strategies). Complex compilations may need more stack. | Medium | High | TVS-D.3 verifies the frozen server's stack size. If insufficient, the DLL (TVS-D.4) can spawn a worker thread with a configurable stack size (e.g., 64MB) for compilation operations. The contingency plan is documented in TVS-D.2. |
| R6 | **Hidden EDIT control thread affinity** — the EDIT control is created on the constructor's thread. `WM_SETTEXT`/`WM_GETTEXT` work cross-thread via `SendMessage`, but the control must remain alive. If the 32-bit server's thread is killed before `tsdev_shutdown`, the control leaks. | Low | Medium | `shutdown_handler()` in the Server32 module (TVS-D.5) calls `tsdev_shutdown()` which destroys the EDIT control and calls `OrlibExit`. |
| R7 | **orlib20.dll not found by the 32-bit server** — the frozen 32-bit Python server may not have the TS Program directory in its DLL search path. | Medium | High | `TsDevLib` constructor calls `SetDllDirectoryW` to the TS Program dir. Additionally, `Client64`'s `add_dll_directory` parameter can add the TS Program dir to the 32-bit server's search path. TVS-D.3 confirms this works. |
| R8 | **DLL export name decoration** — MSVC may decorate `extern "C"` `__cdecl` functions with a leading underscore on 32-bit. ctypes `cdll` handles this, but the exact export names must be verified. | Low | Medium | TVS-D.4 verifies export names with `dumpbin /exports`. Use `extern "C"` + `__declspec(dllexport)` which produces undecorated names on MSVC for `__cdecl`. |
| R9 | **Pickle serialization of large source code** — EasyLanguage source code passed via `request32` is pickled. Very large source files may hit size limits or slow down the IPC. | Low | Low | Pickle handles large strings. If needed, pass source via a temp file or shared memory. Unlikely to be an issue for typical EasyLanguage source. |
| R10 | **Concurrent client instances** — multiple `TsDevClient64` instances would start multiple 32-bit servers, each loading orlib20.dll. orlib20 is ref-counted (`OrlibInit`/`OrlibExit`), but multiple servers modifying the same library could conflict. | Low | High | Document that only one client instance should be active at a time. The singleton pattern in the DLL is per-process (each server has its own). |
| R11 | **Frozen server Python version mismatch** — the frozen server uses Python 3.14.6. If `tsdev_server32.py` uses syntax or features not available in 3.14, it will fail. | Low | Low | Keep the server module simple (standard library only: ctypes, msl.loadlib). TVS-D.1 confirms the Python version. |
| R12 | **Server process not terminated** — if the 64-bit Python process crashes without calling `shutdown_server32()`, the 32-bit server subprocess may be orphaned. | Medium | Medium | `Client64.__del__` attempts to shut down the server. The OS will clean up orphaned processes on Python exit. Document this as a known limitation. |
| R13 | **GetCompilingErrors empty buffer in headless mode** — `GetCompilingErrors` returns an empty buffer in headless mode; `verifyModule` has a fallback that synthesizes a message when `VerifyModule_New != 1`. This affects error-message fidelity, not correctness. | Known (pre-existing) | Low | Inherited from the current implementation. Document in the Python API. The C ABI preserves the fallback behavior. |

---

## 6. Constraints

1. **Standalone (no TSDev.exe):** The DLL and Python bridge must work without the TradeStation Development Environment GUI. All operations use direct orlib20.dll invocation.
2. **No WHServer killing:** The implementation must not terminate any TradeStation processes or services. Operations run headless.
3. **READ-ONLY TS install:** The TradeStation installation directory (`C:\Program Files (x86)\TradeStation 9.5`) must never be modified. The implementation modifies only the `MyWork\Bin` library (adding/removing `.ell` files) and `MyWork\MWRKArea.wrk` (via orlib20's internal save), which is the intended behavior of the library operations.
4. **32-bit orlib20:** orlib20.dll is a 32-bit PE. The bridge DLL must be 32-bit. The 32-bit Python server (frozen `server32-windows.exe`) loads the 32-bit DLL. The 64-bit Python client communicates via IPC.
5. **Adversarial review required:** Every TVS result (research notes, ABI design, DLL implementation, Python modules, integration tests) must be adversarially reviewed by an independent subagent per the AGENTS.md review process (APPROVE/DISAPPROVE loop).
6. **TDD where applicable:** TVS-D.4 (DLL) and TVS-D.7 (integration tests) follow the red-green method: write the test first, then implement to pass.
7. **Component type restriction:** Only the three confirmed warehouse types are supported: 0 = Strategy, 2 = Indicator, 4 = Function. The five unresolved types (ActivityBar, PaintBar, ProbabilityMap, ShowMe, TradingApp) must NOT be exposed (see FEASIBILITY.md §5.2).
8. **No exceptions across the C boundary:** The C ABI functions must catch all C++ exceptions and SEH exceptions, converting them to error codes. A ctypes caller cannot handle C++ exceptions.
9. **One subagent → one subtask:** Each TVS is executed by a single focused subagent. No multi-stage, multi-concern subtasks for any individual subagent.

---

## 7. File Layout (proposed)

```
tsdev_automation/
├── src/
│   ├── tsdev_lib.h              # existing — TsDevLib class
│   ├── tsdev_lib.cpp            # existing — TsDevLib implementation
│   ├── tsdev_bridge.h           # NEW (TVS-D.4) — C ABI header
│   ├── tsdev_bridge.cpp         # NEW (TVS-D.4) — C ABI implementation
│   └── test_bridge.cpp          # NEW (TVS-D.4) — DLL test exe
├── python/
│   ├── tsdev_server32.py        # NEW (TVS-D.5) — Server32 subclass
│   ├── tsdev_client64.py        # NEW (TVS-D.6) — Client64 subclass
│   └── tests/
│       └── test_tsdev_integration.py  # NEW (TVS-D.7) — pytest integration tests
├── docs/
│   ├── D1_msl_loadlib_research.md    # NEW (TVS-D.1) — research notes
│   ├── D2_c_abi_design.md            # NEW (TVS-D.2) — C ABI specification
│   ├── D3_build_env_research.md      # NEW (TVS-D.3) — build/env notes
│   └── HANDOFF_D_PYTHON_DLL.md       # THIS document
└── CMakeLists.txt               # MODIFIED (TVS-D.4) — add tsdev_bridge32 target
```

---

## 8. Key Source References for the Implementing Team

| What | Where | Why |
|------|-------|-----|
| TsDevLib class API | `src/tsdev_lib.h` | The C++ API being wrapped by the C ABI |
| TsDevLib implementation | `src/tsdev_lib.cpp` | The 20-step INSERT flow, DELETE orchestrator, MODIFY composition, SEH wrappers, singleton-relevant constructor/destructor |
| CMake build | `CMakeLists.txt` | Current build setup (Win32/x86, static lib + test exes) |
| orlib20 primitive table | `docs/FEASIBILITY.md` §5.1 | Confirmed signatures for all orlib20 functions |
| WHType enum | `docs/FEASIBILITY.md` §5.2 | Component type mapping (0=Strategy, 2=Indicator, 4=Function) |
| INSERT/MODIFY/DELETE pseudocode | `docs/FEASIBILITY.md` §5.3 | The call sequences the C ABI must replicate |
| Persistence patch | `docs/FEASIBILITY.md` §5.6 / `src/tsdev_lib.cpp` L695-709 | The FUN_10011230 patch (applied in TsDevLib constructor) |
| DELETE orchestrator | `docs/FEASIBILITY.md` §5.7 / `src/tsdev_lib.cpp` L500-623 | The FUN_101a8360 delete flow |
| MODIFY flow | `docs/FEASIBILITY.md` §5.8 / `src/tsdev_lib.cpp` L630-660 | The findExistPos → saveSource → verifyModule composition |
| Thread-check patches | `src/tsdev_lib.cpp` L711-759 | The three INT3→NOP patches enabling cross-thread calls |
| msl-loadlib Server32 | https://msl-loadlib.readthedocs.io/en/latest/_api/msl.loadlib.server32.html | Server32 base class API |
| msl-loadlib Client64 | https://msl-loadlib.readthedocs.io/en/latest/_api/msl.loadlib.client64.html | Client64 base class API |
| msl-loadlib C++ example | https://msl-loadlib.readthedocs.io/en/latest/cpp_source.html | Example C++ DLL with extern "C" exports |
| msl-loadlib IPC examples | https://msl-loadlib.readthedocs.io/en/latest/interprocess_communication.html | Server32/Client64 usage examples |
| msl-loadlib CHANGELOG | https://raw.githubusercontent.com/MSLNZ/msl-loadlib/main/CHANGELOG.md | Version history (v1.1.0, frozen server details) |
