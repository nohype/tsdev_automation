# Unit tests for tsdevctl (pure logic). No TradeStation required.
# Compatible with PowerShell 5.1 and pwsh 7.
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot "build\unit"

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($install) {
            $candidate = Join-Path $install "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
            if (Test-Path $candidate) { $cmake = $candidate }
        }
    }
} else {
    $cmake = $cmake.Source
}
if (-not $cmake) { Write-Error "cmake not found"; exit 1 }

# Win32 (x86) is required: tsdevctl.exe and delete_hook.dll must be 32-bit
# to match TSDev.exe (32-bit) for DLL injection.
& $cmake -S $repoRoot -B $buildDir -A Win32 | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Error "cmake configure failed"; exit 1 }
# Build all targets so that main.cpp and the DLL are compiled under /W4 /WX.
& $cmake --build $buildDir --config Release | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Error "build failed"; exit 1 }

$exe = Join-Path $buildDir "Release\test_tsdevctl_units.exe"
& $exe
exit $LASTEXITCODE
