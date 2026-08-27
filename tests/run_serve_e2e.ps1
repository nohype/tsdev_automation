<#
.SYNOPSIS
    Runs the serve-mode E2E test (tests/test_serve_e2e.py) from a NON-elevated
    Python process via Task Scheduler.

.DESCRIPTION
    This script MUST be run from an ELEVATED PowerShell session. It:
    1. Launches `uv run pytest tests/test_serve_e2e.py` unelevated via a
       Task Scheduler task with -RunLevel Limited.
    2. Captures stdout+stderr to a run-specific temp file.
    3. Records pytest's process exit code to a separate run-specific temp file.
    4. Waits for confirmed task completion, validates the task result and child
       exit-code file, prints the test output, and returns pytest's exit code on
       a validated run (or a distinct runner status for infrastructure errors).

    The non-elevated test process uses the serve broker's normal elevation
    request; UAC consent is handled by Windows.

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

# Every artifact and task name is unique to this invocation. In particular,
# do not reuse a prior run's output or child exit-code file.
$runId = [guid]::NewGuid().ToString('N')
$outputFile = Join-Path $tmpDir "serve_e2e_output_$runId.txt"
$exitCodeFile = Join-Path $tmpDir "serve_e2e_exit_code_$runId.txt"
$innerScript = Join-Path $tmpDir "run_serve_e2e_inner_$runId.ps1"
$taskName = "TsDevServeE2E_$runId"
$taskPath = '\'

# These are runner/infrastructure statuses, not pytest statuses. A real
# pytest result (including a nonzero result) is returned unchanged.
$runnerExitCodeMissingOrInvalid = 20
$runnerExitCodeTimeout = 21
$runnerExitCodeTaskLaunchFailure = 22
$runnerExitCodeTaskResultFailure = 23
$runnerExitCodeCleanupFailure = 24
$runnerExitCodeRunnerFailure = 25
$runnerExitCodeCaptureFailure = 26
$timeoutSeconds = 120

$taskRegistered = $false
$taskStarted = $false
$taskCompleted = $false
$cleanupFailed = $false
$artifactsPreserved = $false
$captureReadable = $false
$childExitCodeValidated = $false
$taskResultValidated = $false
$failureKind = $null
$exitCode = 1
$pytestExitCode = $null
$lastTaskState = $null
$lastTaskResult = $null
$preLastRunTime = $null

function Remove-RunArtifact {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path -ErrorAction Stop)) {
        return
    }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf -ErrorAction Stop)) {
        throw "Run artifact path is not a file: $Path"
    }

    Remove-Item -LiteralPath $Path -Force -ErrorAction Stop
    if (Test-Path -LiteralPath $Path -ErrorAction Stop) {
        throw "Run artifact still exists after deletion: $Path"
    }
}

# --- Find uv ---
$uvPath = (Get-Command uv -ErrorAction Stop).Source
Write-Host "[run_serve_e2e] uv: $uvPath"

# --- Write inner script (runs unelevated) ---
# A native pytest failure does not throw in PowerShell, so LASTEXITCODE is
# captured immediately after the output-capture pipeline. The finally block
# writes that integer before the child exits, including on terminating errors.
$innerContent = @"
`$ErrorActionPreference = 'Stop'
`$pytestExitCode = 1
try {
    Set-Location '$projectRoot'
    & '$uvPath' run pytest tests/test_serve_e2e.py $PytestArgs *>&1 |
        Out-File -FilePath '$outputFile' -Encoding UTF8
    if (`$null -eq `$LASTEXITCODE) {
        throw 'uv did not provide a native process exit code'
    }
    `$pytestExitCode = [int]`$LASTEXITCODE
}
catch {
    `$_ | Out-File -FilePath '$outputFile' -Append -Encoding UTF8
    throw
}
finally {
    [System.IO.File]::WriteAllText(
        '$exitCodeFile',
        ([int]`$pytestExitCode).ToString([System.Globalization.CultureInfo]::InvariantCulture),
        [System.Text.Encoding]::ASCII)
}
exit [int]`$pytestExitCode
"@

try {
    foreach ($artifact in @($innerScript, $outputFile, $exitCodeFile)) {
        Remove-RunArtifact -Path $artifact
    }
    Set-Content -Path $innerScript -Value $innerContent -Encoding UTF8 -ErrorAction Stop

    # --- Register the limited scheduled task ---
    try {
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
        Register-ScheduledTask -TaskName $taskName -TaskPath $taskPath `
            -Action $action -Principal $principal -Settings $settings `
            -ErrorAction Stop | Out-Null
        $taskRegistered = $true
    }
    catch {
        $failureKind = 'TASK_LAUNCH_FAILURE'
        $exitCode = $runnerExitCodeTaskLaunchFailure
        Write-Host "[run_serve_e2e] ERROR: ${failureKind}: scheduled task registration failed: $($_.Exception.Message)"
    }

    if ($null -eq $failureKind) {
        # Snapshot LastRunTime before Start-ScheduledTask. A new task is Ready
        # before it runs, so Ready alone is not evidence that Start succeeded.
        try {
            $preStartInfo = Get-ScheduledTaskInfo -TaskName $taskName -TaskPath $taskPath -ErrorAction Stop
            $preLastRunTime = $preStartInfo.LastRunTime
        }
        catch {
            $failureKind = 'TASK_LAUNCH_FAILURE'
            $exitCode = $runnerExitCodeTaskLaunchFailure
            Write-Host "[run_serve_e2e] ERROR: ${failureKind}: could not query the registered task before start: $($_.Exception.Message)"
        }
    }

    if ($null -eq $failureKind) {
        Write-Host "[run_serve_e2e] Starting unelevated task: $taskName"
        try {
            Start-ScheduledTask -TaskName $taskName -TaskPath $taskPath -ErrorAction Stop
        }
        catch {
            $failureKind = 'TASK_LAUNCH_FAILURE'
            $exitCode = $runnerExitCodeTaskLaunchFailure
            Write-Host "[run_serve_e2e] ERROR: ${failureKind}: Start-ScheduledTask failed: $($_.Exception.Message)"
        }
    }

    if ($null -eq $failureKind) {
        # Wait for evidence that this newly registered task actually ran, then
        # for its terminal Ready state. A Ready state seen before a run is not
        # completion. LastRunTime change or an observed Running/Queued state is
        # the launch evidence; output text is never used for this decision.
        $deadline = (Get-Date).AddSeconds($timeoutSeconds)
        while ((Get-Date) -lt $deadline) {
            try {
                $task = Get-ScheduledTask -TaskName $taskName -TaskPath $taskPath -ErrorAction Stop
                $taskInfo = Get-ScheduledTaskInfo -TaskName $taskName -TaskPath $taskPath -ErrorAction Stop
                $lastTaskState = [string]$task.State
                $lastTaskResult = $taskInfo.LastTaskResult

                if ($lastTaskState -eq 'Running' -or $lastTaskState -eq 'Queued') {
                    $taskStarted = $true
                }
                if ($null -ne $taskInfo.LastRunTime -and
                    (($null -eq $preLastRunTime) -or ($taskInfo.LastRunTime -ne $preLastRunTime))) {
                    $taskStarted = $true
                }

                if ($lastTaskState -eq 'Disabled' -or $lastTaskState -eq 'Unknown') {
                    $failureKind = 'TASK_COMPLETION_FAILURE'
                    $exitCode = $runnerExitCodeTaskResultFailure
                    Write-Host "[run_serve_e2e] ERROR: ${failureKind}: task entered terminal state '$lastTaskState' before confirmed completion."
                    break
                }
                if ($taskStarted -and $lastTaskState -eq 'Ready') {
                    $taskCompleted = $true
                    break
                }
            }
            catch {
                $failureKind = 'TASK_COMPLETION_FAILURE'
                $exitCode = $runnerExitCodeTaskResultFailure
                Write-Host "[run_serve_e2e] ERROR: ${failureKind}: could not query task completion: $($_.Exception.Message)"
                break
            }
            Start-Sleep -Seconds 2
        }

        if ($null -eq $failureKind -and -not $taskCompleted) {
            if ($taskStarted) {
                $failureKind = 'TIMEOUT'
                $exitCode = $runnerExitCodeTimeout
                Write-Host "[run_serve_e2e] ERROR: ${failureKind}: scheduled task did not reach Ready within ${timeoutSeconds}s."
            }
            else {
                $failureKind = 'TASK_LAUNCH_FAILURE'
                $exitCode = $runnerExitCodeTaskLaunchFailure
                Write-Host "[run_serve_e2e] ERROR: ${failureKind}: no scheduled-task run was observed within ${timeoutSeconds}s."
            }
        }
    }

    # --- Read and display captured output for diagnostics ---
    # Output text is never an exit-status oracle, but a missing or unreadable
    # capture is an infrastructure failure.
    try {
        if (-not (Test-Path -LiteralPath $outputFile -PathType Leaf -ErrorAction Stop)) {
            throw 'stdout/stderr capture file is missing or is not a file'
        }
        $output = [System.IO.File]::ReadAllText($outputFile)
        $captureReadable = $true
        Write-Host $output
    }
    catch {
        if ($null -eq $failureKind -or $failureKind -eq 'PYTEST_FAILURE') {
            $failureKind = 'CAPTURE_MISSING_OR_UNREADABLE'
            $exitCode = $runnerExitCodeCaptureFailure
        }
        Write-Host "[run_serve_e2e] ERROR: CAPTURE_MISSING_OR_UNREADABLE: $($_.Exception.Message)"
    }

    if ($null -eq $failureKind) {
        # --- Validate the child's run-specific exit-code file ---
        if (-not (Test-Path -LiteralPath $exitCodeFile -PathType Leaf -ErrorAction Stop)) {
            $failureKind = 'EXIT_CODE_MISSING_OR_INVALID'
            $exitCode = $runnerExitCodeMissingOrInvalid
            Write-Host "[run_serve_e2e] ERROR: ${failureKind}: child exit-code file is missing or is not a file."
        }
        else {
            try {
                $exitCodeText = [System.IO.File]::ReadAllText($exitCodeFile).Trim()
                if ($exitCodeText -notmatch '^\d+$') {
                    throw "expected one non-negative integer, got '$exitCodeText'"
                }
                $pytestExitCode = [int]$exitCodeText
                $childExitCodeValidated = $true
            }
            catch {
                $failureKind = 'EXIT_CODE_MISSING_OR_INVALID'
                $exitCode = $runnerExitCodeMissingOrInvalid
                Write-Host "[run_serve_e2e] ERROR: ${failureKind}: $($_.Exception.Message)"
            }
        }

        if ($null -eq $failureKind) {
            # The task must have completed and its recorded result must agree
            # with the child file. This rejects stale/partial output and also
            # detects scheduler/action failures independently of pytest output.
            try {
                if ($null -eq $lastTaskResult -or
                    [string]::IsNullOrWhiteSpace([string]$lastTaskResult)) {
                    throw 'LastTaskResult was missing'
                }
                $taskResultAsInt64 = [int64]$lastTaskResult
                if ($taskResultAsInt64 -ne [int64]$pytestExitCode) {
                    $failureKind = 'TASK_COMPLETION_FAILURE'
                    $exitCode = $runnerExitCodeTaskResultFailure
                    Write-Host "[run_serve_e2e] ERROR: ${failureKind}: LastTaskResult=$taskResultAsInt64 does not match child pytest exit code $pytestExitCode."
                }
                else {
                    $taskResultValidated = $true
                }
            }
            catch {
                $failureKind = 'TASK_COMPLETION_FAILURE'
                $exitCode = $runnerExitCodeTaskResultFailure
                Write-Host "[run_serve_e2e] ERROR: ${failureKind}: task result was missing or invalid: $($_.Exception.Message)"
            }
        }

        if ($null -eq $failureKind) {
            if ($pytestExitCode -ne 0) {
                $failureKind = 'PYTEST_FAILURE'
                $exitCode = $pytestExitCode
                Write-Host "[run_serve_e2e] ERROR: ${failureKind}: pytest exited with code $pytestExitCode."
            }
            elseif ($taskCompleted -and $lastTaskState -eq 'Ready' -and
                    $captureReadable -and $childExitCodeValidated -and
                    $taskResultValidated) {
                $exitCode = 0
                Write-Host '[run_serve_e2e] PASS: pytest exited 0 and the scheduled task completed with result 0.'
            }
            else {
                $failureKind = 'TASK_COMPLETION_FAILURE'
                $exitCode = $runnerExitCodeTaskResultFailure
                Write-Host "[run_serve_e2e] ERROR: ${failureKind}: task completion, capture, and child exit-code validation did not all succeed."
            }
        }
    }
}
catch {
    if ($null -eq $failureKind) {
        $failureKind = 'RUNNER_FAILURE'
        $exitCode = $runnerExitCodeRunnerFailure
    }
    Write-Host "[run_serve_e2e] ERROR: ${failureKind}: $($_.Exception.Message)"
}
finally {
    # --- Stop and clean up only this run's scheduled task ---
    # If the task is active or its state cannot be queried, leave the task and
    # all run artifacts in place for manual recovery. Unregister only after a
    # confirmed terminal state and successful artifact cleanup.
    $taskTerminalConfirmed = $false
    $safeToRemoveArtifacts = -not $taskRegistered
    $artifactCleanupSucceeded = $false

    if ($taskRegistered) {
        $taskForCleanup = $null
        $stopRequested = $false
        try {
            $taskForCleanup = Get-ScheduledTask -TaskName $taskName -TaskPath $taskPath -ErrorAction Stop
            if ($null -eq $taskForCleanup) {
                throw "Could not query scheduled task $taskName"
            }
            $cleanupState = [string]$taskForCleanup.State
            if ([string]::IsNullOrWhiteSpace($cleanupState)) {
                throw "Scheduled task $taskName returned no state"
            }

            $stopDeadline = (Get-Date).AddSeconds(10)
            while ($true) {
                if (($cleanupState -eq 'Running' -or $cleanupState -eq 'Queued') -and
                    -not $stopRequested) {
                    Write-Host "[run_serve_e2e] Stopping scheduled task before cleanup: $taskName"
                    Stop-ScheduledTask -TaskName $taskName -TaskPath $taskPath -ErrorAction Stop
                    $stopRequested = $true
                }

                if ($cleanupState -eq 'Running' -or $cleanupState -eq 'Queued') {
                    if ((Get-Date) -ge $stopDeadline) {
                        throw "Scheduled task $taskName is still active after stop timeout (state '$cleanupState')"
                    }
                    Start-Sleep -Milliseconds 200
                    $taskForCleanup = Get-ScheduledTask -TaskName $taskName -TaskPath $taskPath -ErrorAction Stop
                    if ($null -eq $taskForCleanup) {
                        throw "Could not query scheduled task $taskName after stop"
                    }
                    $cleanupState = [string]$taskForCleanup.State
                    if ([string]::IsNullOrWhiteSpace($cleanupState)) {
                        throw "Scheduled task $taskName returned no state after stop"
                    }
                    continue
                }

                if ($cleanupState -eq 'Ready' -or $cleanupState -eq 'Disabled') {
                    $taskTerminalConfirmed = $true
                    $safeToRemoveArtifacts = $true
                    break
                }

                throw "Could not confirm a terminal state for scheduled task $taskName (state '$cleanupState')"
            }
        }
        catch {
            $cleanupFailed = $true
            $artifactsPreserved = $true
            if ($null -eq $failureKind) {
                $failureKind = 'CLEANUP_FAILURE'
            }
            Write-Host "[run_serve_e2e] ERROR: CLEANUP_FAILURE: $($_.Exception.Message). Leaving scheduled task $taskName and run artifacts for manual recovery."
        }
    }

    if ($safeToRemoveArtifacts) {
        $artifactCleanupSucceeded = $true
        foreach ($artifact in @($innerScript, $outputFile, $exitCodeFile)) {
            try {
                Remove-RunArtifact -Path $artifact
            }
            catch {
                $artifactCleanupSucceeded = $false
                $cleanupFailed = $true
                if ($null -eq $failureKind) {
                    $failureKind = 'CLEANUP_FAILURE'
                }
                Write-Host "[run_serve_e2e] ERROR: CLEANUP_FAILURE: failed to delete run artifact ${artifact}: $($_.Exception.Message)"
            }
        }
        if (-not $artifactCleanupSucceeded -and $taskRegistered) {
            Write-Host "[run_serve_e2e] ERROR: CLEANUP_FAILURE: scheduled task $taskName remains registered because artifact cleanup failed."
        }
    }

    if ($taskRegistered -and $taskTerminalConfirmed -and
        $safeToRemoveArtifacts -and $artifactCleanupSucceeded) {
        try {
            Unregister-ScheduledTask -TaskName $taskName -TaskPath $taskPath -Confirm:$false -ErrorAction Stop
        }
        catch {
            $cleanupFailed = $true
            if ($null -eq $failureKind) {
                $failureKind = 'CLEANUP_FAILURE'
            }
            Write-Host "[run_serve_e2e] ERROR: CLEANUP_FAILURE: failed to unregister scheduled task ${taskName}: $($_.Exception.Message)"
        }
    }
}

if ($cleanupFailed) {
    Write-Host '[run_serve_e2e] ERROR: cleanup was not fully confirmed.'
    if ($artifactsPreserved) {
        Write-Host "[run_serve_e2e] ERROR: scheduled task $taskName and run artifacts were preserved for manual recovery."
    }
    if ($exitCode -eq 0) { $exitCode = $runnerExitCodeCleanupFailure }
}

Write-Host "[run_serve_e2e] Exit code: $exitCode"
exit $exitCode
