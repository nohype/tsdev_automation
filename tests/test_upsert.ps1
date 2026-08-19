<#
.SYNOPSIS
    E2E CLI test for the `upsert` subcommand of tsdev_cli (TVS-F.3).

.DESCRIPTION
    End-to-end test for the `upsert` subcommand that inserts an item if it
    does not exist, or modifies it if it does (insert-or-update by name +
    component type + source file). The `upsert` subcommand is now
    implemented in tsdev_cli.cpp; these tests verify its behavior (GREEN state).

    The `upsert` op calls findExistPos first, then branches to insert (absent)
    or modify (present). Exit codes:
      0 = success (insert or modify succeeded)
      8 = compile error
      5 = not found (should not happen for upsert - it inserts if absent)
      2 = unknown type string

    The `upsert` op supports the `--json` output mode, emitting a JSON object
    with `success` (bool), `prog_id` (int, > 0 on success), and on failure an
    `error` object with `code` and `message` fields.

    Test cases:
      U1.   upsert strategy <new-name> <valid-source> --json  (absent -> insert)
            - Upserts a NEW strategy with valid source.
            - Expects exit 0, JSON success=true, prog_id > 0.
            - Verifies via `exists` that the item now exists with the same prog_id.
      U2.   upsert strategy <existing-name> <modified-source> --json  (present -> modify)
            - First inserts a strategy, capturing its ProgId.
            - Upserts the SAME name with MODIFIED source.
            - Expects exit 0, JSON success=true, prog_id == original ProgId (preserved).
            - Verifies via `exists` that the item is still there with the same prog_id.
      U3.   upsert bogustype <name> <source> --json  (unknown type)
            - Expects exit 2, JSON success=false, error.code == "unknown_type".
      U4.   upsert strategy <name> <invalid-source> --json  (compile error)
            - Upserts a strategy with INVALID EasyLanguage source (syntax error).
            - Expects exit 8, JSON success=false, error.code == "compile_error".
            - Cleanup: deletes any partial entry that may have been created.
      U5.   upsert function <new-name> <valid-source> --json  (absent -> insert, function type)
            - Upserts a NEW function with valid source.
            - Expects exit 0, JSON success=true, prog_id > 0.
            - Verifies via `exists` that the item now exists.

    Cleanup: deletes all inserted/upserted probe items in a finally block.

.PARAMETER CliPath
    Path to the tsdev_cli.exe executable. Defaults to
    "$PSScriptRoot\..\build\Release\tsdev_cli.exe".

.PARAMETER TsPath
    Optional. TradeStation install directory passed through to the CLI as
    "--ts-path <dir>" BEFORE the subcommand.

.PARAMETER Verbose
    Switch. When set, print full CLI output for every test.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\test_upsert.ps1
    Runs the upsert E2E tests with the default CLI path.

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
$tmpDir = Join-Path $ProjectRoot "tmp\e2e_upsert_$runId"

# Results collector.
$Results = New-Object System.Collections.Generic.List[object]

# Track created component names + types for cleanup. Each entry is a hashtable
# with Name and Type keys.
$CreatedItems = New-Object System.Collections.Generic.List[object]

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
# Keeping stdout and stderr separate is essential: the library emits a
# diagnostic line to stderr ("tsdev: TS install path resolved ...") that is
# NOT part of the JSON result on stdout. Merging the streams with 2>&1 would
# break ConvertFrom-Json. This matches the pattern used by the existing E2E
# tests (test_json_output.ps1 and test_batch_mode.ps1, Invoke-CliRaw).
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
        [string]$TestType,
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

    # Track created component names for cleanup (insert/upsert create entries).
    if ($TestName -and $TestType -and $Arguments[0] -in @('insert', 'upsert')) {
        $already = $false
        foreach ($item in $CreatedItems) {
            if ($item.Name -eq $TestName -and $item.Type -eq $TestType) {
                $already = $true
                break
            }
        }
        if (-not $already) {
            $CreatedItems.Add(@{ Name = $TestName; Type = $TestType }) | Out-Null
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
        TestType     = $TestType
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
    Write-Host "==================== UPSERT E2E SUMMARY ====================" -ForegroundColor Cyan
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
Write-Host "TSDev Automation - upsert E2E Test (TVS-F.3)" -ForegroundColor Cyan
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

# Unique probe names for this run.
$probeNameU1   = "E2E_UpsertTest_${runId}_U1"
$probeNameU2   = "E2E_UpsertTest_${runId}_U2"
$probeNameU3   = "E2E_UpsertTest_${runId}_U3"
$probeNameU4   = "E2E_UpsertTest_${runId}_U4"
$probeNameU5   = "E2E_UpsertTest_${runId}_U5"

# Minimal valid EasyLanguage strategy source (compiles successfully).
# The leading "{ name }" comment block is the EasyLanguage convention for
# naming a strategy/function; it is required for enumerateRecords to
# extract the verbatim stored name.
function Write-NamedSource {
    param(
        [Parameter(Mandatory)] [string]$FileName,
        [Parameter(Mandatory)] [string]$ProbeName,
        [string]$Body = "Inputs: int x(0);`r`nif x > 0 then Buy next bar at market;"
    )
    $source = "{ $ProbeName }`r`n$Body"
    return Write-SourceFile $FileName $source
}

# Modified strategy source body (different body, still valid).
$strategySrcModBody = "Inputs: int x(1);`r`nif x > 0 then Sell next bar at market;"

# Invalid EasyLanguage source (compile error - syntax error).
$badSrc = "this is not valid easylanguage;"

try {
    # =====================================================================
    # U1: Upsert when absent (insert path) - strategy
    # =====================================================================
    Write-Host "--- U1: upsert (absent -> insert) ---" -ForegroundColor Cyan

    $fU1 = Write-NamedSource "u1_src" $probeNameU1

    # Upsert a NEW strategy - expect exit 0, JSON success=true, prog_id > 0.
    $rU1 = Invoke-Test -TestId "U1" -Step "upsert (absent -> insert)" `
        -Arguments @("--json","upsert","strategy",$probeNameU1,$fU1) `
        -ExpectedExit 0 -TestName $probeNameU1 -TestType "strategy" `
        -JsonAssert {
            param($j)
            if (-not $j.success) { return "success field is false (expected true)" }
            if (-not $j.prog_id -or $j.prog_id -le 0) { return "prog_id missing or <= 0" }
            return $true
        }

    # Extract the prog_id from the upsert result for the exists verification.
    $upsertProgIdU1 = $null
    try {
        $jU1 = $rU1.Output | ConvertFrom-Json -ErrorAction Stop
        $upsertProgIdU1 = $jU1.prog_id
    } catch {
        $upsertProgIdU1 = $null
    }

    # Verify via exists that the item now exists with the same prog_id.
    Invoke-Test -TestId "U1.verify" -Step "exists (verify after upsert insert)" `
        -Arguments @("--json","exists","strategy",$probeNameU1) `
        -ExpectedExit 0 -TestName $probeNameU1 -TestType "strategy" `
        -JsonAssert {
            param($j)
            if (-not $j.exists) { return "exists field is false (expected true after upsert insert)" }
            if (-not $j.prog_id -or $j.prog_id -le 0) { return "prog_id missing or <= 0" }
            if ($upsertProgIdU1 -and $j.prog_id -ne $upsertProgIdU1) {
                return "prog_id mismatch: upsert returned $upsertProgIdU1, exists returned $($j.prog_id)"
            }
            return $true
        } | Out-Null

    # =====================================================================
    # U2: Upsert when present (modify path) - strategy, ProgId preserved
    # =====================================================================
    Write-Host "--- U2: upsert (present -> modify) ---" -ForegroundColor Cyan

    # First, insert a strategy via the insert subcommand, capturing its ProgId.
    $fU2Init = Write-NamedSource "u2_init" $probeNameU2
    $rU2Ins = Invoke-Test -TestId "U2.prep" -Step "insert (prep for upsert modify)" `
        -Arguments @("insert","strategy",$probeNameU2,$fU2Init) `
        -ExpectedExit 0 -ExpectSubstr "SUCCESS" -TestName $probeNameU2 -TestType "strategy"

    # Extract the ProgId from the insert output for the preservation check.
    $insertProgIdU2 = $null
    if ($rU2Ins.Output -match 'ProgId=(\d+)') {
        $insertProgIdU2 = [int]$Matches[1]
    }

    # Upsert the SAME strategy name with MODIFIED source - expect exit 0,
    # JSON success=true, prog_id == original insert ProgId (preserved).
    $fU2Mod = Write-NamedSource "u2_mod" $probeNameU2 $strategySrcModBody
    Invoke-Test -TestId "U2" -Step "upsert (present -> modify)" `
        -Arguments @("--json","upsert","strategy",$probeNameU2,$fU2Mod) `
        -ExpectedExit 0 -TestName $probeNameU2 -TestType "strategy" `
        -JsonAssert {
            param($j)
            if (-not $j.success) { return "success field is false (expected true)" }
            if (-not $j.prog_id -or $j.prog_id -le 0) { return "prog_id missing or <= 0" }
            if ($insertProgIdU2 -and $j.prog_id -ne $insertProgIdU2) {
                return "prog_id NOT preserved: insert was $insertProgIdU2, upsert modify returned $($j.prog_id)"
            }
            return $true
        } | Out-Null

    # Verify via exists that the item is still there with the same prog_id.
    Invoke-Test -TestId "U2.verify" -Step "exists (verify after upsert modify)" `
        -Arguments @("--json","exists","strategy",$probeNameU2) `
        -ExpectedExit 0 -TestName $probeNameU2 -TestType "strategy" `
        -JsonAssert {
            param($j)
            if (-not $j.exists) { return "exists field is false (expected true after upsert modify)" }
            if (-not $j.prog_id -or $j.prog_id -le 0) { return "prog_id missing or <= 0" }
            if ($insertProgIdU2 -and $j.prog_id -ne $insertProgIdU2) {
                return "prog_id mismatch: insert was $insertProgIdU2, exists returned $($j.prog_id)"
            }
            return $true
        } | Out-Null

    # =====================================================================
    # U3: Upsert with unknown type
    # =====================================================================
    Write-Host "--- U3: upsert (unknown type) ---" -ForegroundColor Cyan

    $fU3 = Write-NamedSource "u3_src" $probeNameU3
    Invoke-Test -TestId "U3" -Step "upsert (unknown type)" `
        -Arguments @("--json","upsert","bogustype",$probeNameU3,$fU3) `
        -ExpectedExit 2 -TestName $probeNameU3 -TestType "bogustype" `
        -JsonAssert {
            param($j)
            if ($j.success) { return "success field is true (expected false for unknown type)" }
            if (-not $j.error -or -not $j.error.code) { return "error.code missing" }
            if ($j.error.code -ne 'unknown_type') { return "error.code: expected 'unknown_type', got '$($j.error.code)'" }
            return $true
        } | Out-Null

    # =====================================================================
    # U4: Upsert with compile error
    # =====================================================================
    Write-Host "--- U4: upsert (compile error) ---" -ForegroundColor Cyan

    $fU4 = Write-SourceFile "u4_badsrc" $badSrc
    Invoke-Test -TestId "U4" -Step "upsert (compile error)" `
        -Arguments @("--json","upsert","strategy",$probeNameU4,$fU4) `
        -ExpectedExit 8 -TestName $probeNameU4 -TestType "strategy" `
        -JsonAssert {
            param($j)
            if ($j.success) { return "success field is true (expected false for compile error)" }
            if (-not $j.error -or -not $j.error.code) { return "error.code missing" }
            if ($j.error.code -ne 'compile_error') { return "error.code: expected 'compile_error', got '$($j.error.code)'" }
            return $true
        } | Out-Null

    # =====================================================================
    # U5: Upsert across types - function (absent -> insert)
    # =====================================================================
    Write-Host "--- U5: upsert (absent -> insert, function) ---" -ForegroundColor Cyan

    # Function source: the function name must match the component name.
    # Include the EasyLanguage "{ name }" comment block for enumerateRecords.
    $funcSrc = "{ $probeNameU5 }`r`n$probeNameU5 = 1;"
    $fU5 = Write-SourceFile "u5_func" $funcSrc

    # Upsert a NEW function - expect exit 0, JSON success=true, prog_id > 0.
    $rU5 = Invoke-Test -TestId "U5" -Step "upsert (absent -> insert, function)" `
        -Arguments @("--json","upsert","function",$probeNameU5,$fU5) `
        -ExpectedExit 0 -TestName $probeNameU5 -TestType "function" `
        -JsonAssert {
            param($j)
            if (-not $j.success) { return "success field is false (expected true)" }
            if (-not $j.prog_id -or $j.prog_id -le 0) { return "prog_id missing or <= 0" }
            return $true
        }

    # Extract the prog_id from the upsert result for the exists verification.
    $upsertProgIdU5 = $null
    try {
        $jU5 = $rU5.Output | ConvertFrom-Json -ErrorAction Stop
        $upsertProgIdU5 = $jU5.prog_id
    } catch {
        $upsertProgIdU5 = $null
    }

    # Verify via exists that the function now exists with the same prog_id.
    Invoke-Test -TestId "U5.verify" -Step "exists (verify after upsert insert, function)" `
        -Arguments @("--json","exists","function",$probeNameU5) `
        -ExpectedExit 0 -TestName $probeNameU5 -TestType "function" `
        -JsonAssert {
            param($j)
            if (-not $j.exists) { return "exists field is false (expected true after upsert insert)" }
            if (-not $j.prog_id -or $j.prog_id -le 0) { return "prog_id missing or <= 0" }
            if ($upsertProgIdU5 -and $j.prog_id -ne $upsertProgIdU5) {
                return "prog_id mismatch: upsert returned $upsertProgIdU5, exists returned $($j.prog_id)"
            }
            return $true
        } | Out-Null

}
finally {
    # =====================================================================
    # CLEANUP (always runs, even on failure)
    # =====================================================================
    Write-Host ""
    Write-Host "===== CLEANUP =====" -ForegroundColor Cyan

    foreach ($item in $CreatedItems) {
        $cli = Invoke-Cli -Arguments @("delete",$item.Type,$item.Name)
        if ($cli.ExitCode -eq 0) {
            Write-Host "  Cleaned up: $($item.Type) '$($item.Name)'" -ForegroundColor DarkGray
        } elseif ($cli.ExitCode -eq 5) {
            Write-Host "  Already gone: $($item.Type) '$($item.Name)' (exit 5)" -ForegroundColor DarkGray
        } else {
            Write-Host "  WARN: delete $($item.Type) '$($item.Name)' returned exit $($cli.ExitCode)" -ForegroundColor Yellow
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
    Write-Host "ALL UPSERT E2E TESTS PASSED" -ForegroundColor Green
    exit 0
}
else {
    Write-Host "$failureCount TEST(S) FAILED" -ForegroundColor Red
    Write-Host "(One or more upsert E2E tests failed.)" -ForegroundColor DarkYellow
    exit 1
}
