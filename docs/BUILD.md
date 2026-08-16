# Build Guide — tsdev_automation

This project is a C++ CLI that loads the 32-bit `orlib20.dll` from the
TradeStation 9.5 installation. **Every build MUST target the x86 (32-bit)
platform** — a 64-bit binary cannot load a 32-bit DLL.

## Prerequisites

### Visual Studio Build Tools (C++ workload)

The machine has **Visual Studio Build Tools 2026** (VS 18) installed at:

```
C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools
```

The **"Desktop development with C++"** workload (`Microsoft.VisualStudio.Workload.VCTools`)
must be installed. It provides the MSVC compiler (`cl.exe`), the Windows SDK,
`vcvarsall.bat`, and CMake.

If the C++ workload is missing, install it (run from an elevated prompt):

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\Installer\setup.exe" modify `
    --installPath "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools" `
    --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended `
    --quiet --norestart
```

### Tool locations (after C++ workload is installed)

| Tool          | Path                                                                                      |
|---------------|-------------------------------------------------------------------------------------------|
| vcvarsall.bat | `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat` |
| cl.exe (x86)  | `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\<ver>\bin\Hostx64\x86\cl.exe` |
| cmake.exe     | `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` |

> **Note on CMake:** The VS Build Tools ship an internal CMake. You may either
> use that full path or add it to `PATH`. Alternatively, source `vcvarsall.bat`
> (which adds CMake to `PATH`) before running the commands below.

## Build commands

All commands are run from the project root
(`C:\Users\user\projects\tsdev_automation`).

### 1. (Optional) Set up the developer environment

The Visual Studio generator (`-A Win32`) does **not** require sourcing
`vcvarsall.bat` — CMake locates the toolchain automatically. You only need this
step if you want `cmake`/`dumpbin`/`cl` on your `PATH` for manual use:

```cmd
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x86
```

> The `x86` argument selects the 32-bit target environment.

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

### 4. Output binary

```
build\Release\tsdev_probe.exe
```

## Verification (proof the binary is 32-bit and runs)

### Run the executable

```
> build\Release\tsdev_probe.exe
tsdev_probe ok
sizeof(void*) = 4
```

`sizeof(void*) = 4` confirms the process is 32-bit (it would be `8` on x64).

### Confirm the PE header (dumpbin)

From a shell with `vcvarsall.bat x86` sourced:

```
> dumpbin /headers build\Release\tsdev_probe.exe
...
             14C machine (x86)
                   32 bit word machine
             10B magic # (PE32)
...
```

- `14C machine (x86)` — the image targets the x86 (32-bit) instruction set.
- `10B magic # (PE32)` — a 32-bit PE image (PE32, not PE32+ which is 64-bit).

## How it works / extensibility

`CMakeLists.txt` is intentionally minimal:

- `cmake_minimum_required(VERSION 3.20)` + `project(... LANGUAGES CXX)`.
- Forces C++17 (`CMAKE_CXX_STANDARD 17`, required, no extensions).
- A 32-bit guard via `CMAKE_SIZEOF_VOID_P`.
- One executable target, `tsdev_probe`, built from `src/tsdev_probe.cpp`.

To add the real CLI later, add new source files to the `add_executable` call
(or create additional targets) — the 32-bit enforcement and C++17 settings
already apply project-wide.
