<#
.SYNOPSIS
    Orchestrated C++ unit test runner for the TSDev Automation project.

.DESCRIPTION
    Runs all C++ unit-test executables defined in `$TestDefinitions` from the
    build\Release directory in sequence (NOT in parallel, because the shared warehouse file MWRKArea.wrk
    is not process-safe). Captures exit codes, measures per-test duration, and
    returns a single aggregated exit code (0 = all pass, 1 = any fail). The
    entry count is the runner definition inventory (derivable from `$TestDefinitions`);
    it is not a count of results. The CMake `test_regression` dependency list is
    aligned with this inventory.

    The authoritative inventory is the `$TestDefinitions` array below (execution
    order + TS/non-TS classification). Array order is the execution order; each
    entry's `RequiresTs` flag is its TS/non-TS classification (`$true` requires
    TradeStation, `$false` does not). The entry count is derivable from the array.

.PARAMETER BuildDir
    Directory containing the test executables. Defaults to "build\Release"
    relative to the project root (parent of this script's tests\ directory).

.PARAMETER NoTs
    Switch (--no-ts). When set, run ONLY the non-TS-dependent tests (entries
    in `$TestDefinitions` whose `RequiresTs` is `$false`) and skip all
    TS-dependent tests with a "SKIP (requires TS)" message.

.PARAMETER Verbose
    Switch (--verbose). When set, print full stdout/stderr for ALL tests,
    not just for failures.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_unit_tests.ps1
    Runs all tests defined in `$TestDefinitions`.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_unit_tests.ps1 -NoTs
    Runs only the non-TS tests (RequiresTs = $false).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_unit_tests.ps1 -Verbose
    Runs all tests with full output for every test.

.NOTES
    Exit codes:
      0 = all executed tests passed
      1 = one or more tests failed (or build directory missing)
#>

param(
    [string]$BuildDir = "build\Release",
    [switch]$NoTs,
    [switch]$Verbose,
    [switch]$SkipBaselineDllPrompt
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Resolve paths. The script lives in tests\, so the project root is the
# parent of $PSScriptRoot. BuildDir is resolved relative to the project root.
# ---------------------------------------------------------------------------
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$ResolvedBuildDir = Join-Path $ProjectRoot $BuildDir

# ---------------------------------------------------------------------------
# Shared test helpers (baseline DLL prompting + TS install discovery).
# ---------------------------------------------------------------------------
Import-Module (Join-Path $PSScriptRoot "TestHelpers.psm1") -ErrorAction Stop -DisableNameChecking

# Baseline DLL path (required by test_orlib_resolver).
$BaselineDll = Join-Path $ProjectRoot "tests\fixtures\orlib20_baseline.dll"

# ---------------------------------------------------------------------------
# Test definitions in execution order. Each entry carries the executable
# base name and a flag indicating whether it requires TradeStation.
# ---------------------------------------------------------------------------
$TestDefinitions = @(
    [pscustomobject]@{ Name = 'test_no_hardcoded_rvas';      RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_tradingapp_type';        RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_showme_type';            RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_elevation_guard';        RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_crash_handler';          RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_json_serialize';         RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_log_levels';             RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_serve_dispatch';         RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_serve_framing';          RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_batch_progress';         RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_pbar';                   RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_com_apartment_guard';    RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_tsdev_com_init_seam';    RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_persistence_integrity';  RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_create_path_cleanup';    RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_delete_disk_cleanup';    RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_close_warehouse';        RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_preop_hash_classification'; RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_orlib_resolver';         RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_diag_log';               RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_whserver_health';        RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_dispatch_watchdog';      RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_cache_backup';           RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_cache_cli_units';        RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_path_discovery';         RequiresTs = $true  }
    [pscustomobject]@{ Name = 'test_finalization_lifecycle'; RequiresTs = $true  }
    [pscustomobject]@{ Name = 'test_insert';                 RequiresTs = $true  }
    [pscustomobject]@{ Name = 'test_insert_tradingapp';      RequiresTs = $true  }
    [pscustomobject]@{ Name = 'test_auto_cleanup';           RequiresTs = $true  }
    [pscustomobject]@{ Name = 'test_compiler_layout';        RequiresTs = $true  }
    [pscustomobject]@{ Name = 'test_save_module';            RequiresTs = $true  }
    [pscustomobject]@{ Name = 'test_0x40a_notification';     RequiresTs = $true  }
    [pscustomobject]@{ Name = 'test_modify';                 RequiresTs = $true  }
    [pscustomobject]@{ Name = 'test_modify_tradingapp';      RequiresTs = $true  }
    [pscustomobject]@{ Name = 'test_exists';                 RequiresTs = $true  }
    [pscustomobject]@{ Name = 'test_enumerate';              RequiresTs = $true  }
    [pscustomobject]@{ Name = 'test_upsert';                 RequiresTs = $true  }
    [pscustomobject]@{ Name = 'test_batch_upsert';           RequiresTs = $true  }
    [pscustomobject]@{ Name = 'test_directory_batch';        RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_get_source';             RequiresTs = $true  }
    [pscustomobject]@{ Name = 'test_delete';                 RequiresTs = $true  }
    [pscustomobject]@{ Name = 'test_delete_tradingapp';      RequiresTs = $true  }
    [pscustomobject]@{ Name = 'test_delete_check';           RequiresTs = $true  }
    [pscustomobject]@{ Name = 'test_delete_diag';            RequiresTs = $true  }
)

# ---------------------------------------------------------------------------
# Results collector.
# ---------------------------------------------------------------------------
$Results = New-Object System.Collections.Generic.List[object]

# ---------------------------------------------------------------------------
# Invoke-Test: runs a single test executable, captures exit code, duration,
# and combined stdout/stderr. Returns a result object.
# ---------------------------------------------------------------------------
function Invoke-Test {
    param(
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [string]$ExePath,
        [Parameter(Mandatory = $true)] [bool]$VerboseOutput
    )

    Write-Host ""
    Write-Host "===== $Name =====" -ForegroundColor Cyan

    if (-not (Test-Path -Path $ExePath -PathType Leaf)) {
        Write-Host "FAIL: executable not found ($ExePath)" -ForegroundColor Red
        return [pscustomobject]@{
            Name     = $Name
            ExitCode = -1
            Result   = 'FAIL'
            Duration = [timespan]::Zero
            Output   = "executable not found ($ExePath)"
            Skipped   = $false
        }
    }

    $output = $null
    $exitCode = 0
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()

    # Temporarily relax error preference: when a native command writes to
    # stderr and we merge streams with 2>&1, PowerShell wraps each stderr line
    # in a RemoteException. Under 'Stop' that becomes a terminating error,
    # which would abort the run even though the test itself exits 0. 'Continue'
    # lets us collect the merged output and read $LASTEXITCODE normally.
    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & $ExePath 2>&1
        $exitCode = $LASTEXITCODE
    }
    catch {
        # Guard against unexpected runtime errors (e.g. process cannot start).
        $output = $_.Exception.Message
        $exitCode = -2
    }
    finally {
        $ErrorActionPreference = $prevEAP
    }

    $stopwatch.Stop()

    $status = if ($exitCode -eq 0) { 'PASS' } else { 'FAIL' }
    $color  = if ($exitCode -eq 0) { 'Green' } else { 'Red' }

    $durationStr = "{0:0.00}s" -f $stopwatch.Elapsed.TotalSeconds
    Write-Host ("{0} (exit {1}) [{2}]" -f $status, $exitCode, $durationStr) -ForegroundColor $color

    # Print output on failure, or for all tests when -Verbose is set.
    if ($exitCode -ne 0 -or $VerboseOutput) {
        if ($output) {
            Write-Host "--- output ($Name) ---" -ForegroundColor DarkGray
            $output | ForEach-Object { Write-Host $_ }
            Write-Host "--- end output ---" -ForegroundColor DarkGray
        }
    }

    return [pscustomobject]@{
        Name     = $Name
        ExitCode = $exitCode
        Result   = $status
        Duration = $stopwatch.Elapsed
        Output   = $output
        Skipped   = $false
    }
}

# ---------------------------------------------------------------------------
# Print-Summary: prints a summary table and totals, returns the count of
# failures.
# ---------------------------------------------------------------------------
function Print-Summary {
    param(
        [Parameter(Mandatory = $true)] $Results
    )

    Write-Host ""
    Write-Host "==================== SUMMARY ====================" -ForegroundColor Cyan
    $header = "{0,-32} {1,-10} {2,-8} {3,-12}" -f "Test", "ExitCode", "Result", "Duration"
    Write-Host $header
    Write-Host ("{0,-32} {1,-10} {2,-8} {3,-12}" -f ("-" * 32), ("-" * 10), ("-" * 8), ("-" * 12))

    $passed = 0
    $failed = 0
    $skipped = 0

    foreach ($r in $Results) {
        if ($r.Skipped) {
            $skipped++
            $line = "{0,-32} {1,-10} {2,-8} {3,-12}" -f $r.Name, "-", "SKIP", "-"
            Write-Host $line -ForegroundColor DarkYellow
        }
        else {
            $durationStr = "{0:0.00}s" -f $r.Duration.TotalSeconds
            $line = "{0,-32} {1,-10} {2,-8} {3,-12}" -f $r.Name, $r.ExitCode, $r.Result, $durationStr
            $color = if ($r.Result -eq 'PASS') { 'Green' } else { 'Red' }
            Write-Host $line -ForegroundColor $color
            if ($r.Result -eq 'PASS') { $passed++ } else { $failed++ }
        }
    }

    Write-Host ""
    $total = $Results.Count
    Write-Host ("Totals: {0}/{1} passed, {2} failed, {3} skipped" -f $passed, $total, $failed, $skipped) -ForegroundColor Cyan

    return $failed
}

# ---------------------------------------------------------------------------
# Main execution.
# ---------------------------------------------------------------------------
Write-Host "TSDev Automation - C++ Unit Test Runner" -ForegroundColor Cyan
Write-Host "Project root : $ProjectRoot"
Write-Host "Build dir    : $ResolvedBuildDir"
$nonTsCount = @($TestDefinitions | Where-Object { -not $_.RequiresTs }).Count
Write-Host "Mode         : $(if ($NoTs) { "no-ts ($nonTsCount non-TS tests only)" } else { "full (all $($TestDefinitions.Count) tests)" })"
Write-Host "Verbose      : $Verbose"

if (-not (Test-Path -Path $ResolvedBuildDir -PathType Container)) {
    Write-Host ""
    Write-Host "ERROR: Build directory does not exist: $ResolvedBuildDir" -ForegroundColor Red
    Write-Host "Build the project first, e.g.: cmake --build build --config Release" -ForegroundColor Yellow
    exit 1
}

# ---------------------------------------------------------------------------
# Baseline DLL prerequisite for test_orlib_resolver. If missing, prompt the
# user to copy it from the TS install. Do NOT continue without a decision.
# If the user declines (or the copy fails), test_orlib_resolver is marked as
# FAILED with a loud message; other tests still run (they do not need the
# DLL). The script ultimately exits 1 because test_orlib_resolver failed.
# ---------------------------------------------------------------------------
# If invoked from run_regression.ps1 with -SkipBaselineDllPrompt (the
# orchestrator already prompted and the user declined), skip the prompt and
# treat the DLL as unavailable.
if ($SkipBaselineDllPrompt) {
    $baselineDllAvailable = (Test-Path -Path $BaselineDll -PathType Leaf)
    if (-not $baselineDllAvailable) {
        Write-Host ""
        Write-Host "FATAL: baseline DLL not available - test_orlib_resolver cannot run." -ForegroundColor Red
        Write-Host "(prompt suppressed by -SkipBaselineDllPrompt from parent orchestrator)" -ForegroundColor DarkGray
        Write-Host "Other tests will continue, but test_orlib_resolver will be marked FAILED." -ForegroundColor Yellow
    }
} else {
    $baselineDllAvailable = Ensure-BaselineDll -DllPath $BaselineDll
    if (-not $baselineDllAvailable) {
        Write-Host ""
        Write-Host "FATAL: baseline DLL not available - test_orlib_resolver cannot run." -ForegroundColor Red
        Write-Host "Other tests will continue, but test_orlib_resolver will be marked FAILED." -ForegroundColor Yellow
    }
}

foreach ($td in $TestDefinitions) {
    # Honor --no-ts: skip TS-dependent tests.
    if ($NoTs -and $td.RequiresTs) {
        Write-Host ""
        Write-Host "===== $($td.Name) =====" -ForegroundColor Cyan
        Write-Host "SKIP (requires TS)" -ForegroundColor DarkYellow
        $Results.Add([pscustomobject]@{
            Name     = $td.Name
            ExitCode = 0
            Result   = 'SKIP'
            Duration = [timespan]::Zero
            Output   = $null
            Skipped   = $true
        })
        continue
    }

    # If the baseline DLL is unavailable, mark test_orlib_resolver as FAILED
    # with a loud message instead of running it (it would exit 99 anyway,
    # but this gives a clear, immediate explanation).
    if ($td.Name -eq 'test_orlib_resolver' -and -not $baselineDllAvailable) {
        Write-Host ""
        Write-Host "===== $($td.Name) =====" -ForegroundColor Cyan
        Write-Host "FAIL: FATAL - baseline DLL not available (user declined or copy failed)." -ForegroundColor Red
        Write-Host "      test_orlib_resolver requires $BaselineDll." -ForegroundColor Yellow
        $Results.Add([pscustomobject]@{
            Name     = $td.Name
            ExitCode = 99
            Result   = 'FAIL'
            Duration = [timespan]::Zero
            Output   = "FATAL: baseline DLL not available - test_orlib_resolver cannot run."
            Skipped   = $false
        })
        continue
    }

    $exePath = Join-Path $ResolvedBuildDir ("{0}.exe" -f $td.Name)
    $result = Invoke-Test -Name $td.Name -ExePath $exePath -VerboseOutput $Verbose
    $Results.Add($result)
}

$failureCount = Print-Summary -Results $Results

Write-Host ""
if ($failureCount -eq 0) {
    Write-Host "ALL TESTS PASSED" -ForegroundColor Green
    exit 0
}
else {
    Write-Host "$failureCount TEST(S) FAILED" -ForegroundColor Red
    exit 1
}
