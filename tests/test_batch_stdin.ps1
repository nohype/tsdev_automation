<#
.SYNOPSIS
    TVS-F.2: Batch stdin mode test for tsdev_cli (TDD red).

.DESCRIPTION
    Validates the --batch-stdin flag of the batch-insert / batch-modify /
    batch-delete subcommands of tsdev_cli.exe. With --batch-stdin, the
    manifest is read from stdin instead of a manifest file path. The
    existing file-path mode (batch-insert <manifest_file>) is also tested
    for backward compatibility.

    Batch mode ALWAYS emits a JSON ARRAY on stdout (one element per manifest
    item, in manifest order) and uses exit codes:
      0 = all items succeeded
      1 = usage error / manifest empty / missing args
      2 = batch ran but one or more items failed

    Test cases (all use unique BatchStdinTest_ prefixed names and clean up):
      S1. batch-stdin insert: 2 valid strategies + 1 compile-error +
          1 missing source file -> 4 elements, 2 success + 2 failure, exit 2.
      S2. batch-stdin delete: 1 existing (success) + 1 non-existing
          (not_found) -> 2 elements, 1 success + 1 failure, exit 2.
      S3. batch-stdin modify: 1 existing (success, ProgId preserved) +
          1 non-existing (not_found) -> 2 elements, 1 success + 1 failure,
          exit 2.
      S4. batch-stdin comments/blanks: manifest with # comments, blank lines,
          and 2 real valid strategy items -> 2 elements, exit 0.
      S5. batch-stdin empty: manifest with only comments + blank lines ->
          exit 1, no JSON array on stdout, stderr mentions "empty".
      S6. backward-compat file-path: batch-insert <manifest_file> with 1 valid
          strategy -> 1 element, success, exit 0.
      S7. no-stdin-flag-rejected: batch-insert with no manifest path and no
          --batch-stdin -> exit 1 (usage error).
      S8. stdin-content-identical-to-file: same manifest run via --batch-stdin
          and via file-path mode -> same length and per-item success/name/type.

    Exit codes (this script):
      0 = all assertions passed
      1 = one or more assertions failed (or a prerequisite was not met)

    Prerequisites:
      - The project must be built: cmake --build build --config Release --target tsdev_cli
      - TradeStation 9.5 must be installed (or -TsPath provided)
      - No TradeStation processes/services should be running

    NOTE: This is a TDD red test. The --batch-stdin flag is NOT yet
    implemented, so this test is EXPECTED to fail at this stage. That is
    correct and intended.
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
$tmpDir = Join-Path $ProjectRoot "tmp\batch_stdin_test_$runId"

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
# stdout and stderr SEPARATELY. Returns a hashtable with Stdout, Stderr,
# ExitCode. Used for file-path mode (backward compat) and single-item ops.
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
# Invoke-CliStdin: runs tsdev_cli.exe with the given argument array, feeding
# $ManifestFile as stdin via Start-Process -RedirectStandardInput. This is
# the encoding-safe way to pipe a file as stdin (PowerShell's | pipe mangles
# encoding and adds newline translation). Captures stdout and stderr to temp
# files. Returns a hashtable with Stdout, Stderr, ExitCode (same shape as
# Invoke-CliRaw). Prepends --ts-path <dir> when $TsPath is set.
# ---------------------------------------------------------------------------
function Invoke-CliStdin {
    param(
        [Parameter(Mandatory)] [string]$ManifestFile,
        [Parameter(Mandatory)] [string[]]$Arguments
    )
    $fullArgs = @()
    if ($script:TsPath) {
        $fullArgs += @("--ts-path", $script:TsPath)
    }
    $fullArgs += $Arguments

    # Build a single quoted argument string for Start-Process.
    $argParts = @()
    foreach ($a in $fullArgs) {
        if ($a -match '\s') {
            $argParts += "`"$a`""
        } else {
            $argParts += $a
        }
    }
    $argString = ($argParts -join " ")

    $stdoutFile = Join-Path $script:tmpDir "stdout_$([System.Guid]::NewGuid().ToString('N')).txt"
    $stderrFile = Join-Path $script:tmpDir "stderr_$([System.Guid]::NewGuid().ToString('N')).txt"

    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        # Start-Process handles async output redirection internally and
        # guarantees the redirected output files are fully written and CLOSED
        # by the time -Wait returns, eliminating the race condition that
        # affected the previous System.Diagnostics.Process + BeginOutputReadLine
        # approach (async handlers still in-flight after WaitForExit).
        $proc = Start-Process -FilePath $script:CliPath `
            -ArgumentList $argString `
            -RedirectStandardInput $ManifestFile `
            -RedirectStandardOutput $stdoutFile `
            -RedirectStandardError $stderrFile `
            -Wait -NoNewWindow -PassThru
        $exitCode = $proc.ExitCode
    }
    catch {
        $exitCode = -2
    }
    finally {
        $ErrorActionPreference = $prevEAP
    }

    $stdoutStr = ""
    if (Test-Path -Path $stdoutFile -PathType Leaf) {
        $stdoutStr = ([System.IO.File]::ReadAllText($stdoutFile)).TrimEnd()
        Remove-Item -Path $stdoutFile -Force -ErrorAction SilentlyContinue
    }
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
Write-Host "TVS-F.2: batch stdin mode test" -ForegroundColor Cyan
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
$validSrc = @"
{ BatchStdinTest strategy }
Inputs: FastLen(10), SlowLen(20);
Variables: FastMA(0), SlowMA(0);
FastMA = AverageFC(Close, FastLen);
SlowMA = AverageFC(Close, SlowLen);
if FastMA > SlowMA then Buy next bar at Market;
if FastMA < SlowMA then SellShort next bar at Market;
"@

# Invalid EasyLanguage source (compile error).
$badSrc = "This is not valid EasyLanguage !!!"

try {
    # -------------------------------------------------------------------
    # S1: batch-stdin insert — 2 valid + 1 compile-error + 1 missing source
    # -------------------------------------------------------------------
    $s1a = "BatchStdinTest_${runId}_Ins1"; $allTestNames += $s1a
    $s1b = "BatchStdinTest_${runId}_Ins2"; $allTestNames += $s1b
    $s1c = "BatchStdinTest_${runId}_Bad";  $allTestNames += $s1c
    $s1d = "BatchStdinTest_${runId}_NoFile"; $allTestNames += $s1d
    $f1a = Write-SourceFile "s1_ins1" $validSrc
    $f1b = Write-SourceFile "s1_ins2" $validSrc
    $f1Bad = Write-SourceFile "s1_badsrc" $badSrc
    $missingPath = Join-Path $tmpDir "does_not_exist.el"

    $manifestS1 = @(
        "strategy $s1a $f1a",
        "strategy $s1b $f1b",
        "strategy $s1c $f1Bad",
        "strategy $s1d $missingPath"
    )
    $manifestPathS1 = Write-ManifestFile "s1_insert" $manifestS1

    Run-TestCase -TestId "S1-batch-stdin-insert-mixed" -Body {
        $cli = Invoke-CliStdin -ManifestFile $manifestPathS1 -Arguments @("batch-insert", "--batch-stdin")
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

        Assert-True ($arr[1].success -eq $true) "item1 success: expected true"
        Assert-True ($arr[1].prog_id -gt 0) "item1 prog_id: expected >0"

        Assert-True ($arr[2].success -eq $false) "item2 success: expected false"
        Assert-True ($null -ne $arr[2].error.message -and $arr[2].error.message.Length -gt 0) "item2 error.message: expected non-empty"

        Assert-True ($arr[3].success -eq $false) "item3 success: expected false"
    }

    # -------------------------------------------------------------------
    # S2: batch-stdin delete — 1 existing + 1 non-existing
    # -------------------------------------------------------------------
    $s2Existing = $s1a  # inserted in S1
    $s2Missing = "BatchStdinTest_${runId}_DelMissing"
    $manifestS2 = @(
        "strategy $s2Existing",
        "strategy $s2Missing"
    )
    $manifestPathS2 = Write-ManifestFile "s2_delete" $manifestS2

    Run-TestCase -TestId "S2-batch-stdin-delete-mixed" -Body {
        $cli = Invoke-CliStdin -ManifestFile $manifestPathS2 -Arguments @("batch-delete", "--batch-stdin")
        Assert-True ($cli.ExitCode -eq 2) "exit_code: expected 2 (mixed), got $($cli.ExitCode)"
        $arr = $cli.Stdout | ConvertFrom-Json
        Assert-True ($arr -is [array]) "stdout must be a JSON array"
        Assert-True ($arr.Count -eq 2) "array length: expected 2, got $($arr.Count)"
        Assert-True ($arr[0].op -eq 'delete') "item0 op: expected delete"
        Assert-True ($arr[0].success -eq $true) "item0 success: expected true"
        Assert-True ($arr[0].prog_id -gt 0) "item0 prog_id: expected >0"

        Assert-True ($arr[1].success -eq $false) "item1 success: expected false"
        Assert-True ($arr[1].error.code -eq 'not_found') "item1 error.code: expected not_found, got $($arr[1].error.code)"
    }

    # -------------------------------------------------------------------
    # S3: batch-stdin modify — 1 existing (ProgId preserved) + 1 non-existing
    # -------------------------------------------------------------------
    # First insert a component via stdin batch-insert to modify later.
    $s3Mod = "BatchStdinTest_${runId}_Mod"; $allTestNames += $s3Mod
    $fModInit = Write-SourceFile "s3_modinit" $validSrc
    $manifestS3Prep = @("strategy $s3Mod $fModInit")
    $manifestPathS3Prep = Write-ManifestFile "s3_mod_prep" $manifestS3Prep
    $cliPrepS3 = Invoke-CliStdin -ManifestFile $manifestPathS3Prep -Arguments @("batch-insert", "--batch-stdin")
    Assert-True ($cliPrepS3.ExitCode -eq 0) "prep stdin batch-insert for modify test failed: exit $($cliPrepS3.ExitCode)"
    $prepArrS3 = $cliPrepS3.Stdout | ConvertFrom-Json
    Assert-True ($prepArrS3.Count -eq 1) "prep array length: expected 1, got $($prepArrS3.Count)"
    $modProgId = $prepArrS3[0].prog_id

    $fModNew = Write-SourceFile "s3_modnew" ("{ BatchStdinTest modify }`r`nInputs: FastLen(5), SlowLen(15);`r`nVariables: FastMA(0), SlowMA(0);`r`nFastMA = AverageFC(Close, FastLen);`r`nSlowMA = AverageFC(Close, SlowLen);`r`nif FastMA > SlowMA then Buy next bar at Market;`r`nif FastMA < SlowMA then SellShort next bar at Market;")
    $s3ModMissing = "BatchStdinTest_${runId}_ModMissing"
    $manifestS3 = @(
        "strategy $s3Mod $fModNew",
        "strategy $s3ModMissing $fModNew"
    )
    $manifestPathS3 = Write-ManifestFile "s3_modify" $manifestS3

    Run-TestCase -TestId "S3-batch-stdin-modify-mixed" -Body {
        $cli = Invoke-CliStdin -ManifestFile $manifestPathS3 -Arguments @("batch-modify", "--batch-stdin")
        Assert-True ($cli.ExitCode -eq 2) "exit_code: expected 2 (mixed), got $($cli.ExitCode)"
        $arr = $cli.Stdout | ConvertFrom-Json
        Assert-True ($arr -is [array]) "stdout must be a JSON array"
        Assert-True ($arr.Count -eq 2) "array length: expected 2, got $($arr.Count)"
        Assert-True ($arr[0].op -eq 'modify') "item0 op: expected modify"
        Assert-True ($arr[0].success -eq $true) "item0 success: expected true"
        Assert-True ($arr[0].prog_id -gt 0) "item0 prog_id: expected >0"
        # ProgId must be preserved (same as the insert prog_id).
        Assert-True ($arr[0].prog_id -eq $modProgId) "item0 prog_id: expected $modProgId (preserved), got $($arr[0].prog_id)"

        Assert-True ($arr[1].success -eq $false) "item1 success: expected false"
        Assert-True ($arr[1].error.code -eq 'not_found') "item1 error.code: expected not_found, got $($arr[1].error.code)"
    }

    # -------------------------------------------------------------------
    # S4: batch-stdin comments and blank lines are skipped
    # -------------------------------------------------------------------
    $s4a = "BatchStdinTest_${runId}_Cmt1"; $allTestNames += $s4a
    $s4b = "BatchStdinTest_${runId}_Cmt2"; $allTestNames += $s4b
    $f4a = Write-SourceFile "s4_cmt1" $validSrc
    $f4b = Write-SourceFile "s4_cmt2" $validSrc
    $manifestS4 = @(
        "# This is a comment",
        "",
        "   ",
        "strategy $s4a $f4a",
        "# another comment",
        "strategy $s4b $f4b"
    )
    $manifestPathS4 = Write-ManifestFile "s4_comments" $manifestS4

    Run-TestCase -TestId "S4-batch-stdin-comments-blanks" -Body {
        $cli = Invoke-CliStdin -ManifestFile $manifestPathS4 -Arguments @("batch-insert", "--batch-stdin")
        Assert-True ($cli.ExitCode -eq 0) "exit_code: expected 0 (all success), got $($cli.ExitCode)"
        $arr = $cli.Stdout | ConvertFrom-Json
        Assert-True ($arr -is [array]) "stdout must be a JSON array"
        Assert-True ($arr.Count -eq 2) "array length: expected 2 (comments/blanks skipped), got $($arr.Count)"
        Assert-True ($arr[0].name -eq $s4a) "item0 name: expected $s4a, got $($arr[0].name)"
        Assert-True ($arr[1].name -eq $s4b) "item1 name: expected $s4b, got $($arr[1].name)"
        Assert-True ($arr[0].success -eq $true) "item0 success: expected true"
        Assert-True ($arr[1].success -eq $true) "item1 success: expected true"
    }

    # -------------------------------------------------------------------
    # S5: batch-stdin empty manifest (only comments + blank lines)
    # -------------------------------------------------------------------
    $manifestS5 = @(
        "# This manifest has no real items",
        "",
        "   ",
        "# just comments and blanks"
    )
    $manifestPathS5 = Write-ManifestFile "s5_empty" $manifestS5

    Run-TestCase -TestId "S5-batch-stdin-empty" -Body {
        $cli = Invoke-CliStdin -ManifestFile $manifestPathS5 -Arguments @("batch-insert", "--batch-stdin")
        Assert-True ($cli.ExitCode -eq 1) "exit_code: expected 1 (empty manifest), got $($cli.ExitCode)"
        # stdout must NOT be a JSON array (batch did not run).
        $isArr = $false
        try { $t = $cli.Stdout | ConvertFrom-Json; if ($t -is [array]) { $isArr = $true } } catch { }
        Assert-True (-not $isArr) "stdout must NOT be a JSON array when manifest is empty"
        # stderr must contain an error message mentioning "empty".
        Assert-True ($cli.Stderr.Length -gt 0) "stderr must have an error message for empty manifest"
        Assert-True ($cli.Stderr -match 'empty') "stderr should mention 'empty', got: $($cli.Stderr)"
    }

    # -------------------------------------------------------------------
    # S6: backward-compat file-path mode (batch-insert <manifest_file>)
    # -------------------------------------------------------------------
    $s6 = "BatchStdinTest_${runId}_FileMode"; $allTestNames += $s6
    $f6 = Write-SourceFile "s6_filemode" $validSrc
    $manifestS6 = @("strategy $s6 $f6")
    $manifestPathS6 = Write-ManifestFile "s6_filepath" $manifestS6

    Run-TestCase -TestId "S6-batch-stdin-backward-compat-filepath" -Body {
        $cli = Invoke-CliRaw -Arguments @("batch-insert", $manifestPathS6)
        Assert-True ($cli.ExitCode -eq 0) "exit_code: expected 0 (all success), got $($cli.ExitCode)"
        $arr = $cli.Stdout | ConvertFrom-Json
        Assert-True ($arr -is [array]) "stdout must be a JSON array"
        Assert-True ($arr.Count -eq 1) "array length: expected 1, got $($arr.Count)"
        Assert-True ($arr[0].success -eq $true) "item0 success: expected true"
        Assert-True ($arr[0].prog_id -gt 0) "item0 prog_id: expected >0"
    }

    # -------------------------------------------------------------------
    # S7: no manifest path and no --batch-stdin -> usage error (exit 1)
    # -------------------------------------------------------------------
    Run-TestCase -TestId "S7-batch-stdin-no-stdin-flag-rejected" -Body {
        $cli = Invoke-CliRaw -Arguments @("batch-insert")
        Assert-True ($cli.ExitCode -eq 1) "exit_code: expected 1 (usage error), got $($cli.ExitCode)"
        # stdout should NOT be a JSON array.
        $isArr = $false
        try { $t = $cli.Stdout | ConvertFrom-Json; if ($t -is [array]) { $isArr = $true } } catch { }
        Assert-True (-not $isArr) "stdout must NOT be a JSON array for usage error"
    }

    # -------------------------------------------------------------------
    # S8: stdin content identical to file-path mode (equivalence proof)
    #     Same manifest run via --batch-stdin and via file-path mode.
    #     Compare length and per-item success/name/type (ProgIds may differ).
    # -------------------------------------------------------------------
    $s8a = "BatchStdinTest_${runId}_Equiv1"; $allTestNames += $s8a
    $s8b = "BatchStdinTest_${runId}_Equiv2"; $allTestNames += $s8b
    $f8a = Write-SourceFile "s8_equiv1" $validSrc
    $f8b = Write-SourceFile "s8_equiv2" $validSrc
    $manifestS8 = @(
        "strategy $s8a $f8a",
        "strategy $s8b $f8b"
    )
    $manifestPathS8 = Write-ManifestFile "s8_equiv" $manifestS8

    Run-TestCase -TestId "S8-batch-stdin-stdin-content-identical-to-file" -Body {
        # Run via stdin mode.
        $cliStdin = Invoke-CliStdin -ManifestFile $manifestPathS8 -Arguments @("batch-insert", "--batch-stdin")
        Assert-True ($cliStdin.ExitCode -eq 0) "stdin exit_code: expected 0, got $($cliStdin.ExitCode)"
        $arrStdin = $cliStdin.Stdout | ConvertFrom-Json
        Assert-True ($arrStdin -is [array]) "stdin stdout must be a JSON array"
        Assert-True ($arrStdin.Count -eq 2) "stdin array length: expected 2, got $($arrStdin.Count)"

        # Run via file-path mode (same manifest content, different names to
        # avoid already-exists collisions).
        $s8c = "BatchStdinTest_${runId}_Equiv3"; $allTestNames += $s8c
        $s8d = "BatchStdinTest_${runId}_Equiv4"; $allTestNames += $s8d
        $f8c = Write-SourceFile "s8_equiv3" $validSrc
        $f8d = Write-SourceFile "s8_equiv4" $validSrc
        $manifestS8File = @(
            "strategy $s8c $f8c",
            "strategy $s8d $f8d"
        )
        $manifestPathS8File = Write-ManifestFile "s8_equiv_file" $manifestS8File

        $cliFile = Invoke-CliRaw -Arguments @("batch-insert", $manifestPathS8File)
        Assert-True ($cliFile.ExitCode -eq 0) "file exit_code: expected 0, got $($cliFile.ExitCode)"
        $arrFile = $cliFile.Stdout | ConvertFrom-Json
        Assert-True ($arrFile -is [array]) "file stdout must be a JSON array"
        Assert-True ($arrFile.Count -eq 2) "file array length: expected 2, got $($arrFile.Count)"

        # Both arrays must have the same length.
        Assert-True ($arrStdin.Count -eq $arrFile.Count) "length mismatch: stdin=$($arrStdin.Count) file=$($arrFile.Count)"

        # Per-item structural comparison: success, name, type (NOT prog_id,
        # since items are distinct name sets and ProgIds will differ).
        for ($i = 0; $i -lt $arrStdin.Count; $i++) {
            Assert-True ($arrStdin[$i].success -eq $arrFile[$i].success) "item$i success mismatch: stdin=$($arrStdin[$i].success) file=$($arrFile[$i].success)"
            Assert-True ($arrStdin[$i].type -eq $arrFile[$i].type) "item$i type mismatch: stdin=$($arrStdin[$i].type) file=$($arrFile[$i].type)"
            Assert-True ($arrStdin[$i].name -ne $null -and $arrFile[$i].name -ne $null) "item$i name must be non-null in both"
        }
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
Write-Host "==================== BATCH STDIN MODE TEST SUMMARY ====================" -ForegroundColor Cyan
$passed = @($Results | Where-Object { $_.Result -eq 'PASS' }).Count
$total = $Results.Count
Write-Host ("GRAND TOTAL: {0}/{1} passed" -f $passed, $total) -ForegroundColor Cyan

Write-Host ""
if ($failCount -eq 0) {
    Write-Host "ALL BATCH STDIN MODE TESTS PASSED" -ForegroundColor Green
    exit 0
} else {
    Write-Host "$failCount TEST(S) FAILED" -ForegroundColor Red
    exit 1
}
