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
├── tests/          Test files (8 .cpp + 1 .ps1)
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
build\Release\test_delete_diag.exe
build\Release\test_modify.exe
build\Release\test_orlib_resolver.exe
build\Release\test_persistence_validation.exe
build\Release\test_no_hardcoded_rvas.exe
build\Release\test_delete_check.exe
```

Each test exits `0` on success and non-zero on failure. The integration tests
(`test_insert`, `test_delete*`, `test_modify`) require a TradeStation 9.5
installation on the machine (they load the real `orlib20.dll`). The resolver
and signature tests (`test_orlib_resolver`,
`test_no_hardcoded_rvas`, `test_persistence_validation`) operate on an
in-memory PE image and do not require a TradeStation install.

### sigtool test script

A PowerShell-based test for `sigtool` lives at `tests/test_sigtool.ps1`. Run it
from the project root after building:

```powershell
powershell -ExecutionPolicy Bypass -File tests\test_sigtool.ps1
```

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
| `test_insert`                   | exec     | INSERT integration test.                                           |
| `test_delete`                   | exec     | DELETE integration test.                                           |
| `test_delete_check`             | exec     | Quick check that deletions persist across restarts.                |
| `test_delete_diag`              | exec     | DELETE diagnostic test.                                            |
| `test_modify`                   | exec     | MODIFY integration test.                                           |
| `test_persistence_validation`   | exec     | Persistence patch byte validation test.                            |
| `test_no_hardcoded_rvas`        | exec     | Verifies no hardcoded RVAs leak into production code.              |
| `test_orlib_resolver`           | exec     | OrlibResolver unit test (in-memory PE image, no TS install needed).|

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
