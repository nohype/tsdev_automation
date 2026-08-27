<#
.SYNOPSIS
    E2E CLI test for the `exists --case-insensitive` feature of tsdev_cli
    (TVS-F.5, GREEN step).

.DESCRIPTION
    End-to-end test for the case-insensitive search mode of the `exists`
    subcommand. This test verifies the `--case-insensitive` flag and the
    key=value output format (exists, stored_name, progId).

    The case-insensitive mode uses the same exit-code convention as the
    exact-match `exists` op: exit 0 for BOTH found and not-found (not-found
    is a valid query result, not an error). The output distinguishes them
    via the `exists=1` / `exists=0` key=value fields.

    When found, the output also includes:
      stored_name=<verbatim stored name>  (the correctly-cased name in the warehouse)
      progId=<non-zero ProgId>             (the warehouse ProgId)

    When not found:
      stored_name=                         (empty)
      progId=0

    Test cases:
      CI.1  exists strategy ZZZ_CI_PROBE --case-insensitive
            - Creates "zzz_ci_probe" (lowercase), queries with uppercase.
            - Expects exit 0, exists=1, stored_name=zzz_ci_probe, progId=<non-zero>.
      CI.2  exists strategy zzz_MIXED_CASE_probe --case-insensitive
            - Creates "zzz_Mixed_Case_Probe", queries with jumbled case.
            - Expects exit 0, exists=1, stored_name=zzz_Mixed_Case_Probe, progId=<non-zero>.
      CI.3  exists strategy ZZZ_NONEXISTENT_ITEM --case-insensitive
            - Queries a name that does not exist (no item created).
            - Expects exit 0, exists=0, stored_name= (empty), progId=0.
      CI.4  exists strategy zzz_ci_exact  (exact match, no --case-insensitive)
            - Creates "zzz_ci_exact", queries with exact case.
            - Expects exit 0, exists=1.
      CI.5  exists strategy ZZZ_CI_EXACT_2  (wrong case, no --case-insensitive)
            - Creates "zzz_ci_exact_2", queries with wrong case (no flag).
            - Expects exit 0, exists=0 (should NOT match without the flag).

    Cleanup: deletes all created probe items in a finally block.

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
    powershell -ExecutionPolicy Bypass -File tests\test_exists_ci.ps1
    Runs the exists case-insensitive E2E tests with the default CLI path.

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
$tmpDir = Join-Path $ProjectRoot "tmp\e2e_exists_ci_$runId"

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
# output assertions (substring, regex, custom scriptblock), records the
# result, and prints PASS/FAIL.
#
# Parameters:
#   TestId        - short test identifier (e.g. "CI.1")
#   Step          - human-readable description
#   Arguments     - array of CLI arguments (without --ts-path)
#   ExpectedExit  - expected process exit code
#   TestName      - component name (for cleanup tracking on insert)
#   ExpectSubstr  - array of substrings that MUST be present in stdout
#   NotExpectSubstr - array of substrings that must NOT be present in stdout
#   ExpectRegex   - array of regex patterns that MUST match stdout
#   OutputAssert  - scriptblock receiving stdout string; returns $true on
#                   success or a string error message on failure.
# ---------------------------------------------------------------------------
function Invoke-Test {
    param(
        [Parameter(Mandatory)] [string]$TestId,
        [Parameter(Mandatory)] [string]$Step,
        [Parameter(Mandatory)] [string[]]$Arguments,
        [Parameter(Mandatory)] [int]$ExpectedExit,
        [string]$TestName,
        [string[]]$ExpectSubstr,
        [string[]]$NotExpectSubstr,
        [string[]]$ExpectRegex,
        [scriptblock]$OutputAssert
    )

    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $cli = Invoke-Cli -Arguments $Arguments
    $stopwatch.Stop()

    $actualExit = $cli.ExitCode
    $output = $cli.Stdout

    # Verify exit code.
    $exitOk = ($actualExit -eq $ExpectedExit)

    # Verify required substrings.
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

    # Verify negative substrings (must NOT be present).
    $notSubstrOk = $true
    $unexpectedSubstrs = @()
    if ($NotExpectSubstr) {
        foreach ($s in $NotExpectSubstr) {
            if ($output.Contains($s)) {
                $notSubstrOk = $false
                $unexpectedSubstrs += $s
            }
        }
    }

    # Verify regex patterns.
    $regexOk = $true
    $missingRegex = @()
    if ($ExpectRegex) {
        foreach ($pat in $ExpectRegex) {
            if (-not ($output -match $pat)) {
                $regexOk = $false
                $missingRegex += $pat
            }
        }
    }

    # Verify custom output assertion scriptblock.
    $customOk = $true
    $customErrors = @()
    if ($OutputAssert) {
        try {
            $assertResult = & $OutputAssert $output
            if ($assertResult -is [bool] -and -not $assertResult) {
                $customOk = $false
                $customErrors += "output assertion returned false"
            } elseif ($assertResult -is [string]) {
                $customOk = $false
                $customErrors += $assertResult
            }
        } catch {
            $customOk = $false
            $customErrors += "output assertion threw: $($_.Exception.Message)"
        }
    }

    $passed = ($exitOk -and $substrOk -and $notSubstrOk -and $regexOk -and $customOk)
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
        if ($NotExpectSubstr -and -not $notSubstrOk) {
            Write-Host "  UNEXPECTED SUBSTR FOUND: '$($unexpectedSubstrs -join "', '")'" -ForegroundColor Red
        }
        if ($ExpectRegex -and -not $regexOk) {
            Write-Host "  REGEX NOT MATCHED: '$($missingRegex -join "', '")'" -ForegroundColor Red
        }
        if ($customErrors.Count -gt 0) {
            foreach ($e in $customErrors) {
                Write-Host "  OUTPUT ASSERT FAILED: $e" -ForegroundColor Red
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
    Write-Host "=============== EXISTS CASE-INSENSITIVE E2E SUMMARY ===============" -ForegroundColor Cyan
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
Write-Host "TSDev Automation - exists --case-insensitive" -ForegroundColor Cyan
Write-Host "E2E Test (TVS-F.5, GREEN)" -ForegroundColor Cyan
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

# Probe names for this run (use zzz_ prefix so they sort last and are easy
# to identify as test artifacts).
$probeLower    = "zzz_ci_probe"
$probeMixed    = "zzz_Mixed_Case_Probe"
$probeExact    = "zzz_ci_exact"
$probeExact2   = "zzz_ci_exact_2"

# Minimal valid EasyLanguage strategy source. The leading "{ name }" text is
# arbitrary fixture source content only; it is not a metadata convention or a
# lookup mechanism. The CLI supplies the item name, and stored metadata comes
# from orlib20 warehouse records/APIs.
function Write-StrategySource {
    param([Parameter(Mandatory)] [string]$ProbeName)
    $source = "{ $ProbeName }`r`nInputs: int x(0);`r`nif x > 0 then Buy next bar at market;"
    return Write-SourceFile $ProbeName $source
}

try {
    # =====================================================================
    # CI.1: Case-insensitive match — lowercase stored, uppercase query
    # =====================================================================
    Write-Host "--- CI.1: exists --case-insensitive (lowercase stored, uppercase query) ---" -ForegroundColor Cyan

    # Insert the probe strategy (lowercase name).
    $f1 = Write-StrategySource $probeLower
    Invoke-Test -TestId "CI.1a" -Step "insert zzz_ci_probe" `
        -Arguments @("insert","strategy",$probeLower,$f1) `
        -ExpectedExit 0 -ExpectSubstr "SUCCESS" -TestName $probeLower | Out-Null

    # Query with uppercase name + --case-insensitive.
    Invoke-Test -TestId "CI.1b" -Step "exists --case-insensitive (uppercase query)" `
        -Arguments @("exists","strategy","ZZZ_CI_PROBE","--case-insensitive") `
        -ExpectedExit 0 `
        -ExpectSubstr @("exists=1","stored_name=$probeLower") `
        -ExpectRegex @("progId=[1-9]\d*") | Out-Null

    # =====================================================================
    # CI.2: Case-insensitive match — mixed-case stored, jumbled query
    # =====================================================================
    Write-Host "--- CI.2: exists --case-insensitive (mixed-case stored, jumbled query) ---" -ForegroundColor Cyan

    # Insert the probe strategy (mixed-case name).
    $f2 = Write-StrategySource $probeMixed
    Invoke-Test -TestId "CI.2a" -Step "insert zzz_Mixed_Case_Probe" `
        -Arguments @("insert","strategy",$probeMixed,$f2) `
        -ExpectedExit 0 -ExpectSubstr "SUCCESS" -TestName $probeMixed | Out-Null

    # Query with jumbled case + --case-insensitive.
    Invoke-Test -TestId "CI.2b" -Step "exists --case-insensitive (jumbled query)" `
        -Arguments @("exists","strategy","zzz_MIXED_CASE_probe","--case-insensitive") `
        -ExpectedExit 0 `
        -ExpectSubstr @("exists=1","stored_name=$probeMixed") `
        -ExpectRegex @("progId=[1-9]\d*") | Out-Null

    # =====================================================================
    # CI.3: Case-insensitive search for nonexistent item
    # =====================================================================
    Write-Host "--- CI.3: exists --case-insensitive (nonexistent item) ---" -ForegroundColor Cyan

    Invoke-Test -TestId "CI.3" -Step "exists --case-insensitive (nonexistent)" `
        -Arguments @("exists","strategy","ZZZ_NONEXISTENT_ITEM","--case-insensitive") `
        -ExpectedExit 0 `
        -ExpectSubstr @("exists=0","progId=0") `
        -OutputAssert {
            param($out)
            # stored_name= must be present but empty (no value after =).
            if ($out -notmatch '(?m)^stored_name=\s*$') {
                return "stored_name= not found or not empty"
            }
            return $true
        } | Out-Null

    # =====================================================================
    # CI.4: Without --case-insensitive, exact match still works
    # =====================================================================
    Write-Host "--- CI.4: exists (exact match, no --case-insensitive) ---" -ForegroundColor Cyan

    # Insert the probe strategy.
    $f4 = Write-StrategySource $probeExact
    Invoke-Test -TestId "CI.4a" -Step "insert zzz_ci_exact" `
        -Arguments @("insert","strategy",$probeExact,$f4) `
        -ExpectedExit 0 -ExpectSubstr "SUCCESS" -TestName $probeExact | Out-Null

    # Query with exact case (no --case-insensitive flag).
    Invoke-Test -TestId "CI.4b" -Step "exists (exact match)" `
        -Arguments @("exists","strategy",$probeExact) `
        -ExpectedExit 0 `
        -ExpectSubstr @("exists=1") | Out-Null

    # =====================================================================
    # CI.5: Without --case-insensitive, wrong case does NOT match
    # =====================================================================
    Write-Host "--- CI.5: exists (wrong case, no --case-insensitive) ---" -ForegroundColor Cyan

    # Insert the probe strategy (lowercase name).
    $f5 = Write-StrategySource $probeExact2
    Invoke-Test -TestId "CI.5a" -Step "insert zzz_ci_exact_2" `
        -Arguments @("insert","strategy",$probeExact2,$f5) `
        -ExpectedExit 0 -ExpectSubstr "SUCCESS" -TestName $probeExact2 | Out-Null

    # Query with uppercase name (no --case-insensitive flag) — should NOT match.
    Invoke-Test -TestId "CI.5b" -Step "exists (wrong case, no flag)" `
        -Arguments @("exists","strategy","ZZZ_CI_EXACT_2") `
        -ExpectedExit 0 `
        -ExpectSubstr @("exists=0") | Out-Null

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
        } else {
            Write-Host "  Cleanup FAILED for: $name (exit $($cli.ExitCode))" -ForegroundColor DarkYellow
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
    Write-Host "ALL EXISTS CASE-INSENSITIVE E2E TESTS PASSED" -ForegroundColor Green
    exit 0
}
else {
    Write-Host "$failureCount TEST(S) FAILED" -ForegroundColor Red
    exit 1
}
