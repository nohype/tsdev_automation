<#
.SYNOPSIS
    Opt-in REPEATED-INSTANCE lifecycle stress test for TVS-L.5 Unknown Link defect.

.DESCRIPTION
    This is an opt-in extended stress test for TVS-L.5, required to run once
    at the end of every TVS development cycle. It is NOT part of the always-green
    regression suite.

    SCOPE -- what this test IS and IS NOT:
      This is a REPEATED-INSTANCE lifecycle stress test. Each cycle spawns a
      FRESH tsdev_cli.exe process (one process per insert, one per delete), so
      every cycle starts from a clean process: a brand-new TsDevLib, a fresh
      COM apartment, fresh Win32 handles, and a fresh hidden EDIT window. It
      exercises the insert/delete lifecycle and artifact-finalization
      correctness across many independent invocations and all 5 component
      types.

      This test does NOT prove the absence of long-lived COM / handle / window
      leaks. Because each cycle is a separate short-lived process, no single
      process lives long enough for in-process resource growth to be observed.
      Long-lived single-process leak detection is the domain of
      tests/test_stress_batch.cpp (TVS-L.15), which constructs ONE TsDevLib
      instance in ONE process, performs N ops against it, and samples
      process handle count, GDI/User objects, hidden-window count, and
      working-set at start/mid/end to assert no unbounded growth. Do not read
      a clean Suite 14 run as evidence that a long-lived broker/batch process
      is leak-free.

    The test performs 600 insert/delete lifecycle cycles:
      120 cycles per type x 5 component types
      (Strategy, Indicator, Function, TradingApp, ShowMe).

    Each cycle asserts:
      - Insert succeeds (exit 0, JSON success=true, prog_id > 0)
      - ELL artifact exists after insert
      - XML artifact exists after insert
      - Delete succeeds (exit 0)
      - ELL artifact is gone after delete
      - XML artifact is gone after delete
      - No TSCrashReport or WerFault processes spawned

    Progress is printed every 50 cycles. Crash-reporter detection is also
    performed every 50 cycles and aborts the run immediately if found.

    Exit codes:
      0 = all 600 cycles passed
      1 = one or more cycles failed (or the run was aborted)
      2 = prerequisite error (not elevated, CLI missing, MyWork\Bin missing)

.PARAMETER CliPath
    Path to the tsdev_cli.exe executable. Defaults to
    "$PSScriptRoot\..\build\Release\tsdev_cli.exe".

.PARAMETER TsPath
    Optional. TradeStation install directory passed to the CLI as
    "--ts-path <dir>" BEFORE the subcommand. When supplied, MyWork\Bin is
    derived as <TsPath>\MyWork\Bin unless -BinPath is also supplied.

.PARAMETER BinPath
    Optional. Explicit override for the MyWork\Bin directory used for .ell/.xml
    artifact checks.

.PARAMETER Verbose
    Switch. When set, print full CLI output for every step.

.NOTES
    This script must be run from an elevated (administrator) PowerShell prompt.
#>

param(
    [string]$CliPath = (Join-Path $PSScriptRoot "..\build\Release\tsdev_cli.exe"),
    [string]$TsPath,
    [string]$BinPath,
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

if ($BinPath) {
    $MyWorkBin = $BinPath
} elseif ($TsPath) {
    $MyWorkBin = Join-Path $TsPath "MyWork\Bin"
} else {
    $MyWorkBin = "C:\Program Files (x86)\TradeStation 9.5\MyWork\Bin"
}

$runId  = Get-Date -Format "yyyyMMdd_HHmmss"
$tmpDir = Join-Path $ProjectRoot "tmp\e2e_l5_stress_$runId"

# ---------------------------------------------------------------------------
# Component type mappings for the 5 supported CLI types.
# ---------------------------------------------------------------------------
$TypeInfo = @{
    strategy   = @{ cli='strategy';   whType=0;    prefix='elmodule_0_'    }
    indicator  = @{ cli='indicator';  whType=2;    prefix='elmodule_2_'    }
    function   = @{ cli='function';   whType=4;    prefix='elmodule_4_'    }
    tradingapp = @{ cli='tradingapp'; whType=5122; prefix='elmodule_5122_' }
    showme     = @{ cli='showme';     whType=1282; prefix='elmodule_1282_' }
}

# ---------------------------------------------------------------------------
# Elevation check.
# ---------------------------------------------------------------------------
function Test-IsElevated {
    $id = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object System.Security.Principal.WindowsPrincipal($id)
    return $principal.IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)
}

# ---------------------------------------------------------------------------
# Write EasyLanguage source to a temp file (ASCII, no BOM).
# ---------------------------------------------------------------------------
function Write-SourceFile {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string]$Content
    )
    if (-not (Test-Path -Path $script:tmpDir -PathType Container)) {
        New-Item -ItemType Directory -Force -Path $script:tmpDir | Out-Null
    }
    $path = Join-Path $script:tmpDir "$Name.txt"
    [System.IO.File]::WriteAllText($path, $Content, [System.Text.Encoding]::ASCII)
    return $path
}

# ---------------------------------------------------------------------------
# Invoke the production CLI, capturing stdout, stderr, and exit code.
# ---------------------------------------------------------------------------
function Invoke-Cli {
    param([Parameter(Mandatory)] [string[]]$Arguments)

    $fullArgs = @()
    if ($script:TsPath) { $fullArgs += @("--ts-path", $script:TsPath) }
    $fullArgs += $Arguments

    $stderrFile = Join-Path $script:tmpDir "stderr_$([System.Guid]::NewGuid().ToString('N')).txt"

    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $stdout = & $script:CliPath @fullArgs 2>$stderrFile
        $exitCode = $LASTEXITCODE
    } catch {
        $stdout = $_.Exception.Message
        $exitCode = -2
    } finally {
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
# Source fixture helpers.
# ---------------------------------------------------------------------------
function Get-ValidInsertSource {
    param(
        [Parameter(Mandatory)] [string]$Type,
        [Parameter(Mandatory)] [string]$Name
    )
    switch ($Type.ToLower()) {
        'strategy'   { return "Inputs: int x(0);`r`nif x > 0 then Buy next bar at market;" }
        'indicator'  { return "Inputs: int x(0);`r`nPlot1(x, `"MyPlot`");" }
        'function'   { return "$Name = 1;" }
        'tradingapp' { return "using elsystem;" }
        'showme'     { return "Inputs: int x(0);`r`nPlot1(Close, `"MyPlot`");" }
        default { throw "Get-ValidInsertSource: unknown type $Type" }
    }
}

# ---------------------------------------------------------------------------
# Crash-reporter detection / termination.
# ---------------------------------------------------------------------------
function Get-CrashProcesses {
    return Get-Process | Where-Object { $_.ProcessName -match 'TSCrashReport|WerFault' }
}

function Stop-CrashProcesses {
    param([array]$Processes)
    foreach ($p in $Processes) {
        Write-Host "  WARNING: killing crash reporter process $($p.ProcessName) (PID $($p.Id))" -ForegroundColor Yellow
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    }
}

# ---------------------------------------------------------------------------
# Run a single insert/delete lifecycle cycle.
# Returns @{ Passed=$bool; Details=$string; ProgId=$int }
# ---------------------------------------------------------------------------
function Run-LifecycleCycle {
    param(
        [Parameter(Mandatory)] [string]$Type,
        [Parameter(Mandatory)] [int]$CycleNum,
        [Parameter(Mandatory)] [string]$Name
    )

    $info = $script:TypeInfo[$Type]
    $prefix = $info.prefix
    $cliType = $info.cli

    $source = Get-ValidInsertSource -Type $Type -Name $Name
    $srcFile = $null
    $progId = 0
    $ellPath = ""
    $xmlPath = ""
    $failDetails = @()

    try {
        $srcFile = Write-SourceFile -Name "$($Name)_src" -Content $source

        # INSERT
        $cliIns = Invoke-Cli -Arguments @("--json","insert",$cliType,$Name,$srcFile)

        if ($Verbose) {
            Write-Host "  [VERBOSE insert] exit=$($cliIns.ExitCode)" -ForegroundColor DarkGray
            if ($cliIns.Stdout) { Write-Host "  stdout: $($cliIns.Stdout)" -ForegroundColor DarkGray }
            if ($cliIns.Stderr) { Write-Host "  stderr: $($cliIns.Stderr)" -ForegroundColor DarkGray }
        }

        if ($cliIns.ExitCode -ne 0) {
            $failDetails += "insert exit=$($cliIns.ExitCode) stderr=$($cliIns.Stderr)"
            throw "insert failed"
        }

        $jIns = $null
        try {
            $jIns = $cliIns.Stdout | ConvertFrom-Json -ErrorAction Stop
        } catch {
            $failDetails += "insert JSON parse failed: $($_.Exception.Message)"
            throw "insert JSON parse failed"
        }

        if (-not $jIns.success -or -not $jIns.prog_id -or ([int]$jIns.prog_id) -le 0) {
            $failDetails += "insert JSON success=$($jIns.success) prog_id=$($jIns.prog_id)"
            throw "insert JSON did not return success + prog_id"
        }
        $progId = [int]$jIns.prog_id

        $ellPath = Join-Path $script:MyWorkBin "$prefix$progId.ell"
        $xmlPath = Join-Path $script:MyWorkBin "$prefix$progId.xml"

        if (-not (Test-Path -Path $ellPath -PathType Leaf)) {
            $failDetails += "ELL missing after insert: $ellPath"
            throw "ELL missing after insert"
        }
        if (-not (Test-Path -Path $xmlPath -PathType Leaf)) {
            $failDetails += "XML missing after insert: $xmlPath"
            throw "XML missing after insert"
        }

        # DELETE
        $cliDel = Invoke-Cli -Arguments @("--json","delete",$cliType,$Name)

        if ($Verbose) {
            Write-Host "  [VERBOSE delete] exit=$($cliDel.ExitCode)" -ForegroundColor DarkGray
            if ($cliDel.Stdout) { Write-Host "  stdout: $($cliDel.Stdout)" -ForegroundColor DarkGray }
            if ($cliDel.Stderr) { Write-Host "  stderr: $($cliDel.Stderr)" -ForegroundColor DarkGray }
        }

        if ($cliDel.ExitCode -ne 0) {
            $failDetails += "delete exit=$($cliDel.ExitCode) stderr=$($cliDel.Stderr)"
            throw "delete failed"
        }

        if (Test-Path -Path $ellPath -PathType Leaf) {
            $failDetails += "ELL still exists after delete: $ellPath"
            throw "ELL still exists after delete"
        }
        if (Test-Path -Path $xmlPath -PathType Leaf) {
            $failDetails += "XML still exists after delete: $xmlPath"
            throw "XML still exists after delete"
        }

        # CRASH PROCESS CHECK
        $crashProcs = Get-CrashProcesses
        if ($crashProcs) {
            $names = ($crashProcs | Select-Object -ExpandProperty ProcessName) -join ', '
            $failDetails += "crash reporter process(es) detected: $names"
            Stop-CrashProcesses -Processes $crashProcs
            throw "crash reporter process(es) detected: $names"
        }

        return @{ Passed=$true; Details="prog_id=$progId"; ProgId=$progId }
    } catch {
        if ($failDetails.Count -eq 0) {
            $failDetails += "$_"
        }

        # Best-effort cleanup for the current item.
        if ($Name) {
            Write-Host "  Cycle $CycleNum ($Type) FAILED; attempting cleanup" -ForegroundColor Yellow
            if ($Verbose) { Write-Host "  detail: $($failDetails -join '; ')" -ForegroundColor Yellow }
            $cleanupDel = Invoke-Cli -Arguments @("--json","delete",$cliType,$Name)
            if ($Verbose) {
                Write-Host "  [VERBOSE cleanup-delete] exit=$($cleanupDel.ExitCode)" -ForegroundColor DarkGray
            }
            if ($ellPath -and (Test-Path -Path $ellPath -PathType Leaf)) {
                Remove-Item -Path $ellPath -Force -ErrorAction SilentlyContinue
            }
            if ($xmlPath -and (Test-Path -Path $xmlPath -PathType Leaf)) {
                Remove-Item -Path $xmlPath -Force -ErrorAction SilentlyContinue
            }
        }

        return @{ Passed=$false; Details=($failDetails -join "; "); ProgId=$progId }
    } finally {
        if ($srcFile -and (Test-Path -Path $srcFile -PathType Leaf)) {
            Remove-Item -Path $srcFile -Force -ErrorAction SilentlyContinue
        }
    }
}

# ---------------------------------------------------------------------------
# MAIN EXECUTION
# ---------------------------------------------------------------------------
Write-Host "================================================" -ForegroundColor Cyan
Write-Host "TSDev Automation - TVS-L.5 Repeated-Instance Lifecycle Stress" -ForegroundColor Cyan
Write-Host "Opt-in; 600 insert/delete lifecycle cycles (fresh CLI process per op)" -ForegroundColor Cyan
Write-Host "120 cycles x 5 component types" -ForegroundColor Cyan
Write-Host "NOTE: repeated-instance stress; does NOT prove long-lived" -ForegroundColor Cyan
Write-Host "      COM/handle/window leak absence (see test_stress_batch.cpp)." -ForegroundColor Cyan
Write-Host "================================================" -ForegroundColor Cyan
Write-Host "Run ID       : $runId"
Write-Host "Project root : $ProjectRoot"
Write-Host "CLI path     : $CliPath"
Write-Host "TS path      : $(if ($TsPath) { $TsPath } else { 'auto-discovered' })"
Write-Host "MyWork\Bin   : $MyWorkBin"
Write-Host "Verbose      : $Verbose"
Write-Host ""

# ---------------------------------------------------------------------------
# Prerequisite checks.
# ---------------------------------------------------------------------------
if (-not (Test-IsElevated)) {
    Write-Host "ERROR: This stress test must be run from an elevated (administrator)" -ForegroundColor Red
    Write-Host "       terminal. The CLI has a requireAdministrator manifest." -ForegroundColor Red
    exit 2
}

if (-not (Test-Path -Path $CliPath -PathType Leaf)) {
    Write-Host "ERROR: CLI executable not found: $CliPath" -ForegroundColor Red
    Write-Host "       Build the project first: cmake --build build --config Release" -ForegroundColor Red
    exit 2
}

if (-not (Test-Path -Path $MyWorkBin -PathType Container)) {
    Write-Host "ERROR: MyWork\Bin directory not found: $MyWorkBin" -ForegroundColor Red
    Write-Host "       Override with -BinPath or -TsPath." -ForegroundColor Red
    exit 2
}

# Create temp directory.
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null

$types = @('strategy','indicator','function','tradingapp','showme')
$perTypeResults = @{
    strategy   = @{ Passed=0; Failed=0 }
    indicator  = @{ Passed=0; Failed=0 }
    function   = @{ Passed=0; Failed=0 }
    tradingapp = @{ Passed=0; Failed=0 }
    showme     = @{ Passed=0; Failed=0 }
}
$allItems = New-Object System.Collections.Generic.List[object]
$totalPassed = 0
$totalFailed = 0
$firstFailure = $null
$abort = $false
$abortReason = $null
$overallStart = Get-Date

foreach ($type in $types) {
    $info = $TypeInfo[$type]
    Write-Host "--- Starting type=$type (whType=$($info.whType)) ---" -ForegroundColor Cyan

    for ($i = 1; $i -le 120; $i++) {
        $nnn = "{0:D3}" -f $i
        $name = "TS_AUTO_L5_STRESS_$($type.ToUpper())_${runId}_${nnn}"
        $result = Run-LifecycleCycle -Type $type -CycleNum $i -Name $name
        $allItems.Add(@{ Type=$type; Name=$name; ProgId=$result.ProgId }) | Out-Null

        if ($result.Passed) {
            $perTypeResults[$type].Passed++
            $totalPassed++
        } else {
            $perTypeResults[$type].Failed++
            $totalFailed++
            if (-not $firstFailure) {
                $firstFailure = "type=$type cycle=$i name=$name detail=$($result.Details)"
            }
            Write-Host "FAIL type=$type cycle=$i detail=$($result.Details)" -ForegroundColor Red
        }

        # Periodic progress + crash-reporter check every 50 cycles.
        if ($i % 50 -eq 0) {
            Write-Host "[PROGRESS] type=$type cycle=$i/120 passed=$($perTypeResults[$type].Passed) failed=$($perTypeResults[$type].Failed)" -ForegroundColor Cyan

            $crashProcs = Get-CrashProcesses
            if ($crashProcs) {
                $names = ($crashProcs | Select-Object -ExpandProperty ProcessName) -join ', '
                Write-Host "ABORT: crash reporter process(es) detected at cycle $i of type $type`: $names" -ForegroundColor Red
                Stop-CrashProcesses -Processes $crashProcs
                $abort = $true
                $abortReason = "Crash reporter(s) at type=$type cycle=$i`: $names"
                if (-not $firstFailure) { $firstFailure = $abortReason }
                break
            }
        }
    }

    # Final progress and crash-reporter check at the end of each type.
    Write-Host "[PROGRESS] type=$type cycle=120/120 passed=$($perTypeResults[$type].Passed) failed=$($perTypeResults[$type].Failed)" -ForegroundColor Cyan

    $endCrash = Get-CrashProcesses
    if ($endCrash) {
        $names = ($endCrash | Select-Object -ExpandProperty ProcessName) -join ', '
        Write-Host "ABORT: crash reporter process(es) detected at end of type $type`: $names" -ForegroundColor Red
        Stop-CrashProcesses -Processes $endCrash
        $abort = $true
        $abortReason = "Crash reporter(s) at end of type=$type`: $names"
        if (-not $firstFailure) { $firstFailure = $abortReason }
        break
    }

    if ($abort) { break }
}

$overallEnd = Get-Date
$elapsed = $overallEnd - $overallStart

# ---------------------------------------------------------------------------
# Final cleanup sweep: delete any remaining test items.
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "----- Final cleanup sweep -----" -ForegroundColor Cyan
$remainingCount = 0
foreach ($item in $allItems) {
    $type = $item.Type
    $name = $item.Name
    $progId = $item.ProgId

    $ex = Invoke-Cli -Arguments @("--json","exists",$type,$name)
    $exists = $false
    if ($ex.ExitCode -eq 0) {
        try {
            $jEx = $ex.Stdout | ConvertFrom-Json -ErrorAction Stop
            $exists = [bool]$jEx.exists
        } catch { }
    }

    if ($exists) {
        $remainingCount++
        $del = Invoke-Cli -Arguments @("--json","delete",$type,$name)
        Write-Host "  sweep delete $type $name`: exit=$($del.ExitCode)" -ForegroundColor Yellow
    }

    if ($progId -gt 0) {
        $prefix = $TypeInfo[$type].prefix
        $ellPath = Join-Path $script:MyWorkBin "$prefix$progId.ell"
        $xmlPath = Join-Path $script:MyWorkBin "$prefix$progId.xml"
        if (Test-Path -Path $ellPath -PathType Leaf) {
            Remove-Item -Path $ellPath -Force -ErrorAction SilentlyContinue
        }
        if (Test-Path -Path $xmlPath -PathType Leaf) {
            Remove-Item -Path $xmlPath -Force -ErrorAction SilentlyContinue
        }
    }
}
Write-Host "Final sweep removed $remainingCount remaining item(s)." -ForegroundColor Cyan

# ---------------------------------------------------------------------------
# Final summary table.
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "================================================" -ForegroundColor Cyan
Write-Host "TVS-L.5 Repeated-Instance Lifecycle Stress - Final Summary" -ForegroundColor Cyan
Write-Host "================================================" -ForegroundColor Cyan
Write-Host "Type                 Passed   Failed" -ForegroundColor White
Write-Host "-------------------- -------- --------"
foreach ($type in $types) {
    $r = $perTypeResults[$type]
    $line = "{0,-20} {1,8} {2,8}" -f $type, $r.Passed, $r.Failed
    if ($r.Failed -gt 0) {
        Write-Host $line -ForegroundColor Red
    } else {
        Write-Host $line -ForegroundColor Green
    }
}
Write-Host "-------------------- -------- --------"
$totalLine = "{0,-20} {1,8} {2,8}" -f "TOTAL", $totalPassed, $totalFailed
if ($totalFailed -gt 0 -or $abort) {
    Write-Host $totalLine -ForegroundColor Red
} else {
    Write-Host $totalLine -ForegroundColor Green
}
Write-Host "Total attempted cycles : $($totalPassed + $totalFailed)"
Write-Host "Expected cycles        : 600"
Write-Host "Passed                 : $totalPassed"
Write-Host "Failed                 : $totalFailed"
Write-Host "Elapsed                : $elapsed"
if ($abort) {
    Write-Host "Aborted                : YES" -ForegroundColor Red
    Write-Host "Abort reason           : $abortReason" -ForegroundColor Red
} else {
    Write-Host "Aborted                : NO" -ForegroundColor Green
}
if ($firstFailure) {
    Write-Host "First failure          : $firstFailure" -ForegroundColor Red
} else {
    Write-Host "First failure          : (none)" -ForegroundColor Green
}

# Remove temp directory.
if (Test-Path -Path $tmpDir -PathType Container) {
    Remove-Item -Path $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
}

if ($totalFailed -eq 0 -and $totalPassed -eq 600 -and -not $abort) {
    Write-Host "RESULT: PASS" -ForegroundColor Green
    exit 0
} else {
    Write-Host "RESULT: FAIL" -ForegroundColor Red
    exit 1
}
