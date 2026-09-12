<#
.SYNOPSIS
    Master regression runner for the TSDev Automation project (TVS-B.6.3).

.DESCRIPTION
    Orchestrates ALL fifteen regression sub-suites in a single command, in the
    order required by the B.6 acceptance criteria:

      1. sigtool verify gate   (tests/test_sigtool.ps1)
         12 sigtool CLI tests against the baseline DLL. Fast, no live TS.
      2. C++ unit tests        (tests/run_unit_tests.ps1)
         The C++ unit-test inventory and the non-TS classification are
         authoritative in tests/run_unit_tests.ps1; the -NoTs path runs the
         non-TS subset of that inventory.
      3. E2E CLI tests         (tests/run_e2e_tests.ps1)
         E2E CLI test cases are specified in docs/E2E_TEST_SPEC.md. Slowest;
         creates and deletes real warehouse entries.
      4. exists E2E tests      (tests/test_exists.ps1)
         5 exists-subcommand E2E test cases (D1.1-D1.3). Requires live TS;
         skipped under -NoTs or -SkipExists. Creates and deletes a real
         warehouse probe entry.
      5. batch-stdin E2E tests (tests/test_batch_stdin.ps1)
         8 batch-stdin E2E test cases (S1-S8). Requires live TS; skipped
         under -NoTs or -SkipBatchStdin. Inserts and deletes real warehouse
         entries.
      6. upsert E2E tests      (tests/test_upsert.ps1)
         9 upsert-subcommand E2E test cases (U1-U5). Requires live TS;
         skipped under -NoTs or -SkipUpsert. Inserts and deletes real
         warehouse entries.
      7. get-source E2E tests  (tests/test_get_source.ps1)
         7 get-source-subcommand E2E test cases (GS1-GS6). Requires live TS;
         skipped under -NoTs or -SkipGetSource. Inserts and deletes real
         warehouse entries.
      8. exists case-insensitive E2E tests (tests/test_exists_ci.ps1)
         9 exists-subcommand case-insensitive E2E test cases (CI.1-CI.5).
         Requires live TS; skipped under -NoTs or -SkipExistsCi. Inserts and
         deletes real warehouse entries.
      9. batch-upsert E2E tests   (tests/test_batch_upsert.ps1)
         5 batch-upsert-subcommand E2E test cases (BU1-BU5). Requires live TS;
         skipped under -NoTs or -SkipBatchUpsert. Inserts and deletes real
         warehouse entries.
     10. directory-batch E2E tests (tests/test_directory_batch.ps1)
         9 directory-batch-subcommand E2E test cases (DB1-DB9). This suite
         drives live warehouse operations and requires live TS; it is distinct
         from the non-TS C++ test_directory_batch parser unit executable. It is
         skipped under -NoTs or -SkipDirectoryBatch.
     11. serve E2E tests        (tests/run_serve_e2e.ps1)
         Three default tests plus one opt-in stress test gated by
         TS_SERVE_STRESS=1. Requires live TS AND an elevated parent shell;
         skipped under -NoTs or -SkipServeE2e.
         Launches an unelevated Python client via Task Scheduler that
         connects to an elevated broker through a named pipe.
     12. ShowMe E2E tests      (tests/test_showme_e2e.ps1)
         9 ShowMe-component E2E test cases (SM1-SM9). Requires live TS;
         skipped under -NoTs or -SkipShowMeE2e. Inserts and deletes real
         warehouse entries.
     13. Unknown Link regression (tests/test_unknown_link_regression.ps1)
        Artifact invariants, mutations, and semantic-error checks across all 5
        component types (TVS-L.1+L.4+L.5). Requires live TS; skipped under
        -NoTs. Inserts and deletes real warehouse entries and verifies the
        ELL/XML finalization artifacts, GUID byte match, source round-trip,
        and cleanup. Historical aggregate counts are not current acceptance
        evidence. Mandatory in normal mode (no dedicated skip
        flag).
     14. Unknown Link stress (tests/test_unknown_link_stress.ps1)
        REPEATED-INSTANCE lifecycle stress: 600 insert/delete cycles
        (120 per type x 5 types), each cycle spawning a FRESH tsdev_cli.exe
        process. Requires live TS; skipped under -NoTs or when
        -ExtendedStress is not set. Asserts insert/delete success, ELL/XML
        existence after insert, ELL/XML absence after delete, and no crash
        reporter processes. Opt-in only. This is repeated-instance stress
        and does NOT prove long-lived COM/handle/window leak absence; that
        is the domain of tests/test_stress_batch.cpp (single-process
        resource sampling at start/mid/end).
     15. verbose-flag E2E tests (tests/test_verbose_flag.ps1)
        7 verbose-flag E2E test cases (V1-V7) for the repeatable -v/--verbose
        log-level flag: default run suppresses the "TS install path resolved"
        info line, -v shows it with byte-identical stdout, -vv activates
        Debug traces, position-free placement, bundled -vv == -v -v, and the
        --verbose / --verbose=<value> forms. Inserts+deletes its own unique
        Function fixture per run (setup/teardown uncounted). Requires live TS;
        skipped under -NoTs or -SkipVerboseFlag.

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
      - Skip the serve E2E suite entirely (prints "SKIP: serve E2E tests require live TS").
      - Skip the ShowMe E2E suite entirely (prints "SKIP: showme tests require live TS").
      - Skip the verbose-flag E2E suite entirely (prints "SKIP: verbose-flag tests require live TS").
      - Skip the Unknown Link regression suite entirely (prints "SKIP: unknown-link tests require live TS").
      - Skip the Unknown Link stress suite entirely (prints "SKIP: unknown-link stress tests require live TS").
      - Pass -NoTs to run_unit_tests.ps1 (runs only the non-TS unit tests;
        see tests/run_unit_tests.ps1 for the authoritative inventory and
        non-TS classification).
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

.PARAMETER SkipServeE2e
    Switch. Skip the serve E2E test suite (tests/run_serve_e2e.ps1).

.PARAMETER SkipShowMeE2e
    Switch. Skip the ShowMe E2E test suite (tests/test_showme_e2e.ps1).

.PARAMETER SkipVerboseFlag
    Switch. Skip the verbose-flag E2E test suite (tests/test_verbose_flag.ps1).

.PARAMETER ExtendedStress
    Switch. Opt-in for the Unknown Link stress test suite
    (tests/test_unknown_link_stress.ps1). When set, Suite 14 runs after Suite
    13. By default the stress suite is skipped without affecting the overall
    regression result. Requires live TS.

.PARAMETER Verbose
    Switch. Passed through to all sub-scripts (enables full output for
    every test, not just failures).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1
    Processes all 15 suite banners (Suite 14 skipped by default; opt-in via
    -ExtendedStress): sigtool + the C++ unit tests (see
    tests/run_unit_tests.ps1 for the inventory) + the E2E CLI tests (see
    docs/E2E_TEST_SPEC.md for the case list) + 5 exists tests + 8 batch-stdin
    tests + 9 upsert tests + 7 get-source tests + 9 exists-ci tests + 5
    batch-upsert tests + 9 directory-batch tests + three default serve E2E
    tests (plus one opt-in stress test when TS_SERVE_STRESS=1) + 9 ShowMe
    tests + the unknown-link regression suite + 7 verbose-flag tests.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1 -NoTs
    Runs sigtool + the non-TS C++ unit tests (see tests/run_unit_tests.ps1
    for the authoritative inventory); skips E2E, exists, batch-stdin,
    upsert, get-source, exists-ci, batch-upsert, directory-batch, serve,
    ShowMe, verbose-flag, and unknown-link E2E suites (regression and
    stress).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1 -SkipE2e
    Runs sigtool + all C++ unit tests (see tests/run_unit_tests.ps1 for the
    inventory) + 5 exists tests + 8 batch-stdin tests + 9 upsert tests + 7
    get-source tests + 9 exists-ci tests + 5 batch-upsert tests + 9
    directory-batch tests + 9 ShowMe tests + the unknown-link regression
    suite + 7 verbose-flag tests; skips E2E and the stress suite (opt-in:
    -ExtendedStress).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1 -Verbose
    Processes all 15 suite banners with full output for every test (Suite 14
    skipped by default; opt-in via -ExtendedStress).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1 -ExtendedStress
    Runs all 15 suites including the Unknown Link stress test (120
    insert/delete cycles per component type, 600 total).

.NOTES
    Exit codes:
      0 = all executed suites passed
      1 = one or more suites failed (or a prerequisite was not met)

    Prerequisites:
      - The project must be built: cmake --build build --config Release
        (or: cmake --build build --config Release --target test_regression)
      - TradeStation 9.5 must be installed for the E2E, exists, and
        batch-stdin, upsert, get-source, exists-ci, batch-upsert,
        directory-batch, serve, ShowMe, verbose-flag, and unknown-link E2E
        suites (unless -NoTs,
        -SkipE2e, -SkipExists, -SkipBatchStdin, -SkipUpsert,
        -SkipGetSource, -SkipExistsCi, -SkipBatchUpsert,
        -SkipDirectoryBatch, -SkipServeE2e, -SkipShowMeE2e, or
        -SkipVerboseFlag is used).
        The Unknown Link stress suite additionally requires -ExtendedStress.
      - tests/fixtures/orlib20_baseline.dll must exist for the sigtool
        suite and test_orlib_resolver. If missing, the runner PROMPTS the
        user to copy it from the TradeStation installation. If the user
        declines, the sigtool suite and test_orlib_resolver are marked
        FAILED; other suites still run.
      - Runs that execute suites invoking tsdev_cli.exe directly must start
        this runner from an elevated administrator process. The elevation
        contract (including the non-elevated direct-CLI exit code 11
        behavior and the non-TS/sigtool path having no direct-CLI elevation
        prerequisite) is authoritative in AGENTS.md and docs/BUILD.md.
      - No general TradeStation Desktop/TSDev running-state prerequisite
        applies to these CLI/library suites. A suite that explicitly exercises
        Desktop state must document that state in its own procedure.
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
    [switch]$SkipServeE2e,
    [switch]$SkipShowMeE2e,
    [switch]$SkipVerboseFlag,
    [switch]$ExtendedStress,
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
$ServeE2eScript = Join-Path $PSScriptRoot "run_serve_e2e.ps1"
$ShowMeE2eScript = Join-Path $PSScriptRoot "test_showme_e2e.ps1"
$VerboseFlagScript = Join-Path $PSScriptRoot "test_verbose_flag.ps1"
$UnknownLinkScript = Join-Path $PSScriptRoot "test_unknown_link_regression.ps1"
$UnknownLinkStressScript = Join-Path $PSScriptRoot "test_unknown_link_stress.ps1"

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
# Cached result of a safe non-TS probe of the child `powershell` binary so
# -Verbose can evidence that suite children really are Windows PowerShell 5.1.
$script:WinPsChildInfo = $null
function Get-WinPsChildInfo {
    if ($null -eq $script:WinPsChildInfo) {
        try {
            # No quote literals inside -Command: nested quoting would strip
            # them and break the child's parse. Edition and version arrive as
            # separate output lines.
            $out = & powershell -NoProfile -Command `
                'Write-Output $PSVersionTable.PSEdition; Write-Output $PSVersionTable.PSVersion.ToString()' 2>$null
            $lines = @($out | ForEach-Object { "$_".Trim() } | Where-Object { $_ })
            $script:WinPsChildInfo = if ($lines.Count -ge 2) { "$($lines[0])/$($lines[1])" } else { 'unknown' }
        } catch { $script:WinPsChildInfo = 'unknown' }
    }
    return $script:WinPsChildInfo
}

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
    #
    # Put the Windows PowerShell system modules directory first in the child's
    # PSModulePath: when the ambient ordering lists PowerShell-7 module dirs
    # ahead of it, PS5.1 autoload resolves Core-only modules (e.g.
    # Microsoft.PowerShell.Utility) and cmdlets such as Get-FileHash become
    # unrecognizable in the suite children. Process-scoped; the parent's exact
    # value is restored in finally even when the launch or the child fails.
    $winPsModuleDir = Join-Path ([Environment]::GetFolderPath('System')) 'WindowsPowerShell\v1.0\Modules'
    $savedPSModulePath = $env:PSModulePath
    $childModulePath = New-Object 'System.Collections.Generic.List[string]'
    $childModulePath.Add($winPsModuleDir)
    foreach ($entry in ($savedPSModulePath -split ';')) {
        if ([string]::IsNullOrWhiteSpace($entry)) { continue }
        if ($entry -ieq $winPsModuleDir) { continue }
        $childModulePath.Add($entry)
    }
    $env:PSModulePath = ($childModulePath -join ';')
    if ($Verbose) {
        # $Verbose is a declared switch param (it does not set
        # $VerbosePreference), so force emission with -Verbose.
        Write-Verbose ("{0}: child {1}; PSModulePath[0]={2}" -f $SuiteName, (Get-WinPsChildInfo), $winPsModuleDir) -Verbose
    }

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
        $env:PSModulePath = $savedPSModulePath
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
Write-Host "SkipServeE2e : $SkipServeE2e"
Write-Host "SkipShowMeE2e : $SkipShowMeE2e"
Write-Host "SkipVerboseFlag : $SkipVerboseFlag"
Write-Host "ExtendedStress : $ExtendedStress"
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
# live TS -- it inserts and deletes a real warehouse probe entry).
$skipExistsActual = $SkipExists -or $NoTs
if (-not $skipExistsActual) { $suiteCount++ }

# If exists is skipped (e.g. due to -NoTs), still print a SKIP banner so the
# user sees why. Count it in the total for banner numbering.
if ($skipExistsActual) { $suiteCount++ }

# batch-stdin E2E is skipped if -SkipBatchStdin is set OR -NoTs is set
# (batch-stdin requires live TS -- it inserts/deletes real warehouse entries).
$skipBatchStdinActual = $SkipBatchStdin -or $NoTs
if (-not $skipBatchStdinActual) { $suiteCount++ }

# If batch-stdin is skipped (e.g. due to -NoTs), still print a SKIP banner so
# the user sees why. Count it in the total for banner numbering.
if ($skipBatchStdinActual) { $suiteCount++ }

# upsert E2E is skipped if -SkipUpsert is set OR -NoTs is set
# (upsert requires live TS -- it inserts/deletes real warehouse entries).
$skipUpsertActual = $SkipUpsert -or $NoTs
if (-not $skipUpsertActual) { $suiteCount++ }

# If upsert is skipped (e.g. due to -NoTs), still print a SKIP banner so
# the user sees why. Count it in the total for banner numbering.
if ($skipUpsertActual) { $suiteCount++ }

# get-source E2E is skipped if -SkipGetSource is set OR -NoTs is set
# (get-source requires live TS -- it inserts/deletes real warehouse entries).
$skipGetSourceActual = $SkipGetSource -or $NoTs
if (-not $skipGetSourceActual) { $suiteCount++ }

# If get-source is skipped (e.g. due to -NoTs), still print a SKIP banner so
# the user sees why. Count it in the total for banner numbering.
if ($skipGetSourceActual) { $suiteCount++ }

# exists case-insensitive E2E is skipped if -SkipExistsCi is set OR -NoTs is
# set (exists-ci requires live TS -- it inserts/deletes real warehouse entries).
$skipExistsCiActual = $SkipExistsCi -or $NoTs
if (-not $skipExistsCiActual) { $suiteCount++ }

# If exists-ci is skipped (e.g. due to -NoTs), still print a SKIP banner so
# the user sees why. Count it in the total for banner numbering.
if ($skipExistsCiActual) { $suiteCount++ }

# batch-upsert E2E is skipped if -SkipBatchUpsert is set OR -NoTs is set
# (batch-upsert requires live TS -- it inserts/deletes real warehouse entries).
$skipBatchUpsertActual = $SkipBatchUpsert -or $NoTs
if (-not $skipBatchUpsertActual) { $suiteCount++ }

# If batch-upsert is skipped (e.g. due to -NoTs), still print a SKIP banner so
# the user sees why. Count it in the total for banner numbering.
if ($skipBatchUpsertActual) { $suiteCount++ }

# directory-batch E2E is skipped if -SkipDirectoryBatch is set OR -NoTs is set
# (directory-batch requires live TS -- it inserts/deletes real warehouse
# entries).
$skipDirectoryBatchActual = $SkipDirectoryBatch -or $NoTs
if (-not $skipDirectoryBatchActual) { $suiteCount++ }

# If directory-batch is skipped (e.g. due to -NoTs), still print a SKIP banner
# so the user sees why. Count it in the total for banner numbering.
if ($skipDirectoryBatchActual) { $suiteCount++ }

# serve E2E is skipped if -SkipServeE2e is set OR -NoTs is set
# (serve E2E requires live TS + an elevated parent shell).
$skipServeE2eActual = $SkipServeE2e -or $NoTs
if (-not $skipServeE2eActual) { $suiteCount++ }
if ($skipServeE2eActual) { $suiteCount++ }

# ShowMe E2E is skipped if -SkipShowMeE2e is set OR -NoTs is set
# (ShowMe requires live TS -- it inserts/deletes real warehouse entries).
$skipShowMeE2eActual = $SkipShowMeE2e -or $NoTs
if (-not $skipShowMeE2eActual) { $suiteCount++ }

# If ShowMe is skipped (e.g. due to -NoTs), still print a SKIP banner so the
# user sees why. Count it in the total for banner numbering.
if ($skipShowMeE2eActual) { $suiteCount++ }

# Unknown Link regression is skipped if -NoTs is set (it requires live TS --
# it inserts/deletes real warehouse entries across all 5 component types and
# verifies ELL/XML finalization artifacts, mutations, and semantic errors).
# It has NO dedicated skip flag: it is mandatory in normal mode (part of the
# full regression suite).
$skipUnknownLinkActual = $NoTs
if (-not $skipUnknownLinkActual) { $suiteCount++ }

# If unknown-link is skipped (e.g. due to -NoTs), still print a SKIP banner so
# the user sees why. Count it in the total for banner numbering.
if ($skipUnknownLinkActual) { $suiteCount++ }

# Unknown Link stress is skipped if -NoTs is set OR -ExtendedStress is not set
# (it requires live TS and is opt-in). It is always counted so banner
# numbering is stable regardless of the opt-in flag.
$skipUnknownLinkStressActual = $NoTs -or (-not $ExtendedStress)
if (-not $skipUnknownLinkStressActual) { $suiteCount++ }
if ($skipUnknownLinkStressActual) { $suiteCount++ }

# verbose-flag E2E is skipped if -SkipVerboseFlag is set OR -NoTs is set
# (the verbose-flag suite invokes tsdev_cli.exe directly, which requires
# live TS + an elevated parent shell).
$skipVerboseFlagActual = $SkipVerboseFlag -or $NoTs
if (-not $skipVerboseFlagActual) { $suiteCount++ }

# If verbose-flag is skipped (e.g. due to -NoTs), still print a SKIP banner
# so the user sees why. Count it in the total for banner numbering.
if ($skipVerboseFlagActual) { $suiteCount++ }

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
# Suite 11: serve E2E tests
# ---------------------------------------------------------------------------
# serve E2E requires an elevated parent shell for its Task Scheduler harness.
# If not elevated, skip with a clear message rather than launching the suite.
$script:Elevated = $false
try {
    $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object System.Security.Principal.WindowsPrincipal($identity)
    $script:Elevated = $principal.IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)
} catch {
    $script:Elevated = $false
}

if ($skipServeE2eActual -or -not $script:Elevated) {
    $idx++
    if (-not $script:Elevated -and -not $skipServeE2eActual) {
        $reason = "serve E2E requires an elevated parent shell"
    } elseif ($NoTs) {
        $reason = "serve E2E tests require live TS"
    } else {
        $reason = "skipped by -SkipServeE2e"
    }
    Skip-Suite -SuiteIndex $idx -SuiteTotal $suiteCount `
        -SuiteName "serve E2E tests" -Reason $reason | Out-Null
}
else {
    $idx++
    Invoke-Suite -SuiteIndex $idx -SuiteTotal $suiteCount `
        -SuiteName "serve E2E tests" -ScriptPath $ServeE2eScript -Arguments @() | Out-Null
}

# ---------------------------------------------------------------------------
# Suite 12: ShowMe E2E tests
# ---------------------------------------------------------------------------
if ($skipShowMeE2eActual) {
    $idx++
    $reason = if ($NoTs) { "showme tests require live TS" } else { "skipped by -SkipShowMeE2e" }
    Skip-Suite -SuiteIndex $idx -SuiteTotal $suiteCount `
        -SuiteName "ShowMe E2E tests" -Reason $reason | Out-Null
}
else {
    $idx++
    $showMeArgs = @()
    if ($TsPath)   { $showMeArgs += @("-TsPath", $TsPath) }
    if ($Verbose)  { $showMeArgs += "-Verbose" }
    Invoke-Suite -SuiteIndex $idx -SuiteTotal $suiteCount `
        -SuiteName "ShowMe E2E tests" -ScriptPath $ShowMeE2eScript -Arguments $showMeArgs | Out-Null
}

# ---------------------------------------------------------------------------
# Suite 13: Unknown Link regression (TVS-L.1+L.4+L.5 validation harness)
# ---------------------------------------------------------------------------
if ($skipUnknownLinkActual) {
    $idx++
    $reason = "unknown-link tests require live TS"
    Skip-Suite -SuiteIndex $idx -SuiteTotal $suiteCount `
        -SuiteName "Unknown Link regression" -Reason $reason | Out-Null
}
else {
    $idx++
    $unknownLinkArgs = @()
    if ($TsPath)   { $unknownLinkArgs += @("-TsPath", $TsPath) }
    if ($Verbose)  { $unknownLinkArgs += "-Verbose" }
    Invoke-Suite -SuiteIndex $idx -SuiteTotal $suiteCount `
        -SuiteName "Unknown Link regression" -ScriptPath $UnknownLinkScript -Arguments $unknownLinkArgs | Out-Null
}

# ---------------------------------------------------------------------------
# Suite 14: Unknown Link REPEATED-INSTANCE lifecycle stress (opt-in, requires live TS).
# Each cycle spawns a fresh CLI process; this does NOT prove long-lived
# COM/handle/window leak absence (that is test_stress_batch.cpp's domain).
# ---------------------------------------------------------------------------
if ($skipUnknownLinkStressActual) {
    $idx++
    if ($NoTs) {
        $reason = "unknown-link stress tests require live TS"
    } else {
        $reason = "skipped by default (opt-in: -ExtendedStress)"
    }
    Skip-Suite -SuiteIndex $idx -SuiteTotal $suiteCount `
        -SuiteName "Unknown Link stress" -Reason $reason | Out-Null
}
else {
    $idx++
    $unknownLinkStressArgs = @()
    if ($TsPath)   { $unknownLinkStressArgs += @("-TsPath", $TsPath) }
    if ($Verbose)  { $unknownLinkStressArgs += "-Verbose" }
    Invoke-Suite -SuiteIndex $idx -SuiteTotal $suiteCount `
        -SuiteName "Unknown Link stress" -ScriptPath $UnknownLinkStressScript -Arguments $unknownLinkStressArgs | Out-Null
}

# ---------------------------------------------------------------------------
# Suite 15: verbose-flag E2E tests (repeatable -v/--verbose log levels).
# Inserts+deletes its own unique Function fixture per run; V-queries are
# read-only (exists/dependents) against that fixture.
# ---------------------------------------------------------------------------
if ($skipVerboseFlagActual) {
    $idx++
    $reason = if ($NoTs) { "verbose-flag tests require live TS" } else { "skipped by -SkipVerboseFlag" }
    Skip-Suite -SuiteIndex $idx -SuiteTotal $suiteCount `
        -SuiteName "verbose-flag E2E tests" -Reason $reason | Out-Null
}
else {
    $idx++
    $verboseFlagArgs = @()
    if ($TsPath)   { $verboseFlagArgs += @("-TsPath", $TsPath) }
    if ($Verbose)  { $verboseFlagArgs += "-Verbose" }
    Invoke-Suite -SuiteIndex $idx -SuiteTotal $suiteCount `
        -SuiteName "verbose-flag E2E tests" -ScriptPath $VerboseFlagScript -Arguments $verboseFlagArgs | Out-Null
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
