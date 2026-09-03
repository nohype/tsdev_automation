<#
.SYNOPSIS
    E2E CLI test for the `get-source` subcommand of tsdev_cli (TVS-F.4).

.DESCRIPTION
    End-to-end test for the implemented `get-source` subcommand that retrieves
    EasyLanguage source code by name + component type.

    The `get-source` op retrieves the raw EL source text of an existing item.
    Exit codes:
      0 = success (source retrieved)
      5 = not_found (item does not exist — an error for get-source, unlike exists)
      2 = unknown_type (unrecognized component type string)
      1 = usage_error (missing/incorrect arguments)

    Output modes:
      - Raw mode (default): prints the EL source text to stdout.
      - --output <file>: writes the EL source directly to the specified file.
      - --json: emits a JSON object with `success` (bool), `source` (string),
        and on failure an `error` object with `code` and `message` fields.

    Test cases:
      GS1.  get-source strategy <name>  (existing item, raw output)
            - First inserts a minimal valid strategy with known source.
            - Expects exit 0, stdout contains key substrings from the source.
      GS2.  --json get-source strategy <name>  (existing item, JSON output)
            - Expects exit 0, JSON success=true, source field contains source.
      GS3.  get-source strategy <name> --output <temp_file>  (file output)
            - Expects exit 0, the temp file contains key substrings from source.
      GS4.  get-source strategy NonExistentName  (nonexistent item)
            - Expects exit 5 (not_found), NOT exit 0.
      GS5.  get-source  (no args)
            - Expects exit 1 (usage error).
      GS6.  get-source badtype somename  (unknown type)
            - Expects exit 2 (unknown type).

    Cleanup: deletes the inserted probe item in a finally block.

    Prerequisites:
      - Direct `tsdev_cli` requires an elevated administrator process. Run this
        procedure elevated; otherwise, the CLI returns exit code 11
        (`not_elevated`).

    NOTE: This test invokes the current `tsdev_cli get-source` implementation
    and reports failures from its actual exit code and output.

.PARAMETER CliPath
    Path to the tsdev_cli.exe executable. Defaults to
    "$PSScriptRoot\..\build\Release\tsdev_cli.exe".

.PARAMETER TsPath
    Optional. TradeStation install directory passed through to the CLI as
    "--ts-path <dir>" BEFORE the subcommand.

.PARAMETER Verbose
    Switch. When set, print full CLI output for every test.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\test_get_source.ps1
    Runs the get-source E2E tests with the default CLI path.

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
$tmpDir = Join-Path $ProjectRoot "tmp\e2e_get_source_$runId"

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

    # Track created component names for cleanup (insert creates entries).
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
    Write-Host "==================== GET-SOURCE E2E SUMMARY ====================" -ForegroundColor Cyan
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
Write-Host "TSDev Automation - get-source E2E Test (TVS-F.4)" -ForegroundColor Cyan
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
$probeName = "E2E_GetSrc_${runId}_GS1"

# Minimal valid EasyLanguage strategy source (compiles successfully).
# Key substrings used for verification: "Buy" and "EnterLong".
$strategySrc = "Inputs: int x(0);`r`nif x > 0 then Buy (""EnterLong"") next bar at market;"

try {
    # =====================================================================
    # GS1: Insert a probe strategy, then get-source (raw output)
    # =====================================================================
    Write-Host "--- GS1: get-source (existing item, raw output) ---" -ForegroundColor Cyan

    # Insert the probe strategy first.
    $fGS1 = Write-SourceFile "gs1_src" $strategySrc
    Invoke-Test -TestId "GS1.prep" -Step "insert probe strategy" `
        -Arguments @("insert","strategy",$probeName,$fGS1) `
        -ExpectedExit 0 -ExpectSubstr "SUCCESS" -TestName $probeName -TestType "strategy" | Out-Null

    # get-source for the existing item — expect exit 0, stdout contains key
    # substrings from the source.
    Invoke-Test -TestId "GS1" -Step "get-source (existing item, raw)" `
        -Arguments @("get-source","strategy",$probeName) `
        -ExpectedExit 0 -TestName $probeName -TestType "strategy" `
        -ExpectSubstr @("Buy", "EnterLong") | Out-Null

    # =====================================================================
    # GS2: get-source with --json (existing item, JSON output)
    # =====================================================================
    Write-Host "--- GS2: get-source (existing item, JSON output) ---" -ForegroundColor Cyan

    Invoke-Test -TestId "GS2" -Step "get-source (existing item, JSON)" `
        -Arguments @("--json","get-source","strategy",$probeName) `
        -ExpectedExit 0 -TestName $probeName -TestType "strategy" `
        -JsonAssert {
            param($j)
            if (-not $j.success) { return "success field is false (expected true)" }
            if (-not $j.source) { return "source field is missing or empty" }
            if (-not $j.source.Contains("Buy")) { return "source field does not contain 'Buy'" }
            if (-not $j.source.Contains("EnterLong")) { return "source field does not contain 'EnterLong'" }
            return $true
        } | Out-Null

    # =====================================================================
    # GS3: get-source with --output <file> (file output)
    # =====================================================================
    Write-Host "--- GS3: get-source (existing item, file output) ---" -ForegroundColor Cyan

    $outputFile = Join-Path $tmpDir "gs3_output.txt"
    Invoke-Test -TestId "GS3" -Step "get-source (existing item, --output file)" `
        -Arguments @("get-source","strategy",$probeName,"--output",$outputFile) `
        -ExpectedExit 0 -TestName $probeName -TestType "strategy" | Out-Null

    # Verify the output file was created and contains the source text.
    $fileOk = $false
    if (Test-Path -Path $outputFile -PathType Leaf) {
        $fileContent = [System.IO.File]::ReadAllText($outputFile)
        if ($fileContent.Contains("Buy") -and $fileContent.Contains("EnterLong")) {
            $fileOk = $true
        }
    }

    # Record a supplementary check for the file content.
    $gs3FileStatus = if ($fileOk) { 'PASS' } else { 'FAIL' }
    $gs3FileColor = if ($fileOk) { 'Green' } else { 'Red' }
    Write-Host ("[{0}] GS3.file: output file contains source [{1}]" -f `
        $gs3FileStatus, "0.00s") -ForegroundColor $gs3FileColor
    if (-not $fileOk) {
        Write-Host "  OUTPUT FILE CHECK FAILED: expected file to contain 'Buy' and 'EnterLong'" -ForegroundColor Red
    }
    $Results.Add([pscustomobject]@{
        TestId       = "GS3.file"
        Step         = "get-source (output file content check)"
        Arguments    = "(file: $outputFile)"
        ExpectedExit = 0
        ActualExit   = if ($fileOk) { 0 } else { 1 }
        Result       = $gs3FileStatus
        Output       = if ($fileOk) { "file contains source" } else { "file missing or source not found" }
        Duration     = [timespan]::Zero
        TestName     = $null
        TestType     = $null
    }) | Out-Null

    # =====================================================================
    # GS4: get-source for a nonexistent name (expect exit 5 = not_found)
    # =====================================================================
    Write-Host "--- GS4: get-source (nonexistent item) ---" -ForegroundColor Cyan

    Invoke-Test -TestId "GS4" -Step "get-source (nonexistent item)" `
        -Arguments @("get-source","strategy","NonExistentName_GetSrcTest") `
        -ExpectedExit 5 | Out-Null

    # =====================================================================
    # GS5: get-source with no args (expect exit 1 = usage error)
    # =====================================================================
    Write-Host "--- GS5: get-source (no args, usage error) ---" -ForegroundColor Cyan

    Invoke-Test -TestId "GS5" -Step "get-source (no args)" `
        -Arguments @("get-source") `
        -ExpectedExit 1 | Out-Null

    # =====================================================================
    # GS6: get-source with unknown type (expect exit 2 = unknown_type)
    # =====================================================================
    Write-Host "--- GS6: get-source (unknown type) ---" -ForegroundColor Cyan

    Invoke-Test -TestId "GS6" -Step "get-source (unknown type)" `
        -Arguments @("get-source","badtype","somename") `
        -ExpectedExit 2 | Out-Null

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
    Write-Host "ALL GET-SOURCE E2E TESTS PASSED" -ForegroundColor Green
    exit 0
}
else {
    Write-Host "$failureCount TEST(S) FAILED" -ForegroundColor Red
    exit 1
}
