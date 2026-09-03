# E2E for tsdevctl `open` and `exists` against a real TSDev instance.
# REQUIRES: elevated process, TradeStation 9.5 installed. Skippable in CI.
# Compatible with PowerShell 5.1 and pwsh 7.
# Session-state hygiene: TSDev restores previous-session documents on launch
# (phantoms included). Those documents belong to the user's session, so this
# script tolerates them (normalized whole-window-set comparisons) and never
# cleans them; it only asserts that each invocation leaves no NEW documents.
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
& $cmake --build $buildDir --config Release | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Error "build failed"; exit 1 }

$existing = Get-Process -Name TSDev -ErrorAction SilentlyContinue
if ($existing) { Write-Error "TSDev.exe is already running (pid $($existing.Id)); close it before running the E2E"; exit 1 }

$crashBefore = Get-Process -Name TSCrashReport -ErrorAction SilentlyContinue
if ($crashBefore) { Write-Error "TSCrashReport.exe already running; resolve it first"; exit 1 }

if (-not ('TsdevE2E' -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class TsdevE2E {
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindow(string cls, string title);

    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);

    [DllImport("user32.dll")]
    public static extern bool EnumChildWindows(IntPtr parent, EnumProc proc, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassName(IntPtr h, StringBuilder sb, int max);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowText(IntPtr h, StringBuilder sb, int max);

    [DllImport("user32.dll")]
    public static extern IntPtr GetParent(IntPtr h);

    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr w, IntPtr l);

    public static IntPtr MdIClient(IntPtr frame) {
        return FindWindowEx(frame, IntPtr.Zero, "MDIClient", null);
    }

    public static System.Collections.Generic.List<string> DocTitles(IntPtr frame) {
        var titles = new System.Collections.Generic.List<string>();
        IntPtr client = FindWindowEx(frame, IntPtr.Zero, "MDIClient", null);
        if (client == IntPtr.Zero) return titles;
        EnumChildWindows(client, delegate(IntPtr h, IntPtr l) {
            StringBuilder cls = new StringBuilder(256);
            GetClassName(h, cls, 256);
            if (!cls.ToString().StartsWith("Afx:")) return true;
            if (GetParent(h) != client) return true;
            StringBuilder sb = new StringBuilder(1024);
            GetWindowText(h, sb, 1024);
            titles.Add(sb.ToString());
            return true;
        }, IntPtr.Zero);
        return titles;
    }
}
"@
}

$failures = 0

# The trailing '*' on a document title is a state-dependent UI marker (it can
# appear on fresh opens and clear on deactivation, for real and phantom docs
# alike) - never an existence or dirty flag. Residue comparisons normalize it.
function Get-DocTitles { [TsdevE2E]::DocTitles($frame) }
function Get-DocTitlesNormalized {
    Get-DocTitles | ForEach-Object { $_ -replace '\*$', '' }
}

function Invoke-Tsdevctl {
    param([string[]]$VerbArgs)
    $output = & $tsdevctl @VerbArgs
    Write-Host "tsdevctl $($VerbArgs -join ' ') -> exit $($LASTEXITCODE): $output"
    return @($output)
}

# --- open: real item, launched hidden --------------------------------------
$openOutput = Invoke-Tsdevctl @("open", "--type", "Strategy", "--name", "MyTestStrategyA", "--json")
$openExit = $LASTEXITCODE
$open = $null
try { $open = $openOutput | Select-Object -Last 1 | ConvertFrom-Json } catch { Write-Host "FAIL: open output is not valid JSON"; $failures++ }
if ($openExit -ne 0) { Write-Host "FAIL: open expected exit code 0"; $failures++ }
if ($open -and -not $open.ok) { Write-Host "FAIL: open expected ok:true"; $failures++ }
if ($open -and $open.type -ne "Strategy") { Write-Host "FAIL: open expected type Strategy"; $failures++ }
$tsdevPid = 0
if ($open -and $open.tsdev_pid) { $tsdevPid = [int]$open.tsdev_pid }
if (-not $tsdevPid) { Write-Host "FAIL: open expected tsdev_pid"; $failures++ }

# The tool launched TSDev; from here on the frame must exist and stay hidden.
$frame = [TsdevE2E]::FindWindow("TSDEV.EXE TRADESTATION", [NullString]::Value)
if ($frame -eq [IntPtr]::Zero) { Write-Error "TSDev main frame not found after open"; exit 1 }
if (-not [TsdevE2E]::IsWindowVisible($frame)) { Write-Host "ok: TSDev window hidden" }
else { Write-Host "FAIL: TSDev window must stay hidden"; $failures++ }

# TSDev restores previous-session documents asynchronously after the frame
# appears; wait until the document set is stable before taking baselines.
$prev = $null
$havePrev = $false
$settleDeadline = (Get-Date).AddSeconds(20)
while ((Get-Date) -lt $settleDeadline) {
    $current = @(Get-DocTitlesNormalized)
    if ($havePrev -and ((Compare-Object $prev $current) -eq $null)) { break }
    $prev = $current
    $havePrev = $true
    Start-Sleep -Milliseconds 500
}
if (-not $havePrev) { Write-Error "document set never appeared"; exit 1 }

# --- exists: real item ------------------------------------------------------
$before = Get-DocTitlesNormalized
$yesOutput = Invoke-Tsdevctl @("exists", "--type", "Strategy", "--name", "MyTestStrategyA", "--json")
$yesExit = $LASTEXITCODE
$yes = $null
try { $yes = $yesOutput | Select-Object -Last 1 | ConvertFrom-Json } catch { Write-Host "FAIL: exists output is not valid JSON"; $failures++ }
if ($yesExit -ne 0) { Write-Host "FAIL: exists real item expected exit 0"; $failures++ }
if ($yes -and -not $yes.ok) { Write-Host "FAIL: exists real expected ok:true"; $failures++ }
if ($yes -and "$($yes.exists)" -ne "True") { Write-Host "FAIL: exists real expected exists:true"; $failures++ }
if ($yes -and $yes.name -ne "MyTestStrategyA") { Write-Host "FAIL: exists real expected requested name"; $failures++ }
$afterYes = Get-DocTitlesNormalized
if ((Compare-Object $before $afterYes) -ne $null) { Write-Host "FAIL: exists real left document windows behind"; $failures++ }
else { Write-Host "ok: exists real left no residue" }

# --- exists: wrong case (TSDev's own lookup is case-insensitive) ------------
$ciOutput = Invoke-Tsdevctl @("exists", "--type", "Strategy", "--name", "myteststrategya", "--json")
$ciExit = $LASTEXITCODE
$ci = $null
try { $ci = $ciOutput | Select-Object -Last 1 | ConvertFrom-Json } catch { Write-Host "FAIL: case-variant output is not valid JSON"; $failures++ }
if ($ciExit -ne 0) { Write-Host "FAIL: exists wrong-case expected exit 0 (TSDev lookup is case-insensitive)"; $failures++ }
if ($ci -and "$($ci.exists)" -ne "True") { Write-Host "FAIL: exists wrong-case expected exists:true"; $failures++ }
$afterCi = Get-DocTitlesNormalized
$ciDiff = Compare-Object $before $afterCi
if ($ciDiff) { Write-Host "FAIL: exists wrong-case left document windows behind: $($ciDiff | Out-String)"; $failures++ }
else { Write-Host "ok: exists wrong-case left no residue" }

# --- exists: phantom items for every supported type -------------------------
# The type list is the test subject: it must independently state the set of
# types the CLI is required to accept (R13). The authoritative pipe numbers
# live in docs/KNOWLEDGE_BASE.md; this file deliberately does not restate them.
$types = @("Strategy", "Function", "Indicator", "ShowMe", "TradingApp")
$stamp = (Get-Date).ToUniversalTime().ToString("yyyyMMddTHHmmss")
foreach ($type in $types) {
    $phantomName = "E2ePhantom_$type`_$stamp"
    $docsBefore = Get-DocTitlesNormalized
    $out = Invoke-Tsdevctl @("exists", "--type", $type, "--name", $phantomName, "--json")
    $exitCode = $LASTEXITCODE
    $parsed = $null
    try { $parsed = $out | Select-Object -Last 1 | ConvertFrom-Json } catch { Write-Host "FAIL: phantom output is not valid JSON"; $failures++ }
    if ($exitCode -ne 1) { Write-Host "FAIL: phantom exists expected exit code 1 (NotFound)"; $failures++ }
    if ($parsed -and -not $parsed.ok) { Write-Host "FAIL: phantom exists expected ok:true (a 'no' answer is a successful answer)"; $failures++ }
    if ($parsed -and "$($parsed.exists)" -ne "False") { Write-Host "FAIL: phantom expected exists:false"; $failures++ }
    $docsNow = Get-DocTitlesNormalized
    $diff = Compare-Object $docsBefore $docsNow
    if ($diff) { Write-Host "FAIL: phantom exists left residue: $($diff | Out-String)"; $failures++ }
    else { Write-Host "ok: phantom exists ($type) left no residue" }
}

$crashAfter = Get-Process -Name TSCrashReport -ErrorAction SilentlyContinue
if ($crashAfter) { Write-Host "FAIL: TSCrashReport.exe appeared"; $failures++ }

# Cleanup: close the TSDev instance the tool launched (graceful WM_CLOSE).
if ($tsdevPid -ne 0) {
    [TsdevE2E]::PostMessage($frame, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Process -Id $tsdevPid -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
    if (Get-Process -Id $tsdevPid -ErrorAction SilentlyContinue) { Write-Host "FAIL: TSDev did not exit after WM_CLOSE"; $failures++ }
    else { Write-Host "ok: TSDev closed" }
}

if ($failures -gt 0) { Write-Host "$failures E2E check(s) failed"; exit 1 }
Write-Host "E2E passed"
exit 0

