<#
.SYNOPSIS
    TVS-D.2: --json structured output behavior test for the implemented tsdev_cli.

.DESCRIPTION
    Validates the --json global flag on the single-item insert/delete/modify
    subcommands of tsdev_cli.exe. When --json is present, the CLI must emit
    exactly ONE JSON object on stdout (followed by a newline) and suppress the
    human-readable SUCCESS/ERROR stdout lines. When --json is absent, the
    existing human-readable output must be unchanged (backward compatible).

    Test cases (all use unique JsonTest_ prefixed names and clean up):
      1. insert success (strategy)        -> success=true, exit_code=0, prog_id, ell_path
      2. modify success (strategy)        -> success=true, exit_code=0, prog_id, ell_path
      3. delete success (strategy)        -> success=true, exit_code=0, prog_id, no ell_path
      4. delete not-found (strategy)      -> success=false, exit_code=5, error.code=not_found
      5. insert compile-error (strategy)  -> success=false, exit_code=8, error.code=compile_error, compile_errors
      6. backward compat: insert WITHOUT --json -> human-readable "SUCCESS:" line on stdout, NOT valid JSON
      7. insert already-exists (strategy) -> success=false, exit_code=5, error.code=already_exists, error.existing_prog_id>0
      8. batch-insert: default -> EMPTY stdout + human BATCH completion lines
         on stderr (no BATCH START lines); with --json
         -> stdout carries exactly one JSON array while stderr carries one
         JSON progress event object per line

    This suite is standalone: it is not wired into tests/run_regression.ps1.

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
$tmpDir = Join-Path $ProjectRoot "tmp\json_test_$runId"

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
# stdout and stderr SEPARATELY (both via OS-level redirect files).
# Returns a hashtable with Stdout (string), Stderr (string), ExitCode (int).
#
# Keeping stdout and stderr separate is essential for this test: we must
# assert that stdout contains ONLY the JSON object (no human-readable lines).
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
# Assert-True: records a single boolean assertion. Throws on failure (caught
# by the enclosing test case to record a FAIL).
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
Write-Host "TVS-D.2: --json structured output test" -ForegroundColor Cyan
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

try {
    # -------------------------------------------------------------------
    # Test 1: insert success (strategy) with --json
    # -------------------------------------------------------------------
    $n = "JsonTest_${runId}_Ins"; $allTestNames += $n
    $CreatedNames.Add($n) | Out-Null
    $src = "Inputs: int x(0);`r`nif x > 0 then Buy next bar at market;"
    $f = Write-SourceFile "ins" $src

    Run-TestCase -TestId "J1-insert-success" -Body {
        $cli = Invoke-CliRaw -Arguments @("--json", "insert", "strategy", $n, $f)
        Assert-True ($cli.ExitCode -eq 0) "exit_code: expected 0, got $($cli.ExitCode)"
        # stdout must be exactly one JSON object (parseable).
        $obj = $cli.Stdout | ConvertFrom-Json
        Assert-True ($obj.op -eq 'insert') "op: expected 'insert', got '$($obj.op)'"
        Assert-True ($obj.type -eq 'strategy') "type: expected 'strategy', got '$($obj.type)'"
        Assert-True ($obj.name -eq $n) "name: expected '$n', got '$($obj.name)'"
        Assert-True ($obj.success -eq $true) "success: expected true, got '$($obj.success)'"
        Assert-True ($obj.exit_code -eq 0) "exit_code field: expected 0, got '$($obj.exit_code)'"
        Assert-True ($null -ne $obj.prog_id -and $obj.prog_id -gt 0) "prog_id: expected >0, got '$($obj.prog_id)'"
        Assert-True ($null -ne $obj.ell_path -and $obj.ell_path.Length -gt 0) "ell_path: expected non-empty, got '$($obj.ell_path)'"
        # stdout must NOT contain the human-readable SUCCESS line.
        Assert-True (-not $cli.Stdout.Contains('SUCCESS:')) "stdout must not contain human 'SUCCESS:' line"
        # stdout must be a single line (one JSON object + newline).
        $lineCount = ($cli.Stdout -split "`n").Count
        Assert-True ($lineCount -eq 1) "stdout must be a single line, got $lineCount lines"
    }

    # -------------------------------------------------------------------
    # Test 2: modify success (strategy) with --json
    # -------------------------------------------------------------------
    $src2 = "Inputs: int x(1);`r`nif x > 0 then Sell next bar at market;"
    $f2 = Write-SourceFile "mod" $src2

    Run-TestCase -TestId "J2-modify-success" -Body {
        $cli = Invoke-CliRaw -Arguments @("--json", "modify", "strategy", $n, $f2)
        Assert-True ($cli.ExitCode -eq 0) "exit_code: expected 0, got $($cli.ExitCode)"
        $obj = $cli.Stdout | ConvertFrom-Json
        Assert-True ($obj.op -eq 'modify') "op: expected 'modify', got '$($obj.op)'"
        Assert-True ($obj.type -eq 'strategy') "type: expected 'strategy'"
        Assert-True ($obj.name -eq $n) "name mismatch"
        Assert-True ($obj.success -eq $true) "success: expected true"
        Assert-True ($obj.exit_code -eq 0) "exit_code field: expected 0"
        Assert-True ($null -ne $obj.prog_id -and $obj.prog_id -gt 0) "prog_id: expected >0"
        Assert-True ($null -ne $obj.ell_path -and $obj.ell_path.Length -gt 0) "ell_path: expected non-empty"
        Assert-True (-not $cli.Stdout.Contains('SUCCESS:')) "stdout must not contain human 'SUCCESS:' line"
        $lineCount = ($cli.Stdout -split "`n").Count
        Assert-True ($lineCount -eq 1) "stdout must be a single line, got $lineCount lines"
    }

    # -------------------------------------------------------------------
    # Test 3: delete success (strategy) with --json
    # -------------------------------------------------------------------
    Run-TestCase -TestId "J3-delete-success" -Body {
        $cli = Invoke-CliRaw -Arguments @("--json", "delete", "strategy", $n)
        Assert-True ($cli.ExitCode -eq 0) "exit_code: expected 0, got $($cli.ExitCode)"
        $obj = $cli.Stdout | ConvertFrom-Json
        Assert-True ($obj.op -eq 'delete') "op: expected 'delete', got '$($obj.op)'"
        Assert-True ($obj.type -eq 'strategy') "type: expected 'strategy'"
        Assert-True ($obj.name -eq $n) "name mismatch"
        Assert-True ($obj.success -eq $true) "success: expected true"
        Assert-True ($obj.exit_code -eq 0) "exit_code field: expected 0"
        Assert-True ($null -ne $obj.prog_id -and $obj.prog_id -gt 0) "prog_id: expected >0"
        # delete must NOT have ell_path.
        $hasEll = [bool]($obj.PSObject.Properties.Name -contains 'ell_path')
        Assert-True (-not $hasEll) "delete success must NOT include ell_path"
        Assert-True (-not $cli.Stdout.Contains('SUCCESS:')) "stdout must not contain human 'SUCCESS:' line"
        $lineCount = ($cli.Stdout -split "`n").Count
        Assert-True ($lineCount -eq 1) "stdout must be a single line, got $lineCount lines"
    }

    # -------------------------------------------------------------------
    # Test 4: delete not-found (strategy) with --json
    # -------------------------------------------------------------------
    $nMissing = "JsonTest_${runId}_Missing"
    Run-TestCase -TestId "J4-delete-notfound" -Body {
        $cli = Invoke-CliRaw -Arguments @("--json", "delete", "strategy", $nMissing)
        Assert-True ($cli.ExitCode -eq 5) "exit_code: expected 5, got $($cli.ExitCode)"
        $obj = $cli.Stdout | ConvertFrom-Json
        Assert-True ($obj.op -eq 'delete') "op: expected 'delete'"
        Assert-True ($obj.type -eq 'strategy') "type: expected 'strategy'"
        Assert-True ($obj.name -eq $nMissing) "name mismatch"
        Assert-True ($obj.success -eq $false) "success: expected false"
        Assert-True ($obj.exit_code -eq 5) "exit_code field: expected 5"
        Assert-True ($null -ne $obj.error) "error object must be present"
        Assert-True ($obj.error.code -eq 'not_found') "error.code: expected 'not_found', got '$($obj.error.code)'"
        Assert-True ($obj.error.message.Length -gt 0) "error.message must be non-empty"
        Assert-True (-not $cli.Stdout.Contains('ERROR')) "stdout must not contain human 'ERROR' line"
        $lineCount = ($cli.Stdout -split "`n").Count
        Assert-True ($lineCount -eq 1) "stdout must be a single line, got $lineCount lines"
    }

    # -------------------------------------------------------------------
    # Test 5: insert compile-error (strategy) with --json
    # -------------------------------------------------------------------
    $nBad = "JsonTest_${runId}_Bad"; $allTestNames += $nBad
    $CreatedNames.Add($nBad) | Out-Null
    $badSrc = "this is not valid easylanguage;"
    $fBad = Write-SourceFile "bad" $badSrc

    Run-TestCase -TestId "J5-insert-compile-error" -Body {
        $cli = Invoke-CliRaw -Arguments @("--json", "insert", "strategy", $nBad, $fBad)
        Assert-True ($cli.ExitCode -eq 8) "exit_code: expected 8, got $($cli.ExitCode)"
        $obj = $cli.Stdout | ConvertFrom-Json
        Assert-True ($obj.op -eq 'insert') "op: expected 'insert'"
        Assert-True ($obj.type -eq 'strategy') "type: expected 'strategy'"
        Assert-True ($obj.name -eq $nBad) "name mismatch"
        Assert-True ($obj.success -eq $false) "success: expected false"
        Assert-True ($obj.exit_code -eq 8) "exit_code field: expected 8"
        Assert-True ($null -ne $obj.error) "error object must be present"
        Assert-True ($obj.error.code -eq 'compile_error') "error.code: expected 'compile_error', got '$($obj.error.code)'"
        Assert-True ($obj.error.message.Length -gt 0) "error.message must be non-empty"
        $hasCompileErrors = [bool]($obj.error.PSObject.Properties.Name -contains 'compile_errors')
        Assert-True ($hasCompileErrors) "compile_errors field must be present for compile_error"
        Assert-True ($obj.error.compile_errors.Length -gt 0) "compile_errors must be non-empty"
        $lineCount = ($cli.Stdout -split "`n").Count
        Assert-True ($lineCount -eq 1) "stdout must be a single line, got $lineCount lines"
    }

    # -------------------------------------------------------------------
    # Test 6: backward compatibility — insert WITHOUT --json
    # -------------------------------------------------------------------
    $nBw = "JsonTest_${runId}_Bw"; $allTestNames += $nBw
    $CreatedNames.Add($nBw) | Out-Null
    $bwSrc = "Inputs: int x(0);`r`nif x > 0 then Buy next bar at market;"
    $fBw = Write-SourceFile "bw" $bwSrc

    Run-TestCase -TestId "J6-backcompat-no-json" -Body {
        $cli = Invoke-CliRaw -Arguments @("insert", "strategy", $nBw, $fBw)
        Assert-True ($cli.ExitCode -eq 0) "exit_code: expected 0, got $($cli.ExitCode)"
        # Human-readable SUCCESS line must be present on stdout.
        Assert-True ($cli.Stdout.Contains('SUCCESS:')) "stdout must contain human 'SUCCESS:' line (no --json)"
        Assert-True ($cli.Stdout.Contains('ProgId=')) "stdout must contain 'ProgId=' (no --json)"
        # stdout must NOT be valid JSON (it should be the human line).
        $isJson = $false
        try { $null = $cli.Stdout | ConvertFrom-Json; $isJson = $true } catch { }
        Assert-True (-not $isJson) "stdout must NOT be valid JSON when --json is absent"
    }

    # -------------------------------------------------------------------
    # Test 7: insert already-exists (strategy) with --json
    # -------------------------------------------------------------------
    $nAe = "JsonTest_${runId}_AlreadyExists"; $allTestNames += $nAe
    $CreatedNames.Add($nAe) | Out-Null
    $aeSrc = "Inputs: int x(0);`r`nif x > 0 then Buy next bar at market;"
    $fAe = Write-SourceFile "ae" $aeSrc

    Run-TestCase -TestId "J7-insert-already-exists" -Body {
        # Step 1: insert the component — must succeed.
        $cli1 = Invoke-CliRaw -Arguments @("--json", "insert", "strategy", $nAe, $fAe)
        Assert-True ($cli1.ExitCode -eq 0) "step1 exit_code: expected 0, got $($cli1.ExitCode)"
        $obj1 = $cli1.Stdout | ConvertFrom-Json
        Assert-True ($obj1.success -eq $true) "step1 success: expected true, got '$($obj1.success)'"
        Assert-True ($null -ne $obj1.prog_id -and $obj1.prog_id -gt 0) "step1 prog_id: expected >0, got '$($obj1.prog_id)'"

        # Step 2: insert the SAME component again — must fail with already_exists.
        $cli2 = Invoke-CliRaw -Arguments @("--json", "insert", "strategy", $nAe, $fAe)
        Assert-True ($cli2.ExitCode -eq 5) "step2 exit_code: expected 5, got $($cli2.ExitCode)"
        $obj2 = $cli2.Stdout | ConvertFrom-Json
        Assert-True ($obj2.op -eq 'insert') "step2 op: expected 'insert', got '$($obj2.op)'"
        Assert-True ($obj2.type -eq 'strategy') "step2 type: expected 'strategy', got '$($obj2.type)'"
        Assert-True ($obj2.name -eq $nAe) "step2 name mismatch: expected '$nAe', got '$($obj2.name)'"
        Assert-True ($obj2.success -eq $false) "step2 success: expected false, got '$($obj2.success)'"
        Assert-True ($obj2.exit_code -eq 5) "step2 exit_code field: expected 5, got '$($obj2.exit_code)'"
        Assert-True ($null -ne $obj2.error) "step2 error object must be present"
        Assert-True ($obj2.error.code -eq 'already_exists') "step2 error.code: expected 'already_exists', got '$($obj2.error.code)'"
        $hasExistingProgId = [bool]($obj2.error.PSObject.Properties.Name -contains 'existing_prog_id')
        Assert-True ($hasExistingProgId) "step2 error.existing_prog_id field must be present"
        Assert-True ($obj2.error.existing_prog_id -gt 0) "step2 error.existing_prog_id: expected >0, got '$($obj2.error.existing_prog_id)'"
        Assert-True (-not $cli2.Stdout.Contains('ERROR')) "step2 stdout must not contain human 'ERROR' line"
        $lineCount = ($cli2.Stdout -split "`n").Count
        Assert-True ($lineCount -eq 1) "step2 stdout must be a single line, got $lineCount lines"

        # Step 3: clean up — delete the component.
        $cli3 = Invoke-CliRaw -Arguments @("--json", "delete", "strategy", $nAe)
        Assert-True ($cli3.ExitCode -eq 0) "step3 cleanup delete exit_code: expected 0, got $($cli3.ExitCode)"
    }

    # -------------------------------------------------------------------
    # Test 8: batch mode — stderr progress rendering
    #         Default: EMPTY stdout (the results array is emitted only
    #         under --json); human per-item BATCH completion lines on
    #         stderr (no BATCH START lines) ending at BATCH COMPLETE.
    #         --json: one JSON progress event object per stderr line while
    #         stdout carries exactly one JSON array. The --json run re-inserts
    #         the same manifest so both items fail with already_exists —
    #         failed items still produce ordered progress events.
    # -------------------------------------------------------------------
    $nJ8a = "JsonTest_${runId}_Batch1"; $allTestNames += $nJ8a
    $nJ8b = "JsonTest_${runId}_Batch2"; $allTestNames += $nJ8b
    $fJ8a = Write-SourceFile "batch1" $src
    $fJ8b = Write-SourceFile "batch2" $src
    $manifestJ8 = Write-ManifestFile "batchj8" @(
        "strategy $nJ8a $fJ8a",
        "strategy $nJ8b $fJ8b"
    )

    Run-TestCase -TestId "J8-batch-stderr-progress" -Body {
        # Default (no --json): human progress lines on stderr, EMPTY stdout.
        # Hand-built expected results stand in for the absent stdout array.
        $cliHuman = Invoke-CliRaw -Arguments @("batch-insert", $manifestJ8)
        Assert-True ($cliHuman.ExitCode -eq 0) "human exit_code: expected 0, got $($cliHuman.ExitCode)"
        Assert-True ($cliHuman.Stdout.Trim() -eq '') "human stdout must be EMPTY without --json; got: $($cliHuman.Stdout)"
        $expected = @(
            [pscustomobject]@{ op='insert'; type='strategy'; name=$nJ8a; success=$true; exit_code=0; error=$null },
            [pscustomobject]@{ op='insert'; type='strategy'; name=$nJ8b; success=$true; exit_code=0; error=$null }
        )
        Assert-BatchStderrProgress -Stderr $cliHuman.Stderr -Results $expected -ExpectedExitCode 0

        # --json: stdout is still exactly one JSON array; stderr carries one
        # JSON event object per line (no human BATCH lines).
        $cli = Invoke-CliRaw -Arguments @("--json", "batch-insert", $manifestJ8)
        Assert-True ($cli.ExitCode -eq 2) "exit_code: expected 2 (already_exists), got $($cli.ExitCode)"
        $arr = [array]($cli.Stdout | ConvertFrom-Json)
        Assert-True ($arr.Count -eq 2) "array length: expected 2, got $($arr.Count)"
        $lineCount = ($cli.Stdout -split "`n").Count
        Assert-True ($lineCount -eq 1) "stdout must be a single line (one JSON array), got $lineCount lines"
        Assert-True ($arr[0].error.code -eq 'already_exists') "item0 error.code: expected already_exists, got $($arr[0].error.code)"
        Assert-True ($arr[1].error.code -eq 'already_exists') "item1 error.code: expected already_exists, got $($arr[1].error.code)"
        Assert-True ($cli.Stderr -cnotmatch 'BATCH ') "stderr must not contain human BATCH lines under --json, got: $($cli.Stderr)"
        Assert-BatchJsonProgress -Stderr $cli.Stderr -Results $arr -ExpectedExitCode 2
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
Write-Host "==================== JSON OUTPUT TEST SUMMARY ====================" -ForegroundColor Cyan
$passed = @($Results | Where-Object { $_.Result -eq 'PASS' }).Count
$total = $Results.Count
Write-Host ("GRAND TOTAL: {0}/{1} passed" -f $passed, $total) -ForegroundColor Cyan

Write-Host ""
if ($failCount -eq 0) {
    Write-Host "ALL JSON OUTPUT TESTS PASSED" -ForegroundColor Green
    exit 0
} else {
    Write-Host "$failCount TEST(S) FAILED" -ForegroundColor Red
    exit 1
}
