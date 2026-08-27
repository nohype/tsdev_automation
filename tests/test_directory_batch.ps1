<#
.SYNOPSIS
    TVS-I.2: Directory batch mode test for tsdev_cli.

.DESCRIPTION
    Validates the four directory batch subcommands of tsdev_cli.exe:
      batch-insert-dir  <type> <dir_path> [-t <ext>...]
      batch-modify-dir  <type> <dir_path> [-t <ext>...]
      batch-delete-dir  <type> <dir_path> [-t <ext>...]
      batch-upsert-dir  <type> <dir_path> [-t <ext>...]

    Each subcommand enumerates regular files in <dir_path> matching the
    accepted extensions (default: .el, .txt; overridable via the repeatable
    -t <ext> flag). The file stem (sans last extension) is the item name;
    the file content is the EasyLanguage source (for insert/modify/upsert;
    ignored for delete). The directory batch reuses the same runBatch() /
    emitBatchOutput() machinery as the manifest batch ops.

    Directory batch mode ALWAYS emits a JSON ARRAY on stdout (one element
    per processed file, in enumeration order) and uses exit codes:
      0 = all items succeeded
      1 = usage error / empty dir (no matching files) / missing args /
          non-existent directory (batch did not run)
      2 = batch ran but one or more items failed (incl. unknown type)

    Test cases (all use unique E2E_DirBatchTest_ prefixed names and clean up):
      DB1. batch-insert-dir with .el files: create a temp dir with 3 .el
          files (valid EL strategy source), run batch-insert-dir, verify all
          items inserted (use exists --json to verify each).
      DB2. batch-modify-dir with ProgId preservation: insert items first
          (via batch-insert-dir), then modify via batch-modify-dir with
          updated source files, verify ProgIds preserved (exists --json
          before and after).
      DB3. batch-delete-dir: insert items, then delete via batch-delete-dir,
          verify all deleted (exists returns false).
      DB4. batch-upsert-dir mixed insert/modify: some files are new
          (insert-path), some already exist (modify-path with ProgId
          preservation).
      DB5. -t flag override: create dir with .el, .txt, and .abc files; run
          batch-insert-dir -t el -t abc; verify only .el and .abc files
          processed (not .txt).
      DB6. Empty dir: no matching files -> exit 1, no JSON array on stdout,
          stderr message.
      DB7. Unknown type: batch-insert-dir bogusType <dir> -> exit 2.
      DB8. Non-existent dir: batch-insert-dir strategy <missing_path> ->
          exit 1.
      DB9. -t missing arg: batch-insert-dir strategy <dir> -t (trailing -t
          with no argument) -> exit 1, no JSON array, stderr message.

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

    NOTE: The batch-*-dir subcommands are implemented in tsdev_cli.cpp. This
    suite is an integration/regression check for directory enumeration, batch
    results, and exit-code handling.
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
$tmpDir = Join-Path $ProjectRoot "tmp\directory_batch_test_$runId"

# Results collector.
$Results = New-Object System.Collections.Generic.List[object]
$failCount = 0

# Track all test names created during the run for cleanup.
$CreatedNames = New-Object System.Collections.Generic.List[object]

# ---------------------------------------------------------------------------
# Write-SourceFile: writes EasyLanguage source to a temp file in ASCII
# encoding (UTF-8 BOM breaks the EasyLanguage compiler). Returns the path.
# (Inherited from the shared harness pattern; used here for single-item
# prep inserts.)
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
# Write-DirFile: writes EasyLanguage source to a file with an arbitrary
# name + extension inside a given directory, in ASCII encoding (UTF-8 BOM
# breaks the EasyLanguage compiler). Returns the full path. Used to
# populate directory-batch input directories (file stem = item name).
# ---------------------------------------------------------------------------
function Write-DirFile {
    param(
        [Parameter(Mandatory)] [string]$Directory,
        [Parameter(Mandatory)] [string]$FileName,
        [Parameter(Mandatory)] [string]$Content
    )
    $path = Join-Path $Directory $FileName
    [System.IO.File]::WriteAllText($path, $Content, [System.Text.Encoding]::ASCII)
    return $path
}

# ---------------------------------------------------------------------------
# New-TestDir: creates (force) and returns a fresh subdirectory under tmpDir
# for a given test-case label.
# ---------------------------------------------------------------------------
function New-TestDir {
    param(
        [Parameter(Mandatory)] [string]$Label
    )
    $dir = Join-Path $script:tmpDir $Label
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    return $dir
}

# ---------------------------------------------------------------------------
# Invoke-CliRaw: runs tsdev_cli.exe with the given argument array, capturing
# stdout and stderr SEPARATELY. Returns a hashtable with Stdout, Stderr,
# ExitCode. Used for directory batch subcommands and single-item ops
# (exists/insert/delete).
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
Write-Host "TVS-I.2: directory batch mode test" -ForegroundColor Cyan
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
{ DirBatchTest strategy }
Inputs: FastLen(10), SlowLen(20);
Variables: FastMA(0), SlowMA(0);
FastMA = AverageFC(Close, FastLen);
SlowMA = AverageFC(Close, SlowLen);
if FastMA > SlowMA then Buy next bar at Market;
if FastMA < SlowMA then SellShort next bar at Market;
"@

# Modified strategy source body (different inputs, still valid).
$modSrc = @"
{ DirBatchTest modify }
Inputs: FastLen(5), SlowLen(15);
Variables: FastMA(0), SlowMA(0);
FastMA = AverageFC(Close, FastLen);
SlowMA = AverageFC(Close, SlowLen);
if FastMA > SlowMA then Buy next bar at Market;
if FastMA < SlowMA then SellShort next bar at Market;
"@

try {
    # -------------------------------------------------------------------
    # DB1: batch-insert-dir with .el files
    # -------------------------------------------------------------------
    $db1a = "E2E_DirBatchTest_${runId}_DB1_A"; $allTestNames += $db1a
    $db1b = "E2E_DirBatchTest_${runId}_DB1_B"; $allTestNames += $db1b
    $db1c = "E2E_DirBatchTest_${runId}_DB1_C"; $allTestNames += $db1c

    $dirDb1 = New-TestDir -Label "db1_dir"
    Write-DirFile -Directory $dirDb1 -FileName "$db1a.el" -Content $validSrc | Out-Null
    Write-DirFile -Directory $dirDb1 -FileName "$db1b.el" -Content $validSrc | Out-Null
    Write-DirFile -Directory $dirDb1 -FileName "$db1c.el" -Content $validSrc | Out-Null

    Run-TestCase -TestId "DB1-batch-insert-dir-el-files" -Body {
        $cli = Invoke-CliRaw -Arguments @("batch-insert-dir", "strategy", $dirDb1)
        Assert-True ($cli.ExitCode -eq 0) "exit_code: expected 0 (all success), got $($cli.ExitCode)"
        $arr = $cli.Stdout | ConvertFrom-Json
        Assert-True ($arr -is [array]) "stdout must be a JSON array"
        Assert-True ($arr.Count -eq 3) "array length: expected 3, got $($arr.Count)"
        foreach ($el in $arr) {
            Assert-True ($el.success -eq $true) "item success: expected true for $($el.name)"
            Assert-True ($el.op -eq 'insert') "item op: expected insert for $($el.name), got $($el.op)"
            Assert-True ($null -ne $el.prog_id -and $el.prog_id -gt 0) "item prog_id: expected >0 for $($el.name)"
        }
        # Verify each via exists --json.
        foreach ($nm in @($db1a, $db1b, $db1c)) {
            $ex = Invoke-CliRaw -Arguments @("--json", "exists", "strategy", $nm)
            $j = $ex.Stdout | ConvertFrom-Json
            Assert-True ($j.exists -eq $true) "exists ${nm}: expected true"
            Assert-True ($j.prog_id -gt 0) "exists ${nm} prog_id: expected >0, got $($j.prog_id)"
        }
    }

    # -------------------------------------------------------------------
    # DB2: batch-modify-dir with ProgId preservation
    # -------------------------------------------------------------------
    # Items inserted in DB1 are reused here (db1a, db1b). Capture their
    # ProgIds via exists --json, then modify via batch-modify-dir with
    # updated source files in a fresh directory.
    $db2a = $db1a
    $db2b = $db1b

    $progIdBeforeDb2a = ($null)
    $progIdBeforeDb2b = ($null)
    $exDb2a = Invoke-CliRaw -Arguments @("--json", "exists", "strategy", $db2a)
    $jDb2a = $exDb2a.Stdout | ConvertFrom-Json
    Assert-True ($jDb2a.exists -eq $true) "DB2 prep: $db2a must exist (from DB1)"
    $progIdBeforeDb2a = [int]$jDb2a.prog_id
    $exDb2b = Invoke-CliRaw -Arguments @("--json", "exists", "strategy", $db2b)
    $jDb2b = $exDb2b.Stdout | ConvertFrom-Json
    Assert-True ($jDb2b.exists -eq $true) "DB2 prep: $db2b must exist (from DB1)"
    $progIdBeforeDb2b = [int]$jDb2b.prog_id

    $dirDb2 = New-TestDir -Label "db2_dir"
    Write-DirFile -Directory $dirDb2 -FileName "$db2a.el" -Content $modSrc | Out-Null
    Write-DirFile -Directory $dirDb2 -FileName "$db2b.el" -Content $modSrc | Out-Null

    Run-TestCase -TestId "DB2-batch-modify-dir-progid-preserved" -Body {
        $cli = Invoke-CliRaw -Arguments @("batch-modify-dir", "strategy", $dirDb2)
        Assert-True ($cli.ExitCode -eq 0) "exit_code: expected 0 (all success), got $($cli.ExitCode)"
        $arr = $cli.Stdout | ConvertFrom-Json
        Assert-True ($arr -is [array]) "stdout must be a JSON array"
        Assert-True ($arr.Count -eq 2) "array length: expected 2, got $($arr.Count)"
        foreach ($el in $arr) {
            Assert-True ($el.success -eq $true) "item success: expected true for $($el.name)"
            Assert-True ($el.op -eq 'modify') "item op: expected modify for $($el.name), got $($el.op)"
            Assert-True ($el.prog_id -gt 0) "item prog_id: expected >0 for $($el.name)"
        }
        # ProgId preserved: verify via exists --json after modify.
        $exAfterA = Invoke-CliRaw -Arguments @("--json", "exists", "strategy", $db2a)
        $jAfterA = $exAfterA.Stdout | ConvertFrom-Json
        Assert-True ($jAfterA.exists -eq $true) "exists $db2a after modify: expected true"
        Assert-True ($jAfterA.prog_id -eq $progIdBeforeDb2a) "exists $db2a prog_id: expected $progIdBeforeDb2a (preserved), got $($jAfterA.prog_id)"
        $exAfterB = Invoke-CliRaw -Arguments @("--json", "exists", "strategy", $db2b)
        $jAfterB = $exAfterB.Stdout | ConvertFrom-Json
        Assert-True ($jAfterB.exists -eq $true) "exists $db2b after modify: expected true"
        Assert-True ($jAfterB.prog_id -eq $progIdBeforeDb2b) "exists $db2b prog_id: expected $progIdBeforeDb2b (preserved), got $($jAfterB.prog_id)"
    }

    # -------------------------------------------------------------------
    # DB3: batch-delete-dir
    # -------------------------------------------------------------------
    # Insert fresh items (db1c already exists from DB1; add two new ones),
    # then delete via batch-delete-dir.
    $db3a = $db1c  # exists from DB1
    $db3b = "E2E_DirBatchTest_${runId}_DB3_B"; $allTestNames += $db3b
    $db3c = "E2E_DirBatchTest_${runId}_DB3_C"; $allTestNames += $db3c
    # Pre-insert db3b, db3c via single-item insert.
    $fDb3bPrep = Write-SourceFile "db3b_prep" $validSrc
    $cliDb3bPrep = Invoke-CliRaw -Arguments @("insert", "strategy", $db3b, $fDb3bPrep)
    Assert-True ($cliDb3bPrep.ExitCode -eq 0) "DB3 prep insert $db3b failed: exit $($cliDb3bPrep.ExitCode)"
    $fDb3cPrep = Write-SourceFile "db3c_prep" $validSrc
    $cliDb3cPrep = Invoke-CliRaw -Arguments @("insert", "strategy", $db3c, $fDb3cPrep)
    Assert-True ($cliDb3cPrep.ExitCode -eq 0) "DB3 prep insert $db3c failed: exit $($cliDb3cPrep.ExitCode)"

    $dirDb3 = New-TestDir -Label "db3_dir"
    # delete ignores file content; only the stem (name) matters. Write
    # placeholder files named after each item.
    Write-DirFile -Directory $dirDb3 -FileName "$db3a.el" -Content "placeholder" | Out-Null
    Write-DirFile -Directory $dirDb3 -FileName "$db3b.el" -Content "placeholder" | Out-Null
    Write-DirFile -Directory $dirDb3 -FileName "$db3c.el" -Content "placeholder" | Out-Null

    Run-TestCase -TestId "DB3-batch-delete-dir" -Body {
        $cli = Invoke-CliRaw -Arguments @("batch-delete-dir", "strategy", $dirDb3)
        Assert-True ($cli.ExitCode -eq 0) "exit_code: expected 0 (all success), got $($cli.ExitCode)"
        $arr = $cli.Stdout | ConvertFrom-Json
        Assert-True ($arr -is [array]) "stdout must be a JSON array"
        Assert-True ($arr.Count -eq 3) "array length: expected 3, got $($arr.Count)"
        foreach ($el in $arr) {
            Assert-True ($el.success -eq $true) "item success: expected true for $($el.name)"
            Assert-True ($el.op -eq 'delete') "item op: expected delete for $($el.name), got $($el.op)"
            Assert-True ($el.prog_id -gt 0) "item prog_id: expected >0 for $($el.name)"
        }
        # Verify all deleted via exists --json.
        foreach ($nm in @($db3a, $db3b, $db3c)) {
            $ex = Invoke-CliRaw -Arguments @("--json", "exists", "strategy", $nm)
            $j = $ex.Stdout | ConvertFrom-Json
            Assert-True ($j.exists -eq $false) "exists $nm after delete: expected false"
        }
    }

    # -------------------------------------------------------------------
    # DB4: batch-upsert-dir mixed insert/modify
    # -------------------------------------------------------------------
    # db1a already exists (modify-path, ProgId preserved). db4new is new
    # (insert-path). Both files placed in the same directory.
    $db4mod = $db1a  # exists from DB1/DB2
    $db4ins = "E2E_DirBatchTest_${runId}_DB4_Ins"; $allTestNames += $db4ins

    # Capture ProgId of the existing item (modify-path) before upsert.
    $exDb4mod = Invoke-CliRaw -Arguments @("--json", "exists", "strategy", $db4mod)
    $jDb4mod = $exDb4mod.Stdout | ConvertFrom-Json
    Assert-True ($jDb4mod.exists -eq $true) "DB4 prep: $db4mod must exist"
    $progIdBeforeDb4mod = [int]$jDb4mod.prog_id

    $dirDb4 = New-TestDir -Label "db4_dir"
    Write-DirFile -Directory $dirDb4 -FileName "$db4mod.el" -Content $modSrc | Out-Null
    Write-DirFile -Directory $dirDb4 -FileName "$db4ins.el" -Content $validSrc | Out-Null

    Run-TestCase -TestId "DB4-batch-upsert-dir-mixed" -Body {
        $cli = Invoke-CliRaw -Arguments @("batch-upsert-dir", "strategy", $dirDb4)
        Assert-True ($cli.ExitCode -eq 0) "exit_code: expected 0 (all success), got $($cli.ExitCode)"
        $arr = $cli.Stdout | ConvertFrom-Json
        Assert-True ($arr -is [array]) "stdout must be a JSON array"
        Assert-True ($arr.Count -eq 2) "array length: expected 2, got $($arr.Count)"
        # Locate the modify-path and insert-path elements by name (enumeration
        # order is not guaranteed to match file write order).
        $modEl = $arr | Where-Object { $_.name -eq $db4mod }
        $insEl = $arr | Where-Object { $_.name -eq $db4ins }
        Assert-True ($null -ne $modEl) "expected a modify-path element for $db4mod"
        Assert-True ($null -ne $insEl) "expected an insert-path element for $db4ins"
        # Modify-path: success, op=upsert, ProgId preserved.
        Assert-True ($modEl.success -eq $true) "modify-path success: expected true"
        Assert-True ($modEl.op -eq 'upsert') "modify-path op: expected upsert, got $($modEl.op)"
        Assert-True ($modEl.prog_id -eq $progIdBeforeDb4mod) "modify-path prog_id: expected $progIdBeforeDb4mod (preserved), got $($modEl.prog_id)"
        # Insert-path: success, op=upsert, new ProgId > 0.
        Assert-True ($insEl.success -eq $true) "insert-path success: expected true"
        Assert-True ($insEl.op -eq 'upsert') "insert-path op: expected upsert, got $($insEl.op)"
        Assert-True ($insEl.prog_id -gt 0) "insert-path prog_id: expected >0 (new)"
        # Verify both via exists --json.
        $exMod = Invoke-CliRaw -Arguments @("--json", "exists", "strategy", $db4mod)
        $jMod = $exMod.Stdout | ConvertFrom-Json
        Assert-True ($jMod.exists -eq $true) "exists ${db4mod}: expected true"
        Assert-True ($jMod.prog_id -eq $progIdBeforeDb4mod) "exists ${db4mod} prog_id: expected $progIdBeforeDb4mod (preserved), got $($jMod.prog_id)"
        $exIns = Invoke-CliRaw -Arguments @("--json", "exists", "strategy", $db4ins)
        $jIns = $exIns.Stdout | ConvertFrom-Json
        Assert-True ($jIns.exists -eq $true) "exists ${db4ins}: expected true"
        Assert-True ($jIns.prog_id -eq $insEl.prog_id) "exists ${db4ins} prog_id: expected $($insEl.prog_id), got $($jIns.prog_id)"
    }

    # -------------------------------------------------------------------
    # DB5: -t flag override (only .el and .abc processed, not .txt)
    # -------------------------------------------------------------------
    $db5el  = "E2E_DirBatchTest_${runId}_DB5_El";  $allTestNames += $db5el
    $db5abc = "E2E_DirBatchTest_${runId}_DB5_Abc"; $allTestNames += $db5abc
    $db5txt = "E2E_DirBatchTest_${runId}_DB5_Txt"; $allTestNames += $db5txt

    $dirDb5 = New-TestDir -Label "db5_dir"
    Write-DirFile -Directory $dirDb5 -FileName "$db5el.el"  -Content $validSrc | Out-Null
    Write-DirFile -Directory $dirDb5 -FileName "$db5abc.abc" -Content $validSrc | Out-Null
    Write-DirFile -Directory $dirDb5 -FileName "$db5txt.txt" -Content $validSrc | Out-Null

    Run-TestCase -TestId "DB5-batch-insert-dir-t-flag-override" -Body {
        $cli = Invoke-CliRaw -Arguments @("batch-insert-dir", "strategy", $dirDb5, "-t", "el", "-t", "abc")
        Assert-True ($cli.ExitCode -eq 0) "exit_code: expected 0 (all success), got $($cli.ExitCode)"
        $arr = $cli.Stdout | ConvertFrom-Json
        Assert-True ($arr -is [array]) "stdout must be a JSON array"
        Assert-True ($arr.Count -eq 2) "array length: expected 2 (.el + .abc only), got $($arr.Count)"
        $names = @($arr | ForEach-Object { $_.name })
        Assert-True ($names -contains $db5el)  "expected $db5el in results, got: $($names -join ',')"
        Assert-True ($names -contains $db5abc) "expected $db5abc in results, got: $($names -join ',')"
        Assert-True ($names -notcontains $db5txt) "expected $db5txt (.txt) to be EXCLUDED, got: $($names -join ',')"
        # Verify the two processed items exist; the .txt item must NOT exist.
        $exEl = Invoke-CliRaw -Arguments @("--json", "exists", "strategy", $db5el)
        $jEl = $exEl.Stdout | ConvertFrom-Json
        Assert-True ($jEl.exists -eq $true) "exists ${db5el}: expected true"
        $exAbc = Invoke-CliRaw -Arguments @("--json", "exists", "strategy", $db5abc)
        $jAbc = $exAbc.Stdout | ConvertFrom-Json
        Assert-True ($jAbc.exists -eq $true) "exists ${db5abc}: expected true"
        $exTxt = Invoke-CliRaw -Arguments @("--json", "exists", "strategy", $db5txt)
        $jTxt = $exTxt.Stdout | ConvertFrom-Json
        Assert-True ($jTxt.exists -eq $false) "exists $db5txt (.txt excluded): expected false"
    }

    # -------------------------------------------------------------------
    # DB6: Empty dir -> exit 1, no JSON array, stderr message
    # -------------------------------------------------------------------
    $dirDb6 = New-TestDir -Label "db6_dir"  # empty directory

    Run-TestCase -TestId "DB6-batch-insert-dir-empty-dir" -Body {
        $cli = Invoke-CliRaw -Arguments @("batch-insert-dir", "strategy", $dirDb6)
        Assert-True ($cli.ExitCode -eq 1) "exit_code: expected 1 (empty dir), got $($cli.ExitCode)"
        # stdout must NOT be a JSON array (batch did not run).
        $isArr = $false
        try { $t = $cli.Stdout | ConvertFrom-Json; if ($t -is [array]) { $isArr = $true } } catch { }
        Assert-True (-not $isArr) "stdout must NOT be a JSON array when dir has no matching files"
        # stderr must contain an error message.
        Assert-True ($cli.Stderr.Length -gt 0) "stderr must have an error message for empty dir"
    }

    # -------------------------------------------------------------------
    # DB7: Unknown type -> exit 2
    # -------------------------------------------------------------------
    $dirDb7 = New-TestDir -Label "db7_dir"
    Write-DirFile -Directory $dirDb7 -FileName "db7_dummy.el" -Content $validSrc | Out-Null

    Run-TestCase -TestId "DB7-batch-insert-dir-unknown-type" -Body {
        $cli = Invoke-CliRaw -Arguments @("batch-insert-dir", "bogusType", $dirDb7)
        Assert-True ($cli.ExitCode -eq 2) "exit_code: expected 2 (unknown type), got $($cli.ExitCode)"
        # stdout must NOT be a JSON array.
        $isArr = $false
        try { $t = $cli.Stdout | ConvertFrom-Json; if ($t -is [array]) { $isArr = $true } } catch { }
        Assert-True (-not $isArr) "stdout must NOT be a JSON array for unknown type"
        # stderr must mention "unknown type".
        Assert-True ($cli.Stderr.Length -gt 0) "stderr must have an error message for unknown type"
        Assert-True ($cli.Stderr -match 'unknown type') "stderr should mention 'unknown type', got: $($cli.Stderr)"
    }

    # -------------------------------------------------------------------
    # DB8: Non-existent dir -> exit 1
    # -------------------------------------------------------------------
    $dirDb8 = Join-Path $tmpDir "db8_does_not_exist"

    Run-TestCase -TestId "DB8-batch-insert-dir-nonexistent-dir" -Body {
        $cli = Invoke-CliRaw -Arguments @("batch-insert-dir", "strategy", $dirDb8)
        Assert-True ($cli.ExitCode -eq 1) "exit_code: expected 1 (non-existent dir), got $($cli.ExitCode)"
        # stdout must NOT be a JSON array.
        $isArr = $false
        try { $t = $cli.Stdout | ConvertFrom-Json; if ($t -is [array]) { $isArr = $true } } catch { }
        Assert-True (-not $isArr) "stdout must NOT be a JSON array for non-existent dir"
        # stderr must contain an error message.
        Assert-True ($cli.Stderr.Length -gt 0) "stderr must have an error message for non-existent dir"
    }

    # -------------------------------------------------------------------
    # DB9: -t missing argument -> exit 1, no JSON array, stderr message
    # -------------------------------------------------------------------
    $dirDb9 = New-TestDir -Label "db9_dir"
    Write-DirFile -Directory $dirDb9 -FileName "db9_dummy.el" -Content $validSrc | Out-Null

    Run-TestCase -TestId "DB9-batch-insert-dir-t-missing-arg" -Body {
        $cli = Invoke-CliRaw -Arguments @("batch-insert-dir", "strategy", $dirDb9, "-t")
        Assert-True ($cli.ExitCode -eq 1) "exit_code: expected 1 (-t missing arg), got $($cli.ExitCode)"
        # stdout must NOT be a JSON array (batch did not run).
        $isArr = $false
        try { $t = $cli.Stdout | ConvertFrom-Json; if ($t -is [array]) { $isArr = $true } } catch { }
        Assert-True (-not $isArr) "stdout must NOT be a JSON array when -t has no argument"
        # stderr must contain an error message mentioning -t requires an extension argument.
        Assert-True ($cli.Stderr.Length -gt 0) "stderr must have an error message for -t missing arg"
        Assert-True ($cli.Stderr -match 'requires an extension argument') "stderr should mention '-t requires an extension argument', got: $($cli.Stderr)"
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
Write-Host "==================== DIRECTORY BATCH MODE TEST SUMMARY ====================" -ForegroundColor Cyan
$passed = @($Results | Where-Object { $_.Result -eq 'PASS' }).Count
$total = $Results.Count
Write-Host ("GRAND TOTAL: {0}/{1} passed" -f $passed, $total) -ForegroundColor Cyan

Write-Host ""
if ($failCount -eq 0) {
    Write-Host "ALL DIRECTORY BATCH MODE TESTS PASSED" -ForegroundColor Green
    exit 0
} else {
    Write-Host "$failCount TEST(S) FAILED" -ForegroundColor Red
    exit 1
}
