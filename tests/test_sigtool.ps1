<#
.SYNOPSIS
    TDD RED-phase test script for the sigtool CLI.

.DESCRIPTION
    This script exercises every sigtool subcommand against the baseline
    orlib20_baseline.dll and the committed v9.50.01.3344 signature set.

    It is the RED phase of TDD: sigtool.exe does NOT exist yet, so every
    test is expected to fail (the script exits 1). Once sigtool is
    implemented, the same script should go GREEN (exit 0).

    Exit codes:
        0 = all tests passed
        1 = at least one test failed (or a prerequisite was missing)

.NOTES
    Run with:
        powershell -ExecutionPolicy Bypass -File src/test_sigtool.ps1
#>

# ---------------------------------------------------------------------------
# Configuration / paths
# ---------------------------------------------------------------------------

param(
    [switch]$SkipBaselineDllPrompt
)

# sigtool is built into build\Release\ (relative to this script's location).
$sigtool = "$PSScriptRoot\..\build\Release\sigtool.exe"

# Baseline DLL used for all tests.
$dll = "$PSScriptRoot\fixtures\orlib20_baseline.dll"

# Committed signature set for v9.50.01.3344 (12 signatures, 4 patch_site).
$sigJson = "$PSScriptRoot\..\signatures\v9.50.01.3344.json"

# Total number of signatures in the committed set (used for summary checks).
$expectedSignatureCount = 12

# ---------------------------------------------------------------------------
# Robustness settings
# ---------------------------------------------------------------------------

# Do not let PowerShell abort when a native command returns a non-zero exit
# code — we intentionally exercise error paths.
$ErrorActionPreference = 'Continue'

# ---------------------------------------------------------------------------
# Shared test helpers (baseline DLL prompting + TS install discovery).
# ---------------------------------------------------------------------------
Import-Module (Join-Path $PSScriptRoot "TestHelpers.psm1") -ErrorAction Stop -DisableNameChecking

# ---------------------------------------------------------------------------
# Prerequisite checks — fail fast if the fixtures are missing.
# ---------------------------------------------------------------------------

if (-not (Test-Path $sigtool)) {
    Write-Host "FAIL: sigtool.exe not found at $sigtool"
    exit 1
}

# Baseline DLL: if missing, prompt the user to copy it from the TS install.
# Do NOT continue without a user decision. The entire script depends on the
# DLL, so a decline (or failed copy) is a hard failure. When invoked from
# run_regression.ps1 with -SkipBaselineDllPrompt (the orchestrator already
# prompted and the user declined), skip the prompt and fail immediately.
if ($SkipBaselineDllPrompt) {
    if (-not (Test-Path -Path $dll -PathType Leaf)) {
        Write-Host ""
        Write-Host "FATAL: baseline DLL not available - sigtool verify gate cannot run." -ForegroundColor Red
        Write-Host "The entire sigtool test suite requires $dll." -ForegroundColor Yellow
        Write-Host "(prompt suppressed by -SkipBaselineDllPrompt from parent orchestrator)" -ForegroundColor DarkGray
        exit 1
    }
} elseif (-not (Ensure-BaselineDll -DllPath $dll)) {
    Write-Host ""
    Write-Host "FATAL: baseline DLL not available - sigtool verify gate cannot run." -ForegroundColor Red
    Write-Host "The entire sigtool test suite requires $dll." -ForegroundColor Yellow
    exit 1
}

if (-not (Test-Path $sigJson)) {
    Write-Host "FAIL: signature JSON not found at $sigJson"
    exit 1
}

# ---------------------------------------------------------------------------
# Run-Test helper
# ---------------------------------------------------------------------------

<#
.SYNOPSIS
    Runs a single test case and records pass/fail.

.PARAMETER Name
    Human-readable test name printed in the PASS/FAIL line.

.PARAMETER ScriptBlock
    A script block that executes the sigtool command. The block MUST set two
    script-scoped variables:
      $script:out   - combined stdout/stderr text
      $script:code  - the process exit code

.PARAMETER ExpectedExit
    The exit code the command is expected to produce.

.PARAMETER ExtraCheck
    Optional script block returning $true/$false for an additional stdout
    content assertion (e.g. "contains '9.50.01.3344'"). Evaluated only when
    the exit code already matches.
#>
function Run-Test {
    param(
        [string]   $Name,
        [scriptblock] $ScriptBlock,
        [int]      $ExpectedExit,
        [scriptblock] $ExtraCheck
    )

    # Execute the command block — it populates $script:out and $script:code.
    & $ScriptBlock

    $passed = ($script:code -eq $ExpectedExit)

    # If the exit code matched and an extra content check was supplied, run it.
    if ($passed -and $ExtraCheck) {
        $extraResult = & $ExtraCheck
        if (-not $extraResult) {
            $passed = $false
        }
    }

    if ($passed) {
        Write-Host "PASS: $Name"
        $script:passCount++
    }
    else {
        Write-Host "FAIL: $Name (expected exit $ExpectedExit, got $script:code)"
        # Show captured output to aid debugging.
        if ($script:out) {
            Write-Host "  --- output ---"
            foreach ($line in ($script:out -split "`r?`n")) {
                Write-Host "  $line"
            }
            Write-Host "  --- end output ---"
        }
        $script:failCount++
    }
}

# ---------------------------------------------------------------------------
# Test counters
# ---------------------------------------------------------------------------

$script:passCount = 0
$script:failCount = 0

# ===========================================================================
# Test 1: verify (default signatures)
#   sigtool should detect the DLL version, load the committed signature set,
#   and report all 12 signatures as PASS. Exit 0.
# ===========================================================================
Run-Test -Name "verify (default signatures)" `
    -ExpectedExit 0 `
    -ScriptBlock {
        $script:out = & $sigtool verify --dll $dll 2>&1 | Out-String
        $script:code = $LASTEXITCODE
    } `
    -ExtraCheck {
        # Accept either a "12/12"-style summary or 12 individual PASS lines.
        ($script:out -match '12\s*/\s*12') -or
        (($script:out -split "`r?`n" | Select-String -Pattern 'PASS' -CaseSensitive).Count -ge $expectedSignatureCount)
    }

# ===========================================================================
# Test 2: verify --verbose
#   Verbose mode should print per-signature details (RVA, match count) in
#   addition to the PASS/FAIL verdict. Exit 0.
# ===========================================================================
Run-Test -Name "verify --verbose" `
    -ExpectedExit 0 `
    -ScriptBlock {
        $script:out = & $sigtool verify --dll $dll --verbose 2>&1 | Out-String
        $script:code = $LASTEXITCODE
    } `
    -ExtraCheck {
        # Verbose output should mention RVA and match count for signatures.
        ($script:out -match 'RVA') -and ($script:out -match 'match')
    }

# ===========================================================================
# Test 3: verify-patch
#   Validates original bytes at every patch_site signature. All 4 patch sites
#   should match the baseline DLL. Exit 0.
# ===========================================================================
Run-Test -Name "verify-patch" `
    -ExpectedExit 0 `
    -ScriptBlock {
        $script:out = & $sigtool verify-patch --dll $dll 2>&1 | Out-String
        $script:code = $LASTEXITCODE
    }

# ===========================================================================
# Test 4: info
#   Prints DLL metadata. The FileVersion string "9.50.01.3344" must appear.
#   Exit 0.
# ===========================================================================
Run-Test -Name "info" `
    -ExpectedExit 0 `
    -ScriptBlock {
        $script:out = & $sigtool info --dll $dll 2>&1 | Out-String
        $script:code = $LASTEXITCODE
    } `
    -ExtraCheck {
        $script:out -match '9\.50\.01\.3344'
    }

# ===========================================================================
# Test 5: scan (valid pattern — function prologue)
#   "55 8B EC" is the classic x86 function prologue (push ebp; mov ebp,esp).
#   It should appear many times in the .text section. Exit 0 (>= 1 match).
# ===========================================================================
Run-Test -Name "scan (valid pattern)" `
    -ExpectedExit 0 `
    -ScriptBlock {
        $script:out = & $sigtool scan --dll $dll --pattern "55 8B EC" 2>&1 | Out-String
        $script:code = $LASTEXITCODE
    }

# ===========================================================================
# Test 6: scan (no-match pattern)
#   The "DE AD BE EF" pattern repeated 8 times (32 bytes) is a canonical
#   "never appears in real code" test pattern. Exit 1 (no match).
# ===========================================================================
Run-Test -Name "scan (no match pattern)" `
    -ExpectedExit 1 `
    -ScriptBlock {
        $script:out = & $sigtool scan --dll $dll --pattern "DE AD BE EF DE AD BE EF DE AD BE EF DE AD BE EF DE AD BE EF DE AD BE EF DE AD BE EF DE AD BE EF" 2>&1 | Out-String
        $script:code = $LASTEXITCODE
    }

# ===========================================================================
# Test 7: manifest (known version)
#   The baseline DLL's FileVersion is in the version manifest. Output should
#   say "known" (case-insensitive). Exit 0.
# ===========================================================================
Run-Test -Name "manifest (known version)" `
    -ExpectedExit 0 `
    -ScriptBlock {
        $script:out = & $sigtool manifest --dll $dll 2>&1 | Out-String
        $script:code = $LASTEXITCODE
    } `
    -ExtraCheck {
        $script:out -imatch 'known'
    }

# ===========================================================================
# Test 8: verify with explicit --signatures JSON
#   Pointing sigtool at the same JSON it would auto-detect should still
#   produce 12/12 PASS. Exit 0.
# ===========================================================================
Run-Test -Name "verify --signatures JSON" `
    -ExpectedExit 0 `
    -ScriptBlock {
        $script:out = & $sigtool verify --dll $dll --signatures $sigJson 2>&1 | Out-String
        $script:code = $LASTEXITCODE
    } `
    -ExtraCheck {
        ($script:out -match '12\s*/\s*12') -or
        (($script:out -split "`r?`n" | Select-String -Pattern 'PASS' -CaseSensitive).Count -ge $expectedSignatureCount)
    }

# ===========================================================================
# Test 9: verify-patch with explicit --signatures JSON
#   Same as Test 3 but with the JSON supplied explicitly. Exit 0.
# ===========================================================================
Run-Test -Name "verify-patch --signatures JSON" `
    -ExpectedExit 0 `
    -ScriptBlock {
        $script:out = & $sigtool verify-patch --dll $dll --signatures $sigJson 2>&1 | Out-String
        $script:code = $LASTEXITCODE
    }

# ===========================================================================
# Test 10: error case — nonexistent DLL
#   A missing DLL path is a file-not-found error. Exit 2.
# ===========================================================================
Run-Test -Name "error: nonexistent DLL" `
    -ExpectedExit 2 `
    -ScriptBlock {
        $script:out = & $sigtool verify --dll "C:\nonexistent.dll" 2>&1 | Out-String
        $script:code = $LASTEXITCODE
    }

# ===========================================================================
# Test 11: error case — no arguments
#   Invoking sigtool with no subcommand is a usage error. Exit 2.
# ===========================================================================
Run-Test -Name "error: no arguments" `
    -ExpectedExit 2 `
    -ScriptBlock {
        $script:out = & $sigtool 2>&1 | Out-String
        $script:code = $LASTEXITCODE
    }

# ===========================================================================
# Test 12: error case — unknown subcommand
#   "frobnicate" is not a valid subcommand. Exit 2.
# ===========================================================================
Run-Test -Name "error: unknown subcommand" `
    -ExpectedExit 2 `
    -ScriptBlock {
        $script:out = & $sigtool frobnicate --dll $dll 2>&1 | Out-String
        $script:code = $LASTEXITCODE
    }

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

$total = $script:passCount + $script:failCount
Write-Host ""
Write-Host "$($script:passCount)/$total tests passed"

if ($script:failCount -gt 0) {
    exit 1
}
exit 0
