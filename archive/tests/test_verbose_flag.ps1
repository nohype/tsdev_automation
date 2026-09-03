<#
.SYNOPSIS
    E2E CLI test for the repeatable -v/--verbose log-level flag of tsdev_cli.

.DESCRIPTION
    End-to-end test for the runtime log-level system (TVS: CLI log levels +
    repeatable -v/--verbose). The default output must be user-facing only
    (results + warnings + errors on stderr); development diagnostics become
    available at higher verbosity:

      default (0 occurrences) -> Warning  : no "TS install path resolved" info line
      -v / --verbose (1)      -> Info     : info line present
      -vv / -v -v (2+)        -> Debug    : library debug traces present

    The suite is SELF-SUFFICIENT: it creates its own Function fixture with a
    unique per-run name (`--json insert function <name> <srcfile>`, expecting
    exit 0 / success=true / prog_id>0), runs V1-V7 against that fixture, and
    deletes it in the finally cleanup. It does NOT depend on any persistent
    probe item. The fixture setup is an uncounted prerequisite, not a numbered
    case: on success the summary lists exactly the 7 V-cases (V1-V7); if
    fixture creation fails, a SETUP row is recorded, V1-V7 are SKIPPED (they
    must not run against a missing item), and the suite exits 1.

    Test cases:
      V1  default run (exists): stderr does NOT contain
          "TS install path resolved"; stdout captured for byte-compare.
      V2  -v run (exists): stderr DOES contain "TS install path resolved";
          stdout byte-identical to the V1 default run.
      V3  -vv run (dependents): Debug level active — stderr contains the
          "enumCallersByProgId: ENTER" debug trace (unconditional first
          statement of TsDevLib::enumerateCallersByProgId, src/tsdev_lib.cpp,
          reached via doDependents on the EXISTING fixture; dependents on a
          missing item exits 5 "not found" BEFORE the enumeration, so the
          fixture must exist); the Info line is still present (levels are
          inclusive).
      V4  position-free: `exists ... -v` (flag AFTER the subcommand) behaves
          the same as V2's `-v exists ...` (flag BEFORE): info line present,
          stdout byte-identical to V2.
      V5  bundled -vv equals separate -v -v (dependents): both runs reach the
          Debug level — asserted via the debug/info markers (level-class
          assertions). NO cross-run byte equality is asserted for the
          debug-marker cases: Debug traces embed process-dependent values, so
          marker presence/absence is the stable equivalence check. (stdout
          byte-equality is kept only on the existing-fixture `exists` path:
          V2/V4/V6/V7.)
      V6  bare --verbose (exists): counts as one occurrence — info line
          present.
      V7  --verbose=<value> (exists): value ignored, counts as ONE
          occurrence — info line present.

    Prerequisites:
      - Direct `tsdev_cli` requires an elevated administrator process. Run
        this procedure elevated; otherwise the CLI returns exit code 11
        (`not_elevated`).
      - No persistent probe item is required: the fixture Function is created
        per run under a unique name and deleted in cleanup.

    Capture convention:
      stdout and stderr are captured SEPARATELY, each to its own temp file
      (1>file 2>file). File capture (instead of the pipeline) keeps the two
      streams apart so the byte-compare cases (V2/V4/V6/V7) compare
      consistent bytes across runs (PS re-encodes native output — UTF-16LE
      under PS 5.1 — but identically for every run, so equality is still
      exact). This is the same stream-separation rationale as the other E2E
      tests (test_exists.ps1 / test_json_output.ps1), tightened to byte level.

.PARAMETER CliPath
    Path to the tsdev_cli.exe executable. Defaults to
    "$PSScriptRoot\..\build\Release\tsdev_cli.exe".

.PARAMETER TsPath
    Optional. TradeStation install directory passed through to the CLI as
    "--ts-path <dir>" BEFORE the subcommand.

.PARAMETER Verbose
    Switch. When set, print full CLI output for every test.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\test_verbose_flag.ps1
    Runs the verbose-flag E2E tests with the default CLI path.

.NOTES
    Exit codes (returned by this script):
      0 = all tests passed
      1 = one or more tests failed (including fixture setup failure)
#>

param(
    [string]$CliPath = (Join-Path $PSScriptRoot "..\build\Release\tsdev_cli.exe"),
    [string]$TsPath,
    [switch]$Verbose
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Resolve paths and run ID.
# ---------------------------------------------------------------------------
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$resolved = Resolve-Path -Path $CliPath -ErrorAction SilentlyContinue
if ($resolved) {
    $CliPath = $resolved.Path
} elseif (-not (Test-Path -Path $CliPath -PathType Leaf)) {
    $CliPath = Join-Path $ProjectRoot "build\Release\tsdev_cli.exe"
}

$runId = Get-Date -Format "yyyyMMdd_HHmmss"
$tmpDir = Join-Path $ProjectRoot "tmp\e2e_verbose_flag_$runId"

# Results collector.
$Results = New-Object System.Collections.Generic.List[object]
$failCount = 0

# Created-items tracker for cleanup (mirrors test_upsert.ps1's $CreatedItems):
# each entry is a hashtable with Name and Type keys. The fixture is tracked
# BEFORE its insert result is validated, so the finally block attempts the
# delete even when a later case fails or the insert outcome could not be
# confirmed (delete exit 5 = item never created / already gone — handled
# gracefully).
$CreatedItems = New-Object System.Collections.Generic.List[object]

# Fixture Function for this run. Unique per run (same run-id timestamp pattern
# as test_upsert.ps1's probe names) so parallel/stale runs never collide.
# Created in SETUP, deleted in CLEANUP — no persistent probe item is needed.
$ProbeName = "E2E_VerboseFlag_${runId}_Fn"

# Info-level marker: emitted by resolveTsInstallRoot() at Info (visible at
# -v and above, suppressed at the default Warning level).
$InfoMarker = "TS install path resolved"

# Debug-level marker: emitted by TsDevLib::enumerateCallersByProgId() as its
# unconditional first statement ("enumCallersByProgId: ENTER progId=...",
# src/tsdev_lib.cpp). Reached via `dependents` on the EXISTING fixture:
# doDependents exits 5 ("not found") BEFORE the enumeration when the item is
# missing, and `exists` on a found item emits no debug line (found in pass 0;
# refreshCache is lazy) — so the Debug marker uses the read-only `dependents`
# query against the fixture.
$DebugMarker = "enumCallersByProgId: ENTER"

# ---------------------------------------------------------------------------
# Write-SourceFile: writes EasyLanguage source to a temp file in ASCII
# encoding (UTF-8 BOM breaks the EasyLanguage compiler). Returns the path.
# (Same helper as test_upsert.ps1 / test_exists.ps1.)
# ---------------------------------------------------------------------------
function Write-SourceFile {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string]$Content
    )
    $path = Join-Path $script:tmpDir "$Name.txt"
    [System.IO.File]::WriteAllText($path, $Content, [System.Text.Encoding]::ASCII)
    return $path
}

# ---------------------------------------------------------------------------
# Invoke-CliFiles: runs tsdev_cli.exe with the given argument array, capturing
# stdout and stderr SEPARATELY to temp files (1>file 2>file). Returns a
# hashtable with Stdout (string), StdoutBytes (byte[]), Stderr (string),
# ExitCode (int), and the two file paths (for byte-compare helpers).
#
# File capture keeps stdout byte-pure (no pipeline line decoding), which the
# byte-compare cases rely on.
# ---------------------------------------------------------------------------
function Invoke-CliFiles {
    param(
        [Parameter(Mandatory)] [string[]]$Arguments
    )
    $fullArgs = @()
    if ($script:TsPath) {
        $fullArgs += @("--ts-path", $script:TsPath)
    }
    $fullArgs += $Arguments

    $guid = [System.Guid]::NewGuid().ToString('N')
    $stdoutFile = Join-Path $script:tmpDir "stdout_$guid.txt"
    $stderrFile = Join-Path $script:tmpDir "stderr_$guid.txt"

    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $script:CliPath @fullArgs 1>$stdoutFile 2>$stderrFile
        $exitCode = $LASTEXITCODE
    }
    catch {
        Write-Host "ERROR: failed to launch CLI: $($_.Exception.Message)" -ForegroundColor Red
        $exitCode = -2
    }
    finally {
        $ErrorActionPreference = $prevEAP
    }

    $stdoutBytes = [System.IO.File]::ReadAllBytes($stdoutFile)
    # PS 5.1 "1>file" re-encodes native stdout as UTF-16LE (BOM FF FE), so the
    # string must be decoded with BOM auto-detection (same as Stderr below) —
    # an explicit UTF-8 decode yields mojibake and breaks JSON parsing.
    $stdoutStr = [System.IO.File]::ReadAllText($stdoutFile).TrimEnd()
    $stderrStr = ""
    if (Test-Path -Path $stderrFile -PathType Leaf) {
        $stderrStr = ([System.IO.File]::ReadAllText($stderrFile)).TrimEnd()
    }

    $capture = @{
        Stdout      = $stdoutStr
        StdoutBytes = $stdoutBytes
        Stderr      = $stderrStr
        ExitCode    = $exitCode
        StdoutFile  = $stdoutFile
        StderrFile  = $stderrFile
    }
    # Remember the most recent capture for the -Verbose output preview.
    $script:lastCapture = $capture
    return $capture
}

# ---------------------------------------------------------------------------
# Compare-StdoutBytes: byte-compare two stdout byte arrays. Returns $true when
# identical, otherwise records the difference in $script:LastCompareDetail.
# ---------------------------------------------------------------------------
function Compare-StdoutBytes {
    param(
        [byte[]]$A,
        [byte[]]$B,
        [string]$Label
    )
    $script:LastCompareDetail = ""
    if ($A.Length -ne $B.Length) {
        $script:LastCompareDetail = "$Label byte length differs: $($A.Length) vs $($B.Length)"
        return $false
    }
    for ($i = 0; $i -lt $A.Length; $i++) {
        if ($A[$i] -ne $B[$i]) {
            $script:LastCompareDetail = "$Label first byte difference at offset $i ($($A[$i]) vs $($B[$i]))"
            return $false
        }
    }
    return $true
}

# ---------------------------------------------------------------------------
# Assert-True: records a single boolean assertion. Throws on failure (caught
# by the enclosing test case to record a FAIL).
# ---------------------------------------------------------------------------
function Assert-True {
    param(
        [Parameter(Mandatory)] [bool]$Condition,
        # AllowEmptyString: a PASSING assertion legitimately carries no detail
        # (e.g. Compare-StdoutBytes leaves LastCompareDetail empty on success);
        # Mandatory alone would reject the bind before the body runs.
        [Parameter(Mandatory)] [AllowEmptyString()] [string]$Message
    )
    if (-not $Condition) {
        throw $Message
    }
}

# ---------------------------------------------------------------------------
# Run-TestCase: runs a scriptblock as a single test case, captures pass/fail.
# ---------------------------------------------------------------------------
function Run-TestCase {
    param(
        [Parameter(Mandatory)] [string]$TestId,
        [Parameter(Mandatory)] [string]$Step,
        [Parameter(Mandatory)] [scriptblock]$Body
    )
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $status = 'PASS'
    $detail = ''
    try {
        & $Body
    }
    catch {
        $status = 'FAIL'
        $detail = $_.Exception.Message
        $script:failCount++
    }
    $stopwatch.Stop()
    $color = if ($status -eq 'PASS') { 'Green' } else { 'Red' }
    $durationStr = "{0:0.00}s" -f $stopwatch.Elapsed.TotalSeconds
    Write-Host ("[{0}] {1} ({2}): {3} [{4}]" -f $status, $TestId, $Step, $detail, $durationStr) -ForegroundColor $color
    # -Verbose convention (same as test_upsert.ps1): also print a short CLI
    # output preview for PASSING cases; failures always print their detail.
    if ($status -eq 'PASS' -and $script:Verbose -and $script:lastCapture) {
        $preview = (($script:lastCapture.Stdout + ' ' + $script:lastCapture.Stderr) -split "`n" |
            Where-Object { $_.Trim() -ne '' } | Select-Object -First 4) -join ' | '
        if ($preview.Trim() -ne '') {
            Write-Host ("    output: {0}" -f $preview) -ForegroundColor DarkGray
        }
    }
    $script:Results.Add([pscustomobject]@{
        TestId = $TestId
        Step   = $Step
        Result = $status
        Detail = $detail
        Duration = $stopwatch.Elapsed
    }) | Out-Null
}

# ---------------------------------------------------------------------------
# Print-Summary: prints a summary table and returns the count of failures.
# ---------------------------------------------------------------------------
function Print-Summary {
    param($Results)

    Write-Host ""
    Write-Host "==================== VERBOSE-FLAG E2E SUMMARY ====================" -ForegroundColor Cyan
    $header = "{0,-8} {1,-34} {2,-8}" -f "Test#", "Step", "Result"
    Write-Host $header
    Write-Host ("{0,-8} {1,-34} {2,-8}" -f ("-" * 8), ("-" * 34), ("-" * 8))

    $passed = 0
    $failed = 0
    foreach ($r in $Results) {
        $line = "{0,-8} {1,-34} {2,-8}" -f $r.TestId, $r.Step, $r.Result
        $color = if ($r.Result -eq 'PASS') { 'Green' } else { 'Red' }
        Write-Host $line -ForegroundColor $color
        if ($r.Result -eq 'PASS') { $passed++ } else { $failed++ }
    }

    Write-Host ""
    Write-Host ("Totals: {0}/{1} passed, {2} failed" -f $passed, $Results.Count, $failed) -ForegroundColor Cyan
    return $failed
}

# ===========================================================================
# MAIN EXECUTION
# ===========================================================================

Write-Host "============================================" -ForegroundColor Cyan
Write-Host "TSDev Automation - verbose flag E2E Test" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "Run ID     : $runId"
Write-Host "Project root: $ProjectRoot"
Write-Host "CLI path   : $CliPath"
Write-Host "TS path    : $(if ($TsPath) { $TsPath } else { 'auto-discovered' })"
Write-Host "Verbose    : $Verbose"
Write-Host "Temp dir   : $tmpDir"
Write-Host "Fixture    : $ProbeName (created per run; deleted in cleanup)"
Write-Host ""

# Verify CLI exists.
if (-not (Test-Path -Path $CliPath -PathType Leaf)) {
    Write-Host "ERROR: CLI executable not found: $CliPath" -ForegroundColor Red
    Write-Host "Build the project first: cmake --build build --config Release" -ForegroundColor Yellow
    exit 1
}

# Create temp directory.
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null

try {
    # =====================================================================
    # SETUP (uncounted prerequisite for V1-V7): create the fixture Function.
    #
    # Not a numbered V-case: the fixture is scaffolding, not the behavior
    # under test. On success the summary lists exactly the 7 V-cases
    # (V1-V7); if fixture creation fails, a SETUP row is recorded below,
    # V1-V7 are SKIPPED (they must not run against a missing item), and the
    # suite exits 1 via the summary count.
    # =====================================================================
    Write-Host "--- SETUP: insert fixture Function ---" -ForegroundColor Cyan
    $fixtureReady = $false
    $setupDetail = ""
    try {
        # Valid EasyLanguage Function source (same pattern as
        # test_upsert.ps1): "{ name }`r`nname = 1;". The "{ name }" text is
        # arbitrary fixture content only — stored metadata comes from
        # warehouse APIs, never from source text.
        $funcSrc = "{ $ProbeName }`r`n$ProbeName = 1;"
        $fixtureSrcFile = Write-SourceFile "verbose_fixture_src" $funcSrc

        # Track the fixture BEFORE validating the insert result: the finally
        # block then attempts the delete even if success cannot be confirmed
        # (delete exit 5 = never created / already gone — handled gracefully).
        $CreatedItems.Add(@{ Name = $ProbeName; Type = "function" }) | Out-Null

        $setup = Invoke-CliFiles -Arguments @("--json", "insert", "function", $ProbeName, $fixtureSrcFile)
        if ($setup.ExitCode -ne 0) {
            $setupDetail = "insert exit code: expected 0, got $($setup.ExitCode); stderr: $($setup.Stderr)"
        }
        else {
            try {
                $jSetup = $setup.Stdout | ConvertFrom-Json -ErrorAction Stop
                if (-not $jSetup.success) {
                    $setupDetail = "insert JSON success=false (expected true); stdout: $($setup.Stdout)"
                }
                elseif (-not $jSetup.prog_id -or $jSetup.prog_id -le 0) {
                    $setupDetail = "insert prog_id missing or <= 0; stdout: $($setup.Stdout)"
                }
                else {
                    $fixtureReady = $true
                    Write-Host "SETUP: fixture Function '$ProbeName' created (prog_id=$($jSetup.prog_id))" -ForegroundColor Green
                }
            }
            catch {
                $setupDetail = "insert output is not valid JSON: $($_.Exception.Message); stdout: $($setup.Stdout)"
            }
        }
    }
    catch {
        $setupDetail = "fixture setup error: $($_.Exception.Message)"
    }

    if (-not $fixtureReady) {
        Write-Host "ERROR: fixture Function '$ProbeName' could not be created - V1-V7 NOT run." -ForegroundColor Red
        Write-Host "       $setupDetail" -ForegroundColor Red
        $Results.Add([pscustomobject]@{
            TestId   = 'SETUP'
            Step     = 'insert fixture function'
            Result   = 'FAIL'
            Detail   = $setupDetail
            Duration = [timespan]::Zero
        }) | Out-Null
    }
    else {

    # =====================================================================
    # V1: Default run — stderr must NOT contain the Info marker.
    # =====================================================================
    Write-Host "--- V1: default run (warnings + errors only) ---" -ForegroundColor Cyan
    $v1 = $null
    Run-TestCase -TestId "V1" -Step "default run, no info on stderr" -Body {
        $script:v1 = Invoke-CliFiles -Arguments @("exists", "function", $script:ProbeName)
        Assert-True ($script:v1.ExitCode -eq 0) "exit code: expected 0, got $($script:v1.ExitCode); stderr: $($script:v1.Stderr)"
        Assert-True (-not $script:v1.Stderr.Contains($script:InfoMarker)) "default run stderr must NOT contain '$($script:InfoMarker)'; got: $($script:v1.Stderr)"
    }

    # =====================================================================
    # V2: -v run — Info marker present; stdout byte-identical to V1.
    # =====================================================================
    Write-Host "--- V2: -v run (Info level) ---" -ForegroundColor Cyan
    $v2 = $null
    Run-TestCase -TestId "V2" -Step "-v shows info, stdout unchanged" -Body {
        $script:v2 = Invoke-CliFiles -Arguments @("-v", "exists", "function", $script:ProbeName)
        Assert-True ($script:v2.ExitCode -eq 0) "exit code: expected 0, got $($script:v2.ExitCode); stderr: $($script:v2.Stderr)"
        Assert-True ($script:v2.Stderr.Contains($script:InfoMarker)) "-v run stderr must contain '$($script:InfoMarker)'; got: $($script:v2.Stderr)"
        $same = Compare-StdoutBytes -A $script:v1.StdoutBytes -B $script:v2.StdoutBytes -Label "V1 vs V2 stdout"
        Assert-True $same $script:LastCompareDetail
    }

    # =====================================================================
    # V3: -vv run — Debug level active (debug trace present; Info inclusive).
    # =====================================================================
    Write-Host "--- V3: -vv run (Debug level) ---" -ForegroundColor Cyan
    $v3 = $null
    Run-TestCase -TestId "V3" -Step "-vv shows debug trace" -Body {
        $script:v3 = Invoke-CliFiles -Arguments @("-vv", "dependents", "function", $script:ProbeName)
        Assert-True ($script:v3.ExitCode -eq 0) "exit code: expected 0, got $($script:v3.ExitCode); stderr: $($script:v3.Stderr)"
        Assert-True ($script:v3.Stderr.Contains($script:DebugMarker)) `
            "-vv run stderr must contain '$($script:DebugMarker)'; got: $($script:v3.Stderr)"
        Assert-True ($script:v3.Stderr.Contains($script:InfoMarker)) `
            "-vv run stderr must still contain '$($script:InfoMarker)' (levels are inclusive)"
    }

    # =====================================================================
    # V4: Position-free — flag after the subcommand behaves like V2.
    # =====================================================================
    Write-Host "--- V4: position-free flag ---" -ForegroundColor Cyan
    Run-TestCase -TestId "V4" -Step "exists ... -v == -v exists ..." -Body {
        $v4 = Invoke-CliFiles -Arguments @("exists", "function", $script:ProbeName, "-v")
        Assert-True ($v4.ExitCode -eq 0) "exit code: expected 0, got $($v4.ExitCode); stderr: $($v4.Stderr)"
        Assert-True ($v4.Stderr.Contains($script:InfoMarker)) `
            "trailing -v run stderr must contain '$($script:InfoMarker)'; got: $($v4.Stderr)"
        $same = Compare-StdoutBytes -A $script:v2.StdoutBytes -B $v4.StdoutBytes -Label "V2 vs V4 stdout"
        Assert-True $same $script:LastCompareDetail
    }

    # =====================================================================
    # V5: Bundled -vv equals separate -v -v (same op, same level).
    # Level equivalence is asserted via the markers: both runs must reach
    # Debug (enumCallersByProgId ENTER trace) and keep the inclusive Info
    # line. NO cross-run byte equality is asserted here — the debug-marker
    # cases use marker presence/absence (level-class assertions) because
    # Debug stderr traces embed process-dependent values. Byte-equality is
    # kept only on the existing-fixture `exists` path (V2/V4/V6/V7).
    # =====================================================================
    Write-Host "--- V5: bundled -vv == -v -v ---" -ForegroundColor Cyan
    Run-TestCase -TestId "V5" -Step "-vv equals -v -v" -Body {
        $v5 = Invoke-CliFiles -Arguments @("-v", "-v", "dependents", "function", $script:ProbeName)
        Assert-True ($v5.ExitCode -eq 0) "exit code: expected 0, got $($v5.ExitCode); stderr: $($v5.Stderr)"
        Assert-True ($v5.Stderr.Contains($script:DebugMarker)) `
            "-v -v run stderr must contain '$($script:DebugMarker)'; got: $($v5.Stderr)"
        Assert-True ($v5.Stderr.Contains($script:InfoMarker)) `
            "-v -v run stderr must contain '$($script:InfoMarker)'"
    }

    # =====================================================================
    # V6: Bare --verbose counts as one occurrence (Info level).
    # =====================================================================
    Write-Host "--- V6: bare --verbose form ---" -ForegroundColor Cyan
    Run-TestCase -TestId "V6" -Step "--verbose shows info" -Body {
        $v6 = Invoke-CliFiles -Arguments @("--verbose", "exists", "function", $script:ProbeName)
        Assert-True ($v6.ExitCode -eq 0) "exit code: expected 0, got $($v6.ExitCode); stderr: $($v6.Stderr)"
        Assert-True ($v6.Stderr.Contains($script:InfoMarker)) `
            "--verbose run stderr must contain '$($script:InfoMarker)'; got: $($v6.Stderr)"
        $same = Compare-StdoutBytes -A $script:v1.StdoutBytes -B $v6.StdoutBytes -Label "V1 vs V6 stdout"
        Assert-True $same $script:LastCompareDetail
    }

    # =====================================================================
    # V7: --verbose=<value> — value ignored, counts as ONE occurrence.
    # =====================================================================
    Write-Host "--- V7: --verbose=<value> form ---" -ForegroundColor Cyan
    Run-TestCase -TestId "V7" -Step "--verbose=x shows info" -Body {
        $v7 = Invoke-CliFiles -Arguments @("--verbose=1", "exists", "function", $script:ProbeName)
        Assert-True ($v7.ExitCode -eq 0) "exit code: expected 0, got $($v7.ExitCode); stderr: $($v7.Stderr)"
        Assert-True ($v7.Stderr.Contains($script:InfoMarker)) `
            "--verbose=1 run stderr must contain '$($script:InfoMarker)'; got: $($v7.Stderr)"
        $same = Compare-StdoutBytes -A $script:v1.StdoutBytes -B $v7.StdoutBytes -Label "V1 vs V7 stdout"
        Assert-True $same $script:LastCompareDetail
    }

    }  # end: fixture-ready branch (V1-V7 run only against an existing fixture)
}
finally {
    # =====================================================================
    # CLEANUP (always runs, even when cases fail): delete the fixture
    # Function, then remove the temp directory.
    # =====================================================================
    Write-Host ""
    Write-Host "===== CLEANUP =====" -ForegroundColor Cyan

    foreach ($item in $CreatedItems) {
        $cli = Invoke-CliFiles -Arguments @("delete", $item.Type, $item.Name)
        if ($cli.ExitCode -eq 0) {
            Write-Host "  Cleaned up: $($item.Type) '$($item.Name)'" -ForegroundColor DarkGray
        }
        elseif ($cli.ExitCode -eq 5) {
            Write-Host "  Already gone: $($item.Type) '$($item.Name)' (exit 5)" -ForegroundColor DarkGray
        }
        else {
            Write-Host "  WARN: delete $($item.Type) '$($item.Name)' returned exit $($cli.ExitCode)" -ForegroundColor Yellow
        }
    }

    # Remove temp directory.
    if (Test-Path -Path $tmpDir -PathType Container) {
        Remove-Item -Path $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
    }
    Write-Host "CLEANUP: OK (fixture deleted; temp files removed)" -ForegroundColor Green
}

# ===========================================================================
# SUMMARY
# ===========================================================================
$failureCount = Print-Summary -Results $Results

Write-Host ""
if ($failureCount -eq 0) {
    Write-Host "ALL VERBOSE-FLAG E2E TESTS PASSED" -ForegroundColor Green
    exit 0
}
else {
    Write-Host "$failureCount TEST(S) FAILED" -ForegroundColor Red
    exit 1
}
