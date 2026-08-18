<#
.SYNOPSIS
    TVS-D.3: Batch mode test for tsdev_cli (TDD red-green).

.DESCRIPTION
    Validates the batch-insert / batch-modify / batch-delete subcommands of
    tsdev_cli.exe. Batch mode ALWAYS emits a JSON ARRAY on stdout (one
    element per manifest item, in manifest order) and uses exit codes:
      0 = all items succeeded
      1 = usage error / manifest not found / manifest empty
      2 = batch ran but one or more items failed

    Test cases (all use unique BatchTest_ prefixed names and clean up):
      1. batch-insert: 2 valid strategies (success) + 1 compile-error +
         1 missing source file -> 4 elements, 2 success + 2 failure, exit 2.
      2. batch-delete: 1 existing (success) + 1 non-existing (not_found) ->
         2 elements, 1 success + 1 failure, exit 2.
      3. batch-modify: 1 existing (success, ProgId preserved) + 1 non-existing
         (not_found) -> 2 elements, 1 success + 1 failure, exit 2.
      4. Comments (#) and blank lines are skipped (results length == real items).
      5. Exit codes: all-success batch -> exit 0; mixed -> exit 2;
         missing manifest file -> exit 1.
      6. Larger batch (20 items) -> init amortization, no degradation.
      7. Mixed component types in one batch-insert (strategy + function +
         indicator) -> 3 elements, all success, correct type fields, exit 0.
      8. Empty manifest (only comments and blank lines) -> exit 1, no JSON
         array on stdout, stderr error message.
      9. Already-exists in batch-insert: pre-inserted component re-inserted
         via batch -> that item success==false, exit_code==5,
         error.code=="already_exists", error.existing_prog_id > 0; other
         items in the manifest still process.

    Exit codes (this script):
      0 = all assertions passed
      1 = one or more assertions failed (or a prerequisite was not met)

    Prerequisites:
      - The project must be built: cmake --build build --config Release --target tsdev_cli
      - TradeStation 9.5 must be installed (or -TsPath provided)
      - No TradeStation processes/services should be running
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
$tmpDir = Join-Path $ProjectRoot "tmp\batch_test_$runId"

# Results collector.
$Results = New-Object System.Collections.Generic.List[object]
$failCount = 0

# Track all test names created during the run for cleanup.
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
# Write-ManifestFile: writes a manifest (line-based) to a temp file in ASCII.
# Returns the path.
# ---------------------------------------------------------------------------
function Write-ManifestFile {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter()] [string[]]$Lines = @()
    )
    $path = Join-Path $script:tmpDir "$Name.manifest"
    $content = ($Lines -join "`r`n") + "`r`n"
    [System.IO.File]::WriteAllText($path, $content, [System.Text.Encoding]::ASCII)
    return $path
}

# ---------------------------------------------------------------------------
# Invoke-CliRaw: runs tsdev_cli.exe with the given argument array, capturing
# stdout and stderr SEPARATELY. Returns a hashtable with Stdout, Stderr, ExitCode.
# ---------------------------------------------------------------------------
function Invoke-CliRaw {
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
# Assert-True: records a single boolean assertion. Throws on failure.
# ---------------------------------------------------------------------------
function Assert-True {
    param(
        [Parameter(Mandatory)] [bool]$Condition,
        [Parameter(Mandatory)] [string]$Message
    )
    if (-not $Condition) {
        throw $Message
    }
}

# ---------------------------------------------------------------------------
# Run-TestCase: runs a scriptblock as a single test case, captures pass/fail.
# ---------------------------------------------------------------------------
function Run-TestCase {
    param(
        [Parameter(Mandatory)] [string]$TestId,
        [Parameter(Mandatory)] [scriptblock]$Body
    )
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $status = 'PASS'
    $detail = ''
    try {
        & $Body
    }
    catch {
        $status = 'FAIL'
        $detail = $_.Exception.Message
        $script:failCount++
    }
    $stopwatch.Stop()
    $color = if ($status -eq 'PASS') { 'Green' } else { 'Red' }
    $durationStr = "{0:0.00}s" -f $stopwatch.Elapsed.TotalSeconds
    Write-Host ("[{0}] {1}: {2} [{3}]" -f $status, $TestId, $detail, $durationStr) -ForegroundColor $color
    $script:Results.Add([pscustomobject]@{
        TestId = $TestId
        Result = $status
        Detail = $detail
        Duration = $stopwatch.Elapsed
    }) | Out-Null
}

# ---------------------------------------------------------------------------
# Cleanup: re-delete every test name across all types (expecting exit 5).
# ---------------------------------------------------------------------------
function Test-Cleanup {
    param([string[]]$AllNames)
    Write-Host ""
    Write-Host "===== CLEANUP =====" -ForegroundColor Cyan
    $issues = @()
    $types = @('strategy', 'indicator', 'function', 'tradingapp')
    foreach ($name in $AllNames) {
        foreach ($type in $types) {
            $cli = Invoke-CliRaw -Arguments @("delete", $type, $name)
            if ($cli.ExitCode -eq 0) {
                Write-Host "INFO: delete $type '$name' cleaned up successfully (exit 0)" -ForegroundColor DarkGray
            } elseif ($cli.ExitCode -eq 5) {
                Write-Host "INFO: delete $type '$name' already gone (exit 5)" -ForegroundColor DarkGray
            } else {
                $issues += "ERROR: delete $type '$name' returned $($cli.ExitCode) (cleanup failure)"
            }
        }
    }
    if (Test-Path -Path $tmpDir -PathType Container) {
        Remove-Item -Path $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
    }
    if ($issues.Count -eq 0) {
        Write-Host "CLEANUP: OK" -ForegroundColor Green
    } else {
        Write-Host "CLEANUP: ISSUES:" -ForegroundColor Yellow
        foreach ($i in $issues) { Write-Host "  $i" -ForegroundColor Yellow }
    }
}

# ===========================================================================
# MAIN
# ===========================================================================

Write-Host "============================================" -ForegroundColor Cyan
Write-Host "TVS-D.3: batch mode test" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "Run ID      : $runId"
Write-Host "Project root: $ProjectRoot"
Write-Host "CLI path    : $CliPath"
Write-Host "TS path     : $(if ($TsPath) { $TsPath } else { 'auto-discovered' })"
Write-Host "Temp dir    : $tmpDir"
Write-Host ""

if (-not (Test-Path -Path $CliPath -PathType Leaf)) {
    Write-Host "ERROR: CLI executable not found: $CliPath" -ForegroundColor Red
    Write-Host "Build first: cmake --build build --config Release --target tsdev_cli" -ForegroundColor Yellow
    exit 1
}

New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null

$allTestNames = @()

# Valid EasyLanguage strategy source (compiles successfully).
$validSrc = "Inputs: int x(0);`r`nif x > 0 then Buy next bar at market;"
# Invalid EasyLanguage source (compile error).
$badSrc = "this is not valid easylanguage;"

try {
    # -------------------------------------------------------------------
    # Test 1: batch-insert — 2 valid + 1 compile-error + 1 missing source
    # -------------------------------------------------------------------
    $n1 = "BatchTest_${runId}_Ins1"; $allTestNames += $n1
    $n2 = "BatchTest_${runId}_Ins2"; $allTestNames += $n2
    $n3 = "BatchTest_${runId}_Bad";  $allTestNames += $n3
    $f1 = Write-SourceFile "ins1" $validSrc
    $f2 = Write-SourceFile "ins2" $validSrc
    $fBad = Write-SourceFile "badsrc" $badSrc
    $missingPath = Join-Path $tmpDir "does_not_exist.el"

    $manifest1 = @(
        "strategy $n1 $f1",
        "strategy $n2 $f2",
        "strategy $n3 $fBad",
        "strategy BatchTest_${runId}_NoFile $missingPath"
    )
    $manifestPath1 = Write-ManifestFile "insert1" $manifest1

    Run-TestCase -TestId "B1-batch-insert-mixed" -Body {
        $cli = Invoke-CliRaw -Arguments @("batch-insert", $manifestPath1)
        Assert-True ($cli.ExitCode -eq 2) "exit_code: expected 2 (mixed), got $($cli.ExitCode)"
        # stdout must be a valid JSON array.
        $arr = $cli.Stdout | ConvertFrom-Json
        Assert-True ($arr -is [array]) "stdout must be a JSON array"
        Assert-True ($arr.Count -eq 4) "array length: expected 4, got $($arr.Count)"
        # Items 0,1 success; item 2 compile error; item 3 source file error.
        Assert-True ($arr[0].success -eq $true) "item0 success: expected true"
        Assert-True ($arr[0].op -eq 'insert') "item0 op: expected insert"
        Assert-True ($arr[0].type -eq 'strategy') "item0 type: expected strategy"
        Assert-True ($null -ne $arr[0].prog_id -and $arr[0].prog_id -gt 0) "item0 prog_id: expected >0"
        Assert-True ($null -ne $arr[0].ell_path -and $arr[0].ell_path.Length -gt 0) "item0 ell_path: expected non-empty"

        Assert-True ($arr[1].success -eq $true) "item1 success: expected true"
        Assert-True ($arr[1].prog_id -gt 0) "item1 prog_id: expected >0"

        Assert-True ($arr[2].success -eq $false) "item2 success: expected false"
        Assert-True ($arr[2].exit_code -eq 8) "item2 exit_code: expected 8, got $($arr[2].exit_code)"
        Assert-True ($arr[2].error.code -eq 'compile_error') "item2 error.code: expected compile_error, got $($arr[2].error.code)"
        $hasCe = [bool]($arr[2].error.PSObject.Properties.Name -contains 'compile_errors')
        Assert-True ($hasCe) "item2 error.compile_errors must be present"

        Assert-True ($arr[3].success -eq $false) "item3 success: expected false"
        Assert-True ($arr[3].exit_code -eq 3) "item3 exit_code: expected 3, got $($arr[3].exit_code)"
        Assert-True ($arr[3].error.code -eq 'source_file_error') "item3 error.code: expected source_file_error, got $($arr[3].error.code)"
    }

    # -------------------------------------------------------------------
    # Test 2: batch-delete — 1 existing + 1 non-existing
    # -------------------------------------------------------------------
    $nDelExisting = $n1  # inserted in Test 1
    $nDelMissing = "BatchTest_${runId}_DelMissing"
    $manifest2 = @(
        "strategy $nDelExisting",
        "strategy $nDelMissing"
    )
    $manifestPath2 = Write-ManifestFile "delete1" $manifest2

    Run-TestCase -TestId "B2-batch-delete-mixed" -Body {
        $cli = Invoke-CliRaw -Arguments @("batch-delete", $manifestPath2)
        Assert-True ($cli.ExitCode -eq 2) "exit_code: expected 2 (mixed), got $($cli.ExitCode)"
        $arr = $cli.Stdout | ConvertFrom-Json
        Assert-True ($arr -is [array]) "stdout must be a JSON array"
        Assert-True ($arr.Count -eq 2) "array length: expected 2, got $($arr.Count)"
        Assert-True ($arr[0].op -eq 'delete') "item0 op: expected delete"
        Assert-True ($arr[0].success -eq $true) "item0 success: expected true"
        Assert-True ($arr[0].prog_id -gt 0) "item0 prog_id: expected >0"

        Assert-True ($arr[1].success -eq $false) "item1 success: expected false"
        Assert-True ($arr[1].exit_code -eq 5) "item1 exit_code: expected 5, got $($arr[1].exit_code)"
        Assert-True ($arr[1].error.code -eq 'not_found') "item1 error.code: expected not_found, got $($arr[1].error.code)"
    }

    # -------------------------------------------------------------------
    # Test 3: batch-modify — 1 existing (ProgId preserved) + 1 non-existing
    # -------------------------------------------------------------------
    # First insert a component to modify.
    $nMod = "BatchTest_${runId}_Mod"; $allTestNames += $nMod
    $fModInit = Write-SourceFile "modinit" $validSrc
    $cliPrep = Invoke-CliRaw -Arguments @("--json", "insert", "strategy", $nMod, $fModInit)
    Assert-True ($cliPrep.ExitCode -eq 0) "prep insert for modify test failed: exit $($cliPrep.ExitCode)"
    $prepObj = $cliPrep.Stdout | ConvertFrom-Json
    $modProgId = $prepObj.prog_id

    $fModNew = Write-SourceFile "modnew" ("Inputs: int x(5);`r`nif x > 0 then Sell next bar at market;")
    $nModMissing = "BatchTest_${runId}_ModMissing"
    $manifest3 = @(
        "strategy $nMod $fModNew",
        "strategy $nModMissing $fModNew"
    )
    $manifestPath3 = Write-ManifestFile "modify1" $manifest3

    Run-TestCase -TestId "B3-batch-modify-mixed" -Body {
        $cli = Invoke-CliRaw -Arguments @("batch-modify", $manifestPath3)
        Assert-True ($cli.ExitCode -eq 2) "exit_code: expected 2 (mixed), got $($cli.ExitCode)"
        $arr = $cli.Stdout | ConvertFrom-Json
        Assert-True ($arr -is [array]) "stdout must be a JSON array"
        Assert-True ($arr.Count -eq 2) "array length: expected 2, got $($arr.Count)"
        Assert-True ($arr[0].op -eq 'modify') "item0 op: expected modify"
        Assert-True ($arr[0].success -eq $true) "item0 success: expected true"
        Assert-True ($arr[0].prog_id -gt 0) "item0 prog_id: expected >0"
        # ProgId must be preserved (same as the insert prog_id).
        Assert-True ($arr[0].prog_id -eq $modProgId) "item0 prog_id: expected $modProgId (preserved), got $($arr[0].prog_id)"
        Assert-True ($null -ne $arr[0].ell_path -and $arr[0].ell_path.Length -gt 0) "item0 ell_path: expected non-empty"

        Assert-True ($arr[1].success -eq $false) "item1 success: expected false"
        Assert-True ($arr[1].exit_code -eq 5) "item1 exit_code: expected 5, got $($arr[1].exit_code)"
        Assert-True ($arr[1].error.code -eq 'not_found') "item1 error.code: expected not_found, got $($arr[1].error.code)"
    }

    # -------------------------------------------------------------------
    # Test 4: comments and blank lines are skipped
    # -------------------------------------------------------------------
    $n4a = "BatchTest_${runId}_Cmt1"; $allTestNames += $n4a
    $n4b = "BatchTest_${runId}_Cmt2"; $allTestNames += $n4b
    $f4a = Write-SourceFile "cmt1" $validSrc
    $f4b = Write-SourceFile "cmt2" $validSrc
    $manifest4 = @(
        "# This is a comment",
        "",
        "   ",
        "strategy $n4a $f4a",
        "# another comment",
        "strategy $n4b $f4b"
    )
    $manifestPath4 = Write-ManifestFile "comments" $manifest4

    Run-TestCase -TestId "B4-comments-blanks-skipped" -Body {
        $cli = Invoke-CliRaw -Arguments @("batch-insert", $manifestPath4)
        Assert-True ($cli.ExitCode -eq 0) "exit_code: expected 0 (all success), got $($cli.ExitCode)"
        $arr = $cli.Stdout | ConvertFrom-Json
        Assert-True ($arr -is [array]) "stdout must be a JSON array"
        Assert-True ($arr.Count -eq 2) "array length: expected 2 (comments/blanks skipped), got $($arr.Count)"
        Assert-True ($arr[0].name -eq $n4a) "item0 name: expected $n4a, got $($arr[0].name)"
        Assert-True ($arr[1].name -eq $n4b) "item1 name: expected $n4b, got $($arr[1].name)"
        Assert-True ($arr[0].success -eq $true) "item0 success: expected true"
        Assert-True ($arr[1].success -eq $true) "item1 success: expected true"
    }

    # -------------------------------------------------------------------
    # Test 5: exit codes — all-success (exit 0), missing manifest (exit 1)
    # -------------------------------------------------------------------
    $n5 = "BatchTest_${runId}_AllOk"; $allTestNames += $n5
    $f5 = Write-SourceFile "allok" $validSrc
    $manifest5 = @("strategy $n5 $f5")
    $manifestPath5 = Write-ManifestFile "allok" $manifest5

    Run-TestCase -TestId "B5a-all-success-exit0" -Body {
        $cli = Invoke-CliRaw -Arguments @("batch-insert", $manifestPath5)
        Assert-True ($cli.ExitCode -eq 0) "exit_code: expected 0 (all success), got $($cli.ExitCode)"
        $arr = $cli.Stdout | ConvertFrom-Json
        Assert-True ($arr.Count -eq 1) "array length: expected 1, got $($arr.Count)"
        Assert-True ($arr[0].success -eq $true) "item0 success: expected true"
    }

    Run-TestCase -TestId "B5b-missing-manifest-exit1" -Body {
        $missing = Join-Path $tmpDir "no_such_manifest.txt"
        $cli = Invoke-CliRaw -Arguments @("batch-insert", $missing)
        Assert-True ($cli.ExitCode -eq 1) "exit_code: expected 1 (manifest not found), got $($cli.ExitCode)"
        # stdout should not be a valid JSON array (batch did not run).
        $isArr = $false
        try { $t = $cli.Stdout | ConvertFrom-Json; if ($t -is [array]) { $isArr = $true } } catch { }
        Assert-True (-not $isArr) "stdout must NOT be a JSON array when manifest is missing"
    }

    # -------------------------------------------------------------------
    # Test 6: larger batch (20 items) — init amortization, no degradation
    # -------------------------------------------------------------------
    $bigNames = @()
    $bigManifestLines = @()
    for ($i = 0; $i -lt 20; $i++) {
        $bn = "BatchTest_${runId}_Big{0:D2}" -f $i
        $bigNames += $bn
        $allTestNames += $bn
        $bf = Write-SourceFile "big$i" $validSrc
        $bigManifestLines += "strategy $bn $bf"
    }
    $manifestPath6 = Write-ManifestFile "bigbatch" $bigManifestLines

    Run-TestCase -TestId "B6-large-batch-20" -Body {
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $cli = Invoke-CliRaw -Arguments @("batch-insert", $manifestPath6)
        $sw.Stop()
        Assert-True ($cli.ExitCode -eq 0) "exit_code: expected 0 (all 20 success), got $($cli.ExitCode)"
        $arr = $cli.Stdout | ConvertFrom-Json
        Assert-True ($arr -is [array]) "stdout must be a JSON array"
        Assert-True ($arr.Count -eq 20) "array length: expected 20, got $($arr.Count)"
        $successCount = ($arr | Where-Object { $_.success -eq $true }).Count
        Assert-True ($successCount -eq 20) "success count: expected 20, got $successCount"
        # Timing sanity: 20 items in one batch should be well under 20 single
        # spawns (~20 * 411ms init = ~8.2s just for init). Allow generous bound.
        $totalSec = $sw.Elapsed.TotalSeconds
        Write-Host "  20-item batch took $("{0:0.00}" -f $totalSec)s" -ForegroundColor DarkGray
        Assert-True ($totalSec -lt 120) "20-item batch should complete in < 120s, took $totalSec s"
    }

    # -------------------------------------------------------------------
    # Test 7: mixed component types in one batch-insert
    #         (1 strategy + 1 function + 1 indicator)
    # -------------------------------------------------------------------
    $n7s = "BatchTest_${runId}_MixStrat";  $allTestNames += $n7s
    $n7f = "BatchTest_${runId}_MixFunc";   $allTestNames += $n7f
    $n7i = "BatchTest_${runId}_MixInd";    $allTestNames += $n7i

    # Strategy source (valid, compiles).
    $srcStrat = "Inputs: Length(10);`r`nIf Close > Average(Close, Length) Then Buy next bar at Market;"
    $fStrat = Write-SourceFile "mixstrat" $srcStrat
    # Function source: name must match the component name; returns 1.
    $srcFunc = "$n7f = 1;"
    $fFunc = Write-SourceFile "mixfunc" $srcFunc
    # Indicator source: a simple plot (valid for indicators).
    $srcInd = "Plot1(Close, ""Test"");"
    $fInd = Write-SourceFile "mixind" $srcInd

    $manifest7 = @(
        "strategy $n7s $fStrat",
        "function $n7f $fFunc",
        "indicator $n7i $fInd"
    )
    $manifestPath7 = Write-ManifestFile "mixedtypes" $manifest7

    Run-TestCase -TestId "B7-batch-insert-mixed-types" -Body {
        $cli = Invoke-CliRaw -Arguments @("batch-insert", $manifestPath7)
        Assert-True ($cli.ExitCode -eq 0) "exit_code: expected 0 (all success), got $($cli.ExitCode)"
        $arr = $cli.Stdout | ConvertFrom-Json
        Assert-True ($arr -is [array]) "stdout must be a JSON array"
        Assert-True ($arr.Count -eq 3) "array length: expected 3, got $($arr.Count)"
        # Each item must have the correct type field and success==true.
        Assert-True ($arr[0].type -eq 'strategy') "item0 type: expected strategy, got $($arr[0].type)"
        Assert-True ($arr[0].success -eq $true) "item0 success: expected true"
        Assert-True ($arr[0].prog_id -gt 0) "item0 prog_id: expected >0"

        Assert-True ($arr[1].type -eq 'function') "item1 type: expected function, got $($arr[1].type)"
        Assert-True ($arr[1].success -eq $true) "item1 success: expected true"
        Assert-True ($arr[1].prog_id -gt 0) "item1 prog_id: expected >0"

        Assert-True ($arr[2].type -eq 'indicator') "item2 type: expected indicator, got $($arr[2].type)"
        Assert-True ($arr[2].success -eq $true) "item2 success: expected true"
        Assert-True ($arr[2].prog_id -gt 0) "item2 prog_id: expected >0"
    }

    # -------------------------------------------------------------------
    # Test 8: empty manifest (only comments and blank lines)
    # -------------------------------------------------------------------
    $manifest8 = @(
        "# This manifest has no real items",
        "",
        "   ",
        "# just comments and blanks"
    )
    $manifestPath8 = Write-ManifestFile "empty" $manifest8

    Run-TestCase -TestId "B8-empty-manifest-exit1" -Body {
        $cli = Invoke-CliRaw -Arguments @("batch-insert", $manifestPath8)
        Assert-True ($cli.ExitCode -eq 1) "exit_code: expected 1 (empty manifest), got $($cli.ExitCode)"
        # stdout must NOT be a JSON array (batch did not run).
        $isArr = $false
        try { $t = $cli.Stdout | ConvertFrom-Json; if ($t -is [array]) { $isArr = $true } } catch { }
        Assert-True (-not $isArr) "stdout must NOT be a JSON array when manifest is empty"
        # stderr must contain an error message.
        Assert-True ($cli.Stderr.Length -gt 0) "stderr must have an error message for empty manifest"
        Assert-True ($cli.Stderr -match 'empty') "stderr should mention 'empty', got: $($cli.Stderr)"
    }

    # -------------------------------------------------------------------
    # Test 9: already-exists in batch-insert
    #         (pre-insert a component, then batch-insert it again)
    # -------------------------------------------------------------------
    $n9existing = "BatchTest_${runId}_AlreadyExists"; $allTestNames += $n9existing
    $n9other = "BatchTest_${runId}_AlreadyOther";     $allTestNames += $n9other

    # Pre-insert the component via single-item insert so it already exists.
    $fPrep = Write-SourceFile "alreadyexists_prep" $validSrc
    $cliPrep9 = Invoke-CliRaw -Arguments @("--json", "insert", "strategy", $n9existing, $fPrep)
    Assert-True ($cliPrep9.ExitCode -eq 0) "prep insert for already-exists test failed: exit $($cliPrep9.ExitCode)"

    # Build a batch manifest that includes the already-existing name plus a
    # second (new) item to confirm the batch continues past the failure.
    $fOther = Write-SourceFile "alreadyother" $validSrc
    $manifest9 = @(
        "strategy $n9existing $fPrep",
        "strategy $n9other $fOther"
    )
    $manifestPath9 = Write-ManifestFile "alreadyexists" $manifest9

    Run-TestCase -TestId "B9-batch-insert-already-exists" -Body {
        $cli = Invoke-CliRaw -Arguments @("batch-insert", $manifestPath9)
        # Mixed: one failure (already_exists) + one success -> exit 2.
        Assert-True ($cli.ExitCode -eq 2) "exit_code: expected 2 (mixed), got $($cli.ExitCode)"
        $arr = $cli.Stdout | ConvertFrom-Json
        Assert-True ($arr -is [array]) "stdout must be a JSON array"
        Assert-True ($arr.Count -eq 2) "array length: expected 2, got $($arr.Count)"

        # Item 0: the already-existing component -> failure.
        Assert-True ($arr[0].success -eq $false) "item0 success: expected false (already exists)"
        Assert-True ($arr[0].exit_code -eq 5) "item0 exit_code: expected 5, got $($arr[0].exit_code)"
        Assert-True ($arr[0].error.code -eq 'already_exists') "item0 error.code: expected already_exists, got $($arr[0].error.code)"
        Assert-True ($arr[0].error.existing_prog_id -gt 0) "item0 error.existing_prog_id: expected >0, got $($arr[0].error.existing_prog_id)"

        # Item 1: the new component -> success (batch continued).
        Assert-True ($arr[1].success -eq $true) "item1 success: expected true (batch continued)"
        Assert-True ($arr[1].prog_id -gt 0) "item1 prog_id: expected >0"
    }

}
finally {
    # Cleanup all created components (re-delete across all types).
    Test-Cleanup -AllNames $allTestNames
}

# ===========================================================================
# Summary
# ===========================================================================
Write-Host ""
Write-Host "==================== BATCH MODE TEST SUMMARY ====================" -ForegroundColor Cyan
$passed = @($Results | Where-Object { $_.Result -eq 'PASS' }).Count
$total = $Results.Count
Write-Host ("GRAND TOTAL: {0}/{1} passed" -f $passed, $total) -ForegroundColor Cyan

Write-Host ""
if ($failCount -eq 0) {
    Write-Host "ALL BATCH MODE TESTS PASSED" -ForegroundColor Green
    exit 0
} else {
    Write-Host "$failCount TEST(S) FAILED" -ForegroundColor Red
    exit 1
}
