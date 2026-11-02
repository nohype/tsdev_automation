# check_ts_state.ps1 - snapshot WHServer/TSCrashReport/dialog state for E2E trace
# RC-16: WHServer absence is an anomaly ONLY when the server is expected
# (in-session snapshot, -ExpectRunning). Post-run snapshots omit the switch:
# idle self-reap (<17s idle) is NORMAL, so absence prints a non-alarm line.
param([string]$Label = "", [switch]$ExpectRunning)

$tsProcs = Get-Process | Where-Object {
    $_.ProcessName -match 'WHServer|TSDev|TSCrash|TradeStation|ORCAL|ELServer|tsplatform'
} | Select-Object Id, ProcessName, StartTime, MainWindowTitle

Write-Output "=== TS-STATE $Label ==="
Write-Output "--- TS-related processes ---"
$tsProcs | Format-Table -AutoSize | Out-String | Write-Output

$wh = $tsProcs | Where-Object { $_.ProcessName -eq 'WHServer' }
if ($wh) { Write-Output ("WHServer PID: " + ($wh.Id -join ',')) } elseif ($ExpectRunning) { Write-Output "WHServer: ABSENT (ANOMALY)" } else { Write-Output "WHServer: ABSENT (idle self-reap, normal)" }

$crash = Get-Process TSCrashReport -ErrorAction SilentlyContinue
Write-Output ("TSCrashReport count: " + @($crash).Count)

# Enumerate top-level windows with non-empty titles
$sig = @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class WinEnum {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    public static List<string> Wins() {
        var list = new List<string>();
        EnumWindows((h, l) => {
            if (!IsWindowVisible(h)) return true;
            var sb = new StringBuilder(512);
            GetWindowText(h, sb, 512);
            var t = sb.ToString();
            if (t.Length > 0) {
                uint pid; GetWindowThreadProcessId(h, out pid);
                string pn = "?";
                try { pn = System.Diagnostics.Process.GetProcessById((int)pid).ProcessName; } catch {}
                list.Add(pid + "|" + pn + "|" + t);
            }
            return true;
        }, IntPtr.Zero);
        return list;
    }
}
'@
if (-not ('WinEnum' -as [type])) { Add-Type -TypeDefinition $sig }
Write-Output "--- visible top-level windows ---"
$wins = [WinEnum]::Wins()
foreach ($w in $wins) { Write-Output $w }
Write-Output "--- flag: TS-owned or error-like windows ---"
$flagged = $wins | Where-Object { $_ -match 'WHServer|TSDev|TSCrash|TradeStation|ORCAL|ELServer|EasyLanguage|error|Error|crash|Crash|verif|Verif' }
if ($flagged) { foreach ($f in $flagged) { Write-Output ("FLAGGED: " + $f) } } else { Write-Output "(none)" }
Write-Output "=== END TS-STATE ==="
