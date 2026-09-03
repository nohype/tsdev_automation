# E2E for tsdevctl `open` against a real TSDev instance.
# REQUIRES: elevated process, TradeStation 9.5 installed. Skippable in CI.
# Compatible with PowerShell 5.1 and pwsh 7.
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot "build\e2e"
$tsdevctl = Join-Path $buildDir "Release\tsdevctl.exe"

function Test-Elevated {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-Elevated)) { Write-Error "E2E requires an elevated shell (R8)"; exit 1 }

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($install) {
            $candidate = Join-Path $install "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
            if (Test-Path $candidate) { $cmake = $candidate }
        }
    }
} else {
    $cmake = $cmake.Source
}
if (-not $cmake) { Write-Error "cmake not found"; exit 1 }

& $cmake -S $repoRoot -B $buildDir -A x64 | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Error "cmake configure failed"; exit 1 }
& $cmake --build $buildDir --config Release --target tsdevctl | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Error "build failed"; exit 1 }

$existing = Get-Process -Name TSDev -ErrorAction SilentlyContinue
if ($existing) { Write-Error "TSDev.exe is already running (pid $($existing.Id)); close it before running the E2E"; exit 1 }

$crashBefore = Get-Process -Name TSCrashReport -ErrorAction SilentlyContinue
if ($crashBefore) { Write-Error "TSCrashReport.exe already running; resolve it first"; exit 1 }

$itemName = "E2eFn_202608291052"
$output = & $tsdevctl open --type Function --name $itemName --json
$exitCode = $LASTEXITCODE
Write-Host "tsdevctl output: $output"
Write-Host "exit code: $exitCode"

$failures = 0
$result = $null
try { $result = $output | ConvertFrom-Json } catch { Write-Host "FAIL: output is not valid JSON"; $failures++ }

if ($exitCode -ne 0) { Write-Host "FAIL: expected exit code 0"; $failures++ }
if ($result -and -not $result.ok) { Write-Host "FAIL: expected ok:true"; $failures++ }
if ($result -and $result.type -ne "Function") { Write-Host "FAIL: expected type Function"; $failures++ }
if ($result -and $result.name -ne $itemName) { Write-Host "FAIL: unexpected name"; $failures++ }
if (-not ($result -and $result.tsdev_pid)) { Write-Host "FAIL: expected tsdev_pid"; $failures++ }

$tsdevPid = 0
if ($result -and $result.tsdev_pid) { $tsdevPid = [int]$result.tsdev_pid }

# The tool launched TSDev hidden; the window must still be hidden.
if (-not ('Win' -as [type])) {
    Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win {
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
}
"@
}
$frame = [Win]::FindWindow("TSDEV.EXE TRADESTATION", [NullString]::Value)
if ($frame -eq [IntPtr]::Zero) { Write-Host "FAIL: TSDev main frame not found"; $failures++ }
elseif ([Win]::IsWindowVisible($frame)) { Write-Host "FAIL: TSDev window must stay hidden"; $failures++ }
else { Write-Host "ok: TSDev window hidden" }

$crashAfter = Get-Process -Name TSCrashReport -ErrorAction SilentlyContinue
if ($crashAfter) { Write-Host "FAIL: TSCrashReport.exe appeared"; $failures++ }

# Cleanup: close the TSDev instance the tool launched (graceful WM_CLOSE).
if ($tsdevPid -ne 0) {
    if (-not ('Closer' -as [type])) {
        Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class Closer {
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr w, IntPtr l);
}
"@
    }
    [Closer]::PostMessage($frame, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Process -Id $tsdevPid -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
    if (Get-Process -Id $tsdevPid -ErrorAction SilentlyContinue) { Write-Host "FAIL: TSDev did not exit after WM_CLOSE"; $failures++ }
    else { Write-Host "ok: TSDev closed" }
}

if ($failures -gt 0) { Write-Host "$failures E2E check(s) failed"; exit 1 }
Write-Host "E2E passed"
exit 0
