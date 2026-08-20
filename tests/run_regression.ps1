<#
.SYNOPSIS
    Master regression runner for the TSDev Automation project (TVS-B.6.3).

.DESCRIPTION
    Orchestrates ALL nine regression sub-suites in a single command, in the
    order required by the B.6 acceptance criteria:

      1. sigtool verify gate   (tests/test_sigtool.ps1)
         12 sigtool CLI tests against the baseline DLL. Fast, no live TS.
      2. C++ unit tests        (tests/run_unit_tests.ps1)
         19 C++ test executables (or 3 with -NoTs). Medium duration.
      3. E2E CLI tests         (tests/run_e2e_tests.ps1)
         34 E2E CLI test cases per E2E_TEST_SPEC.md. Slowest; creates and
         deletes real warehouse entries.
      4. exists E2E tests      (tests/test_exists.ps1)
         5 exists-subcommand E2E test cases (D1.1-D1.3). Requires live TS;
         skipped under -NoTs or -SkipExists. Creates and deletes a real
         warehouse probe entry.
      5. batch-stdin E2E tests (tests/test_batch_stdin.ps1)
         8 batch-stdin E2E test cases (S1-S8). Requires live TS; skipped
         under -NoTs or -SkipBatchStdin. Inserts and deletes real warehouse
         entries.
      6. upsert E2E tests      (tests/test_upsert.ps1)
         5 upsert-subcommand E2E test cases (U1-U5). Requires live TS;
         skipped under -NoTs or -SkipUpsert. Inserts and deletes real
         warehouse entries.
      7. get-source E2E tests  (tests/test_get_source.ps1)
         6 get-source-subcommand E2E test cases (GS1-GS6). Requires live TS;
         skipped under -NoTs or -SkipGetSource. Inserts and deletes real
         warehouse entries.
      8. exists case-insensitive E2E tests (tests/test_exists_ci.ps1)
         5 exists-subcommand case-insensitive E2E test cases (CI.1-CI.5).
         Requires live TS; skipped under -NoTs or -SkipExistsCi. Inserts and
         deletes real warehouse entries.
      9. batch-upsert E2E tests   (tests/test_batch_upsert.ps1)
         5 batch-upsert-subcommand E2E test cases (BU1-BU5). Requires live TS;
         skipped under -NoTs or -SkipBatchUpsert. Inserts and deletes real
         warehouse entries.
     10. directory-batch E2E tests (tests/test_directory_batch.ps1)
         8 directory-batch-subcommand E2E test cases (DB1-DB8). Requires live
         TS; skipped under -NoTs or -SkipDirectoryBatch. Inserts and deletes
         real warehouse entries.

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
      - Skip the exists E2E suite entirely (prints "SKIP: exists tests require live TS").
      - Skip the batch-stdin E2E suite entirely (prints "SKIP: batch-stdin tests require live TS").
      - Skip the upsert E2E suite entirely (prints "SKIP: upsert tests require live TS").
      - Skip the get-source E2E suite entirely (prints "SKIP: get-source tests require live TS").
      - Skip the exists case-insensitive E2E suite entirely (prints "SKIP: exists-ci tests require live TS").
      - Skip the batch-upsert E2E suite entirely (prints "SKIP: batch-upsert tests require live TS").
      - Skip the directory-batch E2E suite entirely (prints "SKIP: directory-batch tests require live TS").
      - Pass -NoTs to run_unit_tests.ps1 (runs only the 3 non-TS unit tests).
      - Still run the sigtool suite (it does not need live TS, only the
        baseline DLL).

.PARAMETER SkipSigtool
    Switch. Skip the sigtool verify gate suite.

.PARAMETER SkipUnit
    Switch. Skip the C++ unit test suite.

.PARAMETER SkipE2e
    Switch. Skip the E2E CLI test suite.

.PARAMETER SkipExists
    Switch. Skip the exists E2E test suite (tests/test_exists.ps1).

.PARAMETER SkipBatchStdin
    Switch. Skip the batch-stdin E2E test suite (tests/test_batch_stdin.ps1).

.PARAMETER SkipUpsert
    Switch. Skip the upsert E2E test suite (tests/test_upsert.ps1).

.PARAMETER SkipGetSource
    Switch. Skip the get-source E2E test suite (tests/test_get_source.ps1).

.PARAMETER SkipExistsCi
    Switch. Skip the exists case-insensitive E2E test suite
    (tests/test_exists_ci.ps1).

.PARAMETER SkipBatchUpsert
    Switch. Skip the batch-upsert E2E test suite
    (tests/test_batch_upsert.ps1).

.PARAMETER SkipDirectoryBatch
    Switch. Skip the directory-batch E2E test suite
    (tests/test_directory_batch.ps1).

.PARAMETER Verbose
    Switch. Passed through to all sub-scripts (enables full output for
    every test, not just failures).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1
    Runs all 10 suites (sigtool + 19 unit tests + 34 E2E tests + 5 exists tests + 8 batch-stdin tests + 5 upsert tests + 6 get-source tests + 5 exists-ci tests + 5 batch-upsert tests + 8 directory-batch tests).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1 -NoTs
    Runs sigtool + 3 non-TS unit tests; skips E2E, exists, batch-stdin, upsert, get-source, exists-ci, batch-upsert, and directory-batch suites.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1 -SkipE2e
    Runs sigtool + all 19 unit tests + 5 exists tests + 8 batch-stdin tests + 5 upsert tests + 6 get-source tests + 5 exists-ci tests + 5 batch-upsert tests + 8 directory-batch tests; skips E2E.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1 -Verbose
    Runs all 10 suites with full output for every test.

.NOTES
    Exit codes:
      0 = all executed suites passed
      1 = one or more suites failed (or a prerequisite was not met)

    Prerequisites:
      - The project must be built: cmake --build build --config Release
        (or: cmake --build build --config Release --target test_regression)
      - TradeStation 9.5 must be installed for the E2E, exists, and
        batch-stdin, upsert, get-source, exists-ci, batch-upsert, and
        directory-batch suites (unless -NoTs, -SkipE2e, -SkipExists,
        -SkipBatchStdin, -SkipUpsert, -SkipGetSource, -SkipExistsCi,
        -SkipBatchUpsert, or -SkipDirectoryBatch is used).
      - tests/fixtures/orlib20_baseline.dll must exist for the sigtool
        suite and test_orlib_resolver. If missing, the runner PROMPTS the
        user to copy it from the TradeStation installation. If the user
        declines, the sigtool suite and test_orlib_resolver are marked
        FAILED; other suites still run.
      - No TradeStation processes/services should be running for the E2E,
        exists, batch-stdin, upsert, get-source, exists-ci, batch-upsert,
        and directory-batch suites.
#>

param(
    [string]$BuildDir = "build\Release",
    [string]$TsPath,
    [switch]$NoTs,
    [switch]$SkipSigtool,
    [switch]$SkipUnit,
    [switch]$SkipE2e,
    [switch]$SkipExists,
    [switch]$SkipBatchStdin,
    [switch]$SkipUpsert,
    [switch]$SkipGetSource,
    [switch]$SkipExistsCi,
    [switch]$SkipBatchUpsert,
    [switch]$SkipDirectoryBatch,
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
$ExistsScript   = Join-Path $PSScriptRoot "test_exists.ps1"
$BatchStdinScript = Join-Path $PSScriptRoot "test_batch_stdin.ps1"
$UpsertScript   = Join-Path $PSScriptRoot "test_upsert.ps1"
$GetSourceScript = Join-Path $PSScriptRoot "test_get_source.ps1"
$ExistsCiScript  = Join-Path $PSScriptRoot "test_exists_ci.ps1"
$BatchUpsertScript = Join-Path $PSScriptRoot "test_batch_upsert.ps1"
$DirectoryBatchScript = Join-Path $PSScriptRoot "test_directory_batch.ps1"

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
Write-Host "SkipExists   : $SkipExists"
Write-Host "SkipBatchStdin : $SkipBatchStdin"
Write-Host "SkipUpsert   : $SkipUpsert"
Write-Host "SkipGetSource: $SkipGetSource"
Write-Host "SkipExistsCi : $SkipExistsCi"
Write-Host "SkipBatchUpsert : $SkipBatchUpsert"
Write-Host "SkipDirectoryBatch : $SkipDirectoryBatch"
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
    Write-Host "Other suites (non-DLL unit tests, E2E tests, exists tests) will still run." -ForegroundColor Yellow
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

# exists E2E is skipped if -SkipExists is set OR -NoTs is set (exists requires
# live TS — it inserts and deletes a real warehouse probe entry).
$skipExistsActual = $SkipExists -or $NoTs
if (-not $skipExistsActual) { $suiteCount++ }

# If exists is skipped (e.g. due to -NoTs), still print a SKIP banner so the
# user sees why. Count it in the total for banner numbering.
if ($skipExistsActual) { $suiteCount++ }

# batch-stdin E2E is skipped if -SkipBatchStdin is set OR -NoTs is set
# (batch-stdin requires live TS — it inserts/deletes real warehouse entries).
$skipBatchStdinActual = $SkipBatchStdin -or $NoTs
if (-not $skipBatchStdinActual) { $suiteCount++ }

# If batch-stdin is skipped (e.g. due to -NoTs), still print a SKIP banner so
# the user sees why. Count it in the total for banner numbering.
if ($skipBatchStdinActual) { $suiteCount++ }

# upsert E2E is skipped if -SkipUpsert is set OR -NoTs is set
# (upsert requires live TS — it inserts/deletes real warehouse entries).
$skipUpsertActual = $SkipUpsert -or $NoTs
if (-not $skipUpsertActual) { $suiteCount++ }

# If upsert is skipped (e.g. due to -NoTs), still print a SKIP banner so
# the user sees why. Count it in the total for banner numbering.
if ($skipUpsertActual) { $suiteCount++ }

# get-source E2E is skipped if -SkipGetSource is set OR -NoTs is set
# (get-source requires live TS — it inserts/deletes real warehouse entries).
$skipGetSourceActual = $SkipGetSource -or $NoTs
if (-not $skipGetSourceActual) { $suiteCount++ }

# If get-source is skipped (e.g. due to -NoTs), still print a SKIP banner so
# the user sees why. Count it in the total for banner numbering.
if ($skipGetSourceActual) { $suiteCount++ }

# exists case-insensitive E2E is skipped if -SkipExistsCi is set OR -NoTs is
# set (exists-ci requires live TS — it inserts/deletes real warehouse entries).
$skipExistsCiActual = $SkipExistsCi -or $NoTs
if (-not $skipExistsCiActual) { $suiteCount++ }

# If exists-ci is skipped (e.g. due to -NoTs), still print a SKIP banner so
# the user sees why. Count it in the total for banner numbering.
if ($skipExistsCiActual) { $suiteCount++ }

# batch-upsert E2E is skipped if -SkipBatchUpsert is set OR -NoTs is set
# (batch-upsert requires live TS — it inserts/deletes real warehouse entries).
$skipBatchUpsertActual = $SkipBatchUpsert -or $NoTs
if (-not $skipBatchUpsertActual) { $suiteCount++ }

# If batch-upsert is skipped (e.g. due to -NoTs), still print a SKIP banner so
# the user sees why. Count it in the total for banner numbering.
if ($skipBatchUpsertActual) { $suiteCount++ }

# directory-batch E2E is skipped if -SkipDirectoryBatch is set OR -NoTs is set
# (directory-batch requires live TS — it inserts/deletes real warehouse
# entries).
$skipDirectoryBatchActual = $SkipDirectoryBatch -or $NoTs
if (-not $skipDirectoryBatchActual) { $suiteCount++ }

# If directory-batch is skipped (e.g. due to -NoTs), still print a SKIP banner
# so the user sees why. Count it in the total for banner numbering.
if ($skipDirectoryBatchActual) { $suiteCount++ }

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
# Suite 4: exists E2E tests
# ---------------------------------------------------------------------------
if ($skipExistsActual) {
    $idx++
    $reason = if ($NoTs) { "exists tests require live TS" } else { "skipped by -SkipExists" }
    Skip-Suite -SuiteIndex $idx -SuiteTotal $suiteCount `
        -SuiteName "exists E2E tests" -Reason $reason | Out-Null
}
else {
    $idx++
    $existsArgs = @()
    if ($TsPath)   { $existsArgs += @("-TsPath", $TsPath) }
    if ($Verbose)  { $existsArgs += "-Verbose" }
    Invoke-Suite -SuiteIndex $idx -SuiteTotal $suiteCount `
        -SuiteName "exists E2E tests" -ScriptPath $ExistsScript -Arguments $existsArgs | Out-Null
}

# ---------------------------------------------------------------------------
# Suite 5: batch-stdin E2E tests
# ---------------------------------------------------------------------------
if ($skipBatchStdinActual) {
    $idx++
    $reason = if ($NoTs) { "batch-stdin tests require live TS" } else { "skipped by -SkipBatchStdin" }
    Skip-Suite -SuiteIndex $idx -SuiteTotal $suiteCount `
        -SuiteName "batch-stdin E2E tests" -Reason $reason | Out-Null
}
else {
    $idx++
    $batchStdinArgs = @()
    if ($TsPath)   { $batchStdinArgs += @("-TsPath", $TsPath) }
    if ($Verbose)  { $batchStdinArgs += "-Verbose" }
    Invoke-Suite -SuiteIndex $idx -SuiteTotal $suiteCount `
        -SuiteName "batch-stdin E2E tests" -ScriptPath $BatchStdinScript -Arguments $batchStdinArgs | Out-Null
}

# ---------------------------------------------------------------------------
# Suite 6: upsert E2E tests
# ---------------------------------------------------------------------------
if ($skipUpsertActual) {
    $idx++
    $reason = if ($NoTs) { "upsert tests require live TS" } else { "skipped by -SkipUpsert" }
    Skip-Suite -SuiteIndex $idx -SuiteTotal $suiteCount `
        -SuiteName "upsert E2E tests" -Reason $reason | Out-Null
}
else {
    $idx++
    $upsertArgs = @()
    if ($TsPath)   { $upsertArgs += @("-TsPath", $TsPath) }
    if ($Verbose)  { $upsertArgs += "-Verbose" }
    Invoke-Suite -SuiteIndex $idx -SuiteTotal $suiteCount `
        -SuiteName "upsert E2E tests" -ScriptPath $UpsertScript -Arguments $upsertArgs | Out-Null
}

# ---------------------------------------------------------------------------
# Suite 7: get-source E2E tests
# ---------------------------------------------------------------------------
if ($skipGetSourceActual) {
    $idx++
    $reason = if ($NoTs) { "get-source tests require live TS" } else { "skipped by -SkipGetSource" }
    Skip-Suite -SuiteIndex $idx -SuiteTotal $suiteCount `
        -SuiteName "get-source E2E tests" -Reason $reason | Out-Null
}
else {
    $idx++
    $getSourceArgs = @()
    if ($TsPath)   { $getSourceArgs += @("-TsPath", $TsPath) }
    if ($Verbose)  { $getSourceArgs += "-Verbose" }
    Invoke-Suite -SuiteIndex $idx -SuiteTotal $suiteCount `
        -SuiteName "get-source E2E tests" -ScriptPath $GetSourceScript -Arguments $getSourceArgs | Out-Null
}

# ---------------------------------------------------------------------------
# Suite 8: exists case-insensitive E2E tests
# ---------------------------------------------------------------------------
if ($skipExistsCiActual) {
    $idx++
    $reason = if ($NoTs) { "exists-ci tests require live TS" } else { "skipped by -SkipExistsCi" }
    Skip-Suite -SuiteIndex $idx -SuiteTotal $suiteCount `
        -SuiteName "exists case-insensitive E2E tests" -Reason $reason | Out-Null
}
else {
    $idx++
    $existsCiArgs = @()
    if ($TsPath)   { $existsCiArgs += @("-TsPath", $TsPath) }
    if ($Verbose)  { $existsCiArgs += "-Verbose" }
    Invoke-Suite -SuiteIndex $idx -SuiteTotal $suiteCount `
        -SuiteName "exists case-insensitive E2E tests" -ScriptPath $ExistsCiScript -Arguments $existsCiArgs | Out-Null
}

# ---------------------------------------------------------------------------
# Suite 9: batch-upsert E2E tests
# ---------------------------------------------------------------------------
if ($skipBatchUpsertActual) {
    $idx++
    $reason = if ($NoTs) { "batch-upsert tests require live TS" } else { "skipped by -SkipBatchUpsert" }
    Skip-Suite -SuiteIndex $idx -SuiteTotal $suiteCount `
        -SuiteName "batch-upsert E2E tests" -Reason $reason | Out-Null
}
else {
    $idx++
    $batchUpsertArgs = @()
    if ($TsPath)   { $batchUpsertArgs += @("-TsPath", $TsPath) }
    if ($Verbose)  { $batchUpsertArgs += "-Verbose" }
    Invoke-Suite -SuiteIndex $idx -SuiteTotal $suiteCount `
        -SuiteName "batch-upsert E2E tests" -ScriptPath $BatchUpsertScript -Arguments $batchUpsertArgs | Out-Null
}

# ---------------------------------------------------------------------------
# Suite 10: directory-batch E2E tests
# ---------------------------------------------------------------------------
if ($skipDirectoryBatchActual) {
    $idx++
    $reason = if ($NoTs) { "directory-batch tests require live TS" } else { "skipped by -SkipDirectoryBatch" }
    Skip-Suite -SuiteIndex $idx -SuiteTotal $suiteCount `
        -SuiteName "directory-batch E2E tests" -Reason $reason | Out-Null
}
else {
    $idx++
    $directoryBatchArgs = @()
    if ($TsPath)   { $directoryBatchArgs += @("-TsPath", $TsPath) }
    if ($Verbose)  { $directoryBatchArgs += "-Verbose" }
    Invoke-Suite -SuiteIndex $idx -SuiteTotal $suiteCount `
        -SuiteName "directory-batch E2E tests" -ScriptPath $DirectoryBatchScript -Arguments $directoryBatchArgs | Out-Null
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
