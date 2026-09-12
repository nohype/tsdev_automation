<#
.SYNOPSIS
    End-to-end batch CLI verification against a live TradeStation Desktop.
.DESCRIPTION
    Exercises tsdev_cli batch-insert, batch-modify, and batch-delete against a
    live TradeStation Desktop. Uses unique names with a timestamp suffix and
    three component types: function, indicator, and showme.

    Step 14-16 additionally verify the TVS A tagged-serve progress contract
    live: this script launches its own elevated `tsdev_cli serve` broker
    twice (default human stderr, then --json NDJSON stderr) and drives
    tagged insert/duplicate-insert/delete requests over the named-pipe
    frame protocol, asserting the enriched single-frame responses and the
    live stderr progress events.

    Step 17-19 verify the TVS C --batch-progress-control contract live:
    a manifest-path batch driven by stdin decision lines (continue then
    cancel -> exit 3 + exit-12 marker), a framed TSDEV_BATCH_CONTROL/1
    --batch-stdin delete under --json (exit 2 with a real not_found), and
    the repeat/non-batch argument gates (exit 1). This script is
    standalone — run it directly from an elevated shell; it is not part
    of run_regression.ps1.
#>

param(
    [string]$CliPath = (Join-Path (Split-Path -Parent $PSScriptRoot) 'dist\tsdev_cli.exe'),
    [string]$TsPath,
    [switch]$Verbose
)

$ErrorActionPreference = 'Stop'

# Shared test helpers (batch progress event assertions).
Import-Module (Join-Path $PSScriptRoot "TestHelpers.psm1") -ErrorAction Stop -DisableNameChecking

$LogLines = New-Object System.Collections.Generic.List[object]
$StepResults = New-Object System.Collections.Generic.List[object]
$ProgIds = @{}
$Halt = $false
$AllPassed = $true
$script:ExtraItems = New-Object System.Collections.Generic.List[object]

function Write-Log {
    param(
        [Parameter(Mandatory = $false)] [string]$Message = '',
        [string]$Color = 'White'
    )
    $script:LogLines.Add($Message) | Out-Null
    switch ($Color) {
        'Red'    { Write-Host $Message -ForegroundColor Red }
        'Green'  { Write-Host $Message -ForegroundColor Green }
        'Cyan'   { Write-Host $Message -ForegroundColor Cyan }
        'Yellow' { Write-Host $Message -ForegroundColor Yellow }
        default  { Write-Host $Message }
    }
}

Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class TsWindowEnum {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr hWndParent, EnumWindowsProc lpEnumFunc, IntPtr lParam);
    [DllImport("user32.dll", SetLastError = true)] public static extern int GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextLength(IntPtr hWnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr hWnd, StringBuilder lpString, int nMaxCount);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int SendMessage(IntPtr hWnd, int msg, int wParam, StringBuilder lParam);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int SendMessage(IntPtr hWnd, int msg, int wParam, IntPtr lParam);
    [DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow();

    public const int WM_GETTEXT = 0x000D;
    public const int WM_GETTEXTLENGTH = 0x000E;

    public static List<Tuple<IntPtr, int, string>> GetVisibleWindowsForPids(List<int> pids) {
        var list = new List<Tuple<IntPtr, int, string>>();
        EnumWindows((hWnd, lParam) => {
            if (!IsWindowVisible(hWnd)) return true;
            uint pid;
            GetWindowThreadProcessId(hWnd, out pid);
            if (pids.Contains((int)pid)) {
                int len = GetWindowTextLength(hWnd);
                StringBuilder sb = new StringBuilder(len + 1);
                GetWindowText(hWnd, sb, len + 1);
                list.Add(Tuple.Create(hWnd, (int)pid, sb.ToString()));
            }
            return true;
        }, IntPtr.Zero);
        return list;
    }

    // Concatenated, deduplicated, nonempty trimmed text of all child
    // controls of a window (dialog body text). GetWindowText does not work
    // across processes for child controls, so WM_GETTEXT is sent directly.
    public static string GetChildText(IntPtr hWnd) {
        var parts = new List<string>();
        EnumChildWindows(hWnd, (child, lParam) => {
            int len = SendMessage(child, WM_GETTEXTLENGTH, 0, IntPtr.Zero);
            if (len > 0) {
                StringBuilder sb = new StringBuilder(len + 1);
                SendMessage(child, WM_GETTEXT, sb.Capacity, sb);
                string t = sb.ToString().Trim();
                if (t.Length > 0 && !parts.Contains(t)) parts.Add(t);
            }
            return true;
        }, IntPtr.Zero);
        return string.Join(" | ", parts);
    }

    // Window title + concatenated child-control text (for crash-report
    // windows where only the process MainWindowHandle is known).
    public static string GetWindowAndChildText(IntPtr hWnd) {
        var parts = new List<string>();
        int tlen = GetWindowTextLength(hWnd);
        if (tlen > 0) {
            StringBuilder sb = new StringBuilder(tlen + 1);
            GetWindowText(hWnd, sb, tlen + 1);
            string t = sb.ToString().Trim();
            if (t.Length > 0) parts.Add(t);
        }
        string children = GetChildText(hWnd);
        if (children.Length > 0) parts.Add(children);
        return string.Join(" | ", parts);
    }
}
"@

# Non-live seam check for the dialog body-text helper: it must return a
# string (possibly empty) for a real window handle without throwing. No fake
# dialogs are spawned — the console window is just a harmless probe target.
try {
    $smokeHwnd = [TsWindowEnum]::GetConsoleWindow()
    if ($smokeHwnd -eq [IntPtr]::Zero) {
        Write-Log '  (dialog body-text seam check skipped: no console window)' 'Yellow'
    } else {
        $smokeText = [TsWindowEnum]::GetWindowAndChildText($smokeHwnd)
        if ($null -eq $smokeText) { throw 'GetWindowAndChildText returned $null' }
    }
} catch {
    Write-Log "WARNING: dialog body-text seam check failed: $_" 'Yellow'
}

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$SourceDir = Join-Path $ProjectRoot 'tmp\batch_e2e'
$LogFile = Join-Path $SourceDir 'run_log.txt'

if (-not (Test-Path -Path $SourceDir -PathType Container)) {
    New-Item -ItemType Directory -Force -Path $SourceDir | Out-Null
}

if (-not (Test-Path -Path $CliPath -PathType Leaf)) {
    $CliPath = Join-Path $ProjectRoot 'build\Release\tsdev_cli.exe'
}
if (-not (Test-Path -Path $CliPath -PathType Leaf)) {
    Write-Log "ERROR: tsdev_cli.exe not found at $CliPath" 'Red'
    $script:LogLines | Out-File -FilePath $LogFile -Encoding ASCII -Force
    exit 1
}
$script:CliPath = (Resolve-Path -Path $CliPath).Path
$script:TsPath = $TsPath
$script:ServeIoTimeoutMs = 30000

$runId = Get-Date -Format 'yyyyMMddHHmmss'

$FnName = "BatchE2eFn_$runId"
$IndName = "BatchE2eInd_$runId"
$ShowMeName = "BatchE2eShowMe_$runId"

$script:Items = @(
    [pscustomobject]@{
        Type = 'function'
        Name = $FnName
        V1File = 'batch_fn_v1.el'
        V2File = 'batch_fn_v2.el'
        V1Markers = @('= 1')
        V2Markers = @('= 2')
    },
    [pscustomobject]@{
        Type = 'indicator'
        Name = $IndName
        V1File = 'batch_ind_v1.el'
        V2File = 'batch_ind_v2.el'
        V1Markers = @('Plot1(High', '"H")')
        V2Markers = @('Plot1(Low', '"L")')
    },
    [pscustomobject]@{
        Type = 'showme'
        Name = $ShowMeName
        V1File = 'batch_showme_v1.el'
        V2File = 'batch_showme_v2.el'
        V1Markers = @('Highest(High', 'Plot1(High', '"Hi")')
        V2Markers = @('Lowest(Low', 'Plot1(Low', '"Lo")')
    }
)

$FnV1 = "$FnName = 1;"
$FnV2 = "$FnName = 2;"
$IndV1 = 'Plot1(High, "H");'
$IndV2 = 'Plot1(Low, "L");'
$ShowMeV1 = 'if High = Highest(High, 10) then Plot1(High, "Hi");'
$ShowMeV2 = 'if Low = Lowest(Low, 10) then Plot1(Low, "Lo");'

$gapId = Get-Date -Format 'yyyyMMddHHmmssfff'

$UpsertExistingName = $FnName
$UpsertNewFnName = "BatchE2eUpsertFn_$gapId"
$FileIndName = "BatchE2eFileInd_$gapId"
$MixedDupFnName = "BatchE2eMixedDup_$gapId"
$MixedNewFnName = "BatchE2eMixedNew_$gapId"
$MixedMissingFnName = "BatchE2eMixedMissing_$gapId"

$UpsertV3 = "$UpsertExistingName = 3;"
$UpsertNewV1 = "$UpsertNewFnName = 1;"
$FileIndV1 = 'Plot1(Close, "C");'
$MixedDupV1 = "$MixedDupFnName = 1;"
$MixedNewV1 = "$MixedNewFnName = 1;"

function Write-SourceFile {
    param(
        [Parameter(Mandatory)] [string]$FileName,
        [Parameter(Mandatory)] [string]$Content
    )
    $path = Join-Path $script:SourceDir $FileName
    [System.IO.File]::WriteAllText($path, $Content, [System.Text.Encoding]::ASCII)
    return $path
}

function Write-ManifestFile {
    param(
        [Parameter(Mandatory)] [string]$FileName,
        [Parameter()] [string[]]$Lines = @()
    )
    $path = Join-Path $script:SourceDir $FileName
    $content = ($Lines -join "`r`n") + "`r`n"
    [System.IO.File]::WriteAllText($path, $content, [System.Text.Encoding]::ASCII)
    return $path
}

function Build-ArgString {
    param(
        [Parameter(Mandatory)] [string[]]$Arguments
    )
    $fullArgs = @()
    if ($script:TsPath) {
        $fullArgs += @('--ts-path', $script:TsPath)
    }
    $fullArgs += $Arguments
    $parts = @()
    foreach ($a in $fullArgs) {
        if ($a -match '\s') {
            $parts += "`"$a`""
        } else {
            $parts += $a
        }
    }
    return ($parts -join ' ')
}

function Invoke-TsCli {
    param(
        [Parameter(Mandatory)] [string[]]$Arguments
    )
    $argString = Build-ArgString -Arguments $Arguments
    $outFile = Join-Path $script:SourceDir "stdout_$([System.Guid]::NewGuid().ToString('N')).txt"
    $errFile = Join-Path $script:SourceDir "stderr_$([System.Guid]::NewGuid().ToString('N')).txt"
    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $proc = Start-Process -FilePath $script:CliPath -ArgumentList $argString -RedirectStandardOutput $outFile -RedirectStandardError $errFile -Wait -NoNewWindow -PassThru
        $exitCode = $proc.ExitCode
    } catch {
        $exitCode = -2
        $script:LastCliError = $_.Exception.Message
    } finally {
        $ErrorActionPreference = $prevEAP
    }
    $stdout = if (Test-Path $outFile) { [System.IO.File]::ReadAllText($outFile).TrimEnd(); Remove-Item $outFile -Force -ErrorAction SilentlyContinue } else { '' }
    $stderr = if (Test-Path $errFile) { [System.IO.File]::ReadAllText($errFile).TrimEnd(); Remove-Item $errFile -Force -ErrorAction SilentlyContinue } else { '' }
    if ($script:LastCliError) {
        $stderr += "`n$script:LastCliError"
        $script:LastCliError = $null
    }
    return [pscustomobject]@{ ExitCode = $exitCode; Stdout = $stdout; Stderr = $stderr }
}

function Invoke-TsBatch {
    param(
        [Parameter(Mandatory)] [string]$ManifestFile,
        [Parameter(Mandatory)] [string[]]$Arguments,
        [switch]$UseFileMode
    )
    $baseArgs = Build-ArgString -Arguments $Arguments
    if ($UseFileMode) {
        $manifestArg = if ($ManifestFile -match '\s') { "`"$ManifestFile`"" } else { $ManifestFile }
        $argString = "$baseArgs $manifestArg"
    } else {
        $argString = $baseArgs
    }
    $outFile = Join-Path $script:SourceDir "stdout_$([System.Guid]::NewGuid().ToString('N')).txt"
    $errFile = Join-Path $script:SourceDir "stderr_$([System.Guid]::NewGuid().ToString('N')).txt"
    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $startParams = @{
            FilePath = $script:CliPath
            ArgumentList = $argString
            RedirectStandardOutput = $outFile
            RedirectStandardError = $errFile
            Wait = $true
            NoNewWindow = $true
            PassThru = $true
        }
        if (-not $UseFileMode) {
            $startParams['RedirectStandardInput'] = $ManifestFile
        }
        $proc = Start-Process @startParams
        $exitCode = $proc.ExitCode
    } catch {
        $exitCode = -2
        $script:LastCliError = $_.Exception.Message
    } finally {
        $ErrorActionPreference = $prevEAP
    }
    $stdout = if (Test-Path $outFile) { [System.IO.File]::ReadAllText($outFile).TrimEnd(); Remove-Item $outFile -Force -ErrorAction SilentlyContinue } else { '' }
    $stderr = if (Test-Path $errFile) { [System.IO.File]::ReadAllText($errFile).TrimEnd(); Remove-Item $errFile -Force -ErrorAction SilentlyContinue } else { '' }
    if ($script:LastCliError) {
        $stderr += "`n$script:LastCliError"
        $script:LastCliError = $null
    }
    return [pscustomobject]@{ ExitCode = $exitCode; Stdout = $stdout; Stderr = $stderr }
}

function Invoke-TsControlBatch {
    # TVS C helper (Step-BatchControl): like Invoke-TsBatch, but the child
    # always reads its stdin from -StdinFile — raw OS redirect of the exact
    # control bytes (decision lines only, or a framed
    # TSDEV_BATCH_CONTROL/1 manifest + decisions). This allows a manifest
    # path AND a stdin decision stream at once, which Invoke-TsBatch cannot
    # express. The wait has a finite timeout: a wedged control read must
    # never hang the suite — on timeout the dialog surface is inspected and
    # dismissed (AGENTS rule) before the child is killed and reaped.
    param(
        [Parameter(Mandatory)] [string[]]$Arguments,
        [Parameter(Mandatory)] [string]$StdinFile,
        [int]$TimeoutSeconds = 180
    )
    $argString = Build-ArgString -Arguments $Arguments
    $outFile = Join-Path $script:SourceDir "stdout_$([System.Guid]::NewGuid().ToString('N')).txt"
    $errFile = Join-Path $script:SourceDir "stderr_$([System.Guid]::NewGuid().ToString('N')).txt"
    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $timedOut = $false
    try {
        $proc = Start-Process -FilePath $script:CliPath -ArgumentList $argString `
            -RedirectStandardInput $StdinFile -RedirectStandardOutput $outFile `
            -RedirectStandardError $errFile -NoNewWindow -PassThru
        if ($null -eq $proc) { throw 'failed to launch tsdev_cli' }
        # Cache the process handle before exit so PS 5.1 can report ExitCode.
        $null = $proc.Handle
        [void]$proc.WaitForExit($TimeoutSeconds * 1000)
        if (-not $proc.HasExited) {
            $timedOut = $true
            # Read/dismiss any TS failure surface before killing (AGENTS rule).
            try { [void](Check-TsDialogs 'control-batch timeout kill') } catch {}
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
            [void]$proc.WaitForExit(5000)
        }
        $exitCode = if ($timedOut) { -3 } else { $proc.ExitCode }
    } catch {
        $exitCode = -2
        $script:LastCliError = $_.Exception.Message
    } finally {
        $ErrorActionPreference = $prevEAP
    }
    # BOM-aware reads (auto-detect) for the redirected byte streams.
    $stdout = if (Test-Path $outFile) { [System.IO.File]::ReadAllText($outFile).TrimEnd(); Remove-Item $outFile -Force -ErrorAction SilentlyContinue } else { '' }
    $stderr = if (Test-Path $errFile) { [System.IO.File]::ReadAllText($errFile).TrimEnd(); Remove-Item $errFile -Force -ErrorAction SilentlyContinue } else { '' }
    if ($script:LastCliError) {
        $stderr += "`n$script:LastCliError"
        $script:LastCliError = $null
    }
    return [pscustomobject]@{ ExitCode = $exitCode; Stdout = $stdout; Stderr = $stderr; TimedOut = $timedOut }
}

function Assert-True {
    param(
        [Parameter(Mandatory)] [bool]$Condition,
        [Parameter(Mandatory)] [string]$Message
    )
    if (-not $Condition) {
        throw $Message
    }
}

function Record-Step {
    param(
        [Parameter(Mandatory)] [string]$Step,
        [Parameter(Mandatory)] [string]$Result,
        [string]$Detail = ''
    )
    $script:StepResults.Add([pscustomobject]@{ Step = $Step; Result = $Result; Detail = $Detail }) | Out-Null
}

function Check-TsDialogs {
    param(
        [Parameter(Mandatory)] [string]$Phase
    )
    $detected = $false
    Write-Log "--- TS dialog/crash check ($Phase) ---" 'Cyan'

    Start-Sleep -Milliseconds 500

    $procs = @()
    foreach ($name in @('orchart', 'TSDev', 'TSCrashReport', 'WerFault')) {
        $p = Get-Process -Name $name -ErrorAction SilentlyContinue
        if ($p) { $procs += $p }
    }

    $pids = $procs | Select-Object -ExpandProperty Id
    $allPids = New-Object System.Collections.Generic.List[int]
    if ($pids) {
        foreach ($id in $pids) { $allPids.Add($id) | Out-Null }
    }

    if ($allPids.Count -gt 0) {
        $windows = [TsWindowEnum]::GetVisibleWindowsForPids($allPids)
        foreach ($w in $windows) {
            if ($w.Item3 -match 'error|warning|crash|exception') {
                # Record title AND dialog body/control text before dismissal
                # (AGENTS: read the message, never kill silently).
                $body = [TsWindowEnum]::GetChildText($w.Item1)
                if ($body) {
                    Write-Log "  ALERT: PID $($w.Item2) window title: $($w.Item3) | body: $body" 'Red'
                } else {
                    Write-Log "  ALERT: PID $($w.Item2) window title: $($w.Item3)" 'Red'
                }
                $wp = Get-Process -Id $w.Item2 -ErrorAction SilentlyContinue
                if ($wp) { [void]$wp.CloseMainWindow() }
                $detected = $true
            }
        }
    }

    $crashProcs = Get-Process -Name 'TSCrashReport', 'WerFault' -ErrorAction SilentlyContinue
    if ($crashProcs) {
        foreach ($cp in $crashProcs) {
            # Capture the crash-report window text (title + controls) BEFORE
            # stopping the process so the failure surface is recorded.
            $body = ''
            try {
                if ($cp.MainWindowHandle -ne [IntPtr]::Zero) {
                    $body = [TsWindowEnum]::GetWindowAndChildText($cp.MainWindowHandle)
                }
            } catch {}
            if ($body) {
                Write-Log "  CRASH: $($cp.ProcessName)#$($cp.Id) | window text: $body" 'Red'
            } else {
                Write-Log "  CRASH: $($cp.ProcessName)#$($cp.Id) running (no capturable window text)" 'Red'
            }
            Stop-Process -Id $cp.Id -Force -ErrorAction SilentlyContinue
            $detected = $true
        }
    }

    if (-not $detected) {
        Write-Log '  OK: no TS error/crash dialogs detected.' 'Green'
    } else {
        Write-Log '  FAILED: TS error/crash dialog detected; halting test operations.' 'Red'
        $script:Halt = $true
        $script:AllPassed = $false
    }

    return $detected
}

function Step-PreClean {
    Write-Log "--- Step 1: Pre-clean ---" 'Cyan'
    try {
        $preDeleted = @()
        foreach ($item in $script:Items) {
            $res = Invoke-TsCli -Arguments @('exists', $item.Type, $item.Name, '--case-insensitive', '--json')
            if ($res.ExitCode -ne 0) {
                throw "exists pre-check for $($item.Type) '$($item.Name)' failed: exit $($res.ExitCode) stderr: $($res.Stderr)"
            }
            $j = $res.Stdout | ConvertFrom-Json
            if ($j.exists) {
                Write-Log "  Pre-existing item found: $($item.Type) '$($item.Name)' (progId $($j.prog_id)); deleting..."
                $del = Invoke-TsCli -Arguments @('delete', $item.Type, $item.Name, '--json')
                if ($del.ExitCode -ne 0) {
                    throw "pre-delete for $($item.Type) '$($item.Name)' failed: exit $($del.ExitCode) stderr: $($del.Stderr)"
                }
                $preDeleted += "$($item.Type) $($item.Name)"
                Write-Log "  Pre-deleted: $($item.Type) '$($item.Name)'" 'Green'
            } else {
                Write-Log "  No pre-existing item: $($item.Type) '$($item.Name)'" 'Green'
            }
        }
        if ($preDeleted.Count -gt 0) {
            Write-Log "Pre-deleted items: $($preDeleted -join '; ')" 'Green'
        } else {
            Write-Log 'No pre-existing items were found.' 'Green'
        }
        Record-Step '1.Pre-clean' 'PASS'
    } catch {
        Write-Log "  Step 1 FAIL: $_" 'Red'
        Record-Step '1.Pre-clean' 'FAIL' $_
        $script:AllPassed = $false
    }
}

function Step-BatchInsert {
    Write-Log "--- Step 2: batch-insert via stdin ---" 'Cyan'
    try {
        $lines = @()
        foreach ($item in $script:Items) {
            $lines += "$($item.Type) $($item.Name) $(Join-Path $script:SourceDir $item.V1File)"
        }
        $manifest = Write-ManifestFile 'insert_v1.manifest' $lines
        Write-Log "  Manifest file: $manifest"
        $res = Invoke-TsBatch -ManifestFile $manifest -Arguments @('batch-insert', '--batch-stdin')
        Write-Log "  batch-insert exit code: $($res.ExitCode)"
        if ($res.ExitCode -ne 0) {
            throw "batch-insert exit code $($res.ExitCode); stderr: $($res.Stderr)"
        }
        $arr = $res.Stdout | ConvertFrom-Json
        if (-not $arr) {
            throw 'batch-insert stdout is empty or not JSON'
        }
        Write-Log '  batch-insert JSON output:'
        $jsonStr = $arr | ConvertTo-Json -Depth 10
        foreach ($line in $jsonStr -split "`n") { Write-Log "    $line" }
        Assert-True ($arr -is [array]) 'batch-insert stdout is not a JSON array'
        Assert-True ($arr.Count -eq 3) "expected 3 results, got $($arr.Count)"
        for ($i = 0; $i -lt $script:Items.Count; $i++) {
            $item = $script:Items[$i]
            Assert-True ($arr[$i].success -eq $true) "item $i ($($item.Type) $($item.Name)) success expected true"
            Assert-True ($arr[$i].prog_id -gt 0) "item $i ($($item.Type) $($item.Name)) prog_id expected >0"
            $script:ProgIds[$item.Name] = $arr[$i].prog_id
        }
        Assert-BatchStderrProgress -Stderr $res.Stderr -Results $arr -ExpectedExitCode 0
        Write-Log '  All 3 batch-insert results: success=true' 'Green'
        Record-Step '2.batch-insert' 'PASS'
    } catch {
        Write-Log "  Step 2 FAIL: $_" 'Red'
        Record-Step '2.batch-insert' 'FAIL' $_
        $script:AllPassed = $false
    }
}

function Step-ExistsVerify {
    param(
        [Parameter(Mandatory)] [string]$Step,
        [Parameter(Mandatory)] [string]$Phase,
        [Parameter(Mandatory)] [bool]$ExpectedExists,
        [bool]$CheckProgIdPreserved = $false
    )
    Write-Log "--- $Step : exists-verify ($Phase) ---" 'Cyan'
    try {
        foreach ($item in $script:Items) {
            $res = Invoke-TsCli -Arguments @('exists', $item.Type, $item.Name, '--case-insensitive', '--json')
            if ($res.ExitCode -ne 0) {
                throw "exists ($Phase) for $($item.Type) '$($item.Name)' failed: exit $($res.ExitCode) stderr: $($res.Stderr)"
            }
            $j = $res.Stdout | ConvertFrom-Json
            if ($j.exists -ne $ExpectedExists) {
                throw "exists ($Phase) for $($item.Type) '$($item.Name)': expected exists=$ExpectedExists, got $($j.exists)"
            }
            if ($ExpectedExists) {
                if (-not $j.prog_id -or $j.prog_id -le 0) {
                    throw "exists ($Phase) for $($item.Type) '$($item.Name)': prog_id missing or <=0"
                }
                if ($CheckProgIdPreserved) {
                    $expectedProgId = $script:ProgIds[$item.Name]
                    if ($j.prog_id -ne $expectedProgId) {
                        throw "exists ($Phase) for $($item.Type) '$($item.Name)': prog_id changed from $expectedProgId to $($j.prog_id)"
                    }
                } else {
                    $script:ProgIds[$item.Name] = $j.prog_id
                }
                Write-Log "  exists ($Phase): $($item.Type) '$($item.Name)' stored_name='$($j.stored_name)' progId=$($j.prog_id)" 'Green'
            } else {
                Write-Log "  exists ($Phase): $($item.Type) '$($item.Name)' exists=false" 'Green'
            }
        }
        Record-Step $Step 'PASS'
    } catch {
        Write-Log "  $Step FAIL: $_" 'Red'
        Record-Step $Step 'FAIL' $_
        $script:AllPassed = $false
    }
}

function Step-GetSourceVerify {
    param(
        [Parameter(Mandatory)] [string]$Step,
        [Parameter(Mandatory)] [string]$Phase,
        [Parameter(Mandatory)] [string]$Version
    )
    Write-Log "--- $Step : get-source verify ($Phase, $Version) ---" 'Cyan'
    try {
        foreach ($item in $script:Items) {
            $res = Invoke-TsCli -Arguments @('get-source', $item.Type, $item.Name, '--json')
            if ($res.ExitCode -ne 0) {
                throw "get-source ($Phase) for $($item.Type) '$($item.Name)' failed: exit $($res.ExitCode) stderr: $($res.Stderr)"
            }
            $j = $res.Stdout | ConvertFrom-Json
            if (-not $j.success) {
                throw "get-source ($Phase) for $($item.Type) '$($item.Name)' returned success=false"
            }
            if (-not $j.source) {
                throw "get-source ($Phase) for $($item.Type) '$($item.Name)' source missing"
            }
            $markers = if ($Version -eq 'v1') { $item.V1Markers } else { $item.V2Markers }
            foreach ($m in $markers) {
                if (-not $j.source.Contains($m)) {
                    throw "get-source ($Phase) for $($item.Type) '$($item.Name)' missing marker '$m'"
                }
            }
            if ($Version -eq 'v2' -and $item.Type -eq 'function') {
                if ($j.source.Contains($item.V1Markers[0])) {
                    throw "get-source (v2) for function still contains v1 marker '$($item.V1Markers[0])'"
                }
            }
            $snippet = $j.source
            if ($snippet.Length -gt 80) { $snippet = $snippet.Substring(0, 80) + '...' }
            Write-Log "  get-source ($Phase): $($item.Type) '$($item.Name)' snippet: $snippet" 'Green'
        }
        Record-Step $Step 'PASS'
    } catch {
        Write-Log "  $Step FAIL: $_" 'Red'
        Record-Step $Step 'FAIL' $_
        $script:AllPassed = $false
    }
}

function Step-BatchModify {
    Write-Log "--- Step 5: batch-modify via stdin ---" 'Cyan'
    try {
        $lines = @()
        foreach ($item in $script:Items) {
            $lines += "$($item.Type) $($item.Name) $(Join-Path $script:SourceDir $item.V2File)"
        }
        $manifest = Write-ManifestFile 'modify_v2.manifest' $lines
        Write-Log "  Manifest file: $manifest"
        $res = Invoke-TsBatch -ManifestFile $manifest -Arguments @('batch-modify', '--batch-stdin')
        Write-Log "  batch-modify exit code: $($res.ExitCode)"
        if ($res.ExitCode -ne 0) {
            throw "batch-modify exit code $($res.ExitCode); stderr: $($res.Stderr)"
        }
        $arr = $res.Stdout | ConvertFrom-Json
        if (-not $arr) {
            throw 'batch-modify stdout is empty or not JSON'
        }
        Write-Log '  batch-modify JSON output:'
        $jsonStr = $arr | ConvertTo-Json -Depth 10
        foreach ($line in $jsonStr -split "`n") { Write-Log "    $line" }
        Assert-True ($arr -is [array]) 'batch-modify stdout is not a JSON array'
        Assert-True ($arr.Count -eq 3) "expected 3 results, got $($arr.Count)"
        for ($i = 0; $i -lt $script:Items.Count; $i++) {
            $item = $script:Items[$i]
            Assert-True ($arr[$i].success -eq $true) "item $i ($($item.Type) $($item.Name)) success expected true"
            Assert-True ($arr[$i].prog_id -gt 0) "item $i ($($item.Type) $($item.Name)) prog_id expected >0"
            Assert-True ($arr[$i].prog_id -eq $script:ProgIds[$item.Name]) "item $i ($($item.Type) $($item.Name)) prog_id not preserved (expected $($script:ProgIds[$item.Name]), got $($arr[$i].prog_id))"
        }
        Assert-BatchStderrProgress -Stderr $res.Stderr -Results $arr -ExpectedExitCode 0
        Write-Log '  All 3 batch-modify results: success=true, ProgIds preserved' 'Green'
        Record-Step '5.batch-modify' 'PASS'
    } catch {
        Write-Log "  Step 5 FAIL: $_" 'Red'
        Record-Step '5.batch-modify' 'FAIL' $_
        $script:AllPassed = $false
    }
}

function Step-BatchDelete {
    Write-Log "--- Step 8: batch-delete via stdin ---" 'Cyan'
    try {
        $lines = @()
        foreach ($item in $script:Items) {
            $lines += "$($item.Type) $($item.Name)"
        }
        $manifest = Write-ManifestFile 'delete.manifest' $lines
        Write-Log "  Manifest file: $manifest"
        $res = Invoke-TsBatch -ManifestFile $manifest -Arguments @('batch-delete', '--batch-stdin')
        Write-Log "  batch-delete exit code: $($res.ExitCode)"
        if ($res.ExitCode -ne 0) {
            throw "batch-delete exit code $($res.ExitCode); stderr: $($res.Stderr)"
        }
        $arr = $res.Stdout | ConvertFrom-Json
        if (-not $arr) {
            throw 'batch-delete stdout is empty or not JSON'
        }
        Write-Log '  batch-delete JSON output:'
        $jsonStr = $arr | ConvertTo-Json -Depth 10
        foreach ($line in $jsonStr -split "`n") { Write-Log "    $line" }
        Assert-True ($arr -is [array]) 'batch-delete stdout is not a JSON array'
        Assert-True ($arr.Count -eq 3) "expected 3 results, got $($arr.Count)"
        for ($i = 0; $i -lt $script:Items.Count; $i++) {
            $item = $script:Items[$i]
            Assert-True ($arr[$i].success -eq $true) "item $i ($($item.Type) $($item.Name)) success expected true"
        }
        Assert-BatchStderrProgress -Stderr $res.Stderr -Results $arr -ExpectedExitCode 0
        Write-Log '  All 3 batch-delete results: success=true' 'Green'
        Record-Step '8.batch-delete' 'PASS'
    } catch {
        Write-Log "  Step 8 FAIL: $_" 'Red'
        Record-Step '8.batch-delete' 'FAIL' $_
        $script:AllPassed = $false
    }
}

function Step-BatchUpsert {
    Write-Log "--- Step 10: batch-upsert (insert + modify branches) ---" 'Cyan'
    try {
        $existingItem = $script:Items[0]
        $existingProgId = $script:ProgIds[$existingItem.Name]
        if (-not $existingProgId -or $existingProgId -le 0) {
            throw "no recorded ProgId for existing $($existingItem.Type) '$($existingItem.Name)'"
        }

        $newItem = [pscustomobject]@{
            Type = 'function'
            Name = $UpsertNewFnName
            Markers = @('= 1')
        }

        $lines = @(
            "$($existingItem.Type) $($existingItem.Name) $v3Fn"
            "$($newItem.Type) $($newItem.Name) $upsertNewFn"
        )
        $manifest = Write-ManifestFile 'upsert_v3.manifest' $lines
        Write-Log "  Manifest file: $manifest"
        $res = Invoke-TsBatch -ManifestFile $manifest -Arguments @('batch-upsert', '--batch-stdin')
        Write-Log "  batch-upsert exit code: $($res.ExitCode)"
        if ($res.ExitCode -ne 0) {
            throw "batch-upsert exit code $($res.ExitCode); stderr: $($res.Stderr)"
        }
        $arr = $res.Stdout | ConvertFrom-Json
        if (-not $arr) {
            throw 'batch-upsert stdout is empty or not JSON'
        }
        if (-not ($arr -is [array])) { $arr = @($arr) }
        if ($arr.Count -ne 2) {
            throw "expected 2 upsert results, got $($arr.Count)"
        }
        Write-Log '  batch-upsert JSON output:'
        $jsonStr = $arr | ConvertTo-Json -Depth 10
        foreach ($line in $jsonStr -split "`n") { Write-Log "    $line" }

        # Index 0: existing item -> modify path (ProgId preserved)
        Assert-True ($arr[0].success -eq $true) "upsert existing ($($existingItem.Type) $($existingItem.Name)) success expected true"
        Assert-True ($arr[0].prog_id -gt 0) "upsert existing ($($existingItem.Type) $($existingItem.Name)) prog_id expected >0"
        Assert-True ($arr[0].prog_id -eq $existingProgId) "upsert existing ($($existingItem.Type) $($existingItem.Name)) prog_id not preserved (expected $existingProgId, got $($arr[0].prog_id))"

        # Index 1: new item -> insert path (new ProgId)
        Assert-True ($arr[1].success -eq $true) "upsert new ($($newItem.Type) $($newItem.Name)) success expected true"
        Assert-True ($arr[1].prog_id -gt 0) "upsert new ($($newItem.Type) $($newItem.Name)) prog_id expected >0"

        $script:ProgIds[$newItem.Name] = $arr[1].prog_id
        $script:ExtraItems.Add($newItem) | Out-Null

        Write-Log "  upsert existing ($($existingItem.Type) '$($existingItem.Name)'): success, ProgId preserved ($($arr[0].prog_id))" 'Green'
        Write-Log "  upsert new ($($newItem.Type) '$($newItem.Name)'): success, new ProgId $($arr[1].prog_id)" 'Green'

        # Verify both items exist and ProgIds match
        foreach ($n in @($existingItem, $newItem)) {
            $eres = Invoke-TsCli -Arguments @('exists', $n.Type, $n.Name, '--case-insensitive', '--json')
            if ($eres.ExitCode -ne 0) {
                throw "exists (upsert) for $($n.Type) '$($n.Name)' failed: exit $($eres.ExitCode) stderr: $($eres.Stderr)"
            }
            $ej = $eres.Stdout | ConvertFrom-Json
            if (-not $ej.exists) {
                throw "exists (upsert) for $($n.Type) '$($n.Name)': expected exists=true, got $($ej.exists)"
            }
            if ($n.Name -eq $existingItem.Name) {
                if ($ej.prog_id -ne $existingProgId) {
                    throw "exists (upsert) for $($n.Type) '$($n.Name)': prog_id changed (expected $existingProgId, got $($ej.prog_id))"
                }
            } else {
                if ($ej.prog_id -ne $script:ProgIds[$newItem.Name]) {
                    throw "exists (upsert) for $($n.Type) '$($n.Name)': prog_id mismatch (expected $($script:ProgIds[$newItem.Name]), got $($ej.prog_id))"
                }
            }
            Write-Log "  exists (upsert): $($n.Type) '$($n.Name)' stored_name='$($ej.stored_name)' progId=$($ej.prog_id)" 'Green'
        }

        # Verify source for both items
        foreach ($n in @($existingItem, $newItem)) {
            $sres = Invoke-TsCli -Arguments @('get-source', $n.Type, $n.Name, '--json')
            if ($sres.ExitCode -ne 0) {
                throw "get-source (upsert) for $($n.Type) '$($n.Name)' failed: exit $($sres.ExitCode) stderr: $($sres.Stderr)"
            }
            $sj = $sres.Stdout | ConvertFrom-Json
            if (-not $sj.success) {
                throw "get-source (upsert) for $($n.Type) '$($n.Name)' returned success=false"
            }
            if (-not $sj.source) {
                throw "get-source (upsert) for $($n.Type) '$($n.Name)' source missing"
            }
            $expectedMarker = if ($n.Name -eq $existingItem.Name) { '= 3' } else { '= 1' }
            if (-not $sj.source.Contains($expectedMarker)) {
                throw "get-source (upsert) for $($n.Type) '$($n.Name)' missing marker '$expectedMarker'"
            }
            $snippet = $sj.source
            if ($snippet.Length -gt 80) { $snippet = $snippet.Substring(0, 80) + '...' }
            Write-Log "  get-source (upsert): $($n.Type) '$($n.Name)' snippet: $snippet" 'Green'
        }

        Assert-BatchStderrProgress -Stderr $res.Stderr -Results $arr -ExpectedExitCode 0

        # --json selects JSON progress rendering on stderr; stdout unchanged.
        # Re-running the same manifest exercises the modify-path for both.
        $resJson = Invoke-TsBatch -ManifestFile $manifest -Arguments @('--json', 'batch-upsert', '--batch-stdin')
        Write-Log "  batch-upsert --json exit code: $($resJson.ExitCode)"
        if ($resJson.ExitCode -ne 0) {
            throw "batch-upsert --json exit code $($resJson.ExitCode); stderr: $($resJson.Stderr)"
        }
        $arrJson = [array]($resJson.Stdout | ConvertFrom-Json)
        Assert-True ($arrJson.Count -eq 2) "expected 2 upsert --json results, got $($arrJson.Count)"
        Assert-True (($resJson.Stdout -split "`n").Count -eq 1) "batch-upsert --json stdout must be exactly one JSON array line"
        Assert-True ($resJson.Stderr -cnotmatch 'BATCH ') "batch-upsert --json stderr must not contain human BATCH lines"
        Assert-BatchJsonProgress -Stderr $resJson.Stderr -Results $arrJson -ExpectedExitCode 0

        Record-Step '10.batch-upsert' 'PASS'
    } catch {
        Write-Log "  Step 10 FAIL: $_" 'Red'
        Record-Step '10.batch-upsert' 'FAIL' $_
        $script:AllPassed = $false
    }
}

function Step-BatchFile {
    Write-Log "--- Step 12: batch-insert from manifest file (positional path) ---" 'Cyan'
    try {
        $item = [pscustomobject]@{
            Type = 'indicator'
            Name = $FileIndName
            Markers = @('Plot1(Close', '"C")')
        }

        $lines = @("$($item.Type) $($item.Name) $fileInd")
        $manifest = Write-ManifestFile 'insert_file_mode.manifest' $lines
        Write-Log "  Manifest file: $manifest"
        $res = Invoke-TsBatch -ManifestFile $manifest -Arguments @('batch-insert') -UseFileMode
        Write-Log "  batch-insert (file mode) exit code: $($res.ExitCode)"
        if ($res.ExitCode -ne 0) {
            throw "batch-insert (file mode) exit code $($res.ExitCode); stderr: $($res.Stderr)"
        }
        $arr = $res.Stdout | ConvertFrom-Json
        if (-not $arr) {
            throw 'batch-insert (file mode) stdout is empty or not JSON'
        }
        if (-not ($arr -is [array])) { $arr = @($arr) }
        if ($arr.Count -ne 1) {
            throw "expected 1 file-mode result, got $($arr.Count)"
        }

        $r0 = $arr[0]
        Assert-True ($r0.success -eq $true) "file-mode item ($($item.Type) $($item.Name)) success expected true"
        Assert-True ($r0.prog_id -gt 0) "file-mode item ($($item.Type) $($item.Name)) prog_id expected >0"

        $script:ProgIds[$item.Name] = $r0.prog_id
        $script:ExtraItems.Add($item) | Out-Null
        Write-Log "  file-mode insert: $($item.Type) '$($item.Name)' progId=$($r0.prog_id)" 'Green'

        $eres = Invoke-TsCli -Arguments @('exists', $item.Type, $item.Name, '--case-insensitive', '--json')
        if ($eres.ExitCode -ne 0) {
            throw "exists (file mode) for $($item.Type) '$($item.Name)' failed: exit $($eres.ExitCode) stderr: $($eres.Stderr)"
        }
        $ej = $eres.Stdout | ConvertFrom-Json
        if (-not $ej.exists) {
            throw "exists (file mode) for $($item.Type) '$($item.Name)': expected exists=true, got $($ej.exists)"
        }
        if ($ej.prog_id -ne $r0.prog_id) {
            throw "exists (file mode) for $($item.Type) '$($item.Name)': prog_id mismatch (expected $($r0.prog_id), got $($ej.prog_id))"
        }
        Write-Log "  exists (file mode): $($item.Type) '$($item.Name)' stored_name='$($ej.stored_name)' progId=$($ej.prog_id)" 'Green'

        $sres = Invoke-TsCli -Arguments @('get-source', $item.Type, $item.Name, '--json')
        if ($sres.ExitCode -ne 0) {
            throw "get-source (file mode) for $($item.Type) '$($item.Name)' failed: exit $($sres.ExitCode) stderr: $($sres.Stderr)"
        }
        $sj = $sres.Stdout | ConvertFrom-Json
        if (-not $sj.success) {
            throw "get-source (file mode) for $($item.Type) '$($item.Name)' returned success=false"
        }
        if (-not $sj.source) {
            throw "get-source (file mode) for $($item.Type) '$($item.Name)' source missing"
        }
        foreach ($m in $item.Markers) {
            if (-not $sj.source.Contains($m)) {
                throw "get-source (file mode) for $($item.Type) '$($item.Name)' missing marker '$m'"
            }
        }
        $snippet = $sj.source
        if ($snippet.Length -gt 80) { $snippet = $snippet.Substring(0, 80) + '...' }
        Write-Log "  get-source (file mode): $($item.Type) '$($item.Name)' snippet: $snippet" 'Green'

        Assert-BatchStderrProgress -Stderr $res.Stderr -Results $arr -ExpectedExitCode 0

        Record-Step '12.batch-insert-file-mode' 'PASS'
    } catch {
        Write-Log "  Step 12 FAIL: $_" 'Red'
        Record-Step '12.batch-insert-file-mode' 'FAIL' $_
        $script:AllPassed = $false
    }
}

function Step-MixedOutcome {
    Write-Log "--- Step 13: mixed-outcome batch-insert (continue-on-failure) ---" 'Cyan'
    try {
        $aType = 'function'
        $aName = $MixedDupFnName
        $aSource = $mixedDupFn

        # Proactive exists precheck (AGENTS): remove a leftover from a prior
        # run so the pre-insert below is a clean fresh insert, not an
        # unexpected name-collision.
        $eres = Invoke-TsCli -Arguments @('exists', $aType, $aName, '--case-insensitive', '--json')
        if ($eres.ExitCode -ne 0) {
            throw "exists precheck for duplicate $aType '$aName' failed: exit $($eres.ExitCode) stderr: $($eres.Stderr)"
        }
        if (Check-TsDialogs 'mixed-outcome exists precheck') { throw 'TS dialog/crash during mixed-outcome exists precheck' }
        $ej = $eres.Stdout | ConvertFrom-Json
        if ($ej.exists) {
            $dres = Invoke-TsCli -Arguments @('delete', $aType, $aName, '--json')
            if ($dres.ExitCode -ne 0) {
                throw "pre-delete leftover duplicate $aType '$aName' failed: exit $($dres.ExitCode) stderr: $($dres.Stderr)"
            }
            if (Check-TsDialogs 'mixed-outcome pre-delete') { throw 'TS dialog/crash during mixed-outcome pre-delete' }
            Write-Log "  Pre-deleted leftover duplicate fixture $aType '$aName'" 'Yellow'
        }

        # Pre-insert item A so it becomes a duplicate in the subsequent batch
        $ires = Invoke-TsCli -Arguments @('insert', $aType, $aName, $aSource, '--json')
        if ($ires.ExitCode -ne 0) {
            throw "pre-insert duplicate $aType '$aName' failed: exit $($ires.ExitCode) stderr: $($ires.Stderr)"
        }
        if (Check-TsDialogs 'mixed-outcome pre-insert') { throw 'TS dialog/crash during mixed-outcome pre-insert' }
        $ij = $ires.Stdout | ConvertFrom-Json
        if (-not $ij.success) {
            throw "pre-insert duplicate $aType '$aName' returned success=false (exit $($ij.exit_code); $($ij.error.message))"
        }
        $aProgId = $ij.prog_id
        if (-not $aProgId -or $aProgId -le 0) {
            throw "pre-insert duplicate $aType '$aName' returned prog_id <= 0"
        }
        $script:ExtraItems.Add([pscustomobject]@{ Type = $aType; Name = $aName }) | Out-Null
        Write-Log "  Pre-inserted duplicate $aType '$aName' with ProgId $aProgId" 'Green'

        $bType = 'function'
        $bName = $MixedNewFnName
        $bSource = $mixedNewFn
        $cType = 'function'
        $cName = $MixedMissingFnName
        $cMissingPath = Join-Path $script:SourceDir "missing_$([System.Guid]::NewGuid().ToString('N')).el"
        if (Test-Path $cMissingPath) { Remove-Item $cMissingPath -Force -ErrorAction SilentlyContinue }

        $lines = @(
            "$aType $aName $aSource"
            "$bType $bName $bSource"
            "$cType $cName $cMissingPath"
        )
        $manifest = Write-ManifestFile 'mixed_outcome.manifest' $lines
        Write-Log "  Manifest file: $manifest"
        $res = Invoke-TsBatch -ManifestFile $manifest -Arguments @('batch-insert', '--batch-stdin')
        Write-Log "  mixed batch-insert exit code: $($res.ExitCode)"
        if ($res.ExitCode -ne 2) {
            throw "expected mixed batch exit code 2, got $($res.ExitCode); stderr: $($res.Stderr)"
        }
        $arr = $res.Stdout | ConvertFrom-Json
        if (-not $arr) {
            throw 'mixed batch-insert stdout is empty or not JSON'
        }
        if (-not ($arr -is [array])) { $arr = @($arr) }
        if ($arr.Count -ne 3) {
            throw "expected 3 mixed results, got $($arr.Count)"
        }
        Write-Log '  mixed batch-insert JSON output:'
        $jsonStr = $arr | ConvertTo-Json -Depth 10
        foreach ($line in $jsonStr -split "`n") { Write-Log "    $line" }

        # Item A: duplicate -> already exists
        Assert-True ($arr[0].success -eq $false) "mixed result 0 (duplicate $aType '$aName') expected success=false, got $($arr[0].success)"
        Assert-True ($arr[0].exit_code -eq 5) "mixed result 0 (duplicate $aType '$aName') expected exit_code=5, got $($arr[0].exit_code)"
        if ($arr[0].error.existing_prog_id -ne $aProgId) {
            throw "mixed result 0 (duplicate $aType '$aName') existing_prog_id mismatch (expected $aProgId, got $($arr[0].error.existing_prog_id))"
        }
        Write-Log "  result A (duplicate): success=false, exit=5, existing_prog_id=$($arr[0].error.existing_prog_id)" 'Green'

        # Item B: new item -> success
        Assert-True ($arr[1].success -eq $true) "mixed result 1 (new $bType '$bName') expected success=true, got $($arr[1].success)"
        Assert-True ($arr[1].prog_id -gt 0) "mixed result 1 (new $bType '$bName') prog_id expected >0, got $($arr[1].prog_id)"
        $bProgId = $arr[1].prog_id
        $script:ExtraItems.Add([pscustomobject]@{ Type = $bType; Name = $bName }) | Out-Null
        Write-Log "  result B (new item): success=true, prog_id=$bProgId" 'Green'

        # Item C: missing source file -> pre-validation failure. The manifest
        # parser stamps unreadable source files with exit_code=3
        # ("failed to read source file").
        Assert-True ($arr[2].success -eq $false) "mixed result 2 (missing source $cType '$cName') expected success=false, got $($arr[2].success)"
        Assert-True ($arr[2].exit_code -eq 3) "mixed result 2 (missing source $cType '$cName') expected exit_code=3, got $($arr[2].exit_code)"
        Write-Log "  result C (missing source): success=false, exit=3" 'Green'

        # The batch must have persisted item B despite the sibling failures
        $beres = Invoke-TsCli -Arguments @('exists', $bType, $bName, '--case-insensitive', '--json')
        if ($beres.ExitCode -ne 0) {
            throw "exists (mixed B) for $bType '$bName' failed: exit $($beres.ExitCode) stderr: $($beres.Stderr)"
        }
        $bj = $beres.Stdout | ConvertFrom-Json
        if (-not $bj.exists) {
            throw "mixed batch item B ($bType '$bName') should be persisted, but exists=false"
        }
        if ($bj.prog_id -ne $bProgId) {
            throw "mixed batch item B ($bType '$bName') prog_id mismatch (expected $bProgId, got $($bj.prog_id))"
        }
        Write-Log "  item B persisted after mixed batch: exists=true, prog_id=$($bj.prog_id)" 'Green'

        # Confirm duplicate A is still present (batch did not remove or corrupt it)
        $aeres = Invoke-TsCli -Arguments @('exists', $aType, $aName, '--case-insensitive', '--json')
        if ($aeres.ExitCode -ne 0) {
            throw "exists (mixed A) for $aType '$aName' failed: exit $($aeres.ExitCode) stderr: $($aeres.Stderr)"
        }
        $aj = $aeres.Stdout | ConvertFrom-Json
        if (-not $aj.exists) {
            throw "mixed batch duplicate A ($aType '$aName') should still exist, but exists=false"
        }
        Write-Log "  item A (duplicate) still present: exists=true, prog_id=$($aj.prog_id)" 'Green'

        # Verify B source was actually saved
        $bsres = Invoke-TsCli -Arguments @('get-source', $bType, $bName, '--json')
        if ($bsres.ExitCode -ne 0) {
            throw "get-source (mixed B) for $bType '$bName' failed: exit $($bsres.ExitCode) stderr: $($bsres.Stderr)"
        }
        $bsj = $bsres.Stdout | ConvertFrom-Json
        if (-not $bsj.success -or -not $bsj.source) {
            throw "get-source (mixed B) for $bType '$bName' returned invalid result"
        }
        if (-not $bsj.source.Contains('= 1')) {
            throw "get-source (mixed B) for $bType '$bName' missing marker '= 1'"
        }
        Write-Log "  item B get-source contains expected marker '= 1'" 'Green'

        Assert-BatchStderrProgress -Stderr $res.Stderr -Results $arr -ExpectedExitCode 2

        Record-Step '13.mixed-outcome' 'PASS'
    } catch {
        Write-Log "  Step 13 FAIL: $_" 'Red'
        Record-Step '13.mixed-outcome' 'FAIL' $_
        $script:AllPassed = $false
    }
}

# ---------------------------------------------------------------------------
# Tagged-serve progress step (TVS A) — standalone named-pipe integration.
#
# Launches the elevated `tsdev_cli serve` broker itself (once per rendering
# mode: default human stderr, then --json NDJSON stderr) with a unique
# --serve-pipe-file handshake, connects over the 4-byte little-endian uint32
# length prefix + UTF-8 JSON frame protocol, and asserts:
#   * an untagged exists response carries NO "progress" member;
#   * each tagged request (insert ok, duplicate insert dispatch failure,
#     delete cleanup) returns exactly one ordinary response frame enriched
#     with "progress":{"batch_id","index","total","phase":"completed"};
#   * the broker's stderr already holds the exact started/completed progress
#     lines BEFORE quit/process exit (live terminal progress, fflush'd per
#     event) — human BATCH lines or parseable NDJSON event objects;
#   * every response is matched to its request (one frame each — no desync).
# ---------------------------------------------------------------------------
function Send-ServeRequest {
    param(
        [Parameter(Mandatory)] $Pipe,
        [Parameter(Mandatory)] [hashtable]$Request
    )
    $payload = [System.Text.Encoding]::UTF8.GetBytes(($Request | ConvertTo-Json -Depth 10 -Compress))
    $len = [System.BitConverter]::GetBytes([uint32]$payload.Length)
    $Pipe.Write($len, 0, $len.Length)
    $Pipe.Write($payload, 0, $payload.Length)
    $Pipe.Flush()
}

function Read-PipeExact {
    param(
        [Parameter(Mandatory)] $Pipe,
        [Parameter(Mandatory)] [int]$Count,
        [int]$TimeoutMs = $script:ServeIoTimeoutMs
    )
    $buf = New-Object byte[] $Count
    $got = 0
    while ($got -lt $Count) {
        # Bound each partial read with an explicit async wait: the pipe's
        # own ReadTimeout is unusable on .NET Framework (CanTimeout=false).
        $ar = $Pipe.BeginRead($buf, $got, $Count - $got, $null, $null)
        $completed = $false
        try {
            $completed = $ar.AsyncWaitHandle.WaitOne($TimeoutMs)
        } finally {
            $ar.AsyncWaitHandle.Close()
        }
        if (-not $completed) {
            # The pending read is abandoned; Invoke-TaggedServeRun's finally
            # disposes the pipe, which unblocks it.
            throw "serve pipe read timed out after ${TimeoutMs}ms ($got/$Count bytes)"
        }
        $n = $Pipe.EndRead($ar)
        if ($n -le 0) { throw "serve pipe closed mid-frame ($got/$Count bytes)" }
        $got += $n
    }
    return ,$buf
}

function Receive-ServeResponse {
    param([Parameter(Mandatory)] $Pipe)
    $hdr = Read-PipeExact -Pipe $Pipe -Count 4
    $len = [System.BitConverter]::ToUInt32($hdr, 0)
    if ($len -gt 67108864) { throw "serve response frame $len bytes exceeds the 64 MiB cap" }
    if ($len -eq 0) { return $null }
    $payload = Read-PipeExact -Pipe $Pipe -Count ([int]$len)
    return ([System.Text.Encoding]::UTF8.GetString($payload) | ConvertFrom-Json)
}

function Send-ServeRoundTrip {
    param(
        [Parameter(Mandatory)] $Pipe,
        [Parameter(Mandatory)] [hashtable]$Request
    )
    Send-ServeRequest -Pipe $Pipe -Request $Request
    return Receive-ServeResponse -Pipe $Pipe
}

# Read a file another process still holds open for writing (serve stderr
# capture): shared-read open, read what is available so far.
function Read-SharedFile {
    param([Parameter(Mandatory)] [string]$Path)
    $fs = New-Object System.IO.FileStream($Path, [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    try {
        $sr = New-Object System.IO.StreamReader($fs, [System.Text.Encoding]::UTF8)
        try { return $sr.ReadToEnd() } finally { $sr.Close() }
    } finally {
        $fs.Close()
    }
}

# Assert the tagged response's "progress" member is exactly
# {"batch_id":<id>,"index":<i>,"total":<t>,"phase":"completed"} — no more,
# no fewer members.
function Assert-ServeProgressMember {
    param(
        [Parameter(Mandatory)] $Response,
        [Parameter(Mandatory)] [string]$BatchId,
        [Parameter(Mandatory)] [int]$Index,
        [Parameter(Mandatory)] [int]$Total,
        [Parameter(Mandatory)] [string]$Label
    )
    $p = $Response.progress
    Assert-True ($null -ne $p) "[$Label] tagged response missing 'progress' member"
    $names = @($p.PSObject.Properties.Name)
    Assert-True ($names.Count -eq 4) "[$Label] progress must have exactly 4 members, got: $($names -join ',')"
    Assert-True ($p.batch_id -eq $BatchId) "[$Label] progress.batch_id: expected '$BatchId', got '$($p.batch_id)'"
    Assert-True ($p.index -eq $Index) "[$Label] progress.index: expected $Index, got '$($p.index)'"
    Assert-True ($p.total -eq $Total) "[$Label] progress.total: expected $Total, got '$($p.total)'"
    Assert-True ($p.phase -eq 'completed') "[$Label] progress.phase: expected 'completed', got '$($p.phase)'"
}

function Invoke-TaggedServeRun {
    param(
        [Parameter(Mandatory)] [string]$Label,    # 'human' | 'json'
        [Parameter(Mandatory)] [bool]$JsonMode,
        [Parameter(Mandatory)] [string]$FnName,
        [Parameter(Mandatory)] [string]$BatchId
    )
    $pipeFile = Join-Path $script:SourceDir "serve_pipe_$Label.txt"
    $errFile = Join-Path $script:SourceDir "serve_stderr_$Label.txt"
    $outFile = Join-Path $script:SourceDir "serve_stdout_$Label.txt"
    foreach ($f in @($pipeFile, "$pipeFile.tmp", $errFile, $outFile)) {
        if (Test-Path $f) { Remove-Item $f -Force -ErrorAction SilentlyContinue }
    }
    $proc = $null
    $pipe = $null
    try {
        $serveArgs = @('serve', '--serve-pipe-file', $pipeFile)
        if ($JsonMode) { $serveArgs = @('--json') + $serveArgs }
        $argString = Build-ArgString -Arguments $serveArgs
        Write-Log "  [$Label] launching broker: tsdev_cli $argString"
        $prevEAP = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $proc = Start-Process -FilePath $script:CliPath -ArgumentList $argString `
                -RedirectStandardOutput $outFile -RedirectStandardError $errFile `
                -NoNewWindow -PassThru
        } finally {
            $ErrorActionPreference = $prevEAP
        }
        if ($null -eq $proc) { throw "[$Label] failed to launch tsdev_cli serve" }
        # Cache the process handle before exit so PS 5.1 can report ExitCode.
        $null = $proc.Handle

        # Wait for the pipe-name handshake file (broker writes the pipe name
        # on line 1 and its PID on line 2 atomically once the pipe exists).
        $pipeName = $null
        $deadline = [System.Diagnostics.Stopwatch]::StartNew()
        while (-not $pipeName -and $deadline.ElapsedMilliseconds -lt 30000) {
            if ($proc.HasExited) {
                $errText = if (Test-Path $errFile) { [System.IO.File]::ReadAllText($errFile) } else { '' }
                throw "[$Label] serve broker exited before writing the pipe file (exit $($proc.ExitCode)); stderr: $errText"
            }
            if ((Test-Path $pipeFile) -and ((Get-Item $pipeFile).Length -gt 0)) {
                $pipeName = ([System.IO.File]::ReadAllText($pipeFile) -split "`r?`n")[0].Trim()
            }
            if (-not $pipeName) { Start-Sleep -Milliseconds 100 }
        }
        if (-not $pipeName) { throw "[$Label] serve broker did not write the pipe file within 30s" }
        Assert-True ($pipeName -match '^\\\\\.\\pipe\\tsdev_cli_\d+$') "[$Label] unexpected pipe name '$pipeName'"

        $shortName = $pipeName.Substring($pipeName.LastIndexOf('\') + 1)
        $pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', $shortName,
            [System.IO.Pipes.PipeDirection]::InOut)
        # .NET Framework PipeStream.CanTimeout is false regardless of
        # PipeOptions.Asynchronous, so ReadTimeout/WriteTimeout cannot be
        # set: reads are bounded inside Read-PipeExact and Connect(int)
        # bounds the connect.
        $pipe.Connect(15000)
        Write-Log "  [$Label] connected to $pipeName" 'Green'

        # Request 1: untagged exists — proves the unique name is absent and
        # that untagged responses carry no "progress" member.
        $r = Send-ServeRoundTrip -Pipe $pipe -Request @{
            op = 'exists'; type = 'function'; name = $FnName }
        Assert-True ($null -ne $r) "[$Label] exists: no response frame"
        Assert-True ($r.op -eq 'exists') "[$Label] exists: response op '$($r.op)' != 'exists'"
        Assert-True ($r.success -eq $true) "[$Label] exists failed: $($r | ConvertTo-Json -Compress -Depth 5)"
        Assert-True ($r.exists -eq $false) "[$Label] exists: '$FnName' unexpectedly present"
        Assert-True (-not ($r.PSObject.Properties.Name -contains 'progress')) "[$Label] untagged response must not carry 'progress'"
        Write-Log "  [$Label] untagged exists: absent, no progress member" 'Green'
        if (Check-TsDialogs "tagged-serve-$Label exists") { throw "[$Label] TS dialog/crash after exists" }

        # Request 2: tagged insert — dispatch success.
        $r = Send-ServeRoundTrip -Pipe $pipe -Request @{
            op = 'insert'; type = 'function'; name = $FnName
            source = "$FnName = 1;"
            batch = @{ id = $BatchId; index = 1; total = 3 } }
        Assert-True ($r.op -eq 'insert') "[$Label] insert: response op '$($r.op)' != 'insert'"
        Assert-True ($r.success -eq $true) "[$Label] tagged insert failed: $($r | ConvertTo-Json -Compress -Depth 5)"
        Assert-True ($r.exit_code -eq 0) "[$Label] tagged insert exit_code $($r.exit_code) != 0"
        Assert-True ($r.prog_id -gt 0) "[$Label] tagged insert prog_id missing/<=0"
        Assert-ServeProgressMember -Response $r -BatchId $BatchId -Index 1 -Total 3 -Label $Label
        Write-Log "  [$Label] tagged insert [1/3]: success, progress member exact" 'Green'
        if (Check-TsDialogs "tagged-serve-$Label insert") { throw "[$Label] TS dialog/crash after insert" }

        # Request 3: tagged duplicate insert — dispatch failure (the item now
        # exists), NOT a parser failure; the failure response is still
        # enriched with progress.
        $r = Send-ServeRoundTrip -Pipe $pipe -Request @{
            op = 'insert'; type = 'function'; name = $FnName
            source = "$FnName = 1;"
            batch = @{ id = $BatchId; index = 2; total = 3 } }
        Assert-True ($r.op -eq 'insert') "[$Label] dup insert: response op '$($r.op)' != 'insert'"
        Assert-True ($r.success -eq $false) "[$Label] tagged duplicate insert unexpectedly succeeded"
        Assert-True ($r.exit_code -eq 5) "[$Label] tagged duplicate insert exit_code $($r.exit_code) != 5"
        Assert-ServeProgressMember -Response $r -BatchId $BatchId -Index 2 -Total 3 -Label $Label
        Write-Log "  [$Label] tagged duplicate insert [2/3]: exit 5, progress member exact" 'Green'
        if (Check-TsDialogs "tagged-serve-$Label dup-insert") { throw "[$Label] TS dialog/crash after duplicate insert" }

        # Request 4: tagged delete — cleanup.
        $r = Send-ServeRoundTrip -Pipe $pipe -Request @{
            op = 'delete'; type = 'function'; name = $FnName
            batch = @{ id = $BatchId; index = 3; total = 3 } }
        Assert-True ($r.op -eq 'delete') "[$Label] delete: response op '$($r.op)' != 'delete'"
        Assert-True ($r.success -eq $true) "[$Label] tagged delete failed: $($r | ConvertTo-Json -Compress -Depth 5)"
        Assert-True ($r.exit_code -eq 0) "[$Label] tagged delete exit_code $($r.exit_code) != 0"
        Assert-ServeProgressMember -Response $r -BatchId $BatchId -Index 3 -Total 3 -Label $Label
        Write-Log "  [$Label] tagged delete [3/3]: success, progress member exact" 'Green'
        if (Check-TsDialogs "tagged-serve-$Label delete") { throw "[$Label] TS dialog/crash after delete" }

        # The broker is STILL ALIVE here: its stderr must already hold the
        # complete started/completed pair per tagged request (each event is
        # fflush'd) — proving live terminal progress before quit/exit.
        Assert-True (-not $proc.HasExited) "[$Label] broker exited before quit was sent"
        $stderrText = Read-SharedFile $errFile
        Write-Log "  [$Label] broker stderr while alive:"
        foreach ($line in ($stderrText -split "`r?`n")) {
            if ($line) { Write-Log "    $line" }
        }
        # --serve-pipe-file means the broker publishes the pipe name to the
        # file, not stdout — captured stdout must be empty (stdout purity).
        $stdoutText = if (Test-Path $outFile) { Read-SharedFile $outFile } else { '' }
        Assert-True ([string]::IsNullOrWhiteSpace($stdoutText)) "[$Label] broker stdout must be empty with --serve-pipe-file, got: $stdoutText"
        if (-not $JsonMode) {
            $lines = @($stderrText -split "`r?`n" | Where-Object { $_ -cmatch '^BATCH ' })
            Assert-True ($lines.Count -eq 6) "[$Label] expected 6 BATCH stderr lines, got $($lines.Count): $stderrText"
            $exp = @(
                ('BATCH START [1/3] op=insert type="function" name="{0}"' -f $FnName),
                ('BATCH SUCCESS [1/3] op=insert type="function" name="{0}" exit_code=0' -f $FnName),
                ('BATCH START [2/3] op=insert type="function" name="{0}"' -f $FnName),
                $null,
                ('BATCH START [3/3] op=delete type="function" name="{0}"' -f $FnName),
                ('BATCH SUCCESS [3/3] op=delete type="function" name="{0}" exit_code=0' -f $FnName)
            )
            for ($i = 0; $i -lt 6; $i++) {
                if ($i -eq 3) { continue }
                Assert-True ($lines[$i] -ceq $exp[$i]) "[$Label] stderr line ${i}: expected '$($exp[$i])', got '$($lines[$i])'"
            }
            $failPrefix = 'BATCH FAILED [2/3] op=insert type="function" name="{0}" exit_code=5 message="' -f $FnName
            Assert-True ($lines[3].StartsWith($failPrefix) -and $lines[3].EndsWith('"')) "[$Label] stderr line 3: expected '$failPrefix<msg>`"', got '$($lines[3])'"
        } else {
            Assert-True ($stderrText -cnotmatch '(?m)^BATCH ') "[$Label] --json stderr must not contain human BATCH lines"
            $events = @()
            foreach ($line in ($stderrText -split "`r?`n")) {
                if ([string]::IsNullOrWhiteSpace($line)) { continue }
                $o = $null
                try { $o = $line | ConvertFrom-Json } catch {}
                if ($null -ne $o -and ($o.PSObject.Properties.Name -contains 'event')) { $events += $o }
            }
            Assert-True ($events.Count -eq 6) "[$Label] expected 6 NDJSON progress events, got $($events.Count): $stderrText"
            $expected = @(
                @{ ev = 'batch_item_started';   op = 'insert'; idx = 1; ok = $null;  ec = 0 },
                @{ ev = 'batch_item_completed'; op = 'insert'; idx = 1; ok = $true;  ec = 0 },
                @{ ev = 'batch_item_started';   op = 'insert'; idx = 2; ok = $null;  ec = 0 },
                @{ ev = 'batch_item_completed'; op = 'insert'; idx = 2; ok = $false; ec = 5 },
                @{ ev = 'batch_item_started';   op = 'delete'; idx = 3; ok = $null;  ec = 0 },
                @{ ev = 'batch_item_completed'; op = 'delete'; idx = 3; ok = $true;  ec = 0 }
            )
            for ($i = 0; $i -lt 6; $i++) {
                $e = $events[$i]
                $x = $expected[$i]
                Assert-True ($e.event -eq $x.ev) "[$Label] event[$i]: expected '$($x.ev)', got '$($e.event)'"
                Assert-True ($e.batch_operation -eq $x.op) "[$Label] event[$i].batch_operation: expected '$($x.op)', got '$($e.batch_operation)'"
                Assert-True ($e.index -eq $x.idx) "[$Label] event[$i].index: expected $($x.idx), got '$($e.index)'"
                Assert-True ($e.total -eq 3) "[$Label] event[$i].total: expected 3, got '$($e.total)'"
                Assert-True ($e.type -eq 'function') "[$Label] event[$i].type: expected 'function', got '$($e.type)'"
                Assert-True ($e.name -eq $FnName) "[$Label] event[$i].name: expected '$FnName', got '$($e.name)'"
                if ($x.ev -eq 'batch_item_completed') {
                    Assert-True ($e.success -eq $x.ok) "[$Label] event[$i].success: expected $($x.ok), got '$($e.success)'"
                    Assert-True ($e.exit_code -eq $x.ec) "[$Label] event[$i].exit_code: expected $($x.ec), got '$($e.exit_code)'"
                    Assert-True ($null -ne $e.result) "[$Label] event[$i] missing nested 'result' object"
                    Assert-True ($e.result.success -eq $x.ok) "[$Label] event[$i].result.success mismatch"
                    Assert-True ($e.result.exit_code -eq $x.ec) "[$Label] event[$i].result.exit_code mismatch"
                }
            }
        }
        Write-Log "  [$Label] live stderr progress verified before quit" 'Green'

        # Quit: exactly one response frame (untagged — no progress member),
        # then the broker exits cleanly.
        $qr = Send-ServeRoundTrip -Pipe $pipe -Request @{ op = 'quit' }
        Assert-True ($null -ne $qr) "[$Label] quit: no response frame"
        Assert-True ($qr.op -eq 'quit') "[$Label] quit: response op '$($qr.op)' != 'quit'"
        Assert-True ($qr.success -eq $true) "[$Label] quit not acknowledged"
        Assert-True (-not ($qr.PSObject.Properties.Name -contains 'progress')) "[$Label] quit response must not carry 'progress'"
        $pipe.Close()
        $pipe = $null
        [void]$proc.WaitForExit(30000)
        Assert-True ($proc.HasExited) "[$Label] broker did not exit within 30s of quit"
        Assert-True ($proc.ExitCode -eq 0) "[$Label] broker exit $($proc.ExitCode) != 0"
        Write-Log "  [$Label] quit acknowledged; broker exited 0 (5 responses matched 5 requests)" 'Green'
    } finally {
        if ($null -ne $pipe) { try { $pipe.Dispose() } catch {} }
        if ($null -ne $proc) {
            if (-not $proc.HasExited) {
                # A broker still running here means the run did NOT finish
                # cleanly (assertion throw, request timeout, or hung quit) —
                # a TS modal popup may be blocking the request. Read, record
                # and dismiss any failure surface BEFORE killing the broker.
                # On the normal quit path the broker has already exited, so
                # this check cannot produce a false failure.
                try { [void](Check-TsDialogs "tagged-serve-$Label pre-kill") } catch {}
                Write-Log "  [$Label] broker still running - killing PID $($proc.Id)" 'Yellow'
                try { $proc.Kill() } catch {}
            }
            try { $proc.Dispose() } catch {}
        }
        foreach ($f in @($pipeFile, "$pipeFile.tmp", $errFile, $outFile)) {
            if (Test-Path $f) { Remove-Item $f -Force -ErrorAction SilentlyContinue }
        }
    }
}

function Step-TaggedServe {
    Write-Log "--- Step 14: tagged-serve batch progress (TVS A, live broker) ---" 'Cyan'
    # Separate unique names per run so a failed human run cannot poison the
    # JSON run; both are registered up front so Step-FinalCleanup is an
    # idempotent backstop even if the step dies before its tagged delete.
    $serveFnHuman = "BatchE2eSrvHuman_$gapId"
    $serveFnJson = "BatchE2eSrvJson_$gapId"
    $script:ExtraItems.Add([pscustomobject]@{ Type = 'function'; Name = $serveFnHuman }) | Out-Null
    $script:ExtraItems.Add([pscustomobject]@{ Type = 'function'; Name = $serveFnJson }) | Out-Null
    try {
        Invoke-TaggedServeRun -Label 'human' -JsonMode $false -FnName $serveFnHuman -BatchId "svcb-human-$gapId"
        Record-Step '14.tagged-serve-human' 'PASS'
    } catch {
        Write-Log "  Step 14 (human) FAIL: $_" 'Red'
        Record-Step '14.tagged-serve-human' 'FAIL' $_
        # Any human-run failure (dialog, assertion, timeout, pipe, broker)
        # halts further TS-touching verification: the JSON run and the
        # direct cleanup verification are skipped, and Step-FinalCleanup
        # remains the mandatory backstop for the registered ExtraItems.
        $script:AllPassed = $false
        $script:Halt = $true
    }
    if (-not $script:Halt) {
        try {
            Invoke-TaggedServeRun -Label 'json' -JsonMode $true -FnName $serveFnJson -BatchId "svcb-json-$gapId"
            Record-Step '15.tagged-serve-json' 'PASS'
        } catch {
            Write-Log "  Step 15 (json) FAIL: $_" 'Red'
            Record-Step '15.tagged-serve-json' 'FAIL' $_
            $script:AllPassed = $false
            $script:Halt = $true
        }
    }
    if (-not $script:Halt) {
        try {
            # The tagged delete should have removed each run's item; verify
            # both names and best-effort clean any leftover (a leftover
            # means a run died before its delete request — recorded as a
            # failure either way; a nonzero cleanup delete is a hard
            # failure). Only reached when no run failed — on Halt this
            # direct verification is skipped entirely (no row recorded,
            # matching the script's Halt-gating convention) and no TS
            # process is launched here.
            $leftovers = @()
            foreach ($fn in @($serveFnHuman, $serveFnJson)) {
                $res = Invoke-TsCli -Arguments @('exists', 'function', $fn, '--case-insensitive', '--json')
                if ($res.ExitCode -ne 0) {
                    throw "exists (serve cleanup) for '$fn' failed: exit $($res.ExitCode) stderr: $($res.Stderr)"
                }
                if (Check-TsDialogs "serve-cleanup exists $fn") { throw "TS dialog/crash during serve-cleanup exists for '$fn'" }
                $j = $res.Stdout | ConvertFrom-Json
                if ($j.exists) {
                    $leftovers += $fn
                    $d = Invoke-TsCli -Arguments @('delete', 'function', $fn, '--json')
                    if (Check-TsDialogs "serve-cleanup delete $fn") { throw "TS dialog/crash during serve-cleanup delete for '$fn'" }
                    if ($d.ExitCode -ne 0) {
                        throw "cleanup delete for '$fn' failed: exit $($d.ExitCode) stderr: $($d.Stderr)"
                    }
                    Write-Log "  serve item '$fn' was still present; deleted in cleanup" 'Yellow'
                } else {
                    Write-Log "  serve item '$fn' confirmed absent" 'Green'
                }
            }
            if ($leftovers.Count -gt 0) {
                Record-Step '16.tagged-serve-cleanup' 'FAIL' "leftover(s) required cleanup: $($leftovers -join ', ')"
                $script:AllPassed = $false
            } else {
                Record-Step '16.tagged-serve-cleanup' 'PASS'
            }
        } catch {
            Write-Log "  Step 16 (cleanup verify) FAIL: $_" 'Red'
            Record-Step '16.tagged-serve-cleanup' 'FAIL' $_
            $script:AllPassed = $false
        }
    }
}

function Step-BatchControl {
    # TVS C: live --batch-progress-control coverage (steps 17-19).
    #   17: manifest-path control, human stderr — continue then cancel ->
    #       exit 3, full-length array with an exact exit-12 marker.
    #   18: framed-stdin control, --json — TSDEV_BATCH_CONTROL/1 frame +
    #       delete manifest + decisions on one stream -> exit 2 with a real
    #       not_found item failure (no marker, no control error).
    #   19: argument gates — repeated flag and non-batch use are usage
    #       errors (exit 1) with no progress and no stdout array.
    # The non-elevated ordering contract (exit 11 must precede these usage
    # errors) is exercised by the gate harness that can launch a
    # non-elevated child; this elevated script cannot demote itself.
    $bcA = "BatchE2eBcA_$gapId"
    $bcB = "BatchE2eBcB_$gapId"
    $bcC = "BatchE2eBcC_$gapId"
    $bcNames = @($bcA, $bcB, $bcC)
    # Registered up front so Step-FinalCleanup is the mandatory backstop.
    foreach ($n in $bcNames) {
        $script:ExtraItems.Add([pscustomobject]@{ Type = 'function'; Name = $n }) | Out-Null
    }
    $srcA = Write-SourceFile 'control_fn_a.el' "$bcA = 1;"
    $srcB = Write-SourceFile 'control_fn_b.el' "$bcB = 1;"
    $srcC = Write-SourceFile 'control_fn_c.el' "$bcC = 1;"
    $manifest = Write-ManifestFile 'control_insert.manifest' @(
        "function $bcA $srcA",
        "function $bcB $srcB",
        "function $bcC $srcC"
    )
    $decisionsFile = Join-Path $script:SourceDir 'control_decisions.txt'
    $payloadFile = Join-Path $script:SourceDir 'control_delete_framed.bin'
    $emptyStdin = Join-Path $script:SourceDir 'control_empty_stdin.txt'
    [System.IO.File]::WriteAllText($emptyStdin, '', [System.Text.Encoding]::ASCII)

    Write-Log '--- Step 17: batch-control manifest path (human, cancel) ---' 'Cyan'
    try {
        # Precheck: exact unique names must be absent (delete stale
        # leftovers, like Step-PreClean), with a dialog check per action.
        foreach ($n in $bcNames) {
            $res = Invoke-TsCli -Arguments @('exists', 'function', $n, '--case-insensitive', '--json')
            if ($res.ExitCode -ne 0) {
                throw "batch-control precheck exists 'function $n' failed: exit $($res.ExitCode) stderr: $($res.Stderr)"
            }
            if (Check-TsDialogs "batch-control precheck exists $n") { throw 'TS dialog/crash during batch-control precheck' }
            $j = $res.Stdout | ConvertFrom-Json
            if ($j.exists) {
                Write-Log "  Stale leftover found: function '$n'; deleting..." 'Yellow'
                $del = Invoke-TsCli -Arguments @('delete', 'function', $n, '--json')
                if ($del.ExitCode -ne 0) {
                    throw "batch-control precheck delete 'function $n' failed: exit $($del.ExitCode) stderr: $($del.Stderr)"
                }
                if (Check-TsDialogs "batch-control precheck delete $n") { throw 'TS dialog/crash during batch-control precheck delete' }
            }
        }

        # stdin carries exactly "continue\ncancel\n" (LF only); the manifest
        # still comes from its path argument.
        [System.IO.File]::WriteAllText($decisionsFile, "continue`ncancel`n", [System.Text.Encoding]::ASCII)
        $res = Invoke-TsControlBatch -Arguments @('batch-insert', $manifest, '--batch-progress-control') -StdinFile $decisionsFile
        if ($res.TimedOut) { throw 'controlled batch-insert timed out waiting for the child' }
        Write-Log "  batch-insert --batch-progress-control exit code: $($res.ExitCode)"
        Assert-True ($res.ExitCode -eq 3) "expected exit 3 (cooperative cancel), got $($res.ExitCode); stderr: $($res.Stderr)"
        # PS5.1 ConvertFrom-Json emits a top-level JSON array as ONE
        # unenumerated Object[] — wrapping the pipeline in @() nests it
        # (count 1). Assign first, then guard the single-result edge case.
        $arr = $res.Stdout | ConvertFrom-Json
        if (-not ($arr -is [array])) { $arr = @($arr) }
        Assert-True ($arr.Count -eq 3) "expected a full-length 3-element array, got $($arr.Count) [stdout: $($res.Stdout)]"
        Assert-True ($arr[0].success -eq $true -and $arr[0].prog_id -gt 0) "item1 must be a real insert success, got: $($arr[0] | ConvertTo-Json -Compress)"
        Assert-True ($arr[1].success -eq $true -and $arr[1].prog_id -gt 0) "item2 must be a real insert success, got: $($arr[1] | ConvertTo-Json -Compress)"
        $m = $arr[2]
        Assert-True ($m.op -eq 'insert' -and $m.type -eq 'function' -and $m.name -eq $bcC) "marker must preserve op/type/name, got: $($m | ConvertTo-Json -Compress)"
        Assert-True ($m.success -eq $false -and $m.exit_code -eq 12 -and
                     $m.error.code -eq 'cancelled' -and
                     $m.error.message -eq 'Batch cancelled before this item was attempted') `
            "item3 must be the exact exit-12 cancelled marker, got: $($m | ConvertTo-Json -Compress)"
        # stderr: exactly the started/completed pairs for items 1-2, then
        # the CANCELLED summary — no item-3 events, no control error.
        $lines = @($res.Stderr -split "`r?`n" | Where-Object { $_ -cmatch '^BATCH ' })
        Assert-True ($lines.Count -eq 5) "expected 5 progress lines (2 pairs + COMPLETE), got $($lines.Count) [stderr: $($res.Stderr)]"
        Assert-True ($lines[0] -ceq "BATCH START [1/3] op=insert type=`"function`" name=`"$bcA`"") "progress[0]: $($lines[0])"
        Assert-True ($lines[1] -ceq "BATCH SUCCESS [1/3] op=insert type=`"function`" name=`"$bcA`" exit_code=0") "progress[1]: $($lines[1])"
        Assert-True ($lines[2] -ceq "BATCH START [2/3] op=insert type=`"function`" name=`"$bcB`"") "progress[2]: $($lines[2])"
        Assert-True ($lines[3] -ceq "BATCH SUCCESS [2/3] op=insert type=`"function`" name=`"$bcB`" exit_code=0") "progress[3]: $($lines[3])"
        $expComplete = 'BATCH COMPLETE CANCELLED op=insert completed=2 total=3 successes=2 failures=0 not_run=1 cancelled=true close_failed=false exit_code=3'
        Assert-True ($lines[4] -ceq $expComplete) "summary line: expected '$expComplete', got: $($lines[4])"
        Assert-True ($res.Stderr -notmatch 'BATCH CONTROL ERROR') "no control-error diagnostic expected [stderr: $($res.Stderr)]"
        if (Check-TsDialogs 'after batch-control human insert') { throw 'TS dialog/crash during controlled batch-insert' }
        Record-Step '17.batch-control-human' 'PASS'
    } catch {
        Write-Log "  Step 17 FAIL: $_" 'Red'
        Record-Step '17.batch-control-human' 'FAIL' $_
        $script:AllPassed = $false
        # Step 18's fixture expectations (A/B present, C absent) depend on
        # this step's controlled insert having run exactly as scripted —
        # halt all further steps; final cleanup still runs unconditionally.
        $script:Halt = $true
    }

    if (-not $script:Halt) {
    Write-Log '--- Step 18: batch-delete framed --batch-stdin control (JSON) ---' 'Cyan'
    try {
        # One stdin stream: ASCII frame header + exact UTF-8 manifest bytes
        # + decision lines. Items 1-2 (inserted in step 17) delete cleanly;
        # item 3 was never attempted there so it is a real not_found
        # failure — exit 2, no markers.
        $delManifest = "function $bcA`nfunction $bcB`nfunction $bcC`n"
        $delBytes = [System.Text.Encoding]::UTF8.GetBytes($delManifest)
        $hdrBytes = [System.Text.Encoding]::ASCII.GetBytes("TSDEV_BATCH_CONTROL/1 $($delBytes.Length)`n")
        $decBytes = [System.Text.Encoding]::ASCII.GetBytes("continue`ncontinue`n")
        $payload = New-Object byte[] ($hdrBytes.Length + $delBytes.Length + $decBytes.Length)
        [Array]::Copy($hdrBytes, 0, $payload, 0, $hdrBytes.Length)
        [Array]::Copy($delBytes, 0, $payload, $hdrBytes.Length, $delBytes.Length)
        [Array]::Copy($decBytes, 0, $payload, $hdrBytes.Length + $delBytes.Length, $decBytes.Length)
        [System.IO.File]::WriteAllBytes($payloadFile, $payload)
        $res = Invoke-TsControlBatch -Arguments @('--json', 'batch-delete', '--batch-stdin', '--batch-progress-control') -StdinFile $payloadFile
        if ($res.TimedOut) { throw 'controlled batch-delete timed out waiting for the child' }
        Write-Log "  --json batch-delete --batch-stdin --batch-progress-control exit code: $($res.ExitCode)"
        Assert-True ($res.ExitCode -eq 2) "expected exit 2 (ran with one item failure), got $($res.ExitCode); stderr: $($res.Stderr)"
        $arr = $res.Stdout | ConvertFrom-Json
        if (-not ($arr -is [array])) { $arr = @($arr) }
        Assert-True ($arr.Count -eq 3) "expected 3 results, got $($arr.Count) [stdout: $($res.Stdout)]"
        Assert-True ($arr[0].success -eq $true -and $arr[0].exit_code -eq 0) "item1 must be a delete success, got: $($arr[0] | ConvertTo-Json -Compress)"
        Assert-True ($arr[1].success -eq $true -and $arr[1].exit_code -eq 0) "item2 must be a delete success, got: $($arr[1] | ConvertTo-Json -Compress)"
        Assert-True ($arr[2].success -eq $false -and $arr[2].exit_code -eq 5 -and
                     $arr[2].error.code -eq 'not_found') `
            "item3 must be a real not_found failure (not a marker), got: $($arr[2] | ConvertTo-Json -Compress)"
        # 3 started/completed pairs + a non-cancelled batch_completed whose
        # nested results equal the stdout array exactly.
        Assert-BatchJsonProgress -Stderr $res.Stderr -Results $arr -ExpectedExitCode 2
        Assert-True (@($res.Stderr -split "`r?`n" | Where-Object { $_ -match 'batch_control_error' }).Count -eq 0) `
            "no batch_control_error event expected [stderr: $($res.Stderr)]"
        if (Check-TsDialogs 'after batch-control json delete') { throw 'TS dialog/crash during controlled batch-delete' }
        Record-Step '18.batch-control-json' 'PASS'
    } catch {
        Write-Log "  Step 18 FAIL: $_" 'Red'
        Record-Step '18.batch-control-json' 'FAIL' $_
        $script:AllPassed = $false
        # Same no-further-actions policy: stop before the remaining gates.
        $script:Halt = $true
    }
    }

    if (-not $script:Halt) {
    Write-Log '--- Step 19: batch-control argument gates ---' 'Cyan'
    try {
        # Repeated flag: usage error AFTER the elevation guard, before any
        # batch work — no progress events, no stdout array, no mutation.
        $res = Invoke-TsControlBatch -Arguments @('batch-insert', $manifest, '--batch-progress-control', '--batch-progress-control') -StdinFile $emptyStdin
        Assert-True ($res.ExitCode -eq 1) "repeated --batch-progress-control must exit 1, got $($res.ExitCode); stdout: $($res.Stdout) stderr: $($res.Stderr)"
        Assert-True ([string]::IsNullOrWhiteSpace($res.Stdout)) "repeated flag must emit no stdout array, got: $($res.Stdout)"
        Assert-True (@($res.Stderr -split "`r?`n" | Where-Object { $_ -cmatch '^BATCH ' }).Count -eq 0) `
            "repeated flag must emit no progress events [stderr: $($res.Stderr)]"
        # Batch-only: the flag on a non-batch subcommand is a usage error.
        $res2 = Invoke-TsCli -Arguments @('exists', 'function', $bcA, '--batch-progress-control')
        Assert-True ($res2.ExitCode -eq 1) "--batch-progress-control on exists must exit 1, got $($res2.ExitCode); stderr: $($res2.Stderr)"
        if (Check-TsDialogs 'after batch-control arg gates') { throw 'TS dialog/crash during batch-control arg gates' }
        Record-Step '19.batch-control-arg-gates' 'PASS'
    } catch {
        Write-Log "  Step 19 FAIL: $_" 'Red'
        Record-Step '19.batch-control-arg-gates' 'FAIL' $_
        $script:AllPassed = $false
    }
    }

    # Remove only the control-channel input/source files this step created
    # (local files only — still runs when a step above set Halt).
    foreach ($f in @($srcA, $srcB, $srcC, $manifest, $decisionsFile, $payloadFile, $emptyStdin)) {
        if ($f -and (Test-Path $f)) { Remove-Item $f -Force -ErrorAction SilentlyContinue }
    }
}

function Step-FinalCleanup {
    Write-Log "--- Step 11: Final cleanup ---" 'Cyan'
    # Mandatory final sanitation: runs even when $script:Halt is set
    # (cleanup-before-closure), but never resumes feature verification.
    # Every exists/delete must exit 0 and every TS action is followed by a
    # dialog check; any failure throws -> FAIL + $script:AllPassed = $false.
    try {
        $allItems = @()
        foreach ($item in $script:Items) { $allItems += $item }
        foreach ($item in $script:ExtraItems) { $allItems += $item }
        foreach ($item in $allItems) {
            $res = Invoke-TsCli -Arguments @('exists', $item.Type, $item.Name, '--case-insensitive', '--json')
            if ($res.ExitCode -ne 0) {
                throw "cleanup exists for $($item.Type) '$($item.Name)' failed: exit $($res.ExitCode) stderr: $($res.Stderr)"
            }
            if (Check-TsDialogs "final-cleanup exists $($item.Name)") {
                throw "TS dialog/crash detected during final cleanup; stopping further cleanup"
            }
            $j = $res.Stdout | ConvertFrom-Json
            if ($j.exists) {
                Write-Log "  Leftover found: $($item.Type) '$($item.Name)' (progId $($j.prog_id)); deleting..."
                $del = Invoke-TsCli -Arguments @('delete', $item.Type, $item.Name, '--json')
                if ($del.ExitCode -ne 0) {
                    throw "cleanup delete for $($item.Type) '$($item.Name)' failed: exit $($del.ExitCode) stderr: $($del.Stderr)"
                }
                if (Check-TsDialogs "final-cleanup delete $($item.Name)") {
                    throw "TS dialog/crash detected during final cleanup; stopping further cleanup"
                }
                Write-Log "  Deleted leftover: $($item.Type) '$($item.Name)'" 'Green'
            } else {
                Write-Log "  Confirmed gone: $($item.Type) '$($item.Name)'" 'Green'
            }
        }
        Record-Step '11.Final-cleanup' 'PASS'
    } catch {
        Write-Log "  Final cleanup error: $_" 'Red'
        Record-Step '11.Final-cleanup' 'FAIL' $_
        $script:AllPassed = $false
    }
}

function Write-LogFile {
    $script:LogLines | Out-File -FilePath $LogFile -Encoding ASCII -Force
    Write-Log "Log written to $LogFile" 'Cyan'
}

function Print-Summary {
    Write-Log ''
    Write-Log '=== SUMMARY TABLE ===' 'Cyan'
    $header = '{0,-35} {1,-8} {2}' -f 'Step', 'Result', 'Detail'
    Write-Log $header
    Write-Log ('{0,-35} {1,-8} {2}' -f ('-' * 35), ('-' * 8), ('-' * 40))
    foreach ($r in $script:StepResults) {
        $detail = $r.Detail -replace "`r|`n", ' '
        $line = '{0,-35} {1,-8} {2}' -f $r.Step, $r.Result, $detail
        $color = if ($r.Result -eq 'PASS') { 'Green' } else { 'Red' }
        Write-Log $line $color
    }
}

# ---- MAIN ----
Write-Log '============================================' 'Cyan'
Write-Log 'TSDev Batch E2E Verification' 'Cyan'
Write-Log '============================================' 'Cyan'
Write-Log "Run ID      : $runId"
Write-Log "Project root: $ProjectRoot"
Write-Log "CLI path    : $script:CliPath"
Write-Log "Source dir  : $SourceDir"
Write-Log "Log file    : $LogFile"
Write-Log "Items       : function=$FnName, indicator=$IndName, showme=$ShowMeName"
Write-Log ''

$v1Fn = Write-SourceFile 'batch_fn_v1.el' $FnV1
$v2Fn = Write-SourceFile 'batch_fn_v2.el' $FnV2
$v1Ind = Write-SourceFile 'batch_ind_v1.el' $IndV1
$v2Ind = Write-SourceFile 'batch_ind_v2.el' $IndV2
$v1ShowMe = Write-SourceFile 'batch_showme_v1.el' $ShowMeV1
$v2ShowMe = Write-SourceFile 'batch_showme_v2.el' $ShowMeV2
$v3Fn = Write-SourceFile 'batch_fn_v3.el' $UpsertV3
$upsertNewFn = Write-SourceFile 'batch_upsert_new_fn_v1.el' $UpsertNewV1
$fileInd = Write-SourceFile 'batch_file_ind_v1.el' $FileIndV1
$mixedDupFn = Write-SourceFile 'batch_mixed_dup_fn_v1.el' $MixedDupV1
$mixedNewFn = Write-SourceFile 'batch_mixed_new_fn_v1.el' $MixedNewV1
Write-Log "Created source files in $SourceDir" 'Green'

try {
    Step-PreClean
    if (-not $script:Halt) { Step-BatchInsert }
    if (-not $script:Halt) { Check-TsDialogs 'after batch-insert' }

    if (-not $script:Halt) { Step-ExistsVerify -Step '3.exists-after-insert' -Phase 'after insert' -ExpectedExists $true }
    if (-not $script:Halt) { Step-GetSourceVerify -Step '4.get-source-v1' -Phase 'after insert' -Version 'v1' }

    if (-not $script:Halt) { Step-BatchModify }
    if (-not $script:Halt) { Check-TsDialogs 'after batch-modify' }

    if (-not $script:Halt) { Step-ExistsVerify -Step '6.exists-after-modify' -Phase 'after modify' -ExpectedExists $true -CheckProgIdPreserved $true }
    if (-not $script:Halt) { Step-GetSourceVerify -Step '7.get-source-v2' -Phase 'after modify' -Version 'v2' }

    if (-not $script:Halt) { Step-BatchUpsert }
    if (-not $script:Halt) { Check-TsDialogs 'after batch-upsert' }

    if (-not $script:Halt) { Step-BatchDelete }
    if (-not $script:Halt) { Check-TsDialogs 'after batch-delete' }

    if (-not $script:Halt) { Step-ExistsVerify -Step '9.exists-after-delete' -Phase 'after delete' -ExpectedExists $false }

    if (-not $script:Halt) { Step-BatchFile }
    if (-not $script:Halt) { Check-TsDialogs 'after batch-insert-file-mode' }

    if (-not $script:Halt) { Step-MixedOutcome }
    if (-not $script:Halt) { Check-TsDialogs 'after mixed-outcome batch' }

    if (-not $script:Halt) { Step-TaggedServe }
    if (-not $script:Halt) { Check-TsDialogs 'after tagged-serve' }

    if (-not $script:Halt) { Step-BatchControl }
    if (-not $script:Halt) { Check-TsDialogs 'after batch-control' }
} finally {
    Step-FinalCleanup
    Print-Summary
    $exitCode = if ($script:AllPassed -and -not $script:Halt) { 0 } else { 1 }
    $finalColor = if ($exitCode -eq 0) { 'Green' } else { 'Red' }
    Write-Log "=== Final result: $(if ($exitCode -eq 0) { 'PASS' } else { 'FAIL' }) ===" $finalColor
    Write-LogFile
    exit $exitCode
}