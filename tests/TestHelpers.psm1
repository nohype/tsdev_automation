<#
.SYNOPSIS
    Shared helpers for the TSDev Automation test runner scripts (TVS-B.6.6).

.DESCRIPTION
    Provides shared functions used by the PowerShell test runners:

      * Find-TsInstallPath  - discovers the TradeStation install directory
                              (parameter override -> registry -> conventional
                              default).
      * Ensure-BaselineDll  - if the baseline DLL (tests/fixtures/
                              orlib20_baseline.dll, an untracked local-copy
                              fixture) is missing, prompts the user to copy
                              it from the TradeStation installation.
                              Returns $true if the DLL is available
                              (present or successfully copied), $false if
                              the user declined or the DLL could not be
                              copied. On $false the caller treats
                              DLL-dependent tests as SKIPPED (missing
                              prerequisite), not as failures.
      * Assert-BatchStderrProgress - asserts the default (human) batch
                              progress event lines on stderr against the
                              expected per-item results.
      * Assert-BatchJsonProgress  - asserts the --json batch progress event
                              objects on stderr against the authoritative
                              final stdout result array.

.NOTES
    The registry key path does NOT contain WOW6432Node — the KEY_WOW64_32KEY
    flag (0x100 = 256) handles 32-bit registry redirection. This was a critical
    finding in TVS-B.7.
#>


# ---------------------------------------------------------------------------
# Find-TsInstallPath
#
# Discovers the TradeStation install root directory. Resolution order:
#   1. If $TsPath is provided and valid (directory exists), use it directly.
#   2. Registry: HKLM\SOFTWARE\TradeStation Technologies\TradeStation\Installing
#      -> "RootDirectory" value (opened with KEY_WOW64_32KEY).
#   3. Registry fallback: HKLM\SOFTWARE\TradeStation Technologies\TradeStation
#      \Versions\9.05.00 -> "RootDirectory" value (KEY_WOW64_32KEY).
#   4. Conventional default: $env:ProgramFiles(x86)\TradeStation 9.5
#      (read at runtime, NOT hardcoded).
#   5. Return $null if none found.
# ---------------------------------------------------------------------------
function Find-TsInstallPath {
    param(
        [string]$TsPath
    )

    # 1. Explicit override.
    if ($TsPath) {
        if (Test-Path -Path $TsPath -PathType Container) {
            return $TsPath
        }
        # Provided but invalid — fall through to discovery rather than failing.
    }

    # 2 & 3. Registry discovery (KEY_WOW64_32KEY = 0x100 = 256).
    $KEY_WOW64_32KEY = 256
    $registryKeys = @(
        "SOFTWARE\TradeStation Technologies\TradeStation\Installing",
        "SOFTWARE\TradeStation Technologies\TradeStation\Versions\9.05.00"
    )

    foreach ($keyPath in $registryKeys) {
        try {
            $subKey = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey(
                $keyPath,
                [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadSubTree,
                [System.Security.AccessControl.RegistryRights]::ReadKey -bor $KEY_WOW64_32KEY)
            if ($null -ne $subKey) {
                $rootDir = $subKey.GetValue("RootDirectory")
                $subKey.Close()
                if ($rootDir -and (Test-Path -Path $rootDir -PathType Container)) {
                    return $rootDir
                }
            }
        }
        catch {
            # Key not present or access denied — try the next candidate.
        }
    }

    # 4. Conventional default (read env var at runtime, do NOT hardcode).
    $pf86 = ${env:ProgramFiles(x86)}
    if (-not $pf86) { $pf86 = $env:ProgramFiles }
    if ($pf86) {
        $defaultRoot = Join-Path $pf86 "TradeStation 9.5"
        if (Test-Path -Path $defaultRoot -PathType Container) {
            return $defaultRoot
        }
    }

    # 5. Nothing found.
    return $null
}


# ---------------------------------------------------------------------------
# Ensure-BaselineDll
#
# If the baseline DLL is present, returns $true immediately. Otherwise prompts
# the user to copy it from the TradeStation installation. Does NOT continue
# without a user decision (uses Read-Host). Returns $true only when the DLL is
# available at $DllPath on exit; $false when the user declined or the copy
# failed. On $false the caller marks DLL-dependent tests as SKIPPED (missing
# prerequisite); it is an expected skip, not a failure.
# ---------------------------------------------------------------------------
function Ensure-BaselineDll {
    param(
        [Parameter(Mandatory = $true)] [string]$DllPath,
        [string]$TsPath
    )

    # DLL present — nothing to do.
    if (Test-Path -Path $DllPath -PathType Leaf) {
        return $true
    }

    # DLL missing — prompt the user.
    Write-Host ""
    Write-Host "Baseline DLL not found at: $DllPath" -ForegroundColor Yellow
    Write-Host "This DLL is required for: test_orlib_resolver, sigtool verify gate." -ForegroundColor Yellow
    Write-Host ""
    Write-Host "Copy it from the TradeStation installation?" -ForegroundColor Cyan
    Write-Host "  Source: <TS install>\Program\orlib20.dll"
    Write-Host "  Target: $DllPath"
    Write-Host ""

    # Try to locate the TS install.
    $tsRoot = Find-TsInstallPath -TsPath $TsPath
    if ($tsRoot) {
        $sourceDll = Join-Path $tsRoot "Program\orlib20.dll"
        Write-Host "  Detected TS install: $tsRoot"
        Write-Host "  Source DLL: $sourceDll"
        if (Test-Path -Path $sourceDll -PathType Leaf) {
            Write-Host "  Source DLL exists: YES" -ForegroundColor Green
        } else {
            Write-Host "  Source DLL exists: NO (orlib20.dll not found in detected TS install)" -ForegroundColor Red
        }
    } else {
        Write-Host "  TradeStation install NOT detected (registry discovery failed)." -ForegroundColor Red
        Write-Host "  You can manually copy orlib20.dll to the target path." -ForegroundColor Yellow
    }

    # Prompt — do NOT continue without a user decision.
    $response = Read-Host "Copy the DLL now? (Y/N)"

    if ($response -eq 'Y' -or $response -eq 'y') {
        if ($tsRoot -and (Test-Path -Path (Join-Path $tsRoot "Program\orlib20.dll") -PathType Leaf)) {
            # Auto-copy from detected TS install.
            $sourceDll = Join-Path $tsRoot "Program\orlib20.dll"
            $targetDir = Split-Path $DllPath -Parent
            if (-not (Test-Path -Path $targetDir -PathType Container)) {
                New-Item -ItemType Directory -Path $targetDir -Force | Out-Null
            }
            Copy-Item -Path $sourceDll -Destination $DllPath -Force
            Write-Host "SUCCESS: Copied orlib20.dll to $DllPath" -ForegroundColor Green
            return $true
        } else {
            Write-Host "Cannot auto-copy - TS install not detected or orlib20.dll not found." -ForegroundColor Yellow
            Write-Host "Please manually copy orlib20.dll from your TradeStation installation to:" -ForegroundColor Yellow
            Write-Host "  $DllPath" -ForegroundColor Yellow
            Write-Host "Then re-run the tests." -ForegroundColor Yellow
            return $false
        }
    } else {
        # User declined — return false; callers skip DLL-dependent tests.
        Write-Host "User declined to copy the baseline DLL." -ForegroundColor DarkYellow
        Write-Host "Tests requiring this DLL will be SKIPPED:" -ForegroundColor DarkYellow
        Write-Host "  - test_orlib_resolver (signature resolution + patch-site validation)" -ForegroundColor DarkYellow
        Write-Host "  - sigtool verify gate (signature verification)" -ForegroundColor DarkYellow
        Write-Host "To enable them: copy orlib20.dll from your TradeStation installation to:" -ForegroundColor DarkYellow
        Write-Host "  $DllPath" -ForegroundColor DarkYellow
        return $false
    }
}


# ---------------------------------------------------------------------------
# Assert-BatchStderrProgress
#
# Asserts the default (human) batch progress events on stderr under the new
# output contract: NO BATCH START lines at all — exactly one BATCH
# SUCCESS/FAILED completion line per attempted item in order (op/type/name/
# index/total matching the expected results), then exactly one BATCH
# COMPLETE whose counters/exit match the final results. A FAILED line may be
# followed by the item's full human detail block (e.g. "tsdev_cli: ERROR —
# ..." plus compiler error text); detail lines do not start with "BATCH "
# and are ignored by the line filter, like other diagnostics. Requires a
# non-empty $Results array. Throws on the first mismatch.
#
# $Results objects need only op/type/name/success/exit_code (+ error.message
# for failures): pass the parsed --json stdout array, or hand-built
# [pscustomobject]s when the run was human-mode (which emits no stdout
# array).
#
# PRECONDITION: this helper is for healthy-path suites where the batch
# closed cleanly — the BATCH COMPLETE line is validated FIRST and must show
# close_failed=false (plus cancelled=false, not_run=0). Only under a clean
# close do the per-item event results equal the final results; a failed
# end-of-batch closeWarehouse rewrites still-successful final results, so
# this helper must NOT be used for close-failure scenarios.
# ---------------------------------------------------------------------------
function Assert-BatchStderrProgress {
    param(
        [Parameter(Mandatory = $true)] [AllowEmptyString()] [string]$Stderr,
        [Parameter(Mandatory = $true)] $Results,
        [Parameter(Mandatory = $true)] [int]$ExpectedExitCode
    )
    $items = @($Results)
    $total = $items.Count
    if ($total -eq 0) {
        throw "Assert-BatchStderrProgress: -Results must contain at least one result object"
    }
    $lines = @($Stderr -split "`r?`n" | Where-Object { $_ -cmatch '^BATCH ' })
    if ($lines.Count -ne ($total + 1)) {
        throw "stderr progress lines: expected $($total + 1) (one completion line per item + COMPLETE; no BATCH START lines in human mode), got $($lines.Count) [stderr: $Stderr]"
    }
    # Validate the final summary FIRST: these suites require a cleanly closed
    # batch (close_failed=false) before per-item events may be compared to
    # the expected results.
    $successes = @($items | Where-Object { $_.success -eq $true }).Count
    $expComplete = 'BATCH COMPLETE op={0} completed={1} total={1} successes={2} failures={3} not_run=0 cancelled=false close_failed=false exit_code={4}' -f $items[0].op, $total, $successes, ($total - $successes), $ExpectedExitCode
    if ($lines[$total] -cne $expComplete) {
        throw "stderr last progress line: expected '$expComplete', got: $($lines[$total])"
    }
    for ($i = 0; $i -lt $total; $i++) {
        $r = $items[$i]
        $n = $i + 1
        if ($r.success -eq $true) {
            $expOk = 'BATCH SUCCESS [{0}/{1}] op={2} type="{3}" name="{4}" exit_code=0' -f $n, $total, $r.op, $r.type, $r.name
            if ($lines[$i] -cne $expOk) {
                throw "stderr progress[$i]: expected '$expOk', got: $($lines[$i])"
            }
        } else {
            $expFail = 'BATCH FAILED [{0}/{1}] op={2} type="{3}" name="{4}" exit_code={5}' -f $n, $total, $r.op, $r.type, $r.name, $r.exit_code
            $failLine = $lines[$i]
            if ($null -ne $r.error -and $r.error.message) {
                if (-not ($failLine.StartsWith($expFail + ' message="') -and $failLine.EndsWith('"'))) {
                    throw "stderr progress[$i]: expected '$expFail message=`"<msg>`"', got: $failLine"
                }
            } else {
                if ($failLine -cne $expFail) {
                    throw "stderr progress[$i]: expected '$expFail', got: $failLine"
                }
            }
        }
    }
}

# ---------------------------------------------------------------------------
# Assert-BatchJsonProgress
#
# Asserts that under --json the stderr progress lines are one JSON object per
# line in order — a batch_item_started / batch_item_completed pair per
# attempted item (matching the authoritative final stdout result array), then
# exactly one batch_completed whose counters/exit match the final results.
# Each completed event's nested "result" object must be structurally equal to
# the corresponding stdout result object (compact ConvertTo-Json comparison).
# stderr lines that do not parse to an object with an "event" member are
# diagnostics and are ignored. Requires a non-empty $Results array. Throws on
# the first mismatch.
#
# PRECONDITION: this helper is for healthy-path suites where the batch
# closed cleanly — the batch_completed event is validated FIRST and must
# show close_failed=false (plus cancelled=false, not_run=0). Only under a
# clean close do the per-item event results equal the final stdout results;
# a failed end-of-batch closeWarehouse rewrites still-successful final
# results, so this helper must NOT be used for close-failure scenarios.
# ---------------------------------------------------------------------------
function Assert-BatchJsonProgress {
    param(
        [Parameter(Mandatory = $true)] [AllowEmptyString()] [string]$Stderr,
        [Parameter(Mandatory = $true)] $Results,
        [Parameter(Mandatory = $true)] [int]$ExpectedExitCode
    )
    $items = @($Results)
    $total = $items.Count
    if ($total -eq 0) {
        throw "Assert-BatchJsonProgress: -Results must contain at least one final stdout result object"
    }
    $events = @()
    foreach ($line in ($Stderr -split "`r?`n")) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $obj = $null
        try { $obj = $line | ConvertFrom-Json } catch { }
        if ($null -ne $obj -and ($obj.PSObject.Properties.Name -contains 'event')) {
            $events += $obj
        }
    }
    if ($events.Count -ne (2 * $total + 1)) {
        throw "stderr JSON progress events: expected $(2 * $total + 1), got $($events.Count) [stderr: $Stderr]"
    }
    # Validate the final summary FIRST: these suites require a cleanly closed
    # batch (close_failed=false) before per-item events may be compared to
    # the final stdout results.
    $ev = $events[2 * $total]
    $successes = @($items | Where-Object { $_.success -eq $true }).Count
    if ($ev.event -ne 'batch_completed') { throw "last stderr event: expected batch_completed, got '$($ev.event)'" }
    if ($ev.batch_operation -ne $items[0].op) { throw "batch_completed.batch_operation: expected '$($items[0].op)', got '$($ev.batch_operation)'" }
    if ($ev.completed -ne $total) { throw "batch_completed.completed: expected $total, got '$($ev.completed)'" }
    if ($ev.total -ne $total) { throw "batch_completed.total: expected $total, got '$($ev.total)'" }
    if ($ev.successes -ne $successes) { throw "batch_completed.successes: expected $successes, got '$($ev.successes)'" }
    if ($ev.failures -ne ($total - $successes)) { throw "batch_completed.failures: expected $($total - $successes), got '$($ev.failures)'" }
    if ($ev.not_run -ne 0) { throw "batch_completed.not_run: expected 0, got '$($ev.not_run)'" }
    if ($ev.cancelled -ne $false) { throw "batch_completed.cancelled: expected false, got '$($ev.cancelled)'" }
    if ($ev.close_failed -ne $false) { throw "batch_completed.close_failed: expected false, got '$($ev.close_failed)'" }
    if ($ev.exit_code -ne $ExpectedExitCode) { throw "batch_completed.exit_code: expected $ExpectedExitCode, got '$($ev.exit_code)'" }
    for ($i = 0; $i -lt $total; $i++) {
        $r = $items[$i]
        $n = $i + 1
        $ev = $events[2 * $i]
        if ($ev.event -ne 'batch_item_started') { throw "stderr event[$($i * 2)]: expected batch_item_started, got '$($ev.event)'" }
        if ($ev.batch_operation -ne $r.op) { throw "started[$n].batch_operation: expected '$($r.op)', got '$($ev.batch_operation)'" }
        if ($ev.index -ne $n) { throw "started[$n].index: expected $n, got '$($ev.index)'" }
        if ($ev.total -ne $total) { throw "started[$n].total: expected $total, got '$($ev.total)'" }
        if ($ev.type -ne $r.type) { throw "started[$n].type: expected '$($r.type)', got '$($ev.type)'" }
        if ($ev.name -ne $r.name) { throw "started[$n].name: expected '$($r.name)', got '$($ev.name)'" }
        $ev = $events[2 * $i + 1]
        if ($ev.event -ne 'batch_item_completed') { throw "stderr event[$($i * 2 + 1)]: expected batch_item_completed, got '$($ev.event)'" }
        if ($ev.batch_operation -ne $r.op) { throw "completed[$n].batch_operation: expected '$($r.op)', got '$($ev.batch_operation)'" }
        if ($ev.index -ne $n) { throw "completed[$n].index: expected $n, got '$($ev.index)'" }
        if ($ev.total -ne $total) { throw "completed[$n].total: expected $total, got '$($ev.total)'" }
        if ($ev.type -ne $r.type) { throw "completed[$n].type: expected '$($r.type)', got '$($ev.type)'" }
        if ($ev.name -ne $r.name) { throw "completed[$n].name: expected '$($r.name)', got '$($ev.name)'" }
        if ($ev.success -ne $r.success) { throw "completed[$n].success: expected '$($r.success)', got '$($ev.success)'" }
        if ($ev.exit_code -ne $r.exit_code) { throw "completed[$n].exit_code: expected '$($r.exit_code)', got '$($ev.exit_code)'" }
        $expResult = $r | ConvertTo-Json -Depth 10 -Compress
        $gotResult = $ev.result | ConvertTo-Json -Depth 10 -Compress
        if ($gotResult -cne $expResult) { throw "completed[$n].result: expected '$expResult', got '$gotResult'" }
    }
}


Export-ModuleMember -Function Find-TsInstallPath, Ensure-BaselineDll, Assert-BatchStderrProgress, Assert-BatchJsonProgress
