<#
.SYNOPSIS
    E2E CLI test for the ShowMe component type of tsdev_cli.

.DESCRIPTION
    End-to-end test for the ShowMe component type (whType 1282, category 2,
    subtype 0x05). ShowMe studies mark conditions on charts. They CAN use
    Plot statements (like Indicators) but CANNOT use Buy/Sell order
    statements (those are Strategy-only).

    This test exercises the full insert -> exists -> get-source -> modify ->
    get-source -> semantic-error -> delete -> exists lifecycle for a ShowMe
    item, using --json for structured output on every CLI call.

    Test sequence:
      SM1.  insert showme <name> <src>            (valid EL with Plot1)
            - Expects exit 0, JSON success=true, prog_id > 0.
      SM2.  exists showme <name> --json           (exact match)
            - Expects exit 0, JSON exists=true, stored_name == <name>.
      SM3.  exists showme <NAME_LOWER> --case-insensitive --json
            - Expects exit 0, JSON exists=true, stored_name == <name> (original case).
      SM4.  get-source showme <name> --json       (retrieve source)
            - Expects exit 0, JSON source contains "Plot1" and "Up bar".
      SM5.  modify showme <name> <new_src>        (replace source)
            - Expects exit 0, JSON success=true, prog_id preserved.
      SM6.  get-source showme <name> --json       (after modify)
            - Expects exit 0, JSON source contains "Down bar".
      SM7.  insert showme <invalid_name> <bad_src>  (Buy order statement)
            - Expects exit 8 (compile error). Order statements are not
              allowed in ShowMe studies ("This word not allowed in a study.").
      SM8.  delete showme <name>                  (delete the ShowMe)
            - Expects exit 0.
      SM9.  exists showme <name> --json           (after delete)
            - Expects exit 0, JSON exists=false.

    Cleanup: deletes any remaining test items and removes temp files in a
    finally block (always runs, even on failure).

    Prerequisites:
      - Run this procedure from an elevated administrator process because it
        invokes `tsdev_cli` directly. Otherwise, the CLI returns exit code 11
        (`not_elevated`).

.PARAMETER CliPath
    Path to the tsdev_cli.exe executable. Defaults to
    "$PSScriptRoot\..\build\Release\tsdev_cli.exe".

.PARAMETER TsPath
    Optional. TradeStation install directory passed through to the CLI as
    "--ts-path <dir>" BEFORE the subcommand.

.PARAMETER Verbose
    Switch. When set, print full CLI output for every test.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\test_showme_e2e.ps1
    Runs the ShowMe E2E tests with the default CLI path.

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
$tmpDir = Join-Path $ProjectRoot "tmp\e2e_showme_$runId"

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
        [string[]]$ExpectStderrSubstr,
        [scriptblock]$JsonAssert
    )

    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $cli = Invoke-Cli -Arguments $Arguments
    $stopwatch.Stop()

    $actualExit = $cli.ExitCode
    $output = $cli.Stdout
    $stderrOut = $cli.Stderr

    # Verify exit code.
    $exitOk = ($actualExit -eq $ExpectedExit)

    # Verify stdout substring(s). ExpectSubstr may contain one or more required
    # substrings; ALL must be present in stdout for the check to pass.
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

    # Verify stderr substring(s) (used for compile-error message checks).
    $stderrSubstrOk = $true
    $missingStderrSubstrs = @()
    if ($ExpectStderrSubstr) {
        foreach ($s in $ExpectStderrSubstr) {
            if (-not $stderrOut.Contains($s)) {
                $stderrSubstrOk = $false
                $missingStderrSubstrs += $s
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

    $passed = ($exitOk -and $substrOk -and $stderrSubstrOk -and $jsonOk)
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
            Write-Host "  SUBSTR NOT FOUND (stdout): expected '$($missingSubstrs -join "', '")'" -ForegroundColor Red
        }
        if ($ExpectStderrSubstr -and -not $stderrSubstrOk) {
            Write-Host "  SUBSTR NOT FOUND (stderr): expected '$($missingStderrSubstrs -join "', '")'" -ForegroundColor Red
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
    Write-Host "==================== SHOWME E2E SUMMARY ====================" -ForegroundColor Cyan
    $header = "{0,-8} {1,-40} {2,-14} {3,-14} {4,-8}" -f `
        "Test#", "Step", "Expected Exit", "Actual Exit", "Result"
    Write-Host $header
    Write-Host ("{0,-8} {1,-40} {2,-14} {3,-14} {4,-8}" -f `
        ("-" * 8), ("-" * 40), ("-" * 14), ("-" * 14), ("-" * 8))

    $passed = 0
    $failed = 0
    $idx = 0
    foreach ($r in $Results) {
        $idx++
        $line = "{0,-8} {1,-40} {2,-14} {3,-14} {4,-8}" -f `
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
Write-Host "TSDev Automation - ShowMe E2E Test" -ForegroundColor Cyan
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

# Unique test names for this run (PID suffix avoids collisions across runs).
$probeName    = "TestShowMe_K1_$PID"
$invalidName  = "TestShowMe_K1_Invalid_$PID"

# Valid ShowMe EasyLanguage source (uses Plot1 — allowed in ShowMe studies).
# ShowMe studies mark conditions on charts; they can plot but cannot issue
# order statements (Buy/Sell are Strategy-only).
$showmeInsertSrc = @"
{ TestShowMe TVS-K1 }
if Close > Open then
    Plot1(High, "Up bar");
"@

# Modified ShowMe EasyLanguage source (still uses Plot1).
$showmeModifySrc = @"
{ TestShowMe TVS-K1 modified }
if Close < Open then
    Plot1(Low, "Down bar");
"@

# Invalid ShowMe EasyLanguage source (uses Buy order statement — NOT allowed
# in ShowMe studies; the compiler rejects it with "This word not allowed in
# a study.").
$showmeInvalidSrc = @"
{ TestShowMe TVS-K1 invalid }
if Close > Open then
    Buy next bar at market;
"@

# Holds the ProgId assigned at insert, to verify it is preserved across modify.
$insertProgId = 0

try {
    # =====================================================================
    # SM1: INSERT — create a ShowMe with valid EL source (uses Plot1)
    # =====================================================================
    Write-Host "--- SM1: insert showme (valid Plot1 source) ---" -ForegroundColor Cyan

    $fInsert = Write-SourceFile "showme_insert" $showmeInsertSrc
    $sm1 = Invoke-Test -TestId "SM1" -Step "insert showme (valid Plot1)" `
        -Arguments @("--json","insert","showme",$probeName,$fInsert) `
        -ExpectedExit 0 -TestName $probeName -TestType "showme" `
        -JsonAssert {
            param($j)
            if (-not $j.success) { return "success field is false (expected true)" }
            if (-not $j.prog_id -or $j.prog_id -le 0) { return "prog_id missing or <= 0" }
            return $true
        }
    # Capture the ProgId for the modify-preservation check (SM5).
    try {
        $jObj = $sm1.Output | ConvertFrom-Json -ErrorAction Stop
        $script:insertProgId = [int]$jObj.prog_id
    } catch {
        $script:insertProgId = 0
    }

    # =====================================================================
    # SM2: EXISTS (exact) — verify the ShowMe exists with exact name match
    # =====================================================================
    Write-Host "--- SM2: exists showme (exact match) ---" -ForegroundColor Cyan

    Invoke-Test -TestId "SM2" -Step "exists showme (exact)" `
        -Arguments @("--json","exists","showme",$probeName) `
        -ExpectedExit 0 -TestName $probeName -TestType "showme" `
        -JsonAssert {
            param($j)
            if (-not $j.exists) { return "exists field is false (expected true)" }
            if (-not $j.stored_name) { return "stored_name field is missing" }
            if ($j.stored_name -ne $probeName) {
                return "stored_name mismatch: expected '$probeName', got '$($j.stored_name)'"
            }
            if (-not $j.prog_id -or $j.prog_id -le 0) { return "prog_id missing or <= 0" }
            return $true
        } | Out-Null

    # =====================================================================
    # SM3: EXISTS (case-insensitive) — query with different case; verify
    #      stored_name matches the ORIGINAL case.
    # =====================================================================
    Write-Host "--- SM3: exists showme --case-insensitive (different case query) ---" -ForegroundColor Cyan

    # Query with an all-lowercase version of the name. The stored_name in the
    # JSON result must match the ORIGINAL case used at insert time.
    $lowerQuery = $probeName.ToLower()
    Invoke-Test -TestId "SM3" -Step "exists showme --case-insensitive" `
        -Arguments @("--json","exists","showme",$lowerQuery,"--case-insensitive") `
        -ExpectedExit 0 -TestName $probeName -TestType "showme" `
        -JsonAssert {
            param($j)
            if (-not $j.exists) { return "exists field is false (expected true)" }
            if (-not $j.stored_name) { return "stored_name field is missing" }
            if ($j.stored_name -ne $probeName) {
                return "stored_name mismatch (case-insensitive): expected original '$probeName', got '$($j.stored_name)'"
            }
            if (-not $j.prog_id -or $j.prog_id -le 0) { return "prog_id missing or <= 0" }
            return $true
        } | Out-Null

    # =====================================================================
    # SM4: GET-SOURCE — retrieve the ShowMe's source; verify expected content
    # =====================================================================
    Write-Host "--- SM4: get-source showme (after insert) ---" -ForegroundColor Cyan

    Invoke-Test -TestId "SM4" -Step "get-source showme (after insert)" `
        -Arguments @("--json","get-source","showme",$probeName) `
        -ExpectedExit 0 -TestName $probeName -TestType "showme" `
        -JsonAssert {
            param($j)
            if (-not $j.success) { return "success field is false (expected true)" }
            if (-not $j.source) { return "source field is missing or empty" }
            if (-not $j.source.Contains("Plot1")) { return "source does not contain 'Plot1'" }
            if (-not $j.source.Contains("Up bar")) { return "source does not contain 'Up bar'" }
            if (-not $j.source.Contains("Close > Open")) { return "source does not contain 'Close > Open'" }
            return $true
        } | Out-Null

    # =====================================================================
    # SM5: MODIFY — replace the source with new EL source; verify success
    #      and that the ProgId is preserved (same as insert ProgId).
    # =====================================================================
    Write-Host "--- SM5: modify showme (new Plot1 source) ---" -ForegroundColor Cyan

    $fModify = Write-SourceFile "showme_modify" $showmeModifySrc
    Invoke-Test -TestId "SM5" -Step "modify showme (new source)" `
        -Arguments @("--json","modify","showme",$probeName,$fModify) `
        -ExpectedExit 0 -TestName $probeName -TestType "showme" `
        -JsonAssert {
            param($j)
            if (-not $j.success) { return "success field is false (expected true)" }
            if (-not $j.prog_id -or $j.prog_id -le 0) { return "prog_id missing or <= 0" }
            if ($script:insertProgId -gt 0 -and [int]$j.prog_id -ne $script:insertProgId) {
                return "ProgId NOT preserved: insert=$($script:insertProgId), modify=$($j.prog_id)"
            }
            return $true
        } | Out-Null

    # =====================================================================
    # SM6: GET-SOURCE (after modify) — verify the source was updated
    # =====================================================================
    Write-Host "--- SM6: get-source showme (after modify) ---" -ForegroundColor Cyan

    Invoke-Test -TestId "SM6" -Step "get-source showme (after modify)" `
        -Arguments @("--json","get-source","showme",$probeName) `
        -ExpectedExit 0 -TestName $probeName -TestType "showme" `
        -JsonAssert {
            param($j)
            if (-not $j.success) { return "success field is false (expected true)" }
            if (-not $j.source) { return "source field is missing or empty" }
            if (-not $j.source.Contains("Plot1")) { return "source does not contain 'Plot1'" }
            if (-not $j.source.Contains("Down bar")) { return "source does not contain 'Down bar' (modify did not take effect)" }
            if (-not $j.source.Contains("Close < Open")) { return "source does not contain 'Close < Open' (modify did not take effect)" }
            # The old insert source should no longer be the primary content.
            if ($j.source.Contains("Up bar")) { return "source still contains old 'Up bar' content (modify did not replace)" }
            return $true
        } | Out-Null

    # =====================================================================
    # SM7: SEMANTIC ERROR TEST — insert a ShowMe with a Buy order statement.
    #      Order statements are NOT allowed in ShowMe studies (Strategy-only).
    #      Expected: exit 8 (compile error), JSON compile_errors contains
    #      the study rejection message.
    # =====================================================================
    Write-Host "--- SM7: insert showme (Buy order statement -> compile error) ---" -ForegroundColor Cyan

    $fInvalid = Write-SourceFile "showme_invalid" $showmeInvalidSrc
    Invoke-Test -TestId "SM7" -Step "insert showme (Buy -> compile error)" `
        -Arguments @("--json","insert","showme",$invalidName,$fInvalid) `
        -ExpectedExit 8 -TestName $invalidName -TestType "showme" `
        -JsonAssert {
            param($j)
            if ($j.success) { return "success field is true (expected false)" }
            if ($j.exit_code -ne 8) { return "exit_code is $($j.exit_code) (expected 8)" }
            if (-not $j.error) { return "error object is missing" }
            if ($j.error.code -ne "compile_error") { return "error.code is '$($j.error.code)' (expected 'compile_error')" }
            if (-not $j.error.compile_errors) { return "error.compile_errors is missing" }
            if (-not $j.error.compile_errors.Contains("not allowed in a study")) {
                return "error.compile_errors does not contain 'not allowed in a study'"
            }
            return $true
        } | Out-Null

    # =====================================================================
    # SM8: DELETE — delete the ShowMe
    # =====================================================================
    Write-Host "--- SM8: delete showme ---" -ForegroundColor Cyan

    Invoke-Test -TestId "SM8" -Step "delete showme" `
        -Arguments @("--json","delete","showme",$probeName) `
        -ExpectedExit 0 -TestName $probeName -TestType "showme" `
        -JsonAssert {
            param($j)
            if (-not $j.success) { return "success field is false (expected true)" }
            return $true
        } | Out-Null

    # =====================================================================
    # SM9: EXISTS (after delete) — verify it no longer exists
    # =====================================================================
    Write-Host "--- SM9: exists showme (after delete) ---" -ForegroundColor Cyan

    Invoke-Test -TestId "SM9" -Step "exists showme (after delete)" `
        -Arguments @("--json","exists","showme",$probeName) `
        -ExpectedExit 0 -TestName $probeName -TestType "showme" `
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

    # Delete any remaining test items (both the valid probe and the invalid
    # semantic-error probe, in case a warehouse entry was created before the
    # compile failure). Either exit 0 (deleted) or exit 5 (already gone) is
    # acceptable; anything else is a warning.
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
    Write-Host "ALL SHOWME E2E TESTS PASSED" -ForegroundColor Green
    exit 0
}
else {
    Write-Host "$failureCount TEST(S) FAILED" -ForegroundColor Red
    exit 1
}
