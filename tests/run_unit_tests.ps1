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

.PARAMETER TimeoutSec
    Per-test timeout in seconds (default 600). If a test executable does not
    exit within this bound it is killed, reported as FAIL with a
    "TIMEOUT after Ns" message plus its partial log, and the suite continues.
    The timeout counts as a failure in the summary totals.

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
    [switch]$SkipBaselineDllPrompt,
    [ValidateRange(1, 86400)][int]$TimeoutSec = 600
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
    [pscustomobject]@{ Name = 'test_list_items';             RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_invoke_excepinfo';       RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_elevation_guard';        RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_crash_handler';          RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_json_serialize';         RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_list_cmd';               RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_log_levels';             RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_serve_dispatch';         RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_serve_framing';          RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_batch_progress';         RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_pbar';                   RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_com_apartment_guard';    RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_rc01_com_balance';       RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_tsdev_com_init_seam';    RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_persistence_integrity';  RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_create_path_cleanup';    RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_delete_disk_cleanup';    RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_close_warehouse';        RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_name_index_outcome';     RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_master_recs_seh';        RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_preop_hash_classification'; RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_orlib_resolver';         RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_diag_log';               RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_whserver_health';        RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_whserver_census';        RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_whserver_recycle';       RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_dispatch_watchdog';      RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_cache_backup';           RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_cache_cli_units';        RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_recycle_whserver_flag';  RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_recycle_resume';         RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_version_stamp';          RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_update_feed';            RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_update_apply';           RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_update_cmd';             RequiresTs = $false }
    [pscustomobject]@{ Name = 'test_cliresult_default';    RequiresTs = $false }
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
# Invoke-Test: runs a single test executable with a bounded wait, captures
# exit code, duration, and combined stdout/stderr. If the executable does not
# exit within TimeoutSec it is killed and reported as FAIL ("TIMEOUT after
# Ns") with its partial log preserved; the caller continues the suite and the
# timeout counts as a failure. Returns a result object.
# Uses System.Diagnostics.Process with async output capture (PowerShell 5.1
# and pwsh 7 compatible); direct "& $ExePath" invocation has no timeout and
# would hang the whole sequential runner on one wedged test.
# ---------------------------------------------------------------------------
function Invoke-Test {
    param(
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [string]$ExePath,
        [Parameter(Mandatory = $true)] [bool]$VerboseOutput,
        [Parameter(Mandatory = $true)] [int]$TimeoutSec
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

    $logLines = New-Object System.Collections.ArrayList
    $exitCode = 0
    $timedOut = $false
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()

    # Fixed engine-event names: the runner is sequential, and each
    # registration is removed in the finally block below.
    $outEventId = "UTOut"
    $errEventId = "UTErr"
    $proc = $null
    try {
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $ExePath
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow = $true
        $proc = New-Object System.Diagnostics.Process
        $proc.StartInfo = $psi
        # NOTE: raw add_OutputDataReceived scriptblocks run on threadpool
        # threads with no runspace and crash the host; engine object events
        # only enqueue (marshaled by the engine) and are drained below in our
        # own runspace. Works on PowerShell 5.1 and pwsh 7.
        Register-ObjectEvent -InputObject $proc -EventName OutputDataReceived -SourceIdentifier $outEventId | Out-Null
        Register-ObjectEvent -InputObject $proc -EventName ErrorDataReceived -SourceIdentifier $errEventId | Out-Null
        [void]$proc.Start()
        $proc.BeginOutputReadLine()
        $proc.BeginErrorReadLine()
        $exited = $proc.WaitForExit($TimeoutSec * 1000)
        if (-not $exited) {
            # Hung test: kill it, then reap it. The no-arg WaitForExit below
            # ensures all output events are queued before draining, so the
            # partial log is preserved.
            $timedOut = $true
            try { $proc.Kill() } catch { }
        }
        $proc.WaitForExit()
        try { $proc.CancelOutputRead(); $proc.CancelErrorRead() } catch { }
        if ($timedOut) {
            $exitCode = -3
        }
        else {
            $exitCode = $proc.ExitCode
        }
        foreach ($ev in @(@(Get-Event -SourceIdentifier $outEventId -ErrorAction SilentlyContinue) + @(Get-Event -SourceIdentifier $errEventId -ErrorAction SilentlyContinue))) {
            $d = $ev.SourceEventArgs.Data
            if ($d -ne $null) { [void]$logLines.Add([string]$d) }
            $ev | Remove-Event -ErrorAction SilentlyContinue
        }
    }
    catch {
        # Guard against unexpected runtime errors (e.g. process cannot start).
        [void]$logLines.Add($_.Exception.Message)
        $exitCode = -2
    }
    finally {
        Unregister-Event -SourceIdentifier $outEventId -ErrorAction SilentlyContinue
        Unregister-Event -SourceIdentifier $errEventId -ErrorAction SilentlyContinue
        if ($proc -ne $null) { $proc.Close() }
    }

    $stopwatch.Stop()

    $status = if ($exitCode -eq 0) { 'PASS' } else { 'FAIL' }
    $color  = if ($exitCode -eq 0) { 'Green' } else { 'Red' }

    $durationStr = "{0:0.00}s" -f $stopwatch.Elapsed.TotalSeconds
    $output = @($logLines.ToArray())
    if ($timedOut) {
        $output = @("TIMEOUT after ${TimeoutSec}s: hung process killed; partial log preserved below.") + $output
        Write-Host ("FAIL (exit {0}) [{1}] - TIMEOUT after {2}s: killed hung process" -f $exitCode, $durationStr, $TimeoutSec) -ForegroundColor $color
    }
    else {
        Write-Host ("{0} (exit {1}) [{2}]" -f $status, $exitCode, $durationStr) -ForegroundColor $color
    }

    # Print output on failure (timeouts always print: fail-loud with
    # attribution), or for all tests when -Verbose is set.
    if ($exitCode -ne 0 -or $VerboseOutput) {
        if ($timedOut) {
            Write-Host "--- partial log ($Name) ---" -ForegroundColor DarkGray
        }
        else {
            Write-Host "--- output ($Name) ---" -ForegroundColor DarkGray
        }
        if ($output -and $output.Count -gt 0) {
            $output | ForEach-Object { Write-Host $_ }
        }
        else {
            Write-Host "(no output captured)" -ForegroundColor DarkGray
        }
        if ($timedOut) {
            Write-Host "--- end partial log ---" -ForegroundColor DarkGray
        }
        else {
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
Write-Host "Timeout      : $TimeoutSec seconds per test"

if (-not (Test-Path -Path $ResolvedBuildDir -PathType Container)) {
    Write-Host ""
    Write-Host "ERROR: Build directory does not exist: $ResolvedBuildDir" -ForegroundColor Red
    Write-Host "Build the project first, e.g.: cmake --build build --config Release" -ForegroundColor Yellow
    exit 1
}

# ---------------------------------------------------------------------------
# Baseline DLL prerequisite for test_orlib_resolver. The DLL is an untracked
# local-copy fixture (proprietary orlib20.dll; it cannot be redistributed).
# If missing, prompt the user to copy it from the TS install (interactive
# runs). When it cannot be acquired (no TS install detected, prompt
# declined, or -SkipBaselineDllPrompt from the parent orchestrator),
# test_orlib_resolver is marked SKIPPED; all other tests still run (they do
# not need the DLL) and the run can still exit 0.
# ---------------------------------------------------------------------------
if ($SkipBaselineDllPrompt) {
    $baselineDllAvailable = (Test-Path -Path $BaselineDll -PathType Leaf)
} else {
    $baselineDllAvailable = Ensure-BaselineDll -DllPath $BaselineDll
}
if (-not $baselineDllAvailable) {
    Write-Host ""
    Write-Host "NOTE: baseline DLL not available - test_orlib_resolver will be SKIPPED." -ForegroundColor DarkYellow
    Write-Host "To enable it, copy <TS install>\Program\orlib20.dll to:" -ForegroundColor DarkYellow
    Write-Host "  $BaselineDll" -ForegroundColor DarkYellow
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

    # If the baseline DLL is unavailable, mark test_orlib_resolver as
    # SKIPPED (missing prerequisite, same treatment as RequiresTs skips)
    # instead of running it (it would exit 99 anyway).
    if ($td.Name -eq 'test_orlib_resolver' -and -not $baselineDllAvailable) {
        Write-Host ""
        Write-Host "===== $($td.Name) =====" -ForegroundColor Cyan
        Write-Host "SKIP (baseline DLL unavailable)" -ForegroundColor DarkYellow
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

    $exePath = Join-Path $ResolvedBuildDir ("{0}.exe" -f $td.Name)
    $result = Invoke-Test -Name $td.Name -ExePath $exePath -VerboseOutput $Verbose -TimeoutSec $TimeoutSec
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
