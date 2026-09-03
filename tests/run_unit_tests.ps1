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

& $cmake -S $repoRoot -B $buildDir -A x64 | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Error "cmake configure failed"; exit 1 }
# Build both the unit test target and the tsdevctl target so that main.cpp
# is compiled under /W4 /WX (the unit test target only links tsdevlib).
& $cmake --build $buildDir --config Release --target test_tsdevctl_units tsdevctl | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Error "build failed"; exit 1 }

$exe = Join-Path $buildDir "Release\test_tsdevctl_units.exe"
& $exe
exit $LASTEXITCODE
