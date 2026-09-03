<#
.SYNOPSIS
    Shared helpers for the TSDev Automation test runner scripts (TVS-B.6.6).

.DESCRIPTION
    Provides two functions used by the PowerShell test runners that depend on
    the baseline DLL (tests/fixtures/orlib20_baseline.dll):

      * Find-TsInstallPath  - discovers the TradeStation install directory
                              (parameter override -> registry -> conventional
                              default).
      * Ensure-BaselineDll  - if the baseline DLL is missing, prompts the user
                              to copy it from the TradeStation installation.
                              Returns $true if the DLL is available (present or
                              successfully copied), $false if the user declined
                              or the DLL could not be copied. The caller MUST
                              fail loudly when this returns $false.

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
# failed. The caller is responsible for failing loudly on $false.
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
    Write-Host "ERROR: Baseline DLL not found at: $DllPath" -ForegroundColor Red
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
            Write-Host "ERROR: Cannot auto-copy - TS install not detected or orlib20.dll not found." -ForegroundColor Red
            Write-Host "Please manually copy orlib20.dll from your TradeStation installation to:" -ForegroundColor Yellow
            Write-Host "  $DllPath" -ForegroundColor Yellow
            Write-Host "Then re-run the tests." -ForegroundColor Yellow
            return $false
        }
    } else {
        # User declined — return false, caller must fail loudly.
        Write-Host "ERROR: User declined to copy the baseline DLL." -ForegroundColor Red
        Write-Host "Tests requiring this DLL will FAIL. The DLL is needed for:" -ForegroundColor Yellow
        Write-Host "  - test_orlib_resolver (signature resolution + patch-site validation)" -ForegroundColor Yellow
        Write-Host "  - sigtool verify gate (signature verification)" -ForegroundColor Yellow
        Write-Host "To resolve: copy orlib20.dll from your TradeStation installation to:" -ForegroundColor Yellow
        Write-Host "  $DllPath" -ForegroundColor Yellow
        return $false
    }
}


Export-ModuleMember -Function Find-TsInstallPath, Ensure-BaselineDll
