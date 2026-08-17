<#
.SYNOPSIS
    Master regression runner for the TSDev Automation project (TVS-B.6.3).

.DESCRIPTION
    Orchestrates ALL three regression sub-suites in a single command, in the
    order required by the B.6 acceptance criteria:

      1. sigtool verify gate   (tests/test_sigtool.ps1)
         12 sigtool CLI tests against the baseline DLL. Fast, no live TS.
      2. C++ unit tests        (tests/run_unit_tests.ps1)
         14 C++ test executables (or 3 with -NoTs). Medium duration.
      3. E2E CLI tests         (tests/run_e2e_tests.ps1)
         34 E2E CLI test cases per E2E_TEST_SPEC.md. Slowest; creates and
         deletes real warehouse entries.

    Each sub-suite is launched in its own PowerShell child process via
    `& powershell -ExecutionPolicy Bypass -File <path> [args]` so that a
    crash or hang in one suite cannot affect the others. The exit code of
    each child is captured and recorded.

    After all suites run, a grand summary table is printed and a single
    aggregated exit code is returned: 0 = all executed suites passed,
    1 = one or more suites failed.

    This script is the SINGLE COMMAND referenced in the B.6 acceptance
    criteria:
        powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1

.PARAMETER BuildDir
    Directory containing the built test executables and CLI. Defaults to
    "build\Release" relative to the project root. Passed through to
    run_unit_tests.ps1.

.PARAMETER TsPath
    Optional TradeStation install directory. Passed through to
    run_e2e_tests.ps1 as -TsPath. Use when TradeStation is installed in a
    non-default location.

.PARAMETER NoTs
    Switch. When set:
      - Skip the E2E suite entirely (prints "SKIP: E2E tests require live TS").
      - Pass -NoTs to run_unit_tests.ps1 (runs only the 3 non-TS unit tests).
      - Still run the sigtool suite (it does not need live TS, only the
        baseline DLL).

.PARAMETER SkipSigtool
    Switch. Skip the sigtool verify gate suite.

.PARAMETER SkipUnit
    Switch. Skip the C++ unit test suite.

.PARAMETER SkipE2e
    Switch. Skip the E2E CLI test suite.

.PARAMETER Verbose
    Switch. Passed through to all sub-scripts (enables full output for
    every test, not just failures).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1
    Runs all 3 suites (sigtool + 14 unit tests + 34 E2E tests).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1 -NoTs
    Runs sigtool + 3 non-TS unit tests; skips E2E.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1 -SkipE2e
    Runs sigtool + all 14 unit tests; skips E2E.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1 -Verbose
    Runs all 3 suites with full output for every test.

.NOTES
    Exit codes:
      0 = all executed suites passed
      1 = one or more suites failed (or a prerequisite was not met)

    Prerequisites:
      - The project must be built: cmake --build build --config Release
        (or: cmake --build build --config Release --target test_regression)
      - TradeStation 9.5 must be installed for the E2E suite (unless -NoTs
        or -SkipE2e is used).
      - tests/fixtures/orlib20_baseline.dll must exist for the sigtool
        suite and test_orlib_resolver. If missing, the runner PROMPTS the
        user to copy it from the TradeStation installation. If the user
        declines, the sigtool suite and test_orlib_resolver are marked
        FAILED; other suites still run.
      - No TradeStation processes/services should be running for the E2E
        suite.
#>

param(
    [string]$BuildDir = "build\Release",
    [string]$TsPath,
    [switch]$NoTs,
    [switch]$SkipSigtool,
    [switch]$SkipUnit,
    [switch]$SkipE2e,
    [switch]$Verbose
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Resolve paths. The script lives in tests\, so the project root is the
# parent of $PSScriptRoot. BuildDir is resolved relative to the project root.
# ---------------------------------------------------------------------------
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$ResolvedBuildDir = Join-Path $ProjectRoot $BuildDir

# Sub-script paths (all live alongside this script in tests\).
$SigtoolScript  = Join-Path $PSScriptRoot "test_sigtool.ps1"
$UnitScript     = Join-Path $PSScriptRoot "run_unit_tests.ps1"
$E2eScript      = Join-Path $PSScriptRoot "run_e2e_tests.ps1"

# Baseline DLL prerequisite path.
$BaselineDll = Join-Path $ProjectRoot "tests\fixtures\orlib20_baseline.dll"

# ---------------------------------------------------------------------------
# Shared test helpers (baseline DLL prompting + TS install discovery).
# ---------------------------------------------------------------------------
Import-Module (Join-Path $PSScriptRoot "TestHelpers.psm1") -ErrorAction Stop -DisableNameChecking

# ---------------------------------------------------------------------------
# Results collector: one object per suite.
# ---------------------------------------------------------------------------
$SuiteResults = New-Object System.Collections.Generic.List[object]

# ---------------------------------------------------------------------------
# Invoke-Suite: runs a single sub-suite in a child PowerShell process,
# captures the exit code, measures duration, prints PASS/FAIL, and records
# the result. Returns the result object.
#
# Parameters:
#   SuiteIndex - 1-based index for the banner (e.g. 1, 2, 3)
#   SuiteTotal - total number of suites (for the banner, e.g. 3)
#   SuiteName  - human-readable suite name (e.g. "sigtool verify gate")
#   ScriptPath - absolute path to the sub-script .ps1 file
#   Arguments  - array of string arguments to pass to the sub-script
# ---------------------------------------------------------------------------
function Invoke-Suite {
    param(
        [Parameter(Mandatory = $true)] [int]$SuiteIndex,
        [Parameter(Mandatory = $true)] [int]$SuiteTotal,
        [Parameter(Mandatory = $true)] [string]$SuiteName,
        [Parameter(Mandatory = $true)] [string]$ScriptPath,
        [string[]]$Arguments = @()
    )

    Write-Host ""
    Write-Host "===== SUITE $SuiteIndex/$SuiteTotal : $SuiteName =====" -ForegroundColor Cyan

    # Check that the sub-script file exists.
    if (-not (Test-Path -Path $ScriptPath -PathType Leaf)) {
        Write-Host "FAIL: sub-script not found ($ScriptPath)" -ForegroundColor Red
        $result = [pscustomobject]@{
            Suite     = $SuiteName
            Result    = 'FAIL'
            ExitCode  = -1
            Duration  = [timespan]::Zero
            Skipped   = $false
        }
        $SuiteResults.Add($result) | Out-Null
        return $result
    }

    # Build the argument string for display.
    $argDisplay = if ($Arguments.Count -gt 0) { $Arguments -join ' ' } else { '(none)' }
    Write-Host "Script : $ScriptPath"
    Write-Host "Args   : $argDisplay"

    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $exitCode = 0

    # Launch the sub-script in a child PowerShell process. Relax error
    # preference so that a non-zero exit code from the child does not throw
    # a terminating error in this parent script.
    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $allArgs = @("-ExecutionPolicy", "Bypass", "-File", $ScriptPath) + $Arguments
        & powershell @allArgs 2>&1 | Out-Host
        $exitCode = $LASTEXITCODE
    }
    catch {
        # Unexpected runtime error (e.g. process cannot start).
        Write-Host "ERROR: failed to launch sub-script: $($_.Exception.Message)" -ForegroundColor Red
        $exitCode = -2
    }
    finally {
        $ErrorActionPreference = $prevEAP
    }

    $stopwatch.Stop()

    # If the child PowerShell process itself failed to launch, $LASTEXITCODE
    # may be $null. Treat that as a failure.
    if ($null -eq $exitCode) { $exitCode = -3 }

    $status = if ($exitCode -eq 0) { 'PASS' } else { 'FAIL' }
    $color  = if ($exitCode -eq 0) { 'Green' } else { 'Red' }
    $durationStr = "{0:0.00}s" -f $stopwatch.Elapsed.TotalSeconds

    Write-Host ""
    Write-Host ("{0}: {1} (exit {2}) [{3}]" -f $SuiteName, $status, $exitCode, $durationStr) -ForegroundColor $color

    $result = [pscustomobject]@{
        Suite     = $SuiteName
        Result    = $status
        ExitCode  = $exitCode
        Duration  = $stopwatch.Elapsed
        Skipped   = $false
    }
    $SuiteResults.Add($result) | Out-Null
    return $result
}

# ---------------------------------------------------------------------------
# Skip-Suite: records a suite as skipped without running it.
# ---------------------------------------------------------------------------
function Skip-Suite {
    param(
        [Parameter(Mandatory = $true)] [int]$SuiteIndex,
        [Parameter(Mandatory = $true)] [int]$SuiteTotal,
        [Parameter(Mandatory = $true)] [string]$SuiteName,
        [string]$Reason
    )

    Write-Host ""
    Write-Host "===== SUITE $SuiteIndex/$SuiteTotal : $SuiteName =====" -ForegroundColor Cyan
    Write-Host "SKIP: $Reason" -ForegroundColor DarkYellow

    $result = [pscustomobject]@{
        Suite     = $SuiteName
        Result    = 'SKIP'
        ExitCode  = 0
        Duration  = [timespan]::Zero
        Skipped   = $true
    }
    $SuiteResults.Add($result) | Out-Null
    return $result
}

# ---------------------------------------------------------------------------
# Fail-Suite: records a suite as FAILED without running it (used when a
# prerequisite such as the baseline DLL is missing and the user declined to
# remediate).
# ---------------------------------------------------------------------------
function Fail-Suite {
    param(
        [Parameter(Mandatory = $true)] [int]$SuiteIndex,
        [Parameter(Mandatory = $true)] [int]$SuiteTotal,
        [Parameter(Mandatory = $true)] [string]$SuiteName,
        [string]$Reason
    )

    Write-Host ""
    Write-Host "===== SUITE $SuiteIndex/$SuiteTotal : $SuiteName =====" -ForegroundColor Cyan
    Write-Host "FAIL: $Reason" -ForegroundColor Red

    $result = [pscustomobject]@{
        Suite     = $SuiteName
        Result    = 'FAIL'
        ExitCode  = 1
        Duration  = [timespan]::Zero
        Skipped   = $false
    }
    $SuiteResults.Add($result) | Out-Null
    return $result
}

# ---------------------------------------------------------------------------
# Print-GrandSummary: prints the grand summary table and overall verdict.
# Returns the number of failed (non-skipped) suites.
# ---------------------------------------------------------------------------
function Print-GrandSummary {
    param(
        [Parameter(Mandatory = $true)] $Results
    )

    Write-Host ""
    Write-Host "==================== GRAND SUMMARY ====================" -ForegroundColor Cyan
    $header = "{0,-28} {1,-10} {2,-12} {3,-12}" -f "Suite", "Result", "Exit Code", "Duration"
    Write-Host $header
    Write-Host ("{0,-28} {1,-10} {2,-12} {3,-12}" -f ("-" * 28), ("-" * 10), ("-" * 12), ("-" * 12))

    $passed = 0
    $failed = 0
    $skipped = 0

    foreach ($r in $Results) {
        if ($r.Skipped) {
            $skipped++
            $line = "{0,-28} {1,-10} {2,-12} {3,-12}" -f $r.Suite, "SKIP", "-", "-"
            Write-Host $line -ForegroundColor DarkYellow
        }
        else {
            $durationStr = "{0:0.00}s" -f $r.Duration.TotalSeconds
            $line = "{0,-28} {1,-10} {2,-12} {3,-12}" -f $r.Suite, $r.Result, $r.ExitCode, $durationStr
            $color = if ($r.Result -eq 'PASS') { 'Green' } else { 'Red' }
            Write-Host $line -ForegroundColor $color
            if ($r.Result -eq 'PASS') { $passed++ } else { $failed++ }
        }
    }

    $total = $Results.Count
    $executed = $total - $skipped
    Write-Host ""
    if ($skipped -gt 0) {
        Write-Host ("Totals: {0}/{1} suites passed ({2} skipped)" -f $passed, $executed, $skipped) -ForegroundColor Cyan
    } else {
        Write-Host ("Totals: {0}/{1} suites passed, {2} failed" -f $passed, $executed, $failed) -ForegroundColor Cyan
    }

    return $failed
}

# ===========================================================================
# MAIN EXECUTION
# ===========================================================================

Write-Host "============================================" -ForegroundColor Cyan
Write-Host "TSDev Automation - Master Regression Runner" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "Project root : $ProjectRoot"
Write-Host "Build dir    : $ResolvedBuildDir"
Write-Host "NoTs         : $NoTs"
Write-Host "SkipSigtool  : $SkipSigtool"
Write-Host "SkipUnit     : $SkipUnit"
Write-Host "SkipE2e      : $SkipE2e"
Write-Host "Verbose      : $Verbose"
if ($TsPath) { Write-Host "TsPath       : $TsPath" }

# ---------------------------------------------------------------------------
# Prerequisite: build directory must exist (all suites need built executables:
# sigtool.exe, test_*.exe, tsdev_cli.exe).
# ---------------------------------------------------------------------------
if (-not (Test-Path -Path $ResolvedBuildDir -PathType Container)) {
    Write-Host ""
    Write-Host "ERROR: Build directory does not exist: $ResolvedBuildDir" -ForegroundColor Red
    Write-Host "Build the project first, e.g.: cmake --build build --config Release --target test_regression" -ForegroundColor Yellow
    exit 1
}

# ---------------------------------------------------------------------------
# Prerequisite: baseline DLL for the sigtool suite and test_orlib_resolver.
# If missing, prompt the user to copy it from the TS install. Do NOT continue
# without a user decision. This prompt happens ONCE here in the orchestrator;
# the child scripts receive -SkipBaselineDllPrompt so they do not re-prompt.
#   - If the DLL is available (present or copied): child scripts run normally
#     and their own Ensure-BaselineDll check passes (DLL present, no prompt).
#   - If the user declined: the sigtool suite is marked FAILED (not launched),
#     and run_unit_tests.ps1 is launched with -SkipBaselineDllPrompt so it
#     marks test_orlib_resolver as FAILED but still runs the other unit tests.
# ---------------------------------------------------------------------------
$baselineDllAvailable = Ensure-BaselineDll -DllPath $BaselineDll -TsPath $TsPath
if (-not $baselineDllAvailable) {
    Write-Host ""
    Write-Host "FATAL: baseline DLL not available - sigtool suite and test_orlib_resolver will FAIL." -ForegroundColor Red
    Write-Host "Other suites (non-DLL unit tests, E2E tests) will still run." -ForegroundColor Yellow
}

# ---------------------------------------------------------------------------
# Determine which suites will run. The total count (for banners) only
# includes suites that will actually be invoked or skipped-with-banner.
# ---------------------------------------------------------------------------
$suiteCount = 0
if (-not $SkipSigtool) { $suiteCount++ }
if (-not $SkipUnit)    { $suiteCount++ }

# E2E is skipped if -SkipE2e is set OR -NoTs is set (E2E requires live TS).
$skipE2eActual = $SkipE2e -or $NoTs
if (-not $skipE2eActual) { $suiteCount++ }

# If E2E is skipped due to -NoTs, we still print a SKIP banner for it so the
# user sees why it was skipped. Count it in the total for banner numbering.
if ($skipE2eActual) { $suiteCount++ }

# ---------------------------------------------------------------------------
# Suite 1: sigtool verify gate
# ---------------------------------------------------------------------------
$idx = 0
if (-not $SkipSigtool) {
    $idx++
    if (-not $baselineDllAvailable) {
        # DLL missing and user declined - mark the suite FAILED without
        # launching the child script (it would only re-prompt / exit 1).
        Fail-Suite -SuiteIndex $idx -SuiteTotal $suiteCount `
            -SuiteName "sigtool verify gate" `
            -Reason "baseline DLL not available (user declined to copy). Required: $BaselineDll" | Out-Null
    } else {
        $sigArgs = @()
        if ($Verbose) { $sigArgs += "-Verbose" }
        Invoke-Suite -SuiteIndex $idx -SuiteTotal $suiteCount `
            -SuiteName "sigtool verify gate" -ScriptPath $SigtoolScript -Arguments $sigArgs | Out-Null
    }
}

# ---------------------------------------------------------------------------
# Suite 2: C++ unit tests
# ---------------------------------------------------------------------------
if (-not $SkipUnit) {
    $idx++
    $unitArgs = @("-BuildDir", $BuildDir)
    if ($NoTs)   { $unitArgs += "-NoTs" }
    if ($Verbose) { $unitArgs += "-Verbose" }
    # If the orchestrator already prompted and the DLL is unavailable, tell
    # the child script to skip its own prompt (it will mark
    # test_orlib_resolver as FAILED but still run the other unit tests).
    if (-not $baselineDllAvailable) { $unitArgs += "-SkipBaselineDllPrompt" }
    Invoke-Suite -SuiteIndex $idx -SuiteTotal $suiteCount `
        -SuiteName "C++ unit tests" -ScriptPath $UnitScript -Arguments $unitArgs | Out-Null
}

# ---------------------------------------------------------------------------
# Suite 3: E2E CLI tests
# ---------------------------------------------------------------------------
if ($skipE2eActual) {
    $idx++
    $reason = if ($NoTs) { "E2E tests require live TS" } else { "skipped by -SkipE2e" }
    Skip-Suite -SuiteIndex $idx -SuiteTotal $suiteCount `
        -SuiteName "E2E CLI tests" -Reason $reason | Out-Null
}
else {
    $idx++
    $e2eArgs = @()
    if ($TsPath)   { $e2eArgs += @("-TsPath", $TsPath) }
    if ($Verbose)  { $e2eArgs += "-Verbose" }
    Invoke-Suite -SuiteIndex $idx -SuiteTotal $suiteCount `
        -SuiteName "E2E CLI tests" -ScriptPath $E2eScript -Arguments $e2eArgs | Out-Null
}

# ---------------------------------------------------------------------------
# Grand summary and final verdict.
# ---------------------------------------------------------------------------
$failureCount = Print-GrandSummary -Results $SuiteResults

Write-Host ""
if ($failureCount -eq 0) {
    Write-Host "ALL SUITES PASSED" -ForegroundColor Green
    exit 0
}
else {
    Write-Host "$failureCount SUITE(S) FAILED" -ForegroundColor Red
    exit 1
}
