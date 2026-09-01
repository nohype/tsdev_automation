<#
.SYNOPSIS
    E2E CLI test for the `exists` subcommand of tsdev_cli (TVS-F.1).

.DESCRIPTION
    End-to-end test for the implemented `exists` subcommand that queries item
    existence by name + component type (exact-match).

    The `exists` op uses exit code 0 for BOTH found and not-found (not-found is
    a valid query result, not an error). The `exists` boolean in the JSON
    output distinguishes them. Exit code 2 is for unknown type errors only.
    The `exists` op NEVER emits exit 5 / not_found.

    Test cases:
      D1.1  exists strategy <name> --json  (existing item)
            - First inserts a minimal valid strategy.
            - Expects exit code 0, JSON with "exists": true and "prog_id" > 0.
      D1.2  exists strategy NonExistentNameXYZ --json  (nonexistent item)
            - Expects exit code 0, JSON with "exists": false.
      D1.3  exists strategy <name> --json  (after delete — should not exist)
            - Deletes the probe, then queries again.
            - Expects exit code 0, JSON with "exists": false.

    Cleanup: deletes the inserted probe item in a finally block.

    Prerequisites:
      - Direct `tsdev_cli` requires an elevated administrator process. Run this
        procedure elevated; otherwise, the CLI returns exit code 11
        (`not_elevated`).

    NOTE: This test invokes the current `tsdev_cli exists` implementation and
    reports failures from its actual exit code and output.

.PARAMETER CliPath
    Path to the tsdev_cli.exe executable. Defaults to
    "$PSScriptRoot\..\build\Release\tsdev_cli.exe".

.PARAMETER TsPath
    Optional. TradeStation install directory passed through to the CLI as
    "--ts-path <dir>" BEFORE the subcommand.

.PARAMETER Verbose
    Switch. When set, print full CLI output for every test.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\test_exists.ps1
    Runs the exists E2E tests with the default CLI path.

.NOTES
    Exit codes (returned by this script):
      0 = all tests passed
      1 = one or more tests failed
#>

param(
    [string]$CliPath = (Join-Path $PSScriptRoot "..\build\Release\tsdev_cli.exe"),
    [string]$TsPath,
    [switch]$Verbose
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Resolve paths and run ID.
# ---------------------------------------------------------------------------
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$resolved = Resolve-Path -Path $CliPath -ErrorAction SilentlyContinue
if ($resolved) {
    $CliPath = $resolved.Path
} elseif (-not (Test-Path -Path $CliPath -PathType Leaf)) {
    $CliPath = Join-Path $ProjectRoot "build\Release\tsdev_cli.exe"
}

$runId = Get-Date -Format "yyyyMMdd_HHmmss"
$tmpDir = Join-Path $ProjectRoot "tmp\e2e_exists_$runId"

# Results collector.
$Results = New-Object System.Collections.Generic.List[object]

# Track created component names for cleanup.
$CreatedNames = New-Object System.Collections.Generic.List[object]

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
# stdout and stderr SEPARATELY (stdout via the pipeline, stderr via 2>file).
# Returns a hashtable with Stdout (string), Stderr (string), ExitCode (int).
#
# Keeping stdout and stderr separate is essential: the library can emit
# level-gated diagnostics to stderr (default: warnings + errors only; -v/-vv
# add info/debug lines such as "tsdev: TS install path resolved ...") that
# are NOT part of the JSON result on stdout. Merging the streams with 2>&1
# would break ConvertFrom-Json. This matches the pattern used by the existing
# E2E tests (test_json_output.ps1 and test_batch_mode.ps1, Invoke-CliRaw).
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

    $stderrFile = Join-Path $script:tmpDir "stderr_$([System.Guid]::NewGuid().ToString('N')).txt"

    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        # Capture stdout through the pipeline; redirect stderr to a temp file.
        $stdout = & $script:CliPath @fullArgs 2>$stderrFile
        $exitCode = $LASTEXITCODE
    }
    catch {
        $stdout = $_.Exception.Message
        $exitCode = -2
    }
    finally {
        $ErrorActionPreference = $prevEAP
    }

    # Normalize stdout: pipeline may return an array of lines; join them.
    $stdoutStr = if ($stdout -is [array]) { ($stdout -join "`n") } else { "$stdout" }
    $stdoutStr = $stdoutStr.TrimEnd()

    $stderrStr = ""
    if (Test-Path -Path $stderrFile -PathType Leaf) {
        $stderrStr = ([System.IO.File]::ReadAllText($stderrFile)).TrimEnd()
        Remove-Item -Path $stderrFile -Force -ErrorAction SilentlyContinue
    }

    return @{
        Stdout   = $stdoutStr
        Stderr   = $stderrStr
        ExitCode = $exitCode
    }
}

# ---------------------------------------------------------------------------
# Invoke-Test: runs one CLI invocation, verifies the exit code and optional
# JSON field assertions, records the result, and prints PASS/FAIL.
# ---------------------------------------------------------------------------
function Invoke-Test {
    param(
        [Parameter(Mandatory)] [string]$TestId,
        [Parameter(Mandatory)] [string]$Step,
        [Parameter(Mandatory)] [string[]]$Arguments,
        [Parameter(Mandatory)] [int]$ExpectedExit,
        [string]$TestName,
        [string[]]$ExpectSubstr,
        [scriptblock]$JsonAssert
    )

    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $cli = Invoke-Cli -Arguments $Arguments
    $stopwatch.Stop()

    $actualExit = $cli.ExitCode
    $output = $cli.Stdout

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

    # Verify JSON assertions (if provided).
    $jsonOk = $true
    $jsonErrors = @()
    if ($JsonAssert) {
        try {
            $jsonObj = $output | ConvertFrom-Json -ErrorAction Stop
            $assertResult = & $JsonAssert $jsonObj
            if ($assertResult -is [bool] -and -not $assertResult) {
                $jsonOk = $false
                $jsonErrors += "JSON assertion returned false"
            } elseif ($assertResult -is [string]) {
                $jsonOk = $false
                $jsonErrors += $assertResult
            }
        } catch {
            $jsonOk = $false
            $jsonErrors += "JSON parse failed: $($_.Exception.Message)"
        }
    }

    $passed = ($exitOk -and $substrOk -and $jsonOk)
    $status = if ($passed) { 'PASS' } else { 'FAIL' }
    $color = if ($passed) { 'Green' } else { 'Red' }

    $durationStr = "{0:0.00}s" -f $stopwatch.Elapsed.TotalSeconds
    Write-Host ("[{0}] {1}: exit={2} expected={3} [{4}]" -f `
        $status, $TestId, $actualExit, $ExpectedExit, $durationStr) `
        -ForegroundColor $color

    if (-not $passed -or $Verbose) {
        $preview = ($output -split "`n" | Where-Object {
            $_.Trim() -ne '' -and $_ -notmatch 'TS install path resolved'
        } | Select-Object -First 4) -join ' | '
        if ($preview.Length -gt 200) { $preview = $preview.Substring(0, 200) + '...' }
        if ($output) {
            Write-Host "  => $preview" -ForegroundColor DarkGray
        }
        if (-not $exitOk) {
            Write-Host "  EXIT MISMATCH: expected $ExpectedExit, got $actualExit" -ForegroundColor Red
        }
        if ($ExpectSubstr -and -not $substrOk) {
            Write-Host "  SUBSTR NOT FOUND: expected '$($missingSubstrs -join "', '")'" -ForegroundColor Red
        }
        if ($jsonErrors.Count -gt 0) {
            foreach ($e in $jsonErrors) {
                Write-Host "  JSON ASSERT FAILED: $e" -ForegroundColor Red
            }
        }
    }

    # Track created component names for cleanup (insert creates entries).
    if ($TestName -and $Arguments[0] -eq 'insert') {
        if (-not $CreatedNames.Contains($TestName)) {
            $CreatedNames.Add($TestName) | Out-Null
        }
    }

    $result = [pscustomobject]@{
        TestId       = $TestId
        Step         = $Step
        Arguments    = ($Arguments -join ' ')
        ExpectedExit = $ExpectedExit
        ActualExit   = $actualExit
        Result       = $status
        Output       = $output
        Duration     = $stopwatch.Elapsed
        TestName     = $TestName
    }
    $Results.Add($result) | Out-Null
    return $result
}

# ---------------------------------------------------------------------------
# Print-Summary: prints a summary table and returns the count of failures.
# ---------------------------------------------------------------------------
function Print-Summary {
    param($Results)

    Write-Host ""
    Write-Host "==================== EXISTS E2E SUMMARY ====================" -ForegroundColor Cyan
    $header = "{0,-8} {1,-30} {2,-14} {3,-14} {4,-8}" -f `
        "Test#", "Step", "Expected Exit", "Actual Exit", "Result"
    Write-Host $header
    Write-Host ("{0,-8} {1,-30} {2,-14} {3,-14} {4,-8}" -f `
        ("-" * 8), ("-" * 30), ("-" * 14), ("-" * 14), ("-" * 8))

    $passed = 0
    $failed = 0
    $idx = 0
    foreach ($r in $Results) {
        $idx++
        $line = "{0,-8} {1,-30} {2,-14} {3,-14} {4,-8}" -f `
            $idx, $r.Step, $r.ExpectedExit, $r.ActualExit, $r.Result
        $color = if ($r.Result -eq 'PASS') { 'Green' } else { 'Red' }
        Write-Host $line -ForegroundColor $color
        if ($r.Result -eq 'PASS') { $passed++ } else { $failed++ }
    }

    Write-Host ""
    Write-Host ("Totals: {0}/{1} passed, {2} failed" -f $passed, $Results.Count, $failed) -ForegroundColor Cyan
    return $failed
}

# ===========================================================================
# MAIN EXECUTION
# ===========================================================================

Write-Host "============================================" -ForegroundColor Cyan
Write-Host "TSDev Automation - exists E2E Test (TVS-F.1)" -ForegroundColor Cyan
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

# Unique probe name for this run.
$probeName = "E2E_${runId}_ExistsProbe"

# Minimal valid EasyLanguage strategy source. The leading "{ name }" text is
# arbitrary fixture source content only; it is not a metadata convention or a
# lookup mechanism. The CLI supplies the item name, and stored metadata comes
# from orlib20 warehouse records/APIs.
$probeSource = "{ $probeName }`r`nInputs: int x(0);`r`nif x > 0 then Buy next bar at market;"

try {
    # =====================================================================
    # D1.1: Insert a probe strategy, then query exists (should be found)
    # =====================================================================
    Write-Host "--- D1.1: exists (existing item) ---" -ForegroundColor Cyan

    # Insert the probe strategy first.
    $f = Write-SourceFile "probe_ins" $probeSource
    Invoke-Test -TestId "D1.1a" -Step "insert probe strategy" `
        -Arguments @("insert","strategy",$probeName,$f) `
        -ExpectedExit 0 -ExpectSubstr "SUCCESS" -TestName $probeName | Out-Null

    # Query exists for the existing item — expect exit 0, JSON exists=true, prog_id > 0.
    Invoke-Test -TestId "D1.1b" -Step "exists (existing item)" `
        -Arguments @("--json","exists","strategy",$probeName) `
        -ExpectedExit 0 -TestName $probeName `
        -JsonAssert {
            param($j)
            if (-not $j.exists) { return "exists field is false (expected true)" }
            if (-not $j.prog_id -or $j.prog_id -le 0) { return "prog_id missing or <= 0" }
            return $true
        } | Out-Null

    # =====================================================================
    # D1.2: Query exists for a nonexistent name (should not be found)
    # =====================================================================
    Write-Host "--- D1.2: exists (nonexistent item) ---" -ForegroundColor Cyan

    Invoke-Test -TestId "D1.2" -Step "exists (nonexistent item)" `
        -Arguments @("--json","exists","strategy","NonExistentNameXYZ") `
        -ExpectedExit 0 `
        -JsonAssert {
            param($j)
            if ($j.exists) { return "exists field is true (expected false)" }
            if ($j.prog_id -and $j.prog_id -gt 0) { return "prog_id present for nonexistent item" }
            return $true
        } | Out-Null

    # =====================================================================
    # D1.3: Delete the probe, then query exists again (should not be found)
    # =====================================================================
    Write-Host "--- D1.3: exists (after delete) ---" -ForegroundColor Cyan

    # Delete the probe.
    Invoke-Test -TestId "D1.3a" -Step "delete probe strategy" `
        -Arguments @("delete","strategy",$probeName) `
        -ExpectedExit 0 | Out-Null

    # Query exists for the deleted item — expect exit 0, JSON exists=false.
    Invoke-Test -TestId "D1.3b" -Step "exists (after delete)" `
        -Arguments @("--json","exists","strategy",$probeName) `
        -ExpectedExit 0 `
        -JsonAssert {
            param($j)
            if ($j.exists) { return "exists field is true (expected false after delete)" }
            if ($j.prog_id -and $j.prog_id -gt 0) { return "prog_id present for deleted item" }
            return $true
        } | Out-Null

}
finally {
    # =====================================================================
    # CLEANUP (always runs, even on failure)
    # =====================================================================
    Write-Host ""
    Write-Host "===== CLEANUP =====" -ForegroundColor Cyan

    foreach ($name in $CreatedNames) {
        $cli = Invoke-Cli -Arguments @("delete","strategy",$name)
        if ($cli.ExitCode -eq 0) {
            Write-Host "  Cleaned up: $name" -ForegroundColor DarkGray
        }
    }

    # Remove temp directory.
    if (Test-Path -Path $tmpDir -PathType Container) {
        Remove-Item -Path $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# ===========================================================================
# SUMMARY
# ===========================================================================
$failureCount = Print-Summary -Results $Results

Write-Host ""
if ($failureCount -eq 0) {
    Write-Host "ALL EXISTS E2E TESTS PASSED" -ForegroundColor Green
    exit 0
}
else {
    Write-Host "$failureCount TEST(S) FAILED" -ForegroundColor Red
    exit 1
}
