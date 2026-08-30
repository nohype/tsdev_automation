<#
.SYNOPSIS
    End-to-end batch CLI verification against a live TradeStation Desktop.
.DESCRIPTION
    Exercises tsdev_cli batch-insert, batch-modify, and batch-delete against a
    live TradeStation Desktop. Uses unique names with a timestamp suffix and
    three component types: function, indicator, and showme.
#>

param(
    [string]$CliPath = (Join-Path (Split-Path -Parent $PSScriptRoot) 'dist\tsdev_cli.exe'),
    [string]$TsPath,
    [switch]$Verbose
)

$ErrorActionPreference = 'Stop'

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
    [DllImport("user32.dll", SetLastError = true)] public static extern int GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextLength(IntPtr hWnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr hWnd, StringBuilder lpString, int nMaxCount);

    public static List<Tuple<int, string>> GetVisibleTitlesForPids(List<int> pids) {
        var list = new List<Tuple<int, string>>();
        EnumWindows((hWnd, lParam) => {
            if (!IsWindowVisible(hWnd)) return true;
            uint pid;
            GetWindowThreadProcessId(hWnd, out pid);
            if (pids.Contains((int)pid)) {
                int len = GetWindowTextLength(hWnd);
                if (len > 0) {
                    StringBuilder sb = new StringBuilder(len + 1);
                    GetWindowText(hWnd, sb, len + 1);
                    list.Add(Tuple.Create((int)pid, sb.ToString()));
                }
            }
            return true;
        }, IntPtr.Zero);
        return list;
    }
}
"@

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
    foreach ($name in @('orchart', 'TSDev', 'TSCrashReport')) {
        $p = Get-Process -Name $name -ErrorAction SilentlyContinue
        if ($p) { $procs += $p }
    }

    $pids = $procs | Select-Object -ExpandProperty Id
    $allPids = New-Object System.Collections.Generic.List[int]
    if ($pids) {
        foreach ($id in $pids) { $allPids.Add($id) | Out-Null }
    }

    if ($allPids.Count -gt 0) {
        $titles = [TsWindowEnum]::GetVisibleTitlesForPids($allPids)
        foreach ($t in $titles) {
            if ($t.Item2 -match 'error|warning|crash|exception') {
                Write-Log "  ALERT: PID $($t.Item1) window title: $($t.Item2)" 'Red'
                $detected = $true
            }
        }
    }

    $crashProcs = Get-Process -Name 'TSCrashReport' -ErrorAction SilentlyContinue
    if ($crashProcs) {
        Write-Log "  CRASH: TSCrashReport.exe is running (PIDs: $($crashProcs.Id -join ', '))" 'Red'
        $detected = $true
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

        # Pre-insert item A so it becomes a duplicate in the subsequent batch
        $ires = Invoke-TsCli -Arguments @('insert', $aType, $aName, $aSource, '--json')
        if ($ires.ExitCode -ne 0) {
            throw "pre-insert duplicate $aType '$aName' failed: exit $($ires.ExitCode) stderr: $($ires.Stderr)"
        }
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

        # Item C: missing source file -> pre-validation failure
        Assert-True ($arr[2].success -eq $false) "mixed result 2 (missing source $cType '$cName') expected success=false, got $($arr[2].success)"
        Write-Log "  result C (missing source): success=false, exit=$($arr[2].exit_code)" 'Green'

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

        Record-Step '13.mixed-outcome' 'PASS'
    } catch {
        Write-Log "  Step 13 FAIL: $_" 'Red'
        Record-Step '13.mixed-outcome' 'FAIL' $_
        $script:AllPassed = $false
    }
}

function Step-FinalCleanup {
    Write-Log "--- Step 11: Final cleanup ---" 'Cyan'
    try {
        $allItems = @()
        foreach ($item in $script:Items) { $allItems += $item }
        foreach ($item in $script:ExtraItems) { $allItems += $item }
        foreach ($item in $allItems) {
            $res = Invoke-TsCli -Arguments @('exists', $item.Type, $item.Name, '--case-insensitive', '--json')
            $j = $res.Stdout | ConvertFrom-Json
            if ($j.exists) {
                Write-Log "  Leftover found: $($item.Type) '$($item.Name)' (progId $($j.prog_id)); deleting..."
                $del = Invoke-TsCli -Arguments @('delete', $item.Type, $item.Name, '--json')
                if ($del.ExitCode -eq 0) {
                    Write-Log "  Deleted leftover: $($item.Type) '$($item.Name)'" 'Green'
                } else {
                    Write-Log "  WARN: delete leftover $($item.Type) '$($item.Name)' returned exit $($del.ExitCode); stderr: $($del.Stderr)" 'Yellow'
                }
            } else {
                Write-Log "  Confirmed gone: $($item.Type) '$($item.Name)'" 'Green'
            }
        }
        Record-Step '11.Final-cleanup' 'PASS'
    } catch {
        Write-Log "  Final cleanup error: $_" 'Yellow'
        Record-Step '11.Final-cleanup' 'FAIL' $_
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
} finally {
    Step-FinalCleanup
    Print-Summary
    $exitCode = if ($script:AllPassed -and -not $script:Halt) { 0 } else { 1 }
    $finalColor = if ($exitCode -eq 0) { 'Green' } else { 'Red' }
    Write-Log "=== Final result: $(if ($exitCode -eq 0) { 'PASS' } else { 'FAIL' }) ===" $finalColor
    Write-LogFile
    exit $exitCode
}