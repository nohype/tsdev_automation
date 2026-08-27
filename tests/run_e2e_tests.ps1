<#
.SYNOPSIS
    End-to-end CLI regression test runner for the TSDev Automation project (TVS-B.6.1).

.DESCRIPTION
    Implements the full 34-test-case E2E CLI test specification defined in
    docs/E2E_TEST_SPEC.md, covering all 4 supported component types
    (Strategy, Indicator, Function, TradingApp) across three parts:

      Part A (16 tests): insert -> modify -> delete lifecycle for all 4 types.
      Part B (8 tests):   Type-specific semantic errors (Plot in Strategy,
                          Buy in Indicator, etc.) plus 2 positive controls.
      Part C (10 tests):  General errors (invalid syntax, modify/delete
                          nonexistent x4 types, unknown type string).

    Each test writes ASCII-encoded EasyLanguage source to a temp file, invokes
    tsdev_cli.exe, captures stdout+stderr and the exit code, verifies the exit
    code and key output substrings, and records the result.

    A unique run-ID prefix (E2E_<timestamp>_...) is used for every test
    component name to avoid collisions with real components or prior runs.

    Robust try/finally cleanup re-deletes every test name (expecting exit 5 =
    not found), verifies no orphaned .ell files remain in the warehouse Bin
    directory, and removes the temp directory.

    A markdown report is written to tmp/re_artifacts/e2e_all_types_<runId>.md.

    The script returns a single aggregated exit code: 0 = all 34 pass,
    1 = any fail.

.PARAMETER CliPath
    Path to the tsdev_cli.exe executable. Defaults to
    "$PSScriptRoot\..\build\Release\tsdev_cli.exe".

.PARAMETER TsPath
    Optional. TradeStation install directory passed through to the CLI as
    "--ts-path <dir>" BEFORE the subcommand. Use this when TradeStation is
    installed in a non-default location.

.PARAMETER Verbose
    Switch. When set, print full CLI output (stdout+stderr) for every test,
    not just for failures.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_e2e_tests.ps1
    Runs all 34 E2E tests with the default CLI path.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_e2e_tests.ps1 -Verbose
    Runs all 34 tests and prints full CLI output for every test.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_e2e_tests.ps1 -TsPath "D:\TradeStation 9.5"
    Runs all 34 tests using a non-default TradeStation install path.

.NOTES
    Exit codes (returned by this script):
      0 = all 34 tests passed
      1 = one or more tests failed (or a prerequisite was not met)

    CLI exit codes (from the CLI under test, per spec section 3):
      0 = Success
      1 = Usage error
      2 = Unknown type string
      3 = Source file error
      4 = Init failure
      5 = Not found / already exists
      8 = Compile error
      9 = Missing/inconsistent persistence artifact (artifact_missing), including
          missing .ell/.xml artifacts or a detected Fault A stale/inconsistent .ell
      11 = Not elevated (not_elevated), when an asInvoker build reaches the
           runtime elevation guard

    Prerequisites:
      - The project must be built: cmake --build build --config Release
      - TradeStation 9.5 must be installed (or --ts-path provided)
      - The invoking PowerShell process must be an elevated administrator
        process because this runner invokes the default requireAdministrator
        tsdev_cli.exe directly. A non-elevated launch follows normal Windows
        UAC/manifest handling and may fail at process creation with
        ERROR_ELEVATION_REQUIRED (Windows error 740), before the CLI emits an
        exit code. An asInvoker build reaches the runtime guard and returns
        exit code 11 (not_elevated).
      - No general TradeStation Desktop/TSDev running-state prerequisite
        applies; this suite does not use the Desktop UI
#>

param(
    [string]$CliPath = (Join-Path $PSScriptRoot "..\build\Release\tsdev_cli.exe"),
    [string]$TsPath,
    [switch]$Verbose
)

# ---------------------------------------------------------------------------
# Resolve paths and run ID.
# ---------------------------------------------------------------------------
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$resolved = Resolve-Path -Path $CliPath -ErrorAction SilentlyContinue
if ($resolved) {
    $CliPath = $resolved.Path
} elseif (-not (Test-Path -Path $CliPath -PathType Leaf)) {
    # Normalize the default relative path if the file doesn't exist yet.
    $CliPath = Join-Path $ProjectRoot "build\Release\tsdev_cli.exe"
}

$runId = Get-Date -Format "yyyyMMdd_HHmmss"
$artifactsDir = Join-Path $ProjectRoot "tmp\re_artifacts"
$tmpDir = Join-Path $ProjectRoot "tmp\e2e_run_$runId"
$reportPath = Join-Path $artifactsDir "e2e_all_types_$runId.md"

# Warehouse Bin directory for orphan .ell checks. Match the CLI's selected
# TradeStation root when -TsPath is supplied; otherwise retain the default.
if ($TsPath) {
    $binDir = Join-Path $TsPath "MyWork\Bin"
} else {
    $binDir = Join-Path "C:\Program Files (x86)\TradeStation 9.5" "MyWork\Bin"
}

# Results collector: list of result objects.
$Results = New-Object System.Collections.Generic.List[object]

# Track all test names created during the run for cleanup.
$CreatedNames = New-Object System.Collections.Generic.List[object]

# Global stopwatch for the whole run.
$runStopwatch = [System.Diagnostics.Stopwatch]::StartNew()

# ---------------------------------------------------------------------------
# Write-SourceFile: writes EasyLanguage source to a temp file in ASCII
# encoding (UTF-8 BOM breaks the EasyLanguage compiler). Returns the path.
# ---------------------------------------------------------------------------
function Write-SourceFile {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string]$Content
    )
    $path = Join-Path $script:tmpDir "$Name.txt"
    [System.IO.File]::WriteAllText($path, $Content, [System.Text.Encoding]::ASCII)
    return $path
}

# ---------------------------------------------------------------------------
# Invoke-Cli: runs tsdev_cli.exe with the given argument array, capturing
# combined stdout+stderr and the exit code. Returns a hashtable with
# Output (string array) and ExitCode (int).
#
# The --ts-path flag (if provided) is inserted BEFORE the subcommand per
# the CLI argument format. $ErrorActionPreference is temporarily set to
# 'Continue' because merging stderr via 2>&1 wraps lines in RemoteException
# objects which under 'Stop' become terminating errors (false failures).
# ---------------------------------------------------------------------------
function Invoke-Cli {
    param(
        [Parameter(Mandatory)] [string[]]$Arguments
    )
    $fullArgs = @()
    if ($script:TsPath) {
        $fullArgs += @("--ts-path", $script:TsPath)
    }
    $fullArgs += $Arguments

    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & $script:CliPath @fullArgs 2>&1
        $exitCode = $LASTEXITCODE
    }
    catch {
        $output = $_.Exception.Message
        $exitCode = -2
    }
    finally {
        $ErrorActionPreference = $prevEAP
    }

    # Normalize output to a single trimmed string for substring matching.
    # When stderr is merged via 2>&1, PowerShell wraps stderr lines in
    # RemoteException objects whose string form carries a noisy
    # "RemoteException:" prefix. Convert ErrorRecord objects to their
    # underlying message and strip any remaining "RemoteException:" prefix
    # so the captured output (and the markdown report) stays clean.
    $cleanLines = New-Object System.Collections.Generic.List[string]
    foreach ($line in $output) {
        $s = if ($line -is [System.Management.Automation.ErrorRecord]) {
            $line.Exception.Message
        } else {
            "$line"
        }
        if ($s -match '^RemoteException:\s*(.*)') { $s = $Matches[1] }
        $cleanLines.Add($s) | Out-Null
    }
    $outputStr = ($cleanLines | Out-String).Trim()
    return @{
        Output   = $outputStr
        ExitCode = $exitCode
    }
}

# ---------------------------------------------------------------------------
# Invoke-E2eTest: the core test runner. Runs one CLI invocation, verifies
# the exit code and an optional output substring, records the result, and
# prints PASS/FAIL. Returns the result object.
#
# Parameters:
#   TestId      - e.g. "A1.1", "B3", "C2.1"
#   Part        - "A", "B", or "C"
#   Type        - component type string (strategy/indicator/function/tradingapp)
#   Step        - human-readable step description
#   Arguments   - argument array for the CLI (subcommand + type + name + ...)
#   ExpectedExit- expected exit code
#   ExpectSubstr- optional substring expected in combined output (case-sensitive)
#   TestName    - the component name used (for cleanup tracking)
# ---------------------------------------------------------------------------
function Invoke-E2eTest {
    param(
        [Parameter(Mandatory)] [string]$TestId,
        [Parameter(Mandatory)] [string]$Part,
        [Parameter(Mandatory)] [string]$Type,
        [Parameter(Mandatory)] [string]$Step,
        [Parameter(Mandatory)] [string[]]$Arguments,
        [Parameter(Mandatory)] [int]$ExpectedExit,
        [string[]]$ExpectSubstr,
        [string]$TestName,
        [string]$Source
    )

    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $cli = Invoke-Cli -Arguments $Arguments
    $stopwatch.Stop()

    $actualExit = $cli.ExitCode
    $output = $cli.Output

    # Verify exit code.
    $exitOk = ($actualExit -eq $ExpectedExit)

    # Verify substring(s). ExpectSubstr may contain one or more required
    # substrings; ALL must be present for the check to pass.
    $substrOk = $true
    $missingSubstrs = @()
    if ($ExpectSubstr) {
        foreach ($s in $ExpectSubstr) {
            if (-not $output.Contains($s)) {
                $substrOk = $false
                $missingSubstrs += $s
            }
        }
    }

    $passed = ($exitOk -and $substrOk)
    $status = if ($passed) { 'PASS' } else { 'FAIL' }
    $color = if ($passed) { 'Green' } else { 'Red' }

    # Build a short output preview for the console (filter registry noise).
    $preview = ($output -split "`n" | Where-Object {
        $_.Trim() -ne '' -and $_ -notmatch 'TS install path resolved'
    } | Select-Object -First 4) -join ' | '
    if ($preview.Length -gt 200) { $preview = $preview.Substring(0, 200) + '...' }

    $durationStr = "{0:0.00}s" -f $stopwatch.Elapsed.TotalSeconds
    Write-Host ("[{0}] {1} ({2}) {3}: exit={4} expected={5} [{6}]" -f `
        $status, $TestId, $Type, $Step, $actualExit, $ExpectedExit, $durationStr) `
        -ForegroundColor $color

    if (-not $passed -or $Verbose) {
        if ($output) {
            Write-Host "  => $preview" -ForegroundColor DarkGray
        }
        if (-not $exitOk) {
            Write-Host "  EXIT MISMATCH: expected $ExpectedExit, got $actualExit" -ForegroundColor Red
        }
        if ($ExpectSubstr -and -not $substrOk) {
            Write-Host "  SUBSTR NOT FOUND: expected '$($missingSubstrs -join "', '")'" -ForegroundColor Red
        }
    }

    # Track created component names for cleanup (insert/modify create entries).
    if ($TestName -and $Arguments[0] -in @('insert', 'modify')) {
        if ($Arguments[0] -eq 'insert' -and -not $CreatedNames.Contains($TestName)) {
            $CreatedNames.Add($TestName) | Out-Null
        }
    }

    $result = [pscustomobject]@{
        TestId       = $TestId
        Part         = $Part
        Type         = $Type
        Step         = $Step
        Arguments    = ($Arguments -join ' ')
        ExpectedExit = $ExpectedExit
        ActualExit   = $actualExit
        Result       = $status
        Output       = $output
        Duration     = $stopwatch.Elapsed
        ExpectSubstr = $ExpectSubstr
        SubstrOk     = $substrOk
        TestName     = $TestName
        Source       = $Source
    }
    $Results.Add($result) | Out-Null
    return $result
}

# ---------------------------------------------------------------------------
# Extract-ProgId: parses "ProgId=<N>" from CLI output. Returns the integer
# ProgId or $null if not found.
# ---------------------------------------------------------------------------
function Extract-ProgId {
    param([string]$Output)
    if ($Output -match 'ProgId=(\d+)') {
        return [int]$Matches[1]
    }
    return $null
}

# ---------------------------------------------------------------------------
# Test-Cleanup: comprehensive cleanup after all tests. Re-deletes every test
# name across all 4 types (expecting exit 5 = not found), verifies no
# orphaned .ell files remain, and removes the temp directory. Returns a
# cleanup result object.
# ---------------------------------------------------------------------------
function Test-Cleanup {
    param(
        [string[]]$AllNames
    )

    Write-Host ""
    Write-Host "===== CLEANUP =====" -ForegroundColor Cyan

    $cleanupIssues = @()
    $types = @('strategy', 'indicator', 'function', 'tradingapp')

    # 1. Re-delete all test names across all types; all should return exit 5.
    foreach ($name in $AllNames) {
        foreach ($type in $types) {
            $cli = Invoke-Cli -Arguments @("delete", $type, $name)
            $code = $cli.ExitCode
            if ($code -eq 0) {
                $cleanupIssues += "WARN: delete $type '$name' returned 0 (entry still existed)"
            }
        }
    }

    # 2. Verify no orphaned .ell files in the warehouse Bin directory.
    $orphanedElls = @()
    if (Test-Path -Path $binDir -PathType Container) {
        $orphanedElls = @(Get-ChildItem -Path $binDir -Filter "elmodule_*_*.ell" -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match 'E2E' -or $_.Name -match $runId })
        if ($orphanedElls.Count -gt 0) {
            foreach ($f in $orphanedElls) {
                $cleanupIssues += "WARN: orphaned .ell file: $($f.Name)"
            }
        }
    } else {
        $cleanupIssues += "WARN: warehouse Bin directory not found: $binDir"
    }

    # 3. Remove temp directory.
    if (Test-Path -Path $tmpDir -PathType Container) {
        Remove-Item -Path $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
    }

    # Report.
    if ($cleanupIssues.Count -eq 0) {
        Write-Host "CLEANUP: OK - no leftover entries, no orphaned .ell files, temp dir removed" -ForegroundColor Green
    } else {
        Write-Host "CLEANUP: ISSUES FOUND:" -ForegroundColor Yellow
        foreach ($issue in $cleanupIssues) {
            Write-Host "  $issue" -ForegroundColor Yellow
        }
    }

    return @{
        Issues      = $cleanupIssues
        OrphanedElls = $orphanedElls
        Ok          = ($cleanupIssues.Count -eq 0)
    }
}

# ---------------------------------------------------------------------------
# Print-E2eSummary: prints a summary table with per-part and grand totals.
# Returns the number of failures.
# ---------------------------------------------------------------------------
function Print-E2eSummary {
    param($Results)

    Write-Host ""
    Write-Host "==================== E2E SUMMARY ====================" -ForegroundColor Cyan
    $header = "{0,-8} {1,-6} {2,-30} {3,-14} {4,-14} {5,-8}" -f `
        "Part", "Test#", "Test Name", "Expected Exit", "Actual Exit", "Result"
    Write-Host $header
    Write-Host ("{0,-8} {1,-6} {2,-30} {3,-14} {4,-14} {5,-8}" -f `
        ("-" * 8), ("-" * 6), ("-" * 30), ("-" * 14), ("-" * 14), ("-" * 8))

    $idx = 0
    foreach ($r in $Results) {
        $idx++
        $line = "{0,-8} {1,-6} {2,-30} {3,-14} {4,-14} {5,-8}" -f `
            $r.Part, $idx, "$($r.TestId) $($r.Type) $($r.Step)", $r.ExpectedExit, $r.ActualExit, $r.Result
        $color = if ($r.Result -eq 'PASS') { 'Green' } else { 'Red' }
        Write-Host $line -ForegroundColor $color
    }

    # Per-part totals.
    Write-Host ""
    $partA = ($Results | Where-Object { $_.Part -eq 'A' })
    $partB = ($Results | Where-Object { $_.Part -eq 'B' })
    $partC = ($Results | Where-Object { $_.Part -eq 'C' })
    $passA = @($partA | Where-Object { $_.Result -eq 'PASS' }).Count
    $passB = @($partB | Where-Object { $_.Result -eq 'PASS' }).Count
    $passC = @($partC | Where-Object { $_.Result -eq 'PASS' }).Count
    Write-Host ("Part A: {0}/{1} passed" -f $passA, $partA.Count) -ForegroundColor Cyan
    Write-Host ("Part B: {0}/{1} passed" -f $passB, $partB.Count) -ForegroundColor Cyan
    Write-Host ("Part C: {0}/{1} passed" -f $passC, $partC.Count) -ForegroundColor Cyan

    $totalPass = $passA + $passB + $passC
    $total = $Results.Count
    Write-Host ""
    Write-Host ("GRAND TOTAL: {0}/{1} passed" -f $totalPass, $total) -ForegroundColor Cyan

    return ($total - $totalPass)
}

# ---------------------------------------------------------------------------
# Write-MarkdownReport: writes the full markdown report per spec section 9.
# ---------------------------------------------------------------------------
function Write-MarkdownReport {
    param(
        $Results,
        $CleanupResult,
        $ProgIdMap
    )

    $sb = New-Object System.Text.StringBuilder

    [void]$sb.AppendLine("# E2E CLI Test Report - All Component Types")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("**Run ID:** $runId")
    [void]$sb.AppendLine("**Date:** $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')")
    [void]$sb.AppendLine("**CLI path:** $CliPath")
    [void]$sb.AppendLine("**TS path:** $(if ($TsPath) { $TsPath } else { 'auto-discovered' })")
    [void]$sb.AppendLine("**Temp dir:** $tmpDir")
    [void]$sb.AppendLine("**Duration:** $([math]::Round($runStopwatch.Elapsed.TotalSeconds, 2))s")
    [void]$sb.AppendLine("")

    # Build result.
    [void]$sb.AppendLine("## Build")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("CLI executable: ``$CliPath``")
    $cliExists = Test-Path -Path $CliPath -PathType Leaf
    [void]$sb.AppendLine("- CLI exists: **$cliExists**")
    [void]$sb.AppendLine("")

    # Part A results.
    [void]$sb.AppendLine("## Part A - Success Cases (insert / modify / delete)")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("| Test ID | Type | Step | Exit Code | Expected | Result | Key Output |")
    [void]$sb.AppendLine("|---------|------|------|-----------|----------|--------|------------|")
    foreach ($r in ($Results | Where-Object { $_.Part -eq 'A' })) {
        $keyOut = ""
        if ($r.TestName -and $ProgIdMap[$r.TestName]) {
            $keyOut = "ProgId=$($ProgIdMap[$r.TestName])"
        }
        $outShort = ($r.Output -split "`n" | Select-Object -First 1).Trim()
        if ($outShort.Length -gt 60) { $outShort = $outShort.Substring(0, 60) + '...' }
        if ($keyOut) { $outShort = "$keyOut; $outShort" }
        [void]$sb.AppendLine("| $($r.TestId) | $($r.Type) | $($r.Step) | $($r.ActualExit) | $($r.ExpectedExit) | $($r.Result) | $outShort |")
    }
    [void]$sb.AppendLine("")

    # Part B results.
    [void]$sb.AppendLine("## Part B - Type-Specific Semantic Errors")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("| Test ID | Type | Construct | Exit Code | Expected | Result | Error Text |")
    [void]$sb.AppendLine("|---------|------|-----------|-----------|----------|--------|------------|")
    foreach ($r in ($Results | Where-Object { $_.Part -eq 'B' })) {
        $errShort = ($r.Output -split "`n" | Select-Object -First 2) -join ' ' | ForEach-Object { $_.Trim() }
        if ($errShort.Length -gt 80) { $errShort = $errShort.Substring(0, 80) + '...' }
        [void]$sb.AppendLine("| $($r.TestId) | $($r.Type) | $($r.Step) | $($r.ActualExit) | $($r.ExpectedExit) | $($r.Result) | $errShort |")
    }
    [void]$sb.AppendLine("")

    # Part C results.
    [void]$sb.AppendLine("## Part C - General Error Cases")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("| Test ID | Type | Test | Exit Code | Expected | Result | Key Output |")
    [void]$sb.AppendLine("|---------|------|------|-----------|----------|--------|------------|")
    foreach ($r in ($Results | Where-Object { $_.Part -eq 'C' })) {
        $outShort = ($r.Output -split "`n" | Select-Object -First 1).Trim()
        if ($outShort.Length -gt 60) { $outShort = $outShort.Substring(0, 60) + '...' }
        [void]$sb.AppendLine("| $($r.TestId) | $($r.Type) | $($r.Step) | $($r.ActualExit) | $($r.ExpectedExit) | $($r.Result) | $outShort |")
    }
    [void]$sb.AppendLine("")

    # Summary.
    $partA = @($Results | Where-Object { $_.Part -eq 'A' })
    $partB = @($Results | Where-Object { $_.Part -eq 'B' })
    $partC = @($Results | Where-Object { $_.Part -eq 'C' })
    $passA = @($partA | Where-Object { $_.Result -eq 'PASS' }).Count
    $passB = @($partB | Where-Object { $_.Result -eq 'PASS' }).Count
    $passC = @($partC | Where-Object { $_.Result -eq 'PASS' }).Count
    $totalPass = $passA + $passB + $passC
    $total = $Results.Count
    $verdict = if ($totalPass -eq $total -and $CleanupResult.Ok) { 'PASS' } else { 'FAIL' }

    [void]$sb.AppendLine("## Summary")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("| Part | Tests | PASS | FAIL |")
    [void]$sb.AppendLine("|------|-------|------|------|")
    [void]$sb.AppendLine("| A | $($partA.Count) | $passA | $($partA.Count - $passA) |")
    [void]$sb.AppendLine("| B | $($partB.Count) | $passB | $($partB.Count - $passB) |")
    [void]$sb.AppendLine("| C | $($partC.Count) | $passC | $($partC.Count - $passC) |")
    [void]$sb.AppendLine("| **Total** | **$total** | **$totalPass** | **$($total - $totalPass)** |")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("## Overall Verdict")
    [void]$sb.AppendLine("")
    if ($verdict -eq 'PASS') {
        [void]$sb.AppendLine("**PASS** - All $total tests passed and cleanup verified.")
    } else {
        [void]$sb.AppendLine("**FAIL** - $($total - $totalPass) test(s) failed or cleanup issues found.")
    }
    [void]$sb.AppendLine("")

    # Key findings.
    [void]$sb.AppendLine("## Key Findings")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("### ProgId Preservation (Part A)")
    [void]$sb.AppendLine("")
    foreach ($entry in $ProgIdMap.GetEnumerator() | Where-Object { $_.Name -match '_mod$' }) {
        $baseName = $entry.Name -replace '_mod$', ''
        $insProgId = $ProgIdMap["$baseName`_ins"]
        $modProgId = $entry.Value
        $preserved = if ($insProgId -and $modProgId -and $insProgId -eq $modProgId) { 'YES' } else { 'NO' }
        [void]$sb.AppendLine("- $baseName : insert ProgId=$insProgId, modify ProgId=$modProgId, preserved=$preserved")
    }
    [void]$sb.AppendLine("")

    # Cleanup confirmation.
    [void]$sb.AppendLine("## Cleanup Confirmation")
    [void]$sb.AppendLine("")
    if ($CleanupResult.Ok) {
        [void]$sb.AppendLine("- Warehouse entries: all verified deleted (all re-deletes returned exit 5)")
        [void]$sb.AppendLine("- Orphaned .ell files: none found")
        [void]$sb.AppendLine("- Temp directory: removed ($tmpDir)")
    } else {
        [void]$sb.AppendLine("- Issues found:")
        foreach ($issue in $CleanupResult.Issues) {
            [void]$sb.AppendLine("  - $issue")
        }
    }
    [void]$sb.AppendLine("")

    # Appendix: test source code (embedded, self-contained).
    [void]$sb.AppendLine("## Appendix - Test Source Code")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("The actual EasyLanguage source code used in each test case is embedded below, organized by part and test ID.")
    [void]$sb.AppendLine("")
    foreach ($part in @('A','B','C')) {
        $partResults = $Results | Where-Object { $_.Part -eq $part -and $_.Source }
        if (-not $partResults) { continue }
        [void]$sb.AppendLine("### Part $part")
        [void]$sb.AppendLine("")
        foreach ($r in $partResults) {
            [void]$sb.AppendLine("#### $($r.TestId) - $($r.Type) - $($r.Step)")
            [void]$sb.AppendLine("")
            [void]$sb.AppendLine('```easylanguage')
            [void]$sb.AppendLine($r.Source)
            [void]$sb.AppendLine('```')
            [void]$sb.AppendLine("")
        }
    }

    # Write report.
    if (-not (Test-Path -Path $artifactsDir -PathType Container)) {
        New-Item -ItemType Directory -Force -Path $artifactsDir | Out-Null
    }
    [System.IO.File]::WriteAllText($reportPath, $sb.ToString(), [System.Text.Encoding]::UTF8)
    Write-Host ""
    Write-Host "Report written to: $reportPath" -ForegroundColor Cyan
}

# ===========================================================================
# MAIN EXECUTION
# ===========================================================================

Write-Host "============================================" -ForegroundColor Cyan
Write-Host "TSDev Automation - E2E CLI Test Runner" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "Run ID     : $runId"
Write-Host "Project root: $ProjectRoot"
Write-Host "CLI path   : $CliPath"
Write-Host "TS path    : $(if ($TsPath) { $TsPath } else { 'auto-discovered' })"
Write-Host "Verbose    : $Verbose"
Write-Host "Temp dir   : $tmpDir"
Write-Host ""

# Verify CLI exists.
if (-not (Test-Path -Path $CliPath -PathType Leaf)) {
    Write-Host "ERROR: CLI executable not found: $CliPath" -ForegroundColor Red
    Write-Host "Build the project first: cmake --build build --config Release" -ForegroundColor Yellow
    exit 1
}

# Create temp directory.
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null

# ProgId tracking for Part A (insert vs modify).
$ProgIdMap = @{}

# All test names for cleanup.
$allTestNames = @()

# Use try/finally to guarantee cleanup runs even if a test fails mid-run.
try {
    # =====================================================================
    # PART A: Success cases (insert -> modify -> delete) - 16 tests
    # =====================================================================
    Write-Host "--- PART A: Success cases (insert/modify/delete) ---" -ForegroundColor Cyan

    # --- A1: Strategy ---
    $n = "E2E_${runId}_Strat"; $allTestNames += $n
    $src = "Inputs: int x(0);`r`nif x > 0 then Buy next bar at market;"
    $f = Write-SourceFile "strat_ins" $src
    $r = Invoke-E2eTest -TestId "A1.1" -Part "A" -Type "strategy" -Step "insert" `
        -Arguments @("insert","strategy",$n,$f) -ExpectedExit 0 -ExpectSubstr "SUCCESS" -TestName $n -Source $src
    $ProgIdMap["${n}_ins"] = Extract-ProgId -Output $r.Output

    $src = "Inputs: int x(1);`r`nif x > 0 then Sell next bar at market;"
    $f = Write-SourceFile "strat_mod" $src
    $r = Invoke-E2eTest -TestId "A1.2" -Part "A" -Type "strategy" -Step "modify" `
        -Arguments @("modify","strategy",$n,$f) -ExpectedExit 0 -ExpectSubstr "ProgId=" -TestName $n -Source $src
    $ProgIdMap["${n}_mod"] = Extract-ProgId -Output $r.Output

    Invoke-E2eTest -TestId "A1.3" -Part "A" -Type "strategy" -Step "delete" `
        -Arguments @("delete","strategy",$n) -ExpectedExit 0 -ExpectSubstr @("SUCCESS","deleted") | Out-Null

    Invoke-E2eTest -TestId "A1.4" -Part "A" -Type "strategy" -Step "delete-neg" `
        -Arguments @("delete","strategy",$n) -ExpectedExit 5 -ExpectSubstr "not found" | Out-Null

    # --- A2: Indicator ---
    $n = "E2E_${runId}_Ind"; $allTestNames += $n
    $src = "Inputs: int x(0);`r`nPlot1(x, ""MyPlot"");"
    $f = Write-SourceFile "ind_ins" $src
    $r = Invoke-E2eTest -TestId "A2.1" -Part "A" -Type "indicator" -Step "insert" `
        -Arguments @("insert","indicator",$n,$f) -ExpectedExit 0 -ExpectSubstr "SUCCESS" -TestName $n -Source $src
    $ProgIdMap["${n}_ins"] = Extract-ProgId -Output $r.Output

    $src = "Inputs: int x(1);`r`nPlot1(x * 2, ""MyPlot2"");"
    $f = Write-SourceFile "ind_mod" $src
    $r = Invoke-E2eTest -TestId "A2.2" -Part "A" -Type "indicator" -Step "modify" `
        -Arguments @("modify","indicator",$n,$f) -ExpectedExit 0 -ExpectSubstr "ProgId=" -TestName $n -Source $src
    $ProgIdMap["${n}_mod"] = Extract-ProgId -Output $r.Output

    Invoke-E2eTest -TestId "A2.3" -Part "A" -Type "indicator" -Step "delete" `
        -Arguments @("delete","indicator",$n) -ExpectedExit 0 -ExpectSubstr @("SUCCESS","deleted") | Out-Null

    Invoke-E2eTest -TestId "A2.4" -Part "A" -Type "indicator" -Step "delete-neg" `
        -Arguments @("delete","indicator",$n) -ExpectedExit 5 -ExpectSubstr "not found" | Out-Null

    # --- A3: Function ---
    $n = "E2E_${runId}_Func"; $allTestNames += $n
    $src = "E2E_${runId}_Func = 1;"
    $f = Write-SourceFile "func_ins" $src
    $r = Invoke-E2eTest -TestId "A3.1" -Part "A" -Type "function" -Step "insert" `
        -Arguments @("insert","function",$n,$f) -ExpectedExit 0 -ExpectSubstr "SUCCESS" -TestName $n -Source $src
    $ProgIdMap["${n}_ins"] = Extract-ProgId -Output $r.Output

    $src = "E2E_${runId}_Func = 42;"
    $f = Write-SourceFile "func_mod" $src
    $r = Invoke-E2eTest -TestId "A3.2" -Part "A" -Type "function" -Step "modify" `
        -Arguments @("modify","function",$n,$f) -ExpectedExit 0 -ExpectSubstr "ProgId=" -TestName $n -Source $src
    $ProgIdMap["${n}_mod"] = Extract-ProgId -Output $r.Output

    Invoke-E2eTest -TestId "A3.3" -Part "A" -Type "function" -Step "delete" `
        -Arguments @("delete","function",$n) -ExpectedExit 0 -ExpectSubstr @("SUCCESS","deleted") | Out-Null

    Invoke-E2eTest -TestId "A3.4" -Part "A" -Type "function" -Step "delete-neg" `
        -Arguments @("delete","function",$n) -ExpectedExit 5 -ExpectSubstr "not found" | Out-Null

    # --- A4: TradingApp ---
    $n = "E2E_${runId}_TA"; $allTestNames += $n
    $src = "using elsystem;"
    $f = Write-SourceFile "ta_ins" $src
    $r = Invoke-E2eTest -TestId "A4.1" -Part "A" -Type "tradingapp" -Step "insert" `
        -Arguments @("insert","tradingapp",$n,$f) -ExpectedExit 0 -ExpectSubstr "SUCCESS" -TestName $n -Source $src
    $ProgIdMap["${n}_ins"] = Extract-ProgId -Output $r.Output

    $src = "using elsystem;`r`nusing elsystem.windows.forms;"
    $f = Write-SourceFile "ta_mod" $src
    $r = Invoke-E2eTest -TestId "A4.2" -Part "A" -Type "tradingapp" -Step "modify" `
        -Arguments @("modify","tradingapp",$n,$f) -ExpectedExit 0 -ExpectSubstr "ProgId=" -TestName $n -Source $src
    $ProgIdMap["${n}_mod"] = Extract-ProgId -Output $r.Output

    Invoke-E2eTest -TestId "A4.3" -Part "A" -Type "tradingapp" -Step "delete" `
        -Arguments @("delete","tradingapp",$n) -ExpectedExit 0 -ExpectSubstr @("SUCCESS","deleted") | Out-Null

    Invoke-E2eTest -TestId "A4.4" -Part "A" -Type "tradingapp" -Step "delete-neg" `
        -Arguments @("delete","tradingapp",$n) -ExpectedExit 5 -ExpectSubstr "not found" | Out-Null

    # =====================================================================
    # PART B: Type-specific semantic errors - 8 tests
    # =====================================================================
    Write-Host ""
    Write-Host "--- PART B: Type-specific semantic error cases ---" -ForegroundColor Cyan

    # B1: Plot in Strategy (should FAIL, exit 8)
    $n = "E2E_${runId}_B1"; $allTestNames += $n
    $src = "Plot1(Close, ""Test"");"
    $f = Write-SourceFile "b1" $src
    Invoke-E2eTest -TestId "B1" -Part "B" -Type "strategy" -Step "Plot in Strategy" `
        -Arguments @("insert","strategy",$n,$f) -ExpectedExit 8 -ExpectSubstr "not allowed in a Strategy" -TestName $n -Source $src | Out-Null
    Invoke-Cli -Arguments @("delete","strategy",$n) | Out-Null

    # B2: Buy in Indicator (should FAIL, exit 8)
    $n = "E2E_${runId}_B2"; $allTestNames += $n
    $src = "Buy next bar at market;"
    $f = Write-SourceFile "b2" $src
    Invoke-E2eTest -TestId "B2" -Part "B" -Type "indicator" -Step "Buy in Indicator" `
        -Arguments @("insert","indicator",$n,$f) -ExpectedExit 8 -ExpectSubstr "not allowed in a study" -TestName $n -Source $src | Out-Null
    Invoke-Cli -Arguments @("delete","indicator",$n) | Out-Null

    # B3: Plot in Function (should FAIL, exit 8)
    $n = "E2E_${runId}_B3"; $allTestNames += $n
    $src = "Plot1(Close, ""Test"");"
    $f = Write-SourceFile "b3" $src
    Invoke-E2eTest -TestId "B3" -Part "B" -Type "function" -Step "Plot in Function" `
        -Arguments @("insert","function",$n,$f) -ExpectedExit 8 -ExpectSubstr "not allowed in a function" -TestName $n -Source $src | Out-Null
    Invoke-Cli -Arguments @("delete","function",$n) | Out-Null

    # B4: Buy in Function (should FAIL, exit 8)
    $n = "E2E_${runId}_B4"; $allTestNames += $n
    $src = "Buy next bar at market;"
    $f = Write-SourceFile "b4" $src
    Invoke-E2eTest -TestId "B4" -Part "B" -Type "function" -Step "Buy in Function" `
        -Arguments @("insert","function",$n,$f) -ExpectedExit 8 -ExpectSubstr "not allowed in a function" -TestName $n -Source $src | Out-Null
    Invoke-Cli -Arguments @("delete","function",$n) | Out-Null

    # B5: Buy in TradingApp (should FAIL, exit 8)
    $n = "E2E_${runId}_B5"; $allTestNames += $n
    $src = "Buy next bar at market;"
    $f = Write-SourceFile "b5" $src
    Invoke-E2eTest -TestId "B5" -Part "B" -Type "tradingapp" -Step "Buy in TradingApp" `
        -Arguments @("insert","tradingapp",$n,$f) -ExpectedExit 8 -ExpectSubstr "not allowed in a study" -TestName $n -Source $src | Out-Null
    Invoke-Cli -Arguments @("delete","tradingapp",$n) | Out-Null

    # B6: Plot in TradingApp (should FAIL, exit 8)
    $n = "E2E_${runId}_B6"; $allTestNames += $n
    $src = "Plot1(Close, ""Test"");"
    $f = Write-SourceFile "b6" $src
    Invoke-E2eTest -TestId "B6" -Part "B" -Type "tradingapp" -Step "Plot in TradingApp" `
        -Arguments @("insert","tradingapp",$n,$f) -ExpectedExit 8 -ExpectSubstr "only allowed in an Indicator" -TestName $n -Source $src | Out-Null
    Invoke-Cli -Arguments @("delete","tradingapp",$n) | Out-Null

    # B7: Plot in Indicator (positive control, should SUCCEED, exit 0)
    $n = "E2E_${runId}_B7"; $allTestNames += $n
    $src = "Plot1(Close, ""Test"");"
    $f = Write-SourceFile "b7" $src
    Invoke-E2eTest -TestId "B7" -Part "B" -Type "indicator" -Step "Plot in Indicator (positive)" `
        -Arguments @("insert","indicator",$n,$f) -ExpectedExit 0 -ExpectSubstr "SUCCESS" -TestName $n -Source $src | Out-Null
    Invoke-Cli -Arguments @("delete","indicator",$n) | Out-Null

    # B8: Buy in Strategy (positive control, should SUCCEED, exit 0)
    $n = "E2E_${runId}_B8"; $allTestNames += $n
    $src = "Buy next bar at market;"
    $f = Write-SourceFile "b8" $src
    Invoke-E2eTest -TestId "B8" -Part "B" -Type "strategy" -Step "Buy in Strategy (positive)" `
        -Arguments @("insert","strategy",$n,$f) -ExpectedExit 0 -ExpectSubstr "SUCCESS" -TestName $n -Source $src | Out-Null
    Invoke-Cli -Arguments @("delete","strategy",$n) | Out-Null

    # =====================================================================
    # PART C: General error cases - 10 tests
    # =====================================================================
    Write-Host ""
    Write-Host "--- PART C: General error cases ---" -ForegroundColor Cyan

    # C1: Invalid syntax (should FAIL, exit 8)
    $n = "E2E_${runId}_C1"; $allTestNames += $n
    $src = "this is not valid easylanguage;"
    $f = Write-SourceFile "c1" $src
    Invoke-E2eTest -TestId "C1" -Part "C" -Type "strategy" -Step "invalid syntax" `
        -Arguments @("insert","strategy",$n,$f) -ExpectedExit 8 -TestName $n -Source $src | Out-Null
    Invoke-Cli -Arguments @("delete","strategy",$n) | Out-Null

    # C2.x: Modify nonexistent (exit 5) for all 4 types
    $src = "Inputs: int x(0);"
    $f = Write-SourceFile "c2_strat" $src
    Invoke-E2eTest -TestId "C2.1" -Part "C" -Type "strategy" -Step "modify nonexistent" `
        -Arguments @("modify","strategy","E2E_${runId}_NonExist",$f) -ExpectedExit 5 -ExpectSubstr "not found" -Source $src | Out-Null

    $src = "Plot1(Close, ""Test"");"
    $f = Write-SourceFile "c2_ind" $src
    Invoke-E2eTest -TestId "C2.2" -Part "C" -Type "indicator" -Step "modify nonexistent" `
        -Arguments @("modify","indicator","E2E_${runId}_NonExist",$f) -ExpectedExit 5 -ExpectSubstr "not found" -Source $src | Out-Null

    $src = "E2E_${runId}_NonExist = 1;"
    $f = Write-SourceFile "c2_func" $src
    Invoke-E2eTest -TestId "C2.3" -Part "C" -Type "function" -Step "modify nonexistent" `
        -Arguments @("modify","function","E2E_${runId}_NonExist",$f) -ExpectedExit 5 -ExpectSubstr "not found" -Source $src | Out-Null

    $src = "using elsystem;"
    $f = Write-SourceFile "c2_ta" $src
    Invoke-E2eTest -TestId "C2.4" -Part "C" -Type "tradingapp" -Step "modify nonexistent" `
        -Arguments @("modify","tradingapp","E2E_${runId}_NonExist",$f) -ExpectedExit 5 -ExpectSubstr "not found" -Source $src | Out-Null

    # C3.x: Delete nonexistent (exit 5) for all 4 types
    Invoke-E2eTest -TestId "C3.1" -Part "C" -Type "strategy" -Step "delete nonexistent" `
        -Arguments @("delete","strategy","E2E_${runId}_NonExist") -ExpectedExit 5 -ExpectSubstr "not found" | Out-Null

    Invoke-E2eTest -TestId "C3.2" -Part "C" -Type "indicator" -Step "delete nonexistent" `
        -Arguments @("delete","indicator","E2E_${runId}_NonExist") -ExpectedExit 5 -ExpectSubstr "not found" | Out-Null

    Invoke-E2eTest -TestId "C3.3" -Part "C" -Type "function" -Step "delete nonexistent" `
        -Arguments @("delete","function","E2E_${runId}_NonExist") -ExpectedExit 5 -ExpectSubstr "not found" | Out-Null

    Invoke-E2eTest -TestId "C3.4" -Part "C" -Type "tradingapp" -Step "delete nonexistent" `
        -Arguments @("delete","tradingapp","E2E_${runId}_NonExist") -ExpectedExit 5 -ExpectSubstr "not found" | Out-Null

    # C4: Unknown type string (exit 2)
    $src = "using elsystem;"
    $f = Write-SourceFile "c4" $src
    Invoke-E2eTest -TestId "C4" -Part "C" -Type "(any)" -Step "unknown type string" `
        -Arguments @("insert","paintbar","E2E_${runId}_C4",$f) -ExpectedExit 2 -ExpectSubstr "unknown type" -Source $src | Out-Null

}
finally {
    # =====================================================================
    # CLEANUP (always runs, even on failure)
    # =====================================================================
    $runStopwatch.Stop()
    $cleanupResult = Test-Cleanup -AllNames $allTestNames
}

# =====================================================================
# SUMMARY & REPORT
# =====================================================================
$failureCount = Print-E2eSummary -Results $Results
Write-MarkdownReport -Results $Results -CleanupResult $cleanupResult -ProgIdMap $ProgIdMap

Write-Host ""
if ($failureCount -eq 0 -and $cleanupResult.Ok) {
    Write-Host "ALL 34 E2E TESTS PASSED" -ForegroundColor Green
    exit 0
}
else {
    if ($failureCount -gt 0) {
        Write-Host "$failureCount TEST(S) FAILED" -ForegroundColor Red
    }
    if (-not $cleanupResult.Ok) {
        Write-Host "CLEANUP ISSUES DETECTED" -ForegroundColor Yellow
    }
    exit 1
}
