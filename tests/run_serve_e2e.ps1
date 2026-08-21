<#
.SYNOPSIS
    Runs the serve-mode E2E test (tests/test_serve_e2e.py) from a NON-elevated
    Python process via Task Scheduler.

.DESCRIPTION
    This script MUST be run from an ELEVATED PowerShell session. It:
    1. Saves the current ConsentPromptBehaviorAdmin registry value.
    2. Sets ConsentPromptBehaviorAdmin=0 (auto-elevate without UAC prompt).
    3. Launches `uv run pytest tests/test_serve_e2e.py` unelevated via a
       Task Scheduler task with -RunLevel Limited.
    4. Captures stdout+stderr to a temp file.
    5. Restores ConsentPromptBehaviorAdmin to its original value.
    6. Prints the test output and returns the pytest exit code.

    The temporary ConsentPromptBehaviorAdmin=0 setting means ANY elevation
    request during the test session auto-approves silently. This is restored
    in the finally block even on error.

.PARAMETER PytestArgs
    Additional pytest arguments (default: "-v --tb=short").

.EXAMPLE
    .\tests\run_serve_e2e.ps1
    .\tests\run_serve_e2e.ps1 -PytestArgs "-v --tb=long -s"
#>
param(
    [string]$PytestArgs = "-v --tb=short"
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$tmpDir = Join-Path $projectRoot 'tmp'
if (-not (Test-Path $tmpDir)) { New-Item -ItemType Directory -Path $tmpDir -Force | Out-Null }

$outputFile = Join-Path $tmpDir 'serve_e2e_output.txt'
$innerScript = Join-Path $tmpDir 'run_serve_e2e_inner.ps1'
$taskName = "TsDevServeE2E_$(Get-Random)"

# --- Save UAC setting ---
$uacKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System'
$originalConsent = (Get-ItemProperty $uacKey -Name ConsentPromptBehaviorAdmin).ConsentPromptBehaviorAdmin
Write-Host "[run_serve_e2e] ConsentPromptBehaviorAdmin was $originalConsent"

# --- Find uv ---
$uvPath = (Get-Command uv -ErrorAction Stop).Source
Write-Host "[run_serve_e2e] uv: $uvPath"

# --- Write inner script (runs unelevated) ---
$innerContent = @"
`$ErrorActionPreference = 'Stop'
Set-Location '$projectRoot'
& '$uvPath' run pytest tests/test_serve_e2e.py $PytestArgs *>&1 |
    Out-File -FilePath '$outputFile' -Encoding UTF8
"@
Set-Content -Path $innerScript -Value $innerContent -Encoding UTF8

$exitCode = 1

try {
    # --- Set auto-elevate (no UAC prompt) ---
    Set-ItemProperty $uacKey -Name ConsentPromptBehaviorAdmin -Value 0 -Type DWord
    Write-Host "[run_serve_e2e] ConsentPromptBehaviorAdmin set to 0 (auto-elevate)"

    Remove-Item $outputFile -ErrorAction SilentlyContinue

    # --- Register + start scheduled task (unelevated) ---
    $action = New-ScheduledTaskAction `
        -Execute 'powershell.exe' `
        -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$innerScript`"" `
        -WorkingDirectory $projectRoot
    $principal = New-ScheduledTaskPrincipal `
        -UserId "$env:USERNAME" `
        -LogonType Interactive `
        -RunLevel Limited
    $settings = New-ScheduledTaskSettingsSet `
        -AllowStartIfOnBatteries `
        -DontStopIfGoingOnBatteries `
        -ExecutionTimeLimit (New-TimeSpan -Minutes 5)
    Register-ScheduledTask -TaskName $taskName `
        -Action $action -Principal $principal -Settings $settings -Force | Out-Null

    Write-Host "[run_serve_e2e] Starting unelevated task: $taskName"
    Start-ScheduledTask -TaskName $taskName

    # --- Wait for the scheduled task to finish ---
    # The task runs pytest which may have long pauses (broker startup,
    # orlib20 init) with no output. Waiting for the TASK to complete is
    # more reliable than polling for output-file stability.
    $timeout = 120  # seconds
    $elapsed = 0

    while ($elapsed -lt $timeout) {
        Start-Sleep -Seconds 2
        $elapsed += 2
        $taskState = (Get-ScheduledTask -TaskName $taskName).State
        if ($taskState -ne 'Running') { break }
    }

    # Give the output file a moment to finish flushing
    Start-Sleep -Seconds 1

    # --- Read and display output ---
    if (Test-Path $outputFile) {
        $output = Get-Content $outputFile -Raw
        Write-Host $output

        # Determine exit code from pytest output
        if ($output -match '(\d+) passed') {
            $passed = [int]$Matches[1]
            if ($output -match '(\d+) failed') {
                $failed = [int]$Matches[1]
                $exitCode = if ($failed -gt 0) { 1 } else { 0 }
            } else {
                $exitCode = 0
            }
        } elseif ($output -match '(\d+) failed') {
            $exitCode = 1
        } elseif ($output -match 'no tests ran') {
            $exitCode = 5
        } else {
            # Could not parse — check for PASSED/FAILED markers
            if ($output -match 'PASSED') { $exitCode = 0 }
            elseif ($output -match 'FAILED|ERROR') { $exitCode = 1 }
        }
    } else {
        Write-Host "[run_serve_e2e] ERROR: No output file produced within ${timeout}s"
        $exitCode = 2
    }

} finally {
    # --- Restore UAC setting (CRITICAL — always run) ---
    try {
        Set-ItemProperty $uacKey -Name ConsentPromptBehaviorAdmin -Value $originalConsent -Type DWord
        Write-Host "[run_serve_e2e] ConsentPromptBehaviorAdmin restored to $originalConsent"
    } catch {
        Write-Host "[run_serve_e2e] WARNING: Failed to restore ConsentPromptBehaviorAdmin! Manual restore needed: Set-ItemProperty '$uacKey' -Name ConsentPromptBehaviorAdmin -Value $originalConsent -Type DWord"
    }

    # --- Clean up scheduled task ---
    try { Unregister-ScheduledTask -TaskName $taskName -Confirm:$false } catch {}

    # --- Clean up temp files ---
    Remove-Item $innerScript -ErrorAction SilentlyContinue
    Remove-Item $outputFile -ErrorAction SilentlyContinue
}

Write-Host "[run_serve_e2e] Exit code: $exitCode"
exit $exitCode
