<#
.SYNOPSIS
    TVS-I.1: Batch upsert mode test for tsdev_cli.

.DESCRIPTION
    Validates the `batch-upsert` subcommand of tsdev_cli.exe in both manifest
    file mode (`batch-upsert <manifest_file>`) and stdin mode
    (`batch-upsert --batch-stdin`). Batch-upsert inserts an item if it does not
    exist, or modifies it if it does (insert-or-update by name + component type
    + source file), reusing the existing doUpsert() core logic but with
    flushPerItem=false (single closeWarehouse at the end, like the other batch
    ops).

    Each manifest line is `<type> <name> <source_file>` (upsert needs source,
    like insert/modify). Per-item `op` in JSON output is `"upsert"`.

    Batch mode ALWAYS emits a JSON ARRAY on stdout (one element per manifest
    item, in manifest order) and uses exit codes:
      0 = all items succeeded
      1 = usage error / manifest empty / missing args (batch did not run)
      2 = batch ran but one or more items failed

    Test cases (all use unique E2E_BatchUpsertTest_ prefixed names and clean up):
      BU1. batch-upsert via MANIFEST FILE with mixed insert-path + modify-path
          items. Pre-insert one strategy via single-item `insert` (capture
          ProgId). Build a manifest with 2 lines: (a) the pre-inserted name +
          modified source (modify-path, ProgId preserved), (b) a NEW name +
          source (insert-path, new ProgId). Assert exit 0, 2 elements, both
          success=true, element[0] prog_id == pre-inserted ProgId (preserved),
          element[1] prog_id > 0 (new). Verify both via `exists`.
      BU2. batch-upsert via `--batch-stdin` with the same mixed scenario
          (different probe names). Assert same invariants.
      BU3. batch-upsert with a compile-error item. Manifest with 1 valid item +
          1 invalid-EL-source item. Assert exit 2, 2 elements, element[0]
          success=true, element[1] success=false with error.code == "compile_error".
      BU4. batch-upsert empty manifest (only comments/blanks). Assert exit 1,
          no JSON array on stdout, stderr mentions "empty".
      BU5. batch-upsert with no manifest path and no `--batch-stdin` (wrong arg
          count). Assert exit 1, stderr mentions "requires exactly 1 argument".

    Exit codes (this script):
      0 = all assertions passed
      1 = one or more assertions failed (or a prerequisite was not met)

    Prerequisites:
      - The project must be built: cmake --build build --config Release --target tsdev_cli
      - TradeStation 9.5 must be installed (or -TsPath provided)
      - The invoking PowerShell process must be an elevated administrator
        process because this suite invokes tsdev_cli.exe directly. A
        non-elevated direct CLI invocation returns exit code 11 (not_elevated).
      - No general TradeStation Desktop/TSDev running-state prerequisite
        applies; this suite does not use the Desktop UI

    E2E test suite for the `batch-upsert` CLI subcommand (manifest file +
    --batch-stdin modes).
#>

param(
    [string]$CliPath = (Join-Path $PSScriptRoot "..\build\Release\tsdev_cli.exe"),
    [string]$TsPath,
    [switch]$Verbose
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Shared test helpers (batch progress event assertions).
# ---------------------------------------------------------------------------
Import-Module (Join-Path $PSScriptRoot "TestHelpers.psm1") -ErrorAction Stop -DisableNameChecking

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
$tmpDir = Join-Path $ProjectRoot "tmp\batch_upsert_test_$runId"

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
# ExitCode. Used for file-path mode (batch-upsert <manifest_file>) and
# single-item ops (exists/insert/delete/upsert).
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

    $exitCode = -2
    $launchError = ''
    $stdoutStr = ''
    $stderrStr = ''
    try {
        $prevEAP = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            # OS-level redirection writes the child's raw stdout/stderr bytes
            # to files. `2>` on a native command wraps stderr in ErrorRecords —
            # prefixing the first line and injecting error-record text — which
            # corrupts line-exact assertions on batch progress output.
            $proc = Start-Process -FilePath $script:CliPath `
                -ArgumentList $argString `
                -RedirectStandardOutput $stdoutFile `
                -RedirectStandardError $stderrFile `
                -Wait -NoNewWindow -PassThru
            $exitCode = $proc.ExitCode
        }
        catch {
            # Launch/wait failure only: record it for stderr; never
            # fabricate stdout or overwrite a real exit code.
            $launchError = $_.Exception.Message
        }
        finally {
            $ErrorActionPreference = $prevEAP
        }

        if (Test-Path -Path $stdoutFile -PathType Leaf) {
            $stdoutStr = ([System.IO.File]::ReadAllText($stdoutFile)).TrimEnd()
        }
        if (Test-Path -Path $stderrFile -PathType Leaf) {
            $stderrStr = ([System.IO.File]::ReadAllText($stderrFile)).TrimEnd()
        }
        if ($launchError) {
            if ($stderrStr) { $stderrStr += "`n" }
            $stderrStr += $launchError
        }
    }
    finally {
        foreach ($f in @($stdoutFile, $stderrFile)) {
            if (Test-Path -Path $f -PathType Leaf) {
                Remove-Item -Path $f -Force -ErrorAction SilentlyContinue
            }
        }
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
# Cleanup: re-delete every test name across all types (expecting exit 0 or 5).
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
Write-Host "TVS-I.1: batch upsert mode test" -ForegroundColor Cyan
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
{ BatchUpsertTest strategy }
Inputs: FastLen(10), SlowLen(20);
Variables: FastMA(0), SlowMA(0);
FastMA = AverageFC(Close, FastLen);
SlowMA = AverageFC(Close, SlowLen);
if FastMA > SlowMA then Buy next bar at Market;
if FastMA < SlowMA then SellShort next bar at Market;
"@

# Modified strategy source body (different body, still valid).
$modSrcBody = @"
{ BatchUpsertTest modify }
Inputs: FastLen(5), SlowLen(15);
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
    # BU1: batch-upsert via MANIFEST FILE — mixed insert-path + modify-path
    # -------------------------------------------------------------------
    $bu1Mod = "E2E_BatchUpsertTest_${runId}_BU1_Mod"; $allTestNames += $bu1Mod
    $bu1Ins = "E2E_BatchUpsertTest_${runId}_BU1_Ins"; $allTestNames += $bu1Ins

    # Pre-insert one strategy via single-item `insert` (capture its ProgId).
    $fBu1Init = Write-SourceFile "bu1_init" $validSrc
    $cliBu1Prep = Invoke-CliRaw -Arguments @("insert", "strategy", $bu1Mod, $fBu1Init)
    Assert-True ($cliBu1Prep.ExitCode -eq 0) "BU1 prep insert failed: exit $($cliBu1Prep.ExitCode)"
    $bu1ModProgId = $null
    if ($cliBu1Prep.Stdout -match 'ProgId=(\d+)') {
        $bu1ModProgId = [int]$Matches[1]
    }
    Assert-True ($null -ne $bu1ModProgId -and $bu1ModProgId -gt 0) "BU1 prep: could not extract ProgId from insert output"

    # Build manifest: (a) pre-inserted name + modified source (modify-path),
    # (b) NEW name + source (insert-path).
    $fBu1Mod = Write-SourceFile "bu1_modsrc" $modSrcBody
    $fBu1Ins = Write-SourceFile "bu1_inssrc" $validSrc
    $manifestBu1 = @(
        "strategy $bu1Mod $fBu1Mod",
        "strategy $bu1Ins $fBu1Ins"
    )
    $manifestPathBu1 = Write-ManifestFile "bu1_manifest" $manifestBu1

    Run-TestCase -TestId "BU1-batch-upsert-file-mixed" -Body {
        $cli = Invoke-CliRaw -Arguments @("batch-upsert", $manifestPathBu1)
        Assert-True ($cli.ExitCode -eq 0) "exit_code: expected 0 (all success), got $($cli.ExitCode)"
        $arr = $cli.Stdout | ConvertFrom-Json
        Assert-True ($arr -is [array]) "stdout must be a JSON array"
        Assert-True ($arr.Count -eq 2) "array length: expected 2, got $($arr.Count)"
        # Element 0: modify-path (pre-inserted), ProgId preserved.
        Assert-True ($arr[0].success -eq $true) "item0 success: expected true"
        Assert-True ($arr[0].op -eq 'upsert') "item0 op: expected upsert, got $($arr[0].op)"
        Assert-True ($arr[0].prog_id -gt 0) "item0 prog_id: expected >0"
        Assert-True ($arr[0].prog_id -eq $bu1ModProgId) "item0 prog_id: expected $bu1ModProgId (preserved), got $($arr[0].prog_id)"
        # Element 1: insert-path (new), new ProgId.
        Assert-True ($arr[1].success -eq $true) "item1 success: expected true"
        Assert-True ($arr[1].op -eq 'upsert') "item1 op: expected upsert, got $($arr[1].op)"
        Assert-True ($arr[1].prog_id -gt 0) "item1 prog_id: expected >0 (new)"

        # Verify both via exists subcommand.
        $exists0 = Invoke-CliRaw -Arguments @("--json", "exists", "strategy", $bu1Mod)
        $j0 = $exists0.Stdout | ConvertFrom-Json
        Assert-True ($j0.exists -eq $true) "exists item0: expected true"
        Assert-True ($j0.prog_id -eq $bu1ModProgId) "exists item0 prog_id: expected $bu1ModProgId, got $($j0.prog_id)"

        $exists1 = Invoke-CliRaw -Arguments @("--json", "exists", "strategy", $bu1Ins)
        $j1 = $exists1.Stdout | ConvertFrom-Json
        Assert-True ($j1.exists -eq $true) "exists item1: expected true"
        Assert-True ($j1.prog_id -eq $arr[1].prog_id) "exists item1 prog_id: expected $($arr[1].prog_id), got $($j1.prog_id)"
        Assert-BatchStderrProgress -Stderr $cli.Stderr -Results $arr -ExpectedExitCode 0
    }

    # -------------------------------------------------------------------
    # BU2: batch-upsert via --batch-stdin — mixed insert-path + modify-path
    # -------------------------------------------------------------------
    $bu2Mod = "E2E_BatchUpsertTest_${runId}_BU2_Mod"; $allTestNames += $bu2Mod
    $bu2Ins = "E2E_BatchUpsertTest_${runId}_BU2_Ins"; $allTestNames += $bu2Ins

    # Pre-insert one strategy via single-item `insert` (capture its ProgId).
    $fBu2Init = Write-SourceFile "bu2_init" $validSrc
    $cliBu2Prep = Invoke-CliRaw -Arguments @("insert", "strategy", $bu2Mod, $fBu2Init)
    Assert-True ($cliBu2Prep.ExitCode -eq 0) "BU2 prep insert failed: exit $($cliBu2Prep.ExitCode)"
    $bu2ModProgId = $null
    if ($cliBu2Prep.Stdout -match 'ProgId=(\d+)') {
        $bu2ModProgId = [int]$Matches[1]
    }
    Assert-True ($null -ne $bu2ModProgId -and $bu2ModProgId -gt 0) "BU2 prep: could not extract ProgId from insert output"

    # Build manifest for stdin mode.
    $fBu2Mod = Write-SourceFile "bu2_modsrc" $modSrcBody
    $fBu2Ins = Write-SourceFile "bu2_inssrc" $validSrc
    $manifestBu2 = @(
        "strategy $bu2Mod $fBu2Mod",
        "strategy $bu2Ins $fBu2Ins"
    )
    $manifestPathBu2 = Write-ManifestFile "bu2_manifest" $manifestBu2

    Run-TestCase -TestId "BU2-batch-upsert-stdin-mixed" -Body {
        $cli = Invoke-CliStdin -ManifestFile $manifestPathBu2 -Arguments @("batch-upsert", "--batch-stdin")
        Assert-True ($cli.ExitCode -eq 0) "exit_code: expected 0 (all success), got $($cli.ExitCode)"
        $arr = $cli.Stdout | ConvertFrom-Json
        Assert-True ($arr -is [array]) "stdout must be a JSON array"
        Assert-True ($arr.Count -eq 2) "array length: expected 2, got $($arr.Count)"
        # Element 0: modify-path (pre-inserted), ProgId preserved.
        Assert-True ($arr[0].success -eq $true) "item0 success: expected true"
        Assert-True ($arr[0].op -eq 'upsert') "item0 op: expected upsert, got $($arr[0].op)"
        Assert-True ($arr[0].prog_id -gt 0) "item0 prog_id: expected >0"
        Assert-True ($arr[0].prog_id -eq $bu2ModProgId) "item0 prog_id: expected $bu2ModProgId (preserved), got $($arr[0].prog_id)"
        # Element 1: insert-path (new), new ProgId.
        Assert-True ($arr[1].success -eq $true) "item1 success: expected true"
        Assert-True ($arr[1].op -eq 'upsert') "item1 op: expected upsert, got $($arr[1].op)"
        Assert-True ($arr[1].prog_id -gt 0) "item1 prog_id: expected >0 (new)"

        # Verify both via exists subcommand.
        $exists0 = Invoke-CliRaw -Arguments @("--json", "exists", "strategy", $bu2Mod)
        $j0 = $exists0.Stdout | ConvertFrom-Json
        Assert-True ($j0.exists -eq $true) "exists item0: expected true"
        Assert-True ($j0.prog_id -eq $bu2ModProgId) "exists item0 prog_id: expected $bu2ModProgId, got $($j0.prog_id)"

        $exists1 = Invoke-CliRaw -Arguments @("--json", "exists", "strategy", $bu2Ins)
        $j1 = $exists1.Stdout | ConvertFrom-Json
        Assert-True ($j1.exists -eq $true) "exists item1: expected true"
        Assert-True ($j1.prog_id -eq $arr[1].prog_id) "exists item1 prog_id: expected $($arr[1].prog_id), got $($j1.prog_id)"
        Assert-BatchStderrProgress -Stderr $cli.Stderr -Results $arr -ExpectedExitCode 0

        # --json selects JSON progress rendering on stderr; stdout unchanged.
        # Re-running the same manifest exercises the modify-path for both.
        $cliJson = Invoke-CliStdin -ManifestFile $manifestPathBu2 -Arguments @("--json", "batch-upsert", "--batch-stdin")
        Assert-True ($cliJson.ExitCode -eq 0) "json exit_code: expected 0, got $($cliJson.ExitCode)"
        $arrJson = [array]($cliJson.Stdout | ConvertFrom-Json)
        Assert-True ($arrJson.Count -eq 2) "json array length: expected 2, got $($arrJson.Count)"
        Assert-True (($cliJson.Stdout -split "`n").Count -eq 1) "json stdout must be exactly one JSON array line, got: $($cliJson.Stdout)"
        Assert-True ($cliJson.Stderr -cnotmatch 'BATCH ') "json stderr must not contain human BATCH lines, got: $($cliJson.Stderr)"
        Assert-BatchJsonProgress -Stderr $cliJson.Stderr -Results $arrJson -ExpectedExitCode 0
    }

    # -------------------------------------------------------------------
    # BU3: batch-upsert with a compile-error item
    # -------------------------------------------------------------------
    $bu3a = "E2E_BatchUpsertTest_${runId}_BU3_Ok";   $allTestNames += $bu3a
    $bu3b = "E2E_BatchUpsertTest_${runId}_BU3_Bad";  $allTestNames += $bu3b
    $fBu3Ok = Write-SourceFile "bu3_ok" $validSrc
    $fBu3Bad = Write-SourceFile "bu3_bad" $badSrc
    $manifestBu3 = @(
        "strategy $bu3a $fBu3Ok",
        "strategy $bu3b $fBu3Bad"
    )
    $manifestPathBu3 = Write-ManifestFile "bu3_manifest" $manifestBu3

    Run-TestCase -TestId "BU3-batch-upsert-compile-error" -Body {
        $cli = Invoke-CliRaw -Arguments @("batch-upsert", $manifestPathBu3)
        Assert-True ($cli.ExitCode -eq 2) "exit_code: expected 2 (mixed), got $($cli.ExitCode)"
        $arr = $cli.Stdout | ConvertFrom-Json
        Assert-True ($arr -is [array]) "stdout must be a JSON array"
        Assert-True ($arr.Count -eq 2) "array length: expected 2, got $($arr.Count)"
        Assert-True ($arr[0].success -eq $true) "item0 success: expected true"
        Assert-True ($arr[0].op -eq 'upsert') "item0 op: expected upsert, got $($arr[0].op)"
        Assert-True ($arr[1].success -eq $false) "item1 success: expected false"
        Assert-True ($null -ne $arr[1].error -and $null -ne $arr[1].error.code) "item1 error.code: expected non-null"
        Assert-True ($arr[1].error.code -eq 'compile_error') "item1 error.code: expected compile_error, got $($arr[1].error.code)"
        Assert-BatchStderrProgress -Stderr $cli.Stderr -Results $arr -ExpectedExitCode 2
    }

    # -------------------------------------------------------------------
    # BU4: batch-upsert empty manifest (only comments/blanks)
    # -------------------------------------------------------------------
    $manifestBu4 = @(
        "# This manifest has no real items",
        "",
        "   ",
        "# just comments and blanks"
    )
    $manifestPathBu4 = Write-ManifestFile "bu4_empty" $manifestBu4

    Run-TestCase -TestId "BU4-batch-upsert-empty" -Body {
        $cli = Invoke-CliRaw -Arguments @("batch-upsert", $manifestPathBu4)
        Assert-True ($cli.ExitCode -eq 1) "exit_code: expected 1 (empty manifest), got $($cli.ExitCode)"
        # stdout must NOT be a JSON array (batch did not run).
        $isArr = $false
        try { $t = $cli.Stdout | ConvertFrom-Json; if ($t -is [array]) { $isArr = $true } } catch { }
        Assert-True (-not $isArr) "stdout must NOT be a JSON array when manifest is empty"
        # stderr must contain an error message mentioning "empty".
        Assert-True ($cli.Stderr.Length -gt 0) "stderr must have an error message for empty manifest"
        Assert-True ($cli.Stderr -match 'empty') "stderr should mention 'empty', got: $($cli.Stderr)"
        Assert-True ($cli.Stderr -cnotmatch 'BATCH ') "stderr must not contain batch progress events when the batch did not run, got: $($cli.Stderr)"
    }

    # -------------------------------------------------------------------
    # BU5: batch-upsert with no manifest path and no --batch-stdin (usage)
    # -------------------------------------------------------------------
    Run-TestCase -TestId "BU5-batch-upsert-no-args-rejected" -Body {
        $cli = Invoke-CliRaw -Arguments @("batch-upsert")
        Assert-True ($cli.ExitCode -eq 1) "exit_code: expected 1 (usage error), got $($cli.ExitCode)"
        # stdout should NOT be a JSON array.
        $isArr = $false
        try { $t = $cli.Stdout | ConvertFrom-Json; if ($t -is [array]) { $isArr = $true } } catch { }
        Assert-True (-not $isArr) "stdout must NOT be a JSON array for usage error"
        # stderr must mention "requires exactly 1 argument".
        Assert-True ($cli.Stderr.Length -gt 0) "stderr must have an error message for usage error"
        Assert-True ($cli.Stderr -match 'requires exactly 1 argument') "stderr should mention 'requires exactly 1 argument', got: $($cli.Stderr)"
        Assert-True ($cli.Stderr -cnotmatch 'BATCH ') "stderr must not contain batch progress events when the batch did not run, got: $($cli.Stderr)"
    }

}
catch {
    # An error outside any Run-TestCase body: count it once, report it, and
    # still fall through to the finally cleanup and the summary/exit code.
    $script:failCount++
    Write-Host "[FAIL] UNHANDLED ERROR: $($_.Exception.Message)" -ForegroundColor Red
}
finally {
    # Cleanup all created components (re-delete across all types).
    Test-Cleanup -AllNames $allTestNames
}

# ===========================================================================
# Summary
# ===========================================================================
Write-Host ""
Write-Host "==================== BATCH UPSERT MODE TEST SUMMARY ====================" -ForegroundColor Cyan
$passed = @($Results | Where-Object { $_.Result -eq 'PASS' }).Count
$total = $Results.Count
Write-Host ("GRAND TOTAL: {0}/{1} passed" -f $passed, $total) -ForegroundColor Cyan

Write-Host ""
if ($failCount -eq 0) {
    Write-Host "ALL BATCH UPSERT MODE TESTS PASSED" -ForegroundColor Green
    exit 0
} else {
    Write-Host "$failCount TEST(S) FAILED" -ForegroundColor Red
    exit 1
}
