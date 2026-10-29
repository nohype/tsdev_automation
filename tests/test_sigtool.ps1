<#
.SYNOPSIS
    Regression test script for the sigtool CLI.

.DESCRIPTION
    This script exercises every sigtool subcommand against the baseline
    orlib20_baseline.dll and the committed v9.50.01.3344 signature set.

    sigtool.exe is built by the CMake sigtool target and is checked before the
    cases run. A successful run exits 0; failed cases exit 1. When the
    baseline DLL cannot be acquired, the DLL-dependent cases are skipped
    and only the DLL-free error-path cases run (still exit 0 on success).

    Exit codes:
        0 = all executed tests passed (skipped cases do not fail the run)
        1 = at least one test failed (or a hard prerequisite was missing)

.NOTES
    Run with:
        powershell -ExecutionPolicy Bypass -File tests/test_sigtool.ps1
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

# Committed signature set for v9.50.01.3344 (22 signatures, 11 patch_site).
# Workstream M slice M.1: removed CELCompiler_ctor and CELCompiler_dtor
# signature entries (raw ctor/dtor no longer called directly; replaced by
# ordinal-66/67 export resolution). Count went from 24 to 22.
$sigJson = "$PSScriptRoot\..\signatures\v9.50.01.3344.json"

# Total number of signatures in the committed set (used for summary checks).
$expectedSignatureCount = 22

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

# Baseline DLL: an untracked local-copy fixture (proprietary orlib20.dll).
# If missing, prompt the user to copy it from the TS install (interactive
# runs). When invoked from run_regression.ps1 with -SkipBaselineDllPrompt
# (the orchestrator already prompted), skip the prompt. When the DLL cannot
# be acquired, the DLL-dependent cases (1-9) are SKIPPED and only the
# DLL-free error-path cases (10-12) still run.
$baselineSkipReason = "baseline DLL unavailable (tests\fixtures\orlib20_baseline.dll)"
if ($SkipBaselineDllPrompt) {
    $baselineDllAvailable = (Test-Path -Path $dll -PathType Leaf)
} else {
    $baselineDllAvailable = Ensure-BaselineDll -DllPath $dll
}
if (-not $baselineDllAvailable) {
    Write-Host ""
    Write-Host "NOTE: baseline DLL not available - DLL-dependent sigtool cases will be SKIPPED." -ForegroundColor DarkYellow
    Write-Host "To enable them, copy <TS install>\Program\orlib20.dll to:" -ForegroundColor DarkYellow
    Write-Host "  $dll" -ForegroundColor DarkYellow
}

# The signature JSON is only needed by the DLL-dependent cases (8-9).
if ($baselineDllAvailable -and -not (Test-Path $sigJson)) {
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

.PARAMETER Skip
    When set, the case is recorded as SKIPPED (missing prerequisite) instead
    of being executed; the skip reason is taken from $baselineSkipReason.
#>
function Run-Test {
    param(
        [string]   $Name,
        [scriptblock] $ScriptBlock,
        [int]      $ExpectedExit,
        [scriptblock] $ExtraCheck,
        [switch]   $Skip
    )

    if ($Skip) {
        Write-Host "SKIP: $Name ($baselineSkipReason)"
        $script:skipCount++
        return
    }

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
$script:skipCount = 0

# ===========================================================================
# Test 1: verify (default signatures)
#   sigtool should detect the DLL version, load the committed signature set,
#   and report all 22 signatures as PASS. Exit 0.
# ===========================================================================
Run-Test -Name "verify (default signatures)" `
    -ExpectedExit 0 `
    -Skip:(-not $baselineDllAvailable) `
    -ScriptBlock {
        $script:out = & $sigtool verify --dll $dll 2>&1 | Out-String
        $script:code = $LASTEXITCODE
    } `
    -ExtraCheck {
        # Accept either a "22/22"-style summary or 22 individual PASS lines.
        ($script:out -match "$expectedSignatureCount\s*/\s*$expectedSignatureCount") -or
        (($script:out -split "`r?`n" | Select-String -Pattern 'PASS' -CaseSensitive).Count -ge $expectedSignatureCount)
    }

# ===========================================================================
# Test 2: verify --verbose
#   Verbose mode should print per-signature details (RVA, match count) in
#   addition to the PASS/FAIL verdict. Exit 0.
# ===========================================================================
Run-Test -Name "verify --verbose" `
    -ExpectedExit 0 `
    -Skip:(-not $baselineDllAvailable) `
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
#   Validates original bytes at every patch_site signature. All 11 patch sites
#   should match the baseline DLL. Exit 0.
# ===========================================================================
Run-Test -Name "verify-patch" `
    -ExpectedExit 0 `
    -Skip:(-not $baselineDllAvailable) `
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
    -Skip:(-not $baselineDllAvailable) `
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
    -Skip:(-not $baselineDllAvailable) `
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
    -Skip:(-not $baselineDllAvailable) `
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
    -Skip:(-not $baselineDllAvailable) `
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
#   produce 22/22 PASS. Exit 0.
# ===========================================================================
Run-Test -Name "verify --signatures JSON" `
    -ExpectedExit 0 `
    -Skip:(-not $baselineDllAvailable) `
    -ScriptBlock {
        $script:out = & $sigtool verify --dll $dll --signatures $sigJson 2>&1 | Out-String
        $script:code = $LASTEXITCODE
    } `
    -ExtraCheck {
        ($script:out -match "$expectedSignatureCount\s*/\s*$expectedSignatureCount") -or
        (($script:out -split "`r?`n" | Select-String -Pattern 'PASS' -CaseSensitive).Count -ge $expectedSignatureCount)
    }

# ===========================================================================
# Test 9: verify-patch with explicit --signatures JSON
#   Same as Test 3 but with the JSON supplied explicitly. Exit 0.
# ===========================================================================
Run-Test -Name "verify-patch --signatures JSON" `
    -ExpectedExit 0 `
    -Skip:(-not $baselineDllAvailable) `
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

$total = $script:passCount + $script:failCount + $script:skipCount
Write-Host ""
if ($script:skipCount -gt 0) {
    Write-Host "$($script:passCount)/$total tests passed, $($script:skipCount) skipped"
} else {
    Write-Host "$($script:passCount)/$total tests passed"
}

if ($script:failCount -gt 0) {
    exit 1
}
exit 0
