<#
.SYNOPSIS
    Standalone crash-handler test runner for test_crash_handler.

.DESCRIPTION
    Runs the test_crash_handler executable in two modes:

      1. Normal mode (no args): a pure unit test that verifies the
         vehCrashHandler symbol exists and AddVectoredExceptionHandler is
         available. Should exit 0.

      2. Crash mode (--crash): deliberately triggers an access violation.
         If the VEH works, the process exits with code 0xC0000005
         (=-1073741819 as a signed int32) and NO crash dialog
         (TSCrashReport.exe / WerFault.exe) spawns.

    After the crash test, this script verifies that NO TSCrashReport.exe or
    WerFault.exe process is running -- if either is present, the VEH failed to
    suppress the crash dialog and the test FAILS (the stray process is
    force-killed first so it does not linger).

    This script is NOT wired into run_regression.ps1 or run_unit_tests.ps1.
    It is a standalone script intended to be run manually to validate the
    VEH crash handler end-to-end.

.PARAMETER BuildDir
    Directory containing the test executable. Defaults to "build\Release"
    relative to the project root (parent of this script's tests\ directory).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\test_crash_handler.ps1
    Runs both the normal-mode unit test and the deliberate-crash test.

.NOTES
    Exit codes:
      0 = all checks passed (normal mode exit 0, crash mode exit -1073741819,
          no crash-reporter processes spawned)
      1 = one or more checks failed
#>

param(
    [string]$BuildDir = "build\Release"
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Resolve paths. The script lives in tests\, so the project root is the
# parent of $PSScriptRoot. BuildDir is resolved relative to the project root.
# ---------------------------------------------------------------------------
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$ResolvedBuildDir = Join-Path $ProjectRoot $BuildDir
$ExePath = Join-Path $ResolvedBuildDir "test_crash_handler.exe"

# The expected exit code when the VEH catches an access violation:
# 0xC0000005 reinterpreted as a signed 32-bit integer.
$ExpectedCrashExitCode = -1073741819

Write-Host "TSDev Automation - Crash Handler Test Runner" -ForegroundColor Cyan
Write-Host "Project root : $ProjectRoot"
Write-Host "Build dir    : $ResolvedBuildDir"
Write-Host "Executable   : $ExePath"
Write-Host ""

if (-not (Test-Path -Path $ResolvedBuildDir -PathType Container)) {
    Write-Host "ERROR: Build directory does not exist: $ResolvedBuildDir" -ForegroundColor Red
    Write-Host "Build the project first, e.g.: cmake --build build --config Release" -ForegroundColor Yellow
    exit 1
}

if (-not (Test-Path -Path $ExePath -PathType Leaf)) {
    Write-Host "ERROR: test_crash_handler.exe not found at $ExePath" -ForegroundColor Red
    Write-Host "Build the test target first, e.g.: cmake --build build --config Release --target test_crash_handler" -ForegroundColor Yellow
    exit 1
}

$failed = $false

# ---------------------------------------------------------------------------
# Phase 1: Normal mode (pure unit test, no crash). Should exit 0.
# ---------------------------------------------------------------------------
Write-Host "===== Phase 1: Normal mode (unit test) =====" -ForegroundColor Cyan
$prevEAP = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    $normalOutput = & $ExePath 2>&1
    $normalExit = $LASTEXITCODE
}
catch {
    $normalOutput = $_.Exception.Message
    $normalExit = -2
}
finally {
    $ErrorActionPreference = $prevEAP
}

if ($normalOutput) {
    $normalOutput | ForEach-Object { Write-Host $_ }
}

if ($normalExit -eq 0) {
    Write-Host "PASS (exit $normalExit)" -ForegroundColor Green
} else {
    Write-Host "FAIL (exit $normalExit) -- expected 0" -ForegroundColor Red
    $failed = $true
}

# ---------------------------------------------------------------------------
# Phase 2: Crash mode (--crash). Should exit with 0xC0000005 (=-1073741819).
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "===== Phase 2: Crash mode (--crash) =====" -ForegroundColor Cyan

$prevEAP = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    $crashOutput = & $ExePath --crash 2>&1
    $crashExit = $LASTEXITCODE
}
catch {
    $crashOutput = $_.Exception.Message
    $crashExit = -2
}
finally {
    $ErrorActionPreference = $prevEAP
}

if ($crashOutput) {
    $crashOutput | ForEach-Object { Write-Host $_ }
}

Write-Host "  Exit code: $crashExit (expected $ExpectedCrashExitCode = 0xC0000005)" -ForegroundColor DarkGray

if ($crashExit -eq $ExpectedCrashExitCode) {
    Write-Host "PASS (exit $crashExit == 0xC0000005)" -ForegroundColor Green
} else {
    Write-Host "FAIL (exit $crashExit) -- expected $ExpectedCrashExitCode (0xC0000005)" -ForegroundColor Red
    $failed = $true
}

# ---------------------------------------------------------------------------
# Phase 3: Verify NO crash-reporter processes spawned.
# The VEH should have terminated the process before orlib20's unhandled
# exception filter could spawn TSCrashReport.exe, and SEM_NOGPFAULTERRORBOX
# should have suppressed WerFault.exe. If either is running, the VEH failed.
# Give the OS a brief moment to spawn any crash reporter before checking.
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "===== Phase 3: Crash-reporter process check =====" -ForegroundColor Cyan
Start-Sleep -Milliseconds 500

$crashReport = Get-Process -Name TSCrashReport -ErrorAction SilentlyContinue
if ($crashReport) {
    Stop-Process -Name TSCrashReport -Force
    Write-Host "[FAIL] TSCrashReport.exe spawned -- VEH did not suppress crash dialog" -ForegroundColor Red
    $failed = $true
} else {
    Write-Host "PASS: no TSCrashReport.exe running" -ForegroundColor Green
}

$werFault = Get-Process -Name WerFault -ErrorAction SilentlyContinue
if ($werFault) {
    Stop-Process -Name WerFault -Force
    Write-Host "[FAIL] WerFault.exe spawned -- error mode did not suppress crash dialog" -ForegroundColor Red
    $failed = $true
} else {
    Write-Host "PASS: no WerFault.exe running" -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# Final result.
# ---------------------------------------------------------------------------
Write-Host ""
if (-not $failed) {
    Write-Host "ALL CHECKS PASSED" -ForegroundColor Green
    exit 0
} else {
    Write-Host "ONE OR MORE CHECKS FAILED" -ForegroundColor Red
    exit 1
}
