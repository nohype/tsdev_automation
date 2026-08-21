# Build Guide — tsdev_automation

This project is a C++ CLI that loads the 32-bit `orlib20.dll` from the
TradeStation 9.5 installation to manage the TSDev Library (insert, modify,
delete, exists, get-source, upsert, and batch operations on EasyLanguage
strategies and functions) without the TSDev UI. **Every
build MUST target the x86 (32-bit) platform** — a 64-bit binary cannot load a
32-bit DLL.

## Prerequisites

### Visual Studio Build Tools (C++ workload)

The machine has **Visual Studio Build Tools 2026** (VS 18, MSVC 19.51)
installed at:

```
C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools
```

The **"Desktop development with C++"** workload
(`Microsoft.VisualStudio.Workload.VCTools`) must be installed. It provides the
MSVC compiler (`cl.exe`), the Windows SDK, `vcvarsall.bat`, and CMake.

If the C++ workload is missing, install it (run from an elevated prompt):

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\Installer\setup.exe" modify `
    --installPath "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools" `
    --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended `
    --quiet --norestart
```

### CMake

CMake is required to generate build files. The VS Build Tools ship an internal
CMake at:

```
C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
```

You may use that full path, add it to `PATH`, or source `vcvarsall.bat` (which
adds CMake to `PATH`) before running the commands below.

### Python 3

**Python 3** is required for the build-time signature codegen step
(`tools/sigcodegen.py`). CMake locates it automatically via
`find_package(Python COMPONENTS Interpreter REQUIRED)`. Ensure `python.exe` is
on your `PATH` (or set the `Python_EXECUTABLE` CMake variable).

### Tool locations (after C++ workload is installed)

| Tool          | Path                                                                                      |
|---------------|-------------------------------------------------------------------------------------------|
| vcvarsall.bat | `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat` |
| cl.exe (x86)  | `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\<ver>\bin\Hostx64\x86\cl.exe` |
| cmake.exe     | `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` |

## Project structure

```
tsdev_automation/
├── src/            Production source code (6 files)
│   ├── tsdev_lib.cpp / tsdev_lib.h       orlib20 wrapper engine (RAII API)
│   ├── orlib_resolver.cpp / orlib_resolver.h   signature-scan resolver layer
│   ├── tsdev_cli.cpp                     production CLI (insert/modify/delete/exists/get-source/upsert/batch-insert/batch-modify/batch-delete/batch-upsert/batch-insert-dir/batch-modify-dir/batch-delete-dir/batch-upsert-dir)
│   └── sigtool.cpp                       signature-verification CLI
├── tests/          Test files (27 .cpp + 16 .ps1 + 2 .py + 1 .psm1)
├── archive/        Archived experimental probes (27 files, NOT built)
├── signatures/     JSON signature data — the single source of truth
│   ├── v9.50.01.3344.json                per-version signatures + patch sites
│   └── version_manifest.json             version manifest (lists known builds)
├── tools/          Build-time tooling
│   ├── sigcodegen.py                     generates signatures.gen.h from JSON
│   └── create_bundle_zip.ps1             assembles the artifacts/*.zip bundle zip (invoked by CMake POST_BUILD)
├── third_party/    Vendored libraries
│   └── nlohmann/                         nlohmann/json (used by sigtool)
├── docs/           Documentation
├── tmp/            Gitignored reverse-engineering artifacts (NOT needed to build)
├── CMakeLists.txt  Build definition
└── build/          CMake build output (generated)
```

### Source files (`src/`)

| File                  | Purpose                                                        |
|-----------------------|---------------------------------------------------------------|
| `tsdev_lib.cpp/.h`    | RAII wrapper around orlib20.dll: init, lookup, create, save, compile/verify, close, delete, modify. |
| `orlib_resolver.cpp/.h` | Encapsulates all version-specific orlib20.dll knowledge: loads the DLL, detects the version, resolves internal functions by signature scan, validates and applies patch sites. |
| `tsdev_cli.cpp`       | Production CLI exposing `insert`, `modify`, `delete`, `exists`, `get-source`, `upsert`, `batch-insert`, `batch-modify`, `batch-delete`, `batch-upsert`, `batch-insert-dir`, `batch-modify-dir`, `batch-delete-dir`, `batch-upsert-dir`, and `serve` subcommands. Also includes VEH crash handler, elevation guard, serve dispatch, and named pipe server. |
| `sigtool.cpp`         | CLI for verifying signatures against a DLL on disk, validating patch sites, inspecting DLL info, and ad-hoc pattern scanning. |

## Build instructions

All commands are run from the project root
(`.`).

### 1. (Optional) Set up the developer environment

The Visual Studio generator (`-A Win32`) does **not** require sourcing
`vcvarsall.bat` — CMake locates the toolchain automatically. You only need this
step if you want `cmake`/`dumpbin`/`cl` on your `PATH` for manual use:

```cmd
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x86
```

### 2. Configure (generates build files, forces 32-bit)

```cmd
cmake -S . -B build -A Win32
```

`-A Win32` is the key flag: it tells the Visual Studio generator to target the
x86 platform. The `CMakeLists.txt` also contains a guard that aborts
configuration with a clear error if `sizeof(void*) != 4`, so a misconfigured
64-bit build cannot slip through.

### 3. Build

```cmd
cmake --build build --config Release
```

### 4. Output binaries

```
build\Release\tsdev_cli.exe        (production CLI)
build\Release\sigtool.exe          (signature-verification CLI)
build\Release\test_*.exe           (test executables)
```

### 5. Bundle output (`dist/` and `artifacts/`)

The same `cmake --build build --config Release` command automatically
assembles a distributable bundle via a POST_BUILD custom command on the
`tsdev_cli` target. No separate build step is required.

**`dist/`** — the assembled bundle at the repo root:

```
dist/
├── tsdev_cli.exe        # copied from build\Release\tsdev_cli.exe
├── tsdev_automation/    # copied from src\tsdev_automation\*.py (7 files)
└── README.md            # copied from docs\BUNDLE_README.md (quick-start guide)
```

`copy_if_different` is used throughout so unchanged files keep their
timestamps. The `tsdev_automation/` subdirectory is wiped and re-copied on
every build so renamed/deleted `.py` files do not go stale.

**`artifacts/`** — an automatically generated timestamped zip, ready to
distribute:

```
artifacts/tsdev_automation_<yymmdd><HHMM>.zip
```

For example, `tsdev_automation_2608201430.zip` for a build on 2026-08-20 at
14:30. The zip is **selective** — it contains exactly the three bundle
components (`tsdev_cli.exe`, `tsdev_automation/`, `README.md`) and nothing
else (no wheels, sdist, or `__pycache__/`). Each build creates a new
timestamped zip; a build in the same minute as a previous one overwrites it
(`-Force`).

Both `dist/` and `artifacts/` are gitignored via the root `.gitignore`, so
generated artifacts never appear in `git status`.

## Codegen step (automatic)

Signature data lives in committed JSON files under `signatures/` — this is the
**single source of truth** for all orlib20.dll function RVAs, patch-site byte
patterns, and version fingerprints. At build time, CMake runs
`tools/sigcodegen.py` automatically (via the custom target `siggen`) to
generate `build/gen/signatures.gen.h` from
`signatures/version_manifest.json` and `signatures/v9.50.01.3344.json`.

`orlib_resolver.cpp` consumes the generated header (it no longer includes the
gitignored `tmp/re_artifacts/b1_signatures.h`). **No manual codegen step is
needed** — it runs as part of every build. The generated header is rebuilt
automatically whenever the JSON sources or `sigcodegen.py` change (CMake
dependency tracking).

## Fresh-clone buildability

A fresh clone builds successfully **without** `tmp/`. The `tmp/` directory
holds gitignored reverse-engineering artifacts (Ghidra evidence, Frida traces,
probe binaries) and is **not** a build dependency. All signature data required
to build is committed in `signatures/*.json`, so a clean checkout configures
and builds end-to-end with just the commands above.

## Test instructions

### C++ test executables

The test executables are built by the same `cmake --build` command above. Run
them directly from the build output directory:

```cmd
build\Release\test_insert.exe
build\Release\test_insert_tradingapp.exe
build\Release\test_auto_cleanup.exe
build\Release\test_delete.exe
build\Release\test_delete_tradingapp.exe
build\Release\test_delete_check.exe
build\Release\test_delete_diag.exe
build\Release\test_modify.exe
build\Release\test_modify_tradingapp.exe
build\Release\test_tradingapp_type.exe
build\Release\test_path_discovery.exe
build\Release\test_persistence_validation.exe
build\Release\test_no_hardcoded_rvas.exe
build\Release\test_orlib_resolver.exe
build\Release\test_exists.exe
build\Release\test_enumerate.exe
build\Release\test_upsert.exe
build\Release\test_batch_upsert.exe
build\Release\test_get_source.exe
build\Release\test_directory_batch.exe
build\Release\test_elevation_guard.exe
build\Release\test_json_serialize.exe
build\Release\test_serve_dispatch.exe
build\Release\test_serve_framing.exe
build\Release\test_crash_handler.exe
build\Release\test_showme_type.exe
```

Each test exits `0` on success and non-zero on failure.

**TS dependency:** the integration tests (`test_insert`, `test_insert_tradingapp`,
`test_auto_cleanup`, `test_delete`, `test_delete_tradingapp`, `test_delete_check`,
`test_delete_diag`, `test_modify`, `test_modify_tradingapp`, `test_exists`,
`test_enumerate`, `test_upsert`, `test_batch_upsert`, `test_get_source`,
`test_directory_batch`) require a TradeStation 9.5 installation
on the machine — they load the real `orlib20.dll` and exercise the live warehouse.
`test_path_discovery` and `test_persistence_validation` also require a TradeStation
install: they construct `TsDevLib`, which loads `orlib20.dll`. The following tests do **not** require a live TradeStation install:

- `test_no_hardcoded_rvas` — static check against the generated signature header.
- `test_tradingapp_type` — pure unit test for the `TradingApp` enum entry and
  `getCategory`/`getSubType` helpers (no orlib20 dependency).
- `test_orlib_resolver` — operates on an in-memory PE image (a copied
  `orlib20_baseline.dll`); no live TradeStation install needed.
- `test_elevation_guard` — pure unit test for the `isElevated()` helper and `errorCodeString(11)` (no orlib20 dependency).
- `test_json_serialize` — pure unit test for `cliResultToJson()` serialization (no orlib20 dependency).
- `test_serve_dispatch` — pure unit test for `parseServeRequest()` validation logic (no orlib20 dependency).
- `test_serve_framing` — pure unit test for `writeFramed`/`readFramed` helpers (no orlib20 dependency).
- `test_crash_handler` — tests the VEH crash handler in both normal mode (exit 0) and `--crash` mode (deliberately triggers an access violation; the VEH catches it and calls `TerminateProcess` with the NT status code). No orlib20 dependency. **Note:** the `--crash` mode test deliberately crashes the process — run it in isolation, not alongside other tests.
- `test_showme_type` — pure unit test for the `ShowMe` enum entry and `getCategory`/`getSubType` helpers (no orlib20 dependency).

### Test runner scripts

PowerShell scripts in `tests/` orchestrate the test suites. Run them from
the project root after building. The master runner (`run_regression.ps1`)
invokes all sub-suites in a single command; the individual runners are
documented below for developers who want to run a single sub-suite
independently.

#### `tests/run_regression.ps1` — master regression runner

The single-command regression runner orchestrates **all 12 sub-suites** (135
test cases total) in the order required by the B.6 acceptance criteria:

1. **sigtool verify gate** (`tests/test_sigtool.ps1`) — 12 sigtool CLI tests
   against the baseline DLL. Fast; no live TradeStation install needed.
2. **C++ unit tests** (`tests/run_unit_tests.ps1`) — 26 C++ test executables
   (or 9 with `-NoTs`). Medium duration.
3. **E2E CLI tests** (`tests/run_e2e_tests.ps1`) — 34 E2E CLI test cases per
   `docs/E2E_TEST_SPEC.md`. Slowest; creates and deletes real warehouse entries.
4. **exists tests** (`tests/test_exists.ps1`) — 5 exists CLI test cases.
5. **batch-stdin tests** (`tests/test_batch_stdin.ps1`) — 8 inline batch test
   cases via stdin pipe.
6. **upsert tests** (`tests/test_upsert.ps1`) — 9 upsert CLI test cases.
7. **get-source tests** (`tests/test_get_source.ps1`) — 7 get-source CLI test
   cases.
8. **exists-ci tests** (`tests/test_exists_ci.ps1`) — 9 case-insensitive exists
   CLI test cases.
9. **batch-upsert tests** (`tests/test_batch_upsert.ps1`) — 5 batch-upsert CLI test cases (manifest file + --batch-stdin modes, mixed insert/modify paths, ProgId preservation, compile-error, empty manifest, no-args).
10. **directory-batch tests** (`tests/test_directory_batch.ps1`) — 9 directory-batch CLI test cases (DB1–DB9: batch-insert-dir/batch-modify-dir/batch-delete-dir/batch-upsert-dir with ProgId preservation, `-t` extension override, empty dir, unknown type, non-existent dir, missing `-t` arg). Requires live TS; skipped under `-NoTs` or `-SkipDirectoryBatch`.
11. **serve E2E tests** (`tests/run_serve_e2e.ps1` → `tests/test_serve_e2e.py`) — 2 serve-mode end-to-end pytest cases. Requires live TS AND an elevated parent shell; skipped under `-NoTs` or `-SkipServeE2e`.
12. **ShowMe E2E tests** (`tests/test_showme_e2e.ps1`) — 9 ShowMe-component E2E test cases (SM1–SM9). Requires live TS; skipped under `-NoTs` or `-SkipShowMeE2e`.

Each sub-suite runs in its own child PowerShell process so a crash or hang in
one suite cannot affect the others. After all suites run, a grand summary table
is printed and a single aggregated exit code is returned.

| Parameter       | Type    | Default          | Description                                                              |
|-----------------|---------|------------------|--------------------------------------------------------------------------|
| `$BuildDir`     | string  | `build\Release`  | Directory containing the built executables; passed to `run_unit_tests`. |
| `$TsPath`       | string  | (none)           | TradeStation install dir; passed to E2E runner as `-TsPath`.            |
| `-NoTs`         | switch  | —                | Skip all TS-dependent suites (3–12); pass `-NoTs` to unit tests (9 non-TS tests only); still run sigtool. |
| `-SkipSigtool`  | switch  | —                | Skip the sigtool verify gate suite.                                      |
| `-SkipUnit`     | switch  | —                | Skip the C++ unit test suite.                                            |
| `-SkipE2e`      | switch  | —                | Skip the E2E CLI test suite.                                             |
| `-SkipExists`   | switch  | —                | Skip the exists E2E test suite.                                          |
| `-SkipBatchStdin` | switch | —               | Skip the batch-stdin E2E test suite.                                     |
| `-SkipUpsert`   | switch  | —                | Skip the upsert E2E test suite.                                          |
| `-SkipGetSource` | switch | —                | Skip the get-source E2E test suite.                                      |
| `-SkipExistsCi` | switch  | —                | Skip the exists-ci E2E test suite.                                       |
| `-SkipBatchUpsert` | switch | —              | Skip the batch-upsert E2E test suite.                                    |
| `-SkipDirectoryBatch` | switch | —           | Skip the directory-batch E2E test suite.                                 |
| `-SkipServeE2e` | switch  | —                | Skip the serve E2E test suite.                                           |
| `-SkipShowMeE2e` | switch | —                | Skip the ShowMe E2E test suite.                                          |
| `-Verbose`      | switch  | —                | Passed through to all sub-scripts (full output for every test).         |

Exit codes: `0` = all executed suites passed, `1` = one or more suites failed
(or a prerequisite was not met).

**Prerequisites:**
- All executables built. The dedicated CMake target builds every test
  executable plus `tsdev_cli` and `sigtool` in one step:
  `cmake --build build --config Release --target test_regression`
- TradeStation 9.5 installed (for the TS-dependent suites 3–12, unless
  `-NoTs` or the matching `-Skip*` flag is used).
- Baseline DLL at `tests/fixtures/orlib20_baseline.dll` (for the sigtool
  suite and `test_orlib_resolver`). This file is **committed to the repo**,
  so it is available immediately after clone/checkout — no copy needed. If
  the DLL is somehow missing, the test runners will **prompt you to copy it
  from the TradeStation installation** (see
  [Interactive baseline DLL prompting](#interactive-baseline-dll-prompting)
  below). See [Updating the baseline DLL](#updating-the-baseline-dll) below
  for how to replace it when a new TradeStation version is released.
- No TradeStation processes/services running (for the TS-dependent suites 3–12).

```powershell
# Full run — all 12 suites (135 test cases: sigtool 12 + 26 unit + 34 E2E + 5 exists + 8 batch-stdin + 9 upsert + 7 get-source + 9 exists-ci + 5 batch-upsert + 9 directory-batch + 2 serve E2E + 9 ShowMe E2E):
powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1

# No-TradeStation run — sigtool + 9 non-TS unit tests; skips all TS-dependent suites:
powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1 -NoTs

# Non-default TradeStation install path:
powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1 -TsPath "D:\TradeStation 9.5"

# Selective runs:
powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1 -SkipE2e   # sigtool + all 26 unit tests + sub-suites 4–12
powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1 -SkipUnit  # sigtool + E2E + sub-suites 4–12 (no C++ unit tests)
powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1 -Verbose   # full output for every test
```

#### `tests/run_unit_tests.ps1` — C++ unit test runner

Runs all 26 C++ test executables in sequence (not in parallel — the shared
warehouse file `MWRKArea.wrk` is not process-safe), captures exit codes, measures
per-test duration, and returns a single aggregated exit code.

| Parameter    | Type    | Default          | Description                                                  |
|--------------|---------|------------------|--------------------------------------------------------------|
| `$BuildDir`  | string  | `build\Release`  | Directory containing the test executables.                   |
| `-NoTs`      | switch  | —                | Run only the 9 non-TS-dependent tests; skip the rest.        |
| `-Verbose`   | switch  | —                | Print full stdout/stderr for every test, not just failures.  |

Exit codes: `0` = all executed tests passed, `1` = one or more failed (or the
build directory is missing).

```powershell
# Run all 26 tests:
powershell -ExecutionPolicy Bypass -File tests\run_unit_tests.ps1

# Run only the 9 non-TS tests (no TradeStation install needed):
powershell -ExecutionPolicy Bypass -File tests\run_unit_tests.ps1 -NoTs

# Run all tests with full output:
powershell -ExecutionPolicy Bypass -File tests\run_unit_tests.ps1 -Verbose
```

#### `tests/run_e2e_tests.ps1` — E2E CLI test runner

Implements the 34-test-case E2E CLI test specification in
`docs/E2E_TEST_SPEC.md`, covering 4 of the 5 supported component types
(Strategy, Indicator, Function, TradingApp) across three parts: Part A (16 tests,
insert/modify/delete lifecycle), Part B (8 tests, type-specific semantic
errors), Part C (10 tests, general errors). This runner contains **no ShowMe
cases** — ShowMe end-to-end coverage lives in the separate
`tests/test_showme_e2e.ps1` script (cases SM1–SM9), which the master
regression runner (`tests/run_regression.ps1`) executes as Suite 12. Each test
writes ASCII-encoded
EasyLanguage source to a temp file, invokes `tsdev_cli.exe`, and verifies the
exit code and key output substrings. A markdown report is written to
`tmp/re_artifacts/e2e_all_types_<runId>.md`. Robust try/finally cleanup
re-deletes every test name and verifies no orphaned `.ell` files remain.

| Parameter   | Type    | Default                                   | Description                                                       |
|-------------|---------|-------------------------------------------|-------------------------------------------------------------------|
| `$CliPath`  | string  | `build\Release\tsdev_cli.exe` (resolved)  | Path to the `tsdev_cli.exe` executable.                           |
| `$TsPath`   | string  | (none)                                    | TradeStation install dir, passed to the CLI as `--ts-path <dir>`. |
| `-Verbose`  | switch  | —                                         | Print full CLI output for every test, not just failures.          |

Exit codes: `0` = all 34 tests passed, `1` = one or more failed (or a
prerequisite was not met). Requires a TradeStation 9.5 install (or `--ts-path`
provided) and no TradeStation processes/services running.

```powershell
# Run all 34 E2E tests:
powershell -ExecutionPolicy Bypass -File tests\run_e2e_tests.ps1

# Use a non-default TradeStation install path:
powershell -ExecutionPolicy Bypass -File tests\run_e2e_tests.ps1 -TsPath "D:\TradeStation 9.5"

# Run with full CLI output for every test:
powershell -ExecutionPolicy Bypass -File tests\run_e2e_tests.ps1 -Verbose
```

### sigtool test script

A PowerShell-based test for `sigtool` lives at `tests/test_sigtool.ps1`. It
exercises every `sigtool` subcommand (12 test cases) against the baseline DLL
and the committed `v9.50.01.3344` signature set. Run it from the project root
after building:

```powershell
powershell -ExecutionPolicy Bypass -File tests\test_sigtool.ps1
```

**Prerequisite — baseline DLL:** the sigtool tests (and `test_orlib_resolver`)
require a baseline copy of `orlib20.dll` at
`tests/fixtures/orlib20_baseline.dll`. This file is **committed to the repo**,
so it is available immediately after clone/checkout — no copy needed. If the
DLL is missing, the script will **prompt you to copy it from the TradeStation
installation** (see
[Interactive baseline DLL prompting](#interactive-baseline-dll-prompting)
below). See [Updating the baseline DLL](#updating-the-baseline-dll) below for
how to replace it when a new TradeStation version is released.

Exit codes: `0` = all 12 tests passed, `1` = at least one failed (or a
prerequisite fixture was missing).

### Interactive baseline DLL prompting

If the baseline DLL (`tests/fixtures/orlib20_baseline.dll`) is missing at test
start, the dependent test runners (`tests/test_sigtool.ps1`,
`tests/run_unit_tests.ps1`, `tests/run_regression.ps1`) **prompt the user**
whether to copy it from the TradeStation installation. They do **not**
continue without a user decision. If the user declines, the dependent tests
**fail with a loud, clear message** rather than silently skipping or
proceeding with cryptic errors.

The prompting logic lives in the shared module `tests/TestHelpers.psm1`
(`Ensure-BaselineDll`), which uses `Find-TsInstallPath` to discover the
TradeStation install directory:

1. **`--ts-path` override** — if a `-TsPath` parameter is provided and the
   directory exists, it is used directly. (`run_regression.ps1` accepts
   `-TsPath` and passes it through.)
2. **Registry** — `HKLM\SOFTWARE\TradeStation Technologies\TradeStation\Installing`
   → `RootDirectory` value, opened with the `KEY_WOW64_32KEY` flag (the key
   path does **not** contain `WOW6432Node` — the 32-bit redirection flag
   handles it).
3. **Registry fallback** —
   `HKLM\SOFTWARE\TradeStation Technologies\TradeStation\Versions\9.05.00`
   → `RootDirectory` value (same `KEY_WOW64_32KEY` flag).
4. **Conventional default** — `$env:ProgramFiles(x86)\TradeStation 9.5`
   (read at runtime, not hardcoded).
5. Returns `$null` if none of the above resolve to an existing directory.

When the DLL is missing and a TS install is detected, the prompt offers to
auto-copy `<TS install>\Program\orlib20.dll` to
`tests/fixtures/orlib20_baseline.dll`. If the user accepts, the copy is
performed and the tests proceed. If the user declines (or the TS install
cannot be found), the dependent tests fail loudly:

- `test_sigtool.ps1` exits `1` immediately (the entire script depends on the
  DLL).
- `run_unit_tests.ps1` marks `test_orlib_resolver` as FAILED with a loud
  "FATAL: baseline DLL not available" message, continues running the other
  unit tests (they do not need the DLL), and ultimately exits `1`.
- `run_regression.ps1` prompts once in the orchestrator; if the user
  declines, the sigtool suite is marked FAILED (not launched) and
  `run_unit_tests.ps1` is launched with `-SkipBaselineDllPrompt` so it marks
  `test_orlib_resolver` as FAILED without re-prompting. Other suites (non-DLL
  unit tests, E2E tests) still run; the script ultimately exits `1`.

## sigtool

`sigtool` is a CLI for verifying orlib20.dll signatures against a DLL on disk.
It loads a signature set from the committed JSON, scans the DLL's `.text`
section, and reports whether each target matched uniquely. It also supports
patch-site validation, DLL info inspection, ad-hoc pattern scanning, and
version-manifest lookup. Run `sigtool --help` for the full command reference.

## CMake targets

| Target                          | Type     | Description                                                        |
|---------------------------------|----------|--------------------------------------------------------------------|
| `siggen`                        | custom   | Runs `sigcodegen.py` to generate `build/gen/signatures.gen.h`.     |
| `orlib_resolver`                | library  | Static library encapsulating orlib20.dll version-specific knowledge. |
| `tsdev_lib`                     | library  | Static library wrapping the orlib20 operation set (links `orlib_resolver`). |
| `tsdev_cli`                     | exec     | Production CLI (`insert`/`modify`/`delete`/`exists`/`get-source`/`upsert`/`batch-insert`/`batch-modify`/`batch-delete`/`batch-upsert`/`batch-insert-dir`/`batch-modify-dir`/`batch-delete-dir`/`batch-upsert-dir`/`serve`). Includes VEH crash handler, elevation guard, serve dispatch, and named pipe server. |
| `sigtool`                       | exec     | Signature-verification CLI.                                        |
| `test_regression`               | custom   | Build-only dependency target: builds all 26 test executables + `tsdev_cli` + `sigtool`. Test execution is performed by `tests/run_regression.ps1`. |
| `test_insert`                   | exec     | INSERT integration test (requires TS).                            |
| `test_insert_tradingapp`        | exec     | TradingApp INSERT integration test (requires TS).                 |
| `test_auto_cleanup`             | exec     | Auto-cleanup of partial warehouse entry on compile failure (requires TS — constructs TsDevLib). |
| `test_delete`                   | exec     | DELETE integration test (requires TS).                            |
| `test_delete_tradingapp`        | exec     | TradingApp DELETE integration test (requires TS).                 |
| `test_delete_check`             | exec     | Quick check that deletions persist across restarts (requires TS). |
| `test_delete_diag`              | exec     | DELETE diagnostic test (requires TS).                             |
| `test_modify`                   | exec     | MODIFY integration test (requires TS).                            |
| `test_modify_tradingapp`        | exec     | TradingApp MODIFY integration test (requires TS).                 |
| `test_path_discovery`           | exec     | TS install path discovery test (requires TS — constructs TsDevLib). |
| `test_persistence_validation`   | exec     | Persistence patch byte validation test (requires TS — constructs TsDevLib). |
| `test_tradingapp_type`          | exec     | TradingApp ComponentType unit test (no TS dependency).            |
| `test_no_hardcoded_rvas`        | exec     | Verifies no hardcoded RVAs leak into production code (no TS dependency). |
| `test_orlib_resolver`           | exec     | OrlibResolver unit test (in-memory PE image, no live TS install needed). |
| `test_exists`                   | exec     | EXISTS integration test — query item existence by name + type (requires TS). |
| `test_enumerate`                | exec     | Enumerate integration test — list warehouse items by type (requires TS). |
| `test_upsert`                   | exec     | UPSERT integration test — insert-or-update by name + type (requires TS). |
| `test_batch_upsert`             | exec     | BATCH-UPSERT integration test — multi-item upsert on shared TsDevLib, single closeWarehouse, ProgId preservation (requires TS). |
| `test_get_source`               | exec     | GET-SOURCE integration test — retrieve EL source by name + type (requires TS). |
| `test_directory_batch`          | exec     | DIRECTORY-BATCH unit test — parseDirectory() filesystem logic: extension filtering, stem extraction, empty dir, subdirectory skipping, source reading (requires TS — constructs TsDevLib via included tsdev_cli.cpp). |
| `test_elevation_guard`          | exec     | Elevation guard unit test — isElevated() helper + errorCodeString(11) (no TS dependency). |
| `test_json_serialize`           | exec     | JSON serialization unit test — cliResultToJson() (no TS dependency). |
| `test_serve_dispatch`           | exec     | Serve dispatch unit test — parseServeRequest() validation (no TS dependency). |
| `test_serve_framing`            | exec     | Serve framing unit test — writeFramed/readFramed helpers (no TS dependency). |
| `test_crash_handler`            | exec     | VEH crash handler test — normal mode (exit 0) + --crash mode (deliberate access violation, VEH catches + TerminateProcess). No TS dependency. Run --crash mode in isolation. |
| `test_showme_type`              | exec     | ShowMe ComponentType unit test — enum entry, getCategory/getSubType helpers (no TS dependency). |
| `test_stress_batch`             | exec     | Opt-in batch feasibility stress test (TVS-D.1) — N sequential insert+compile+verify ops then delete all N on ONE TsDevLib instance/worker thread (`STRESS_BATCH_N` env var, default 600). Not run by `run_regression.ps1`; build/run explicitly via `--target test_stress_batch`. Requires TS + admin. |

## Verification (confirm the binary is 32-bit)

### Run the CLI

```
> build\Release\tsdev_cli.exe --help
```

### Confirm the PE header (dumpbin)

From a shell with `vcvarsall.bat x86` sourced:

```
> dumpbin /headers build\Release\tsdev_cli.exe
...
             14C machine (x86)
                   32 bit word machine
             10B magic # (PE32)
...
```

- `14C machine (x86)` — the image targets the x86 (32-bit) instruction set.
- `10B magic # (PE32)` — a 32-bit PE image (PE32, not PE32+ which is 64-bit).

## Updating the baseline DLL

The baseline DLL (`tests/fixtures/orlib20_baseline.dll`) is a copy of
`orlib20.dll` from the TradeStation installation, committed to the repo so
that the regression suite works on a fresh clone without a TradeStation
install. When a new TradeStation version is released and the baseline needs
to be updated:

1. Copy the new `orlib20.dll` from the TradeStation install:
   ```powershell
   Copy-Item "C:\Program Files (x86)\TradeStation 9.5\Program\orlib20.dll" `
             "tests\fixtures\orlib20_baseline.dll"
   ```
2. Verify the file size and SHA-256 hash:
   ```powershell
   (Get-Item tests\fixtures\orlib20_baseline.dll).Length
   (Get-FileHash tests\fixtures\orlib20_baseline.dll -Algorithm SHA256).Hash
   ```
3. Update the expected hash and size in `docs/VERSION_DETECTION.md` and any
   other docs that reference the baseline DLL's fingerprint.
4. Update the committed signature set (`signatures/v9.50.01.3344.json`) if
   the version changed — the signatures must match the new DLL.
5. Run the regression suite to confirm all tests still pass:
   ```powershell
   powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1
   ```
6. Commit the updated DLL and signature set.

## Admin Privileges (TSDEV_REQUIRE_ADMIN)

The CMake option `TSDEV_REQUIRE_ADMIN` (default **ON**) controls whether
**all TS-accessing executables** require administrator privileges to start.
This covers `tsdev_cli.exe` plus the TS-dependent test executables that
load `orlib20.dll`. The 9 non-TS unit tests
(`test_no_hardcoded_rvas`, `test_tradingapp_type`, `test_orlib_resolver`,
`test_elevation_guard`, `test_json_serialize`, `test_serve_dispatch`,
`test_serve_framing`, `test_crash_handler`, `test_showme_type`) and
`sigtool` are excluded — they do not access the TSDev Library and must run
non-elevated in CI.

- **When ON (default):** each TS-accessing executable embeds
  `requestedExecutionLevel="requireAdministrator"` in its application manifest.
  Windows refuses to start them without an elevated token.
- **When OFF:** the manifests default to `asInvoker` (no elevation
  requirement). Pass `-DTSDEV_REQUIRE_ADMIN=OFF` at configure time to
  produce a non-elevated build variant:
  ```cmd
  cmake -S . -B build -A Win32 -DTSDEV_REQUIRE_ADMIN=OFF
  ```

**Why:** TradeStation Desktop and `TSDev.exe` always run elevated, so the
TSDev Library (`orlib20.dll`) is only accessible to elevated processes. The
CLI and TS-dependent test executables load this library and therefore must be
elevated.

**UX (one UAC prompt per session):** Run the *parent* process — your terminal,
Python session, or test runner — as Administrator once. All CLI subprocesses
spawned by that parent inherit the elevated token silently, so there are **no
per-invocation UAC prompts**. You get exactly one UAC prompt per session (to
elevate the parent), then silent inheritance for every CLI child.

**CI:** CI builds with the default ON. The 9 non-TS unit tests and `sigtool`
are excluded from the admin manifest (they do not access the TSDev Library),
so they run non-elevated in CI. Non-e2e Python tests mock `subprocess.run`, so
CI is unaffected by the elevation requirement.

## Serve mode (`--serve` / `use_serve`)

Serve mode is the **one-UAC UX** for the Python wrapper. Instead of requiring
the *parent* Python process to be elevated (which means one UAC prompt for the
terminal/IDE, then silent subprocess inheritance), serve mode lets a
**non-elevated** Python session trigger exactly **one** UAC prompt to start an
elevated broker process, then routes all subsequent operations to that broker
over a Windows named pipe. When the Python session is already elevated, the
direct `subprocess.run` path is used with zero overhead (no broker is started).

### `tsdev_cli serve` — the elevated named-pipe broker

The `serve` subcommand starts the CLI as an elevated named-pipe broker. It
creates a Windows named pipe, services multiple requests in a loop on a
long-lived 64 MB-stack worker thread (one `TsDevLib` instance for the whole
session, like `runBatch`), and exits when the pipe is closed or a `quit`
request is received.

```
tsdev_cli serve [--serve-pipe-file <path>]
```

- **Pipe name:** `\\.\pipe\tsdev_cli_<PID>` where `<PID>` is the broker's
  process ID (unique per broker instance). The broker writes the full pipe
  name (and its PID) to the file specified by `--serve-pipe-file <path>`
  immediately after `CreateNamedPipeW` succeeds, so the Python wrapper can
  poll that file and connect. If `--serve-pipe-file` is omitted, the pipe
  name is printed to stdout (for manual use).
- **Framing protocol:** every message (request and response) is a 4-byte
  little-endian length prefix followed by a UTF-8 JSON payload. The payload
  length is capped at 64 MiB (`67108864` bytes); messages outside this range
  close the connection. No trailing newline — the length prefix delimits
  messages.
- **Request/response JSON schema:** requests carry an `op` field
  (`insert`/`modify`/`delete`/`exists`/`upsert`/`get-source`/`quit`) plus
  `type`, `name`, `source` (raw EL text, not a file path), and optional
  `case_insensitive`/`output_path` fields. Responses are the **exact same
  JSON object** that `--json` mode emits today (produced by
  `cliResultToJson()`), plus a `{"op":"quit","success":true,"exit_code":0}`
  acknowledgement for the `quit` op. The full request/response schema,
  error-handling table, and connection-lifecycle details are specified in
  [`docs/workstream_serve_plan.md`](workstream_serve_plan.md) §2 (Protocol
  specification) — this section summarizes it and does not duplicate the
  spec.
- **`quit` op:** a request with `"op":"quit"` signals the broker to shut
  down gracefully. The broker responds with a success `CliResult`, then
  closes the pipe, calls `closeWarehouse()`, and exits. The Python wrapper
  sends `quit` on `TsDevClient` teardown (with a `TerminateProcess` fallback
  after a 5 s timeout).
- **Security:** the pipe is created with an explicit security descriptor
  (`D:P(A;;GA;;;BA)(A;;GWGR;;;SY)(A;;GWGR;;;OW)S:(ML;;NW;;;ME)`) —
  owner/admins/SYSTEM full control, owner read/write, and a Medium-IL
  mandatory label so a non-elevated (Medium-IL) client can write to the
  elevated (High-IL) broker's pipe. See
  [`docs/workstream_serve_plan.md`](workstream_serve_plan.md) §3 for the
  full security design.

### `use_serve` Python flag on `TsDevClient`

The `TsDevClient` constructor accepts a `use_serve` flag (default **`True`**)
that controls routing:

- **Default `True` (auto-elevate when not elevated):** when the Python
  session is **not** elevated, the first operation lazily auto-starts the
  elevated broker via `ShellExecuteExW` with `lpVerb="runas"` (one UAC
  prompt). All subsequent single-item ops (`insert`/`modify`/`delete`/
  `exists`/`get_source`/`upsert`) are routed over the named pipe to that
  same broker process — no further UAC prompts.
- **Falls back to `subprocess.run` when already elevated:** if the Python
  session is already elevated, the direct subprocess path is used with
  **zero overhead** (no broker is started, no pipe I/O). This preserves the
  pre-serve behavior for elevated sessions.
- **Batch ops:** batch methods (`batch_insert`/`batch_modify`/`batch_delete`/
  `batch_upsert` and the `batch_*_dir` variants) are **not** routed through
  the broker in the MVP. When the session is not elevated and serve is
  active, batch ops raise `NotImplementedError("batch ops require an
  elevated session; use serve mode for single ops or run elevated")`.
  Batch-over-serve is deferred (see
  [`docs/workstream_serve_plan.md`](workstream_serve_plan.md) §6).

### One-UAC UX

From a **non-elevated** Python session, constructing `TsDevClient()` (the
default) and performing any single-item operation triggers **one** UAC prompt
(the broker auto-start). All subsequent operations reuse the same broker
process over the named pipe — no per-call prompts. When the `TsDevClient` is
garbage-collected or the interpreter exits, the broker receives a `quit`
request and shuts down gracefully.

### Running the serve E2E test

The serve-mode end-to-end test is `tests/run_serve_e2e.ps1`, which drives
`tests/test_serve_e2e.py` (2 pytest cases). It **must be run from an elevated
PowerShell session** — the script launches pytest unelevated via Task
Scheduler (with `-RunLevel Limited`) so the test exercises the real
non-elevated → broker auto-start path. It temporarily sets
`ConsentPromptBehaviorAdmin=0` (auto-elevate, no UAC prompt) so the broker
auto-start is silent, and restores the original value in a `finally` block.

```powershell
# From an ELEVATED PowerShell session:
powershell -ExecutionPolicy Bypass -File tests\run_serve_e2e.ps1

# With extra pytest args:
powershell -ExecutionPolicy Bypass -File tests\run_serve_e2e.ps1 -PytestArgs "-v --tb=long -s"
```

The master regression runner (`tests/run_regression.ps1`) executes this as
**Suite 11** automatically; skip it with `-SkipServeE2e` (or `-NoTs`). See
[`docs/E2E_TEST_SPEC.md`](E2E_TEST_SPEC.md) (Part F) for the full test
procedure and expected results.

## CI (GitHub Actions)

The repo has a GitHub Actions workflow at `.github/workflows/ci.yml`.

**Triggers:** Builds on every push to any branch, on PRs targeting `main`,
and on version tag pushes (`v*`).

**Build job** (`windows-latest`):

- Sets up uv + Python 3.14, configures CMake (Win32), and builds all targets
  (Release).
- Runs the regression suite in `-NoTs` mode (sigtool + 9 non-TradeStation
  unit tests).
- Runs non-e2e Python tests (`pytest -m "not e2e"`; pytest collects 130 tests,
  of which the 15 e2e-marked tests are deselected without a live TradeStation
  install, so 115 mocked tests run).
- Uploads the assembled `dist/` bundle contents as a workflow artifact named
  `tsdev-automation-bundle` (`actions/upload-artifact` zips them once).

**Release job** (`ubuntu-latest`, tag pushes only):

- Runs only on tag pushes (`v*`). Downloads the `tsdev-automation-bundle`
  artifact, re-zips its contents as `tsdev_automation_<tag>.zip`, and attaches
  that zip to a GitHub Release. The release version is the tag name;
  release notes are auto-generated from commits since the last tag.

**TradeStation dependency:** Most C++ regression suites and all e2e Python
tests require a live TradeStation installation, which is not available on CI.
Only the `-NoTs` subset runs in CI.

**Triggering a release:**

```powershell
git tag v0.1.0
git push origin v0.1.0
```
