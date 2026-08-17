# Build Guide — tsdev_automation

This project is a C++ CLI that loads the 32-bit `orlib20.dll` from the
TradeStation 9.5 installation to manage the TSDev Library (insert, delete, and
modify EasyLanguage strategies and functions) without the TSDev UI. **Every
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
│   ├── tsdev_cli.cpp                     production CLI (insert/delete/modify)
│   └── sigtool.cpp                       signature-verification CLI
├── tests/          Test files (13 .cpp + 4 .ps1)
├── archive/        Archived experimental probes (27 files, NOT built)
├── signatures/     JSON signature data — the single source of truth
│   ├── v9.50.01.3344.json                per-version signatures + patch sites
│   └── version_manifest.json             version manifest (lists known builds)
├── tools/          Build-time tooling
│   └── sigcodegen.py                     generates signatures.gen.h from JSON
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
| `tsdev_cli.cpp`       | Production CLI exposing `insert`, `delete`, and `modify` subcommands. |
| `sigtool.cpp`         | CLI for verifying signatures against a DLL on disk, validating patch sites, inspecting DLL info, and ad-hoc pattern scanning. |

## Build instructions

All commands are run from the project root
(`C:\Users\user\projects\tsdev_automation`).

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
build\Release\test_delete.exe
build\Release\test_delete_tradingapp.exe
build\Release\test_delete_check.exe
build\Release\test_delete_diag.exe
build\Release\test_modify.exe
build\Release\test_modify_tradingapp.exe
build\Release\test_insert_tradingapp.exe
build\Release\test_tradingapp_type.exe
build\Release\test_path_discovery.exe
build\Release\test_persistence_validation.exe
build\Release\test_no_hardcoded_rvas.exe
build\Release\test_orlib_resolver.exe
```

Each test exits `0` on success and non-zero on failure.

**TS dependency:** the integration tests (`test_insert`, `test_insert_tradingapp`,
`test_delete`, `test_delete_tradingapp`, `test_delete_check`, `test_delete_diag`,
`test_modify`, `test_modify_tradingapp`) require a TradeStation 9.5 installation
on the machine — they load the real `orlib20.dll` and exercise the live warehouse.
`test_path_discovery` and `test_persistence_validation` also require a TradeStation
install: they construct `TsDevLib`, which loads `orlib20.dll`. The remaining three
tests do **not** require a live TradeStation install:

- `test_no_hardcoded_rvas` — static check against the generated signature header.
- `test_tradingapp_type` — pure unit test for the `TradingApp` enum entry and
  `getCategory`/`getSubType` helpers (no orlib20 dependency).
- `test_orlib_resolver` — operates on an in-memory PE image (a copied
  `orlib20_baseline.dll`); no live TradeStation install needed.

### Test runner scripts

Four PowerShell scripts in `tests/` orchestrate the test suites. Run them from
the project root after building. The master runner (`run_regression.ps1`)
invokes the other three in a single command; the individual runners are
documented below for developers who want to run a single sub-suite
independently.

#### `tests/run_regression.ps1` — master regression runner

The single-command regression runner orchestrates **all three** sub-suites in
the order required by the B.6 acceptance criteria:

1. **sigtool verify gate** (`tests/test_sigtool.ps1`) — 12 sigtool CLI tests
   against the baseline DLL. Fast; no live TradeStation install needed.
2. **C++ unit tests** (`tests/run_unit_tests.ps1`) — 13 C++ test executables
   (or 3 with `-NoTs`). Medium duration.
3. **E2E CLI tests** (`tests/run_e2e_tests.ps1`) — 34 E2E CLI test cases per
   `docs/E2E_TEST_SPEC.md`. Slowest; creates and deletes real warehouse entries.

Each sub-suite runs in its own child PowerShell process so a crash or hang in
one suite cannot affect the others. After all suites run, a grand summary table
is printed and a single aggregated exit code is returned.

| Parameter       | Type    | Default          | Description                                                              |
|-----------------|---------|------------------|--------------------------------------------------------------------------|
| `$BuildDir`     | string  | `build\Release`  | Directory containing the built executables; passed to `run_unit_tests`. |
| `$TsPath`       | string  | (none)           | TradeStation install dir; passed to E2E runner as `-TsPath`.            |
| `-NoTs`         | switch  | —                | Skip E2E; pass `-NoTs` to unit tests (3 non-TS tests only); still run sigtool. |
| `-SkipSigtool`  | switch  | —                | Skip the sigtool verify gate suite.                                      |
| `-SkipUnit`     | switch  | —                | Skip the C++ unit test suite.                                            |
| `-SkipE2e`      | switch  | —                | Skip the E2E CLI test suite.                                             |
| `-Verbose`      | switch  | —                | Passed through to all sub-scripts (full output for every test).         |

Exit codes: `0` = all executed suites passed, `1` = one or more suites failed
(or a prerequisite was not met).

**Prerequisites:**
- All executables built. The dedicated CMake target builds every test
  executable plus `tsdev_cli` and `sigtool` in one step:
  `cmake --build build --config Release --target test_regression`
- TradeStation 9.5 installed (for the E2E suite, unless `-NoTs`/`-SkipE2e`).
- Baseline DLL at `tests/fixtures/orlib20_baseline.dll` (for the sigtool
  suite and `test_orlib_resolver`). This file is **committed to the repo**,
  so it is available immediately after clone/checkout — no copy needed. If
  the DLL is somehow missing, the test runners will **prompt you to copy it
  from the TradeStation installation** (see
  [Interactive baseline DLL prompting](#interactive-baseline-dll-prompting)
  below). See [Updating the baseline DLL](#updating-the-baseline-dll) below
  for how to replace it when a new TradeStation version is released.
- No TradeStation processes/services running (for the E2E suite).

```powershell
# Full run — all 3 suites (sigtool + 13 unit tests + 34 E2E tests):
powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1

# No-TradeStation run — sigtool + 3 non-TS unit tests; skips E2E:
powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1 -NoTs

# Non-default TradeStation install path:
powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1 -TsPath "D:\TradeStation 9.5"

# Selective runs:
powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1 -SkipE2e   # sigtool + all 13 unit tests
powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1 -SkipUnit  # sigtool + E2E only
powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1 -Verbose   # full output for every test
```

#### `tests/run_unit_tests.ps1` — C++ unit test runner

Runs all 13 C++ test executables in sequence (not in parallel — the shared
warehouse file `MWRKArea.wrk` is not process-safe), captures exit codes, measures
per-test duration, and returns a single aggregated exit code.

| Parameter    | Type    | Default          | Description                                                  |
|--------------|---------|------------------|--------------------------------------------------------------|
| `$BuildDir`  | string  | `build\Release`  | Directory containing the test executables.                   |
| `-NoTs`      | switch  | —                | Run only the 3 non-TS-dependent tests; skip the rest.        |
| `-Verbose`   | switch  | —                | Print full stdout/stderr for every test, not just failures.  |

Exit codes: `0` = all executed tests passed, `1` = one or more failed (or the
build directory is missing).

```powershell
# Run all 13 tests:
powershell -ExecutionPolicy Bypass -File tests\run_unit_tests.ps1

# Run only the 3 non-TS tests (no TradeStation install needed):
powershell -ExecutionPolicy Bypass -File tests\run_unit_tests.ps1 -NoTs

# Run all tests with full output:
powershell -ExecutionPolicy Bypass -File tests\run_unit_tests.ps1 -Verbose
```

#### `tests/run_e2e_tests.ps1` — E2E CLI test runner

Implements the 34-test-case E2E CLI test specification in
`docs/E2E_TEST_SPEC.md`, covering all 4 supported component types (Strategy,
Indicator, Function, TradingApp) across three parts: Part A (16 tests,
insert/modify/delete lifecycle), Part B (8 tests, type-specific semantic
errors), Part C (10 tests, general errors). Each test writes ASCII-encoded
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
| `tsdev_cli`                     | exec     | Production CLI (`insert`/`delete`/`modify`).                       |
| `sigtool`                       | exec     | Signature-verification CLI.                                        |
| `test_regression`               | custom   | Build-only dependency target: builds all 13 test executables + `tsdev_cli` + `sigtool`. Test execution is performed by `tests/run_regression.ps1`. |
| `test_insert`                   | exec     | INSERT integration test (requires TS).                            |
| `test_insert_tradingapp`        | exec     | TradingApp INSERT integration test (requires TS).                 |
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
