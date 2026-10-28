<#
.SYNOPSIS
    E2E CLI test for the global --pbar progress-bar flag of tsdev_cli.

.DESCRIPTION
    End-to-end test for the --pbar flag: a pinned, stderr-only progress bar
    redrawn in place via '\r' (no ANSI) with strictly ASCII glyphs - '#'
    (0x23) filled cells and '-' (0x2D) empty cells - in the pinned format

      <op> [<30-cell bar>] <done>/<total> <pct>%[ <type> "<name>"][ - <n> failed]

    (the ' - <n> failed' suffix uses a plain ASCII dash, 0x2D). stdout is
    never touched by the bar. Contract details (src/tsdev_cli.cpp):

      * Global, position-free, idempotent boolean: bare "--pbar" or
        "--pbar=<value>" (value ignored; repeats harmless).
      * Batch runs pin the bar at the bottom of stderr from the start
        (0/total); successful items emit NO BATCH line - each failed item
        scrolls a BATCH FAILED marker plus its full human detail block
        above the bar; the bar's final frame is committed as a permanent
        line. BATCH COMPLETE is printed underneath it (last stderr line)
        only when the run was not clean (failure/cancel/not-run/close
        failure) - a fully clean run ends with the committed bar itself.
      * Batch stdout is EMPTY under --pbar: the JSON results array is
        emitted only with --json, and --pbar + --json is a usage error.
      * Single-item subcommands (exists/delete/...) emit the whole bar in
        one draw sequence after the op: 0/1 labelled, then filled to 1/1
        (with " - 1 failed" on failure), pinned by a newline.
        Mutation success stdout ("SUCCESS: ...") is suppressed under
        --pbar; query answers and failure detail are unchanged.
      * --pbar + --json is a usage error (exit 1, JSON error object on
        stdout, stderr empty). --pbar + serve is a usage error (exit 1).
      * --pbar + --batch-progress-control is permitted - no interaction.
      * Non-pbar paths emit no bar bytes (no '\r' bar frames, no block
        glyphs). Plain human batch stderr carries one BATCH SUCCESS/FAILED
        line per attempted item (failed items also print their detail
        block; there are NO BATCH START lines), BATCH COMPLETE last; batch
        stdout is empty without --json.

    The suite is SELF-SUFFICIENT: it creates two fixture Functions with
    unique per-run names (`--json insert function <name> <srcfile>`,
    expecting exit 0 / success=true / prog_id>0), runs PB1-PB10 (PB9 and
    PB10 create their own additional throwaway items inside the case),
    and deletes them in the finally cleanup (verified exists=0).
    The fixture setup is an uncounted prerequisite, not a numbered case:
    on success the summary lists exactly the 10 PB-cases (PB1-PB10); if
    fixture creation fails, a SETUP row is recorded, PB1-PB10 are SKIPPED
    (they must not run against missing items), and the suite exits 1.

    Test cases:
      PB1  --pbar exists function <probe>: exit 0; stdout keeps the human
           exists=1 / stored_name / progId lines; stderr contains '\r'
           redraw bytes, the ASCII glyph bytes, a 1/1 100% frame,
           and the probe name in the <type> "<name>" label.
      PB2  --pbar delete function <nonexistent>: exit 5; stderr bar reaches
           1/1 with the " - 1 failed" suffix; stdout is empty (the human
           ERROR line stays on stderr - the stdout contract is unchanged).
      PB3  --pbar --json exists function <probe>: exit 1 usage error;
           stdout is a JSON error object whose message mentions --pbar;
           stderr is EMPTY (no bar bytes - the flag pair is rejected
           before the bar renderer runs).
      PB4  --pbar serve: exit 1; stderr mentions "not valid with serve".
      PB5  --pbar batch-delete <manifest of 3: 1 real probe + 2
           nonexistent>: exit 2; stderr contains '\r' bar frames ending at
           3/3 with " - 2 failed", the successful item emits no
           BATCH line, each failed item scrolls a BATCH FAILED marker plus
           its detail block above the bar, and the last stderr line is
           BATCH COMPLETE (the run is not clean); stdout is EMPTY.
      PB6  Flag forms: `exists function <probe> --pbar` (position-free,
           flag after the subcommand), `--pbar=1` and `--pbar=0` (equals
           form, value ignored - presence selects), `--pbar --pbar`
           (idempotent repeat) - all exit 0 with bar bytes on stderr.
      PB7  Non-pbar guard: plain `batch-delete <same manifest>` - exit 2;
           stderr byte check: NO bare '\r' (every 0x0D belongs to a CRLF
           pair - there are no bar redraw frames), NO '#' (0x23) filled-cell
           byte, and no bar-interior opening ('[' immediately followed by
           '-' - the bare '-' byte itself cannot be asserted negatively
           because legit human text such as "ERROR --" carries it). stderr
           carries the human per-item BATCH completion
           lines (no BATCH START) ending at BATCH COMPLETE; stdout is
           EMPTY without --json.
      PB8  --pbar --batch-progress-control batch-delete <manifest> with
           "continue\n" decision lines piped on stdin: completes (exit 2
           here - the manifest items are already gone after PB5), the bar
           and the control channel coexist, stdout stays EMPTY (no --json),
           not cancelled.
      PB9  --pbar insert function <new item> <srcfile>: exit 0; stdout is
           EMPTY (the human "SUCCESS: ..." line is suppressed under
           --pbar - the post-hoc bar is the only output); stderr carries
           the '\r'-redrawn bar reaching 1/1 100% and contains no
           "SUCCESS:" text.
      PB10 --pbar batch-delete <manifest of 2, all existing> created
           inside the case: a fully clean run - exit 0; stdout is EMPTY;
           stderr is ONLY the pinned bar ('\r' frames ending at 2/2 100%,
           no " - <n> failed" suffix, no BATCH lines at all) and
           the last stderr line is the committed bar frame itself (no
           BATCH COMPLETE on a clean run).

    After EVERY TS-touching CLI invocation the suite checks for crash /
    error popup surfaces (TSCrashReport / WerFault processes and
    error-titled windows on TS-related processes); a detected surface fails
    the case loudly.

    Prerequisites:
      - Direct `tsdev_cli` requires an elevated administrator process. Run
        this procedure elevated; otherwise the CLI returns exit code 11
        (`not_elevated`).
      - No persistent probe item is required: the fixture Functions are
        created per run under unique names and deleted in cleanup.

    Capture convention:
      stdout and stderr are captured SEPARATELY via OS-level redirect files
      (Start-Process -RedirectStandardOutput/-RedirectStandardError), which
      store the child's RAW bytes - unlike PS `2>file`, which wraps stderr
      in ErrorRecords and (PS 5.1) re-encodes to UTF-16LE. The bar bytes
      are raw UTF-8 + '\r'; byte-level assertions therefore read the files
      via [System.IO.File]::ReadAllBytes, and string assertions via
      [System.IO.File]::ReadAllText (BOM auto-detect -> UTF-8 decode), NEVER
      an explicit non-BOM-aware decode. This is the same stream-separation
      rationale as test_json_output.ps1 / test_batch_stdin.ps1.

.PARAMETER CliPath
    Path to the tsdev_cli.exe executable. Defaults to
    "$PSScriptRoot\..\build\Release\tsdev_cli.exe".

.PARAMETER TsPath
    Optional. TradeStation install directory passed through to the CLI as
    "--ts-path <dir>" BEFORE the subcommand.

.PARAMETER Verbose
    Switch. When set, print full CLI output for every test.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\test_pbar.ps1
    Runs the pbar E2E tests with the default CLI path.

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
$tmpDir = Join-Path $ProjectRoot "tmp\e2e_pbar_$runId"

# Results collector.
$Results = New-Object System.Collections.Generic.List[object]
$failCount = 0

# Created-items tracker for cleanup (same convention as test_verbose_flag.ps1
# / test_upsert.ps1): each entry is a hashtable with Name and Type keys.
# Entries are tracked BEFORE their insert result is validated so the finally
# block attempts the delete even when a later case fails or the insert
# outcome could not be confirmed (delete exit 5 = item never created /
# already gone - handled gracefully). The two never-created manifest names
# are also tracked so a partial insert cannot leak them.
$CreatedItems = New-Object System.Collections.Generic.List[object]

# Fixture Functions for this run, unique per run (same run-id timestamp
# pattern as the other E2E suites) so parallel/stale runs never collide.
# Created in SETUP, deleted in CLEANUP - no persistent probe item is needed.
$ProbeName      = "PbarE2E_${runId}"      # exists target (PB1/PB6)
$BatchProbeName = "PbarE2E_${runId}_B"    # the 1 real item of the manifest (PB5)
$GhostName1     = "PbarE2E_${runId}_X1"   # nonexistent manifest item
$GhostName2     = "PbarE2E_${runId}_X2"   # nonexistent manifest item
$DeleteGhost    = "PbarE2E_${runId}_NoDel" # PB2's nonexistent delete target
$InsertName     = "PbarE2E_${runId}_I"     # PB9's --pbar insert target (created inside the case)
$CleanDelName1  = "PbarE2E_${runId}_D1"    # PB10 all-success manifest item (created inside the case)
$CleanDelName2  = "PbarE2E_${runId}_D2"    # PB10 all-success manifest item (created inside the case)

# Bar byte markers (strictly ASCII, matching kPbar*Glyph in
# src/tsdev_cli.cpp). The script file stays ASCII-only, so the glyphs are
# matched on raw bytes and as plain chars for the string assertions.
$FullGlyphBytes = [byte[]](0x23)              # '#' filled bar cell
$ShadeGlyphBytes = [byte[]](0x2D)             # '-' empty bar cell
$FailSep = "-"
$CrByte = [byte]0x0D
$LfByte = [byte]0x0A

# ---------------------------------------------------------------------------
# Write-SourceFile: writes EasyLanguage source to a temp file in ASCII
# encoding (UTF-8 BOM breaks the EasyLanguage compiler). Returns the path.
# (Same helper as test_verbose_flag.ps1 / test_upsert.ps1 / test_exists.ps1.)
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
# Write-ManifestFile: writes a line-based batch manifest to a temp file in
# ASCII (same convention as test_json_output.ps1 / test_batch_stdin.ps1).
# batch-delete lines carry only <type> <name>.
# ---------------------------------------------------------------------------
function Write-ManifestFile {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter()] [string[]]$Lines = @()
    )
    $path = Join-Path $script:tmpDir "$Name.manifest"
    $content = ($Lines -join "`r`n") + "`r`n"
    [System.IO.File]::WriteAllText($path, $content, [System.Text.Encoding]::ASCII)
    return $path
}

# ---------------------------------------------------------------------------
# Get-TsFailureSurfaces: crash / error popup check run after EVERY
# TS-touching CLI invocation (AGENTS rule). Returns a list of detected
# surfaces:
#   * TSCrashReport / WerFault reporter processes (recorded with their main
#     window title text, then killed - a crash report means the action
#     provoked a crash, never a success).
#   * visible windows with error-ish titles on TS-related processes
#     (tsdev_cli / TSDev / orchart / TSCrashReport / WerFault) - the title
#     text is recorded and the main window close is requested; the process
#     itself is not killed (except the crash reporters above).
# An empty return value means no failure surface was detected.
# ---------------------------------------------------------------------------
function Get-TsFailureSurfaces {
    $surfaces = New-Object System.Collections.Generic.List[string]

    # Crash reporters can take a moment to spawn after the child exits.
    Start-Sleep -Milliseconds 300

    $crashProcs = @(Get-Process -Name 'TSCrashReport', 'WerFault' -ErrorAction SilentlyContinue)
    foreach ($cp in $crashProcs) {
        $title = ''
        try { $title = $cp.MainWindowTitle } catch {}
        if ($title) {
            $surfaces.Add("crash reporter $($cp.ProcessName)#$($cp.Id) running, window title: '$title'") | Out-Null
        } else {
            $surfaces.Add("crash reporter $($cp.ProcessName)#$($cp.Id) running") | Out-Null
        }
    }

    # Error-titled windows on TS-related processes (a dialog provoked by the
    # action is a failure surface). Title-only check - no message pump, no
    # cross-process child-text scraping; the title is enough to fail loudly.
    $tsProcs = @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
        $_.ProcessName -match '^(tsdev_cli|TSDev|orchart|TSCrashReport|WerFault)$' -and
        $_.MainWindowTitle -match 'error|warning|crash|exception'
    })
    foreach ($p in $tsProcs) {
        $surfaces.Add("error-titled window on $($p.ProcessName)#$($p.Id): '$($p.MainWindowTitle)'") | Out-Null
        try { [void]$p.CloseMainWindow() } catch {}
    }

    # Dismiss crash reporters AFTER the surface text is recorded (AGENTS:
    # read, dismiss, correct course - never kill silently).
    foreach ($cp in $crashProcs) {
        try { Stop-Process -Id $cp.Id -Force -ErrorAction SilentlyContinue } catch {}
    }

    return $surfaces
}

# ---------------------------------------------------------------------------
# Invoke-Cli: runs tsdev_cli.exe with the given argument array, capturing
# stdout and stderr SEPARATELY to OS-level redirect files so the files hold
# the child's RAW bytes (the bar is raw UTF-8 + '\r' - PS `2>file` would
# wrap it in ErrorRecords and, under PS 5.1, re-encode it as UTF-16LE).
# Returns a hashtable with Stdout (string), Stderr (string), StdoutBytes
# (byte[]), StderrBytes (byte[]), ExitCode (int), Popup (string - empty when
# no crash/error popup surface was detected), and TimedOut (bool).
#
# -StdinFile redirects that file's raw bytes as the child's stdin (the
# --batch-progress-control decision channel; PowerShell's | pipe would
# mangle encoding and translate newlines, so a file redirect is used).
#
# The wait has a finite timeout: a wedged control-channel read (or any
# other hang) must never hang the suite - on timeout the popup surface is
# inspected first, then the child is killed and reaped.
# ---------------------------------------------------------------------------
function Invoke-Cli {
    param(
        [Parameter(Mandatory)] [string[]]$Arguments,
        [string]$StdinFile,
        [int]$TimeoutSeconds = 240
    )
    $fullArgs = @()
    if ($script:TsPath) {
        $fullArgs += @("--ts-path", $script:TsPath)
    }
    $fullArgs += $Arguments

    # Build a single quoted argument string for Start-Process (same
    # convention as test_json_output.ps1 / test_batch_stdin.ps1).
    $argParts = @()
    foreach ($a in $fullArgs) {
        if ($a -match '\s') {
            $argParts += "`"$a`""
        } else {
            $argParts += $a
        }
    }
    $argString = ($argParts -join " ")

    $stdoutFile = Join-Path $script:tmpDir "stdout_$([System.Guid]::NewGuid().ToString('N')).txt"
    $stderrFile = Join-Path $script:tmpDir "stderr_$([System.Guid]::NewGuid().ToString('N')).txt"

    $exitCode = -2
    $launchError = ''
    $timedOut = $false
    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $startParams = @{
            FilePath               = $script:CliPath
            ArgumentList           = $argString
            RedirectStandardOutput = $stdoutFile
            RedirectStandardError  = $stderrFile
            NoNewWindow            = $true
            PassThru               = $true
        }
        if ($StdinFile) {
            $startParams['RedirectStandardInput'] = $StdinFile
        }
        $proc = Start-Process @startParams
        if ($null -eq $proc) { throw 'failed to launch tsdev_cli' }
        # Cache the process handle before exit so PS 5.1 can report ExitCode.
        $null = $proc.Handle
        [void]$proc.WaitForExit($TimeoutSeconds * 1000)
        if (-not $proc.HasExited) {
            $timedOut = $true
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
            [void]$proc.WaitForExit(5000)
        }
        $exitCode = if ($timedOut) { -3 } else { $proc.ExitCode }
    }
    catch {
        # Launch/wait failure only: record it for stderr; never fabricate
        # stdout or overwrite a real exit code.
        $launchError = $_.Exception.Message
    }
    finally {
        $ErrorActionPreference = $prevEAP
    }

    # Raw byte captures (byte-level bar assertions read these).
    $stdoutBytes = [byte[]]@()
    $stderrBytes = [byte[]]@()
    if (Test-Path -Path $stdoutFile -PathType Leaf) {
        $stdoutBytes = [System.IO.File]::ReadAllBytes($stdoutFile)
    }
    if (Test-Path -Path $stderrFile -PathType Leaf) {
        $stderrBytes = [System.IO.File]::ReadAllBytes($stderrFile)
    }

    # String views via BOM-aware reads (auto-detect): the raw byte files
    # have no BOM, so they decode as UTF-8 - the bar's ASCII '#'/'-' glyph
    # bytes arrive as real characters.
    $stdoutStr = ''
    $stderrStr = ''
    if (Test-Path -Path $stdoutFile -PathType Leaf) {
        $stdoutStr = ([System.IO.File]::ReadAllText($stdoutFile)).TrimEnd()
    }
    if (Test-Path -Path $stderrFile -PathType Leaf) {
        $stderrStr = ([System.IO.File]::ReadAllText($stderrFile)).TrimEnd()
    }
    if ($launchError) {
        if ($stderrStr) { $stderrStr += "`n" }
        $stderrStr += $launchError
    }

    foreach ($f in @($stdoutFile, $stderrFile)) {
        if (Test-Path -Path $f -PathType Leaf) {
            Remove-Item -Path $f -Force -ErrorAction SilentlyContinue
        }
    }

    # Crash / error popup check after EVERY TS-touching invocation (AGENTS).
    # Any detected surface is recorded on the capture; the test case asserts
    # it empty so a provoked popup fails the case loudly.
    $popupSurfaces = @(Get-TsFailureSurfaces)
    $popup = ($popupSurfaces -join '; ')

    $capture = @{
        Stdout      = $stdoutStr
        Stderr      = $stderrStr
        StdoutBytes = $stdoutBytes
        StderrBytes = $stderrBytes
        ExitCode    = $exitCode
        TimedOut    = $timedOut
        Popup       = $popup
    }
    # Remember the most recent capture for the -Verbose output preview.
    $script:lastCapture = $capture
    return $capture
}

# ---------------------------------------------------------------------------
# Test-BytesContain: returns $true when the byte array contains the needle
# subsequence.
# ---------------------------------------------------------------------------
function Test-BytesContain {
    param(
        [Parameter(Mandatory)] [byte[]]$Haystack,
        [Parameter(Mandatory)] [byte[]]$Needle
    )
    if ($Needle.Length -eq 0) { return $true }
    if ($Haystack.Length -lt $Needle.Length) { return $false }
    for ($i = 0; $i -le $Haystack.Length - $Needle.Length; $i++) {
        $match = $true
        for ($j = 0; $j -lt $Needle.Length; $j++) {
            if ($Haystack[$i + $j] -ne $Needle[$j]) { $match = $false; break }
        }
        if ($match) { return $true }
    }
    return $false
}

# ---------------------------------------------------------------------------
# Test-HasBareCR: returns $true when the byte array contains a CR byte that
# is NOT part of a CRLF pair (i.e. a '\r'-redrawn bar frame or erase
# sequence). Normal event lines end CRLF in text mode, so the bar's pinned
# redraws are what a bare CR detects.
# ---------------------------------------------------------------------------
function Test-HasBareCR {
    param([Parameter(Mandatory)] [byte[]]$Bytes)
    for ($i = 0; $i -lt $Bytes.Length; $i++) {
        if ($Bytes[$i] -eq 0x0D) {
            if ($i + 1 -ge $Bytes.Length -or $Bytes[$i + 1] -ne 0x0A) {
                return $true
            }
        }
    }
    return $false
}

# ---------------------------------------------------------------------------
# Assert-True: records a single boolean assertion. Throws on failure (caught
# by the enclosing test case to record a FAIL).
# ---------------------------------------------------------------------------
function Assert-True {
    param(
        [Parameter(Mandatory)] [bool]$Condition,
        # AllowEmptyString: a PASSING assertion legitimately carries no detail
        # (e.g. the popup field is empty when clean); Mandatory alone would
        # reject the bind before the body runs.
        [Parameter(Mandatory)] [AllowEmptyString()] [string]$Message
    )
    if (-not $Condition) {
        throw $Message
    }
}

# ---------------------------------------------------------------------------
# Assert-NoPopup: fails the case when the capture's popup surface is
# non-empty (crash reporter or error-titled window detected after the CLI
# invocation).
# ---------------------------------------------------------------------------
function Assert-NoPopup {
    param([Parameter(Mandatory)] $Capture)
    Assert-True ($Capture.Popup -eq '') "crash/error popup surface detected after CLI invocation: $($Capture.Popup)"
}

# ---------------------------------------------------------------------------
# Assert-BarBytes: shared bar evidence on a capture's raw stderr bytes:
# a bare '\r' redraw byte plus at least one ASCII bar-glyph byte.
# ---------------------------------------------------------------------------
function Assert-BarBytes {
    param(
        [Parameter(Mandatory)] $Capture,
        [Parameter(Mandatory)] [AllowEmptyString()] [string]$Context
    )
    Assert-True (Test-HasBareCR -Bytes $Capture.StderrBytes) `
        "$Context stderr must contain a bare '\r' bar-redraw byte; stderr bytes length=$($Capture.StderrBytes.Length)"
    $hasGlyph = (Test-BytesContain -Haystack $Capture.StderrBytes -Needle $script:FullGlyphBytes) -or
                (Test-BytesContain -Haystack $Capture.StderrBytes -Needle $script:ShadeGlyphBytes)
    Assert-True $hasGlyph "$Context stderr must contain an ASCII bar-glyph byte (0x23 '#' or 0x2D '-')"
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
    # -Verbose convention (same as test_verbose_flag.ps1): also print a short
    # CLI output preview for PASSING cases; failures always print their detail.
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
    Write-Host "==================== PBAR E2E SUMMARY ====================" -ForegroundColor Cyan
    $header = "{0,-8} {1,-46} {2,-8}" -f "Test#", "Step", "Result"
    Write-Host $header
    Write-Host ("{0,-8} {1,-46} {2,-8}" -f ("-" * 8), ("-" * 46), ("-" * 8))

    $passed = 0
    $failed = 0
    foreach ($r in $Results) {
        $line = "{0,-8} {1,-46} {2,-8}" -f $r.TestId, $r.Step, $r.Result
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
Write-Host "TSDev Automation - --pbar flag E2E Test" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "Run ID     : $runId"
Write-Host "Project root: $ProjectRoot"
Write-Host "CLI path   : $CliPath"
Write-Host "TS path    : $(if ($TsPath) { $TsPath } else { 'auto-discovered' })"
Write-Host "Verbose    : $Verbose"
Write-Host "Temp dir   : $tmpDir"
Write-Host "Fixtures   : $ProbeName, $BatchProbeName (created per run; deleted in cleanup)"
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
    # SETUP (uncounted prerequisite for PB1-PB10): create the two fixture
    # Functions.
    #
    # Not a numbered PB-case: the fixtures are scaffolding, not the behavior
    # under test. On success the summary lists exactly the 10 PB-cases
    # (PB1-PB10); if fixture creation fails, a SETUP row is recorded below,
    # PB1-PB10 are SKIPPED (they must not run against missing items), and the
    # suite exits 1 via the summary count.
    # =====================================================================
    Write-Host "--- SETUP: insert fixture Functions ---" -ForegroundColor Cyan
    $fixtureReady = $false
    $setupDetail = ""
    try {
        # Valid minimal EasyLanguage Function source ("<Name> = 1;"), ASCII.
        $probeSrcFile = Write-SourceFile "pbar_probe_src" "$ProbeName = 1;"
        $batchSrcFile = Write-SourceFile "pbar_batch_src" "$BatchProbeName = 1;"

        # Track the fixtures BEFORE validating the insert results: the finally
        # block then attempts the deletes even if success cannot be confirmed
        # (delete exit 5 = never created / already gone - handled gracefully).
        # The never-created manifest ghost names are tracked too so a partial
        # batch can never leak them. The names created inside PB9/PB10 are
        # tracked up-front for the same reason (a partial insert cannot leak).
        $CreatedItems.Add(@{ Name = $ProbeName;      Type = "function" }) | Out-Null
        $CreatedItems.Add(@{ Name = $BatchProbeName; Type = "function" }) | Out-Null
        $CreatedItems.Add(@{ Name = $GhostName1;     Type = "function" }) | Out-Null
        $CreatedItems.Add(@{ Name = $GhostName2;     Type = "function" }) | Out-Null
        $CreatedItems.Add(@{ Name = $DeleteGhost;    Type = "function" }) | Out-Null
        $CreatedItems.Add(@{ Name = $InsertName;     Type = "function" }) | Out-Null
        $CreatedItems.Add(@{ Name = $CleanDelName1;  Type = "function" }) | Out-Null
        $CreatedItems.Add(@{ Name = $CleanDelName2;  Type = "function" }) | Out-Null

        $okCount = 0
        foreach ($fx in @(@{ Name = $ProbeName; File = $probeSrcFile },
                          @{ Name = $BatchProbeName; File = $batchSrcFile })) {
            $setup = Invoke-Cli -Arguments @("--json", "insert", "function", $fx.Name, $fx.File)
            if ($setup.Popup -ne '') {
                $setupDetail = "insert $($fx.Name): crash/error popup detected: $($setup.Popup)"
                break
            }
            if ($setup.ExitCode -ne 0) {
                $setupDetail = "insert $($fx.Name) exit code: expected 0, got $($setup.ExitCode); stderr: $($setup.Stderr)"
                break
            }
            try {
                $jSetup = $setup.Stdout | ConvertFrom-Json -ErrorAction Stop
                if (-not $jSetup.success) {
                    $setupDetail = "insert $($fx.Name) JSON success=false (expected true); stdout: $($setup.Stdout)"
                    break
                }
                elseif (-not $jSetup.prog_id -or $jSetup.prog_id -le 0) {
                    $setupDetail = "insert $($fx.Name) prog_id missing or <= 0; stdout: $($setup.Stdout)"
                    break
                }
                else {
                    $okCount++
                    Write-Host "SETUP: fixture Function '$($fx.Name)' created (prog_id=$($jSetup.prog_id))" -ForegroundColor Green
                }
            }
            catch {
                $setupDetail = "insert $($fx.Name) output is not valid JSON: $($_.Exception.Message); stdout: $($setup.Stdout)"
                break
            }
        }
        if ($okCount -eq 2) { $fixtureReady = $true }
    }
    catch {
        $setupDetail = "fixture setup error: $($_.Exception.Message)"
    }

    if (-not $fixtureReady) {
        Write-Host "ERROR: fixture Functions could not be created - PB1-PB10 NOT run." -ForegroundColor Red
        Write-Host "       $setupDetail" -ForegroundColor Red
        $Results.Add([pscustomobject]@{
            TestId   = 'SETUP'
            Step     = 'insert fixture functions'
            Result   = 'FAIL'
            Detail   = $setupDetail
            Duration = [timespan]::Zero
        }) | Out-Null
    }
    else {

    # The batch manifest (1 real probe + 2 nonexistent) is shared by PB5,
    # PB7 and PB8. batch-delete lines carry only <type> <name>.
    $manifestFile = Write-ManifestFile "pbar_manifest" @(
        "function $BatchProbeName",
        "function $GhostName1",
        "function $GhostName2"
    )

    # =====================================================================
    # PB1: --pbar exists function <probe> - exit 0; stdout keeps the human
    # exists lines; stderr carries the '\r'-redrawn bar reaching 1/1 100%
    # with the probe name in the label.
    # =====================================================================
    Write-Host "--- PB1: --pbar exists function <probe> ---" -ForegroundColor Cyan
    Run-TestCase -TestId "PB1" -Step "pbar exists: exit 0 + bar to 1/1 100%" -Body {
        $cli = Invoke-Cli -Arguments @("--pbar", "exists", "function", $script:ProbeName)
        Assert-NoPopup $cli
        Assert-True ($cli.ExitCode -eq 0) "exit code: expected 0, got $($cli.ExitCode); stderr: $($cli.Stderr)"
        # stdout keeps the human exists contract (the bar is stderr-only).
        Assert-True ($cli.Stdout -match 'exists=1') "stdout must contain 'exists=1'; got: $($cli.Stdout)"
        Assert-True ($cli.Stdout.Contains("stored_name=$($script:ProbeName)")) `
            "stdout must contain 'stored_name=$($script:ProbeName)'; got: $($cli.Stdout)"
        Assert-True ($cli.Stdout -match 'progId=[1-9]\d*') "stdout must contain a nonzero progId; got: $($cli.Stdout)"
        Assert-BarBytes $cli "PB1"
        Assert-True ($cli.Stderr.Contains("1/1")) "stderr bar must reach '1/1'; got: $($cli.Stderr)"
        Assert-True ($cli.Stderr.Contains("100%")) "stderr bar must reach '100%'; got: $($cli.Stderr)"
        # The label is <type> "<name>"; the line is capped at 79 cells so a
        # long name is truncated with '...' - assert the label form plus a
        # run-unique prefix of the probe name (the first 15 chars always
        # survive the truncation).
        $namePrefix = $script:ProbeName.Substring(0, [Math]::Min(15, $script:ProbeName.Length))
        Assert-True ($cli.Stderr.Contains("function `"$namePrefix")) `
            "stderr bar label must contain 'function `"$namePrefix' (truncated probe name); got: $($cli.Stderr)"
    }

    # =====================================================================
    # PB2: --pbar delete function <nonexistent> - exit 5; the bar still
    # reaches 1/1 and gains the " - 1 failed" suffix; stdout stays empty
    # (the human ERROR line is on stderr - the stdout contract is unchanged).
    # =====================================================================
    Write-Host "--- PB2: --pbar delete function <nonexistent> ---" -ForegroundColor Cyan
    Run-TestCase -TestId "PB2" -Step "pbar delete not-found: exit 5 + ' - 1 failed'" -Body {
        $cli = Invoke-Cli -Arguments @("--pbar", "delete", "function", $script:DeleteGhost)
        Assert-NoPopup $cli
        Assert-True ($cli.ExitCode -eq 5) "exit code: expected 5, got $($cli.ExitCode); stderr: $($cli.Stderr)"
        Assert-True ($cli.Stdout.Trim() -eq '') "stdout must be empty (human ERROR stays on stderr); got: $($cli.Stdout)"
        Assert-BarBytes $cli "PB2"
        Assert-True ($cli.Stderr.Contains("1/1")) "stderr bar must reach '1/1'; got: $($cli.Stderr)"
        Assert-True ($cli.Stderr.Contains("$($script:FailSep) 1 failed")) `
            "stderr bar must carry the ' - 1 failed' suffix; got: $($cli.Stderr)"
        Assert-True ($cli.Stderr.Contains("ERROR")) "stderr must still carry the human ERROR line; got: $($cli.Stderr)"
    }

    # =====================================================================
    # PB3: --pbar --json exists function <probe> - exit 1 usage error; a
    # JSON error object on stdout mentions --pbar; stderr is EMPTY (the
    # flag conflict is rejected before the bar renderer runs).
    # =====================================================================
    Write-Host "--- PB3: --pbar --json is a usage error ---" -ForegroundColor Cyan
    Run-TestCase -TestId "PB3" -Step "pbar+json: exit 1, JSON error, stderr empty" -Body {
        $cli = Invoke-Cli -Arguments @("--pbar", "--json", "exists", "function", $script:ProbeName)
        Assert-NoPopup $cli
        Assert-True ($cli.ExitCode -eq 1) "exit code: expected 1, got $($cli.ExitCode); stdout: $($cli.Stdout); stderr: $($cli.Stderr)"
        $obj = $null
        try { $obj = $cli.Stdout | ConvertFrom-Json -ErrorAction Stop } catch {}
        Assert-True ($null -ne $obj) "stdout must be a JSON error object; got: $($cli.Stdout)"
        Assert-True ($obj.success -eq $false) "stdout JSON success must be false; got: $($cli.Stdout)"
        Assert-True ($null -ne $obj.error -and $obj.error.message -match '--pbar') `
            "stdout JSON error.message must mention --pbar; got: $($cli.Stdout)"
        Assert-True ($cli.StderrBytes.Length -eq 0) `
            "stderr must be EMPTY (no bar bytes) for --pbar --json; got $($cli.StderrBytes.Length) bytes: $($cli.Stderr)"
    }

    # =====================================================================
    # PB4: --pbar serve - exit 1 usage error; stderr mentions the serve
    # restriction.
    # =====================================================================
    Write-Host "--- PB4: --pbar serve is a usage error ---" -ForegroundColor Cyan
    Run-TestCase -TestId "PB4" -Step "pbar+serve: exit 1, stderr mentions serve" -Body {
        $cli = Invoke-Cli -Arguments @("--pbar", "serve")
        Assert-NoPopup $cli
        Assert-True ($cli.ExitCode -eq 1) "exit code: expected 1, got $($cli.ExitCode); stderr: $($cli.Stderr)"
        Assert-True ($cli.Stderr -match 'not valid with serve') `
            "stderr must mention 'not valid with serve'; got: $($cli.Stderr)"
        # Usage rejection happens before any bar renderer runs.
        Assert-True (-not (Test-HasBareCR -Bytes $cli.StderrBytes)) `
            "stderr must contain no bar-redraw bytes for --pbar serve; got: $($cli.Stderr)"
    }

    # =====================================================================
    # PB5: --pbar batch-delete <manifest of 3: 1 real + 2 nonexistent> -
    # exit 2; '\r' bar frames end at 3/3 with " - 2 failed"; under
    # --pbar the successful item emits no BATCH line while each failed item
    # scrolls a BATCH FAILED marker + its full detail block above the bar;
    # the last stderr line is BATCH COMPLETE (the run is not clean); stdout
    # is EMPTY (the results array is emitted only with --json, which --pbar
    # rejects as a usage error).
    # =====================================================================
    Write-Host "--- PB5: --pbar batch-delete <manifest of 3> ---" -ForegroundColor Cyan
    Run-TestCase -TestId "PB5" -Step "pbar batch-delete: bar to 3/3 + FAILED lines + COMPLETE last" -Body {
        $cli = Invoke-Cli -Arguments @("--pbar", "batch-delete", $script:manifestFile)
        Assert-NoPopup $cli
        Assert-True ($cli.ExitCode -eq 2) "exit code: expected 2, got $($cli.ExitCode); stderr: $($cli.Stderr)"

        # stdout: EMPTY without --json. The batch results array is emitted
        # only with --json, and --pbar + --json is a usage error - no array
        # can appear here.
        Assert-True ($cli.Stdout.Trim() -eq '') "stdout must be EMPTY without --json; got: $($cli.Stdout)"

        # stderr: '\r'-redrawn bar frames ending at 3/3 with the failure
        # suffix. Under --pbar the one successful item emits NO BATCH line;
        # each failed item scrolls a BATCH FAILED marker plus its full
        # human detail block above the bar.
        Assert-BarBytes $cli "PB5"
        # "] 3/3" pins the BAR frame itself: the bar's ']' immediately
        # precedes the count, whereas a bare "3/3" would also match a
        # "BATCH FAILED [3/3]" event line and pass without a full bar.
        Assert-True ($cli.Stderr.Contains("] 3/3")) "stderr bar frame must reach '] 3/3'; got: $($cli.Stderr)"
        Assert-True ($cli.Stderr.Contains("$($script:FailSep) 2 failed")) `
            "stderr bar must carry the ' - 2 failed' suffix; got: $($cli.Stderr)"
        Assert-True (-not $cli.Stderr.Contains("BATCH START")) `
            "stderr must contain no BATCH START lines (removed from human batch output); got: $($cli.Stderr)"
        Assert-True (-not $cli.Stderr.Contains("BATCH SUCCESS")) `
            "stderr must contain no BATCH SUCCESS line (suppressed under --pbar); got: $($cli.Stderr)"
        $failMarkers = @($cli.Stderr -split "`n" | Where-Object { $_.Contains('BATCH FAILED') })
        Assert-True ($failMarkers.Count -eq 2) `
            "stderr must contain exactly 2 BATCH FAILED markers (one per failed item); got: $($cli.Stderr)"
        Assert-True ($failMarkers[0].Contains("[2/3]") -and $failMarkers[0].Contains("name=`"$($script:GhostName1)`"") -and $failMarkers[0].Contains("exit_code=5")) `
            "first BATCH FAILED must be [2/3] $GhostName1 exit_code=5; got: $($failMarkers[0])"
        Assert-True ($failMarkers[1].Contains("[3/3]") -and $failMarkers[1].Contains("name=`"$($script:GhostName2)`"") -and $failMarkers[1].Contains("exit_code=5")) `
            "second BATCH FAILED must be [3/3] $GhostName2 exit_code=5; got: $($failMarkers[1])"
        # Each failed item's full human detail block scrolls above the bar
        # too: 'tsdev_cli: ERROR -- function "X" not found in the
        # warehouse.'
        $detail1 = "tsdev_cli: ERROR -- function `"$($script:GhostName1)`" not found in the warehouse."
        $detail2 = "tsdev_cli: ERROR -- function `"$($script:GhostName2)`" not found in the warehouse."
        Assert-True ($cli.Stderr.Contains($detail1)) "stderr must contain GhostName1's failure detail block; got: $($cli.Stderr)"
        Assert-True ($cli.Stderr.Contains($detail2)) "stderr must contain GhostName2's failure detail block; got: $($cli.Stderr)"
        # The bar's final frame is committed as a permanent line; the run
        # was not clean (2 failures), so the summary prints underneath it:
        # the last stderr line is BATCH COMPLETE.
        $lines = @($cli.Stderr -split "`n" | Where-Object { $_.Trim() -ne '' })
        $lastLine = $lines[$lines.Count - 1]
        Assert-True ($lastLine.StartsWith("BATCH COMPLETE")) `
            "last stderr line must be BATCH COMPLETE; got: $lastLine"
        Assert-True ($lastLine.Contains("completed=3") -and $lastLine.Contains("failures=2") -and $lastLine.Contains("exit_code=2")) `
            "BATCH COMPLETE must show completed=3 failures=2 exit_code=2; got: $lastLine"
    }

    # =====================================================================
    # PB6: flag forms - position-free trailing, --pbar=<value>, and an
    # idempotent repeat. All exit 0 with bar bytes on stderr.
    # =====================================================================
    Write-Host "--- PB6: --pbar flag forms ---" -ForegroundColor Cyan
    Run-TestCase -TestId "PB6" -Step "position-free, --pbar=1/=0, --pbar --pbar all draw" -Body {
        # Position-free: flag AFTER the subcommand args behaves like the
        # leading form (same global pre-scan).
        $cli = Invoke-Cli -Arguments @("exists", "function", $script:ProbeName, "--pbar")
        Assert-NoPopup $cli
        Assert-True ($cli.ExitCode -eq 0) "trailing --pbar: expected exit 0, got $($cli.ExitCode); stderr: $($cli.Stderr)"
        Assert-BarBytes $cli "trailing --pbar"

        # Equals form: any value after '=' is ignored - presence selects.
        $cli = Invoke-Cli -Arguments @("--pbar=1", "exists", "function", $script:ProbeName)
        Assert-NoPopup $cli
        Assert-True ($cli.ExitCode -eq 0) "--pbar=1: expected exit 0, got $($cli.ExitCode); stderr: $($cli.Stderr)"
        Assert-BarBytes $cli "--pbar=1"

        # Equals form with a falsy-looking value: presence == true, so the
        # bar must still render despite "=0".
        $cli = Invoke-Cli -Arguments @("--pbar=0", "exists", "function", $script:ProbeName)
        Assert-NoPopup $cli
        Assert-True ($cli.ExitCode -eq 0) "--pbar=0: expected exit 0 (value ignored), got $($cli.ExitCode); stderr: $($cli.Stderr)"
        Assert-BarBytes $cli "--pbar=0"

        # Idempotent repeat: a second --pbar keeps the flag true.
        $cli = Invoke-Cli -Arguments @("--pbar", "--pbar", "exists", "function", $script:ProbeName)
        Assert-NoPopup $cli
        Assert-True ($cli.ExitCode -eq 0) "--pbar --pbar: expected exit 0, got $($cli.ExitCode); stderr: $($cli.Stderr)"
        Assert-BarBytes $cli "--pbar --pbar"
    }

    # =====================================================================
    # PB7: non-pbar guard - plain `batch-delete <same manifest>` must emit
    # NO bar bytes: no bare '\r' (every CR belongs to a CRLF line ending)
    # and no block-glyph UTF-8 sequences. Under the new human batch
    # contract stdout is EMPTY (no results array without --json) and stderr
    # carries one BATCH completion line per attempted item (failures also
    # print their detail block) - no BATCH START lines - ending at BATCH
    # COMPLETE.
    # =====================================================================
    Write-Host "--- PB7: non-pbar batch-delete has no bar bytes ---" -ForegroundColor Cyan
    Run-TestCase -TestId "PB7" -Step "plain batch-delete: no bare CR, no glyphs" -Body {
        $cli = Invoke-Cli -Arguments @("batch-delete", $script:manifestFile)
        Assert-NoPopup $cli
        Assert-True ($cli.ExitCode -eq 2) "exit code: expected 2, got $($cli.ExitCode); stderr: $($cli.Stderr)"
        # stdout: EMPTY without --json (the results array is emitted only
        # with --json).
        Assert-True ($cli.Stdout.Trim() -eq '') "stdout must be EMPTY without --json; got: $($cli.Stdout)"
        Assert-True (-not (Test-HasBareCR -Bytes $cli.StderrBytes)) `
            "non-pbar stderr must contain NO bare '\r' bar-redraw bytes (only CRLF line endings); stderr bytes length=$($cli.StderrBytes.Length)"
        Assert-True (-not (Test-BytesContain -Haystack $cli.StderrBytes -Needle $script:FullGlyphBytes)) `
            "non-pbar stderr must contain NO '#' (0x23) filled-cell byte"
        # The bare '-' byte cannot be asserted negatively (legit human text
        # such as "tsdev_cli: ERROR --" carries it); the bar's shade interior
        # is pinned instead by its opening: '[' immediately followed by '-'.
        Assert-True (-not (Test-BytesContain -Haystack $cli.StderrBytes -Needle ([byte[]](0x5B, 0x2D)))) `
            "non-pbar stderr must contain NO bar-interior opening ('[' + '-' bytes)"
        # The human per-item completion lines are still emitted - one per
        # attempted item (the manifest items are already gone after PB5, so
        # they are BATCH FAILED markers followed by their detail blocks) -
        # with NO BATCH START lines, and BATCH COMPLETE still last.
        Assert-True ($cli.Stderr.Contains("BATCH FAILED")) "stderr must contain per-item BATCH FAILED lines; got: $($cli.Stderr)"
        Assert-True ($cli.Stderr.Contains("not found in the warehouse")) `
            "stderr must contain the failed items' detail blocks; got: $($cli.Stderr)"
        Assert-True (-not $cli.Stderr.Contains("BATCH START")) `
            "stderr must contain no BATCH START lines (removed from human batch output); got: $($cli.Stderr)"
        $lines = @($cli.Stderr -split "`n" | Where-Object { $_.Trim() -ne '' })
        Assert-True ($lines[$lines.Count - 1].StartsWith("BATCH COMPLETE")) `
            "last stderr line must be BATCH COMPLETE; got: $($lines[$lines.Count - 1])"
    }

    # =====================================================================
    # PB8: --pbar --batch-progress-control batch-delete <manifest> with
    # "continue\n" decision lines piped on stdin - the bar and the control
    # channel coexist (no interaction); the run completes all items
    # (cancelled=false) and stdout stays EMPTY (the results array is
    # emitted only with --json, which --pbar rejects).
    #
    # The decision stream carries ONLY LF-terminated "continue" lines: the
    # control channel reads exactly one "continue\n"/"cancel\n" token per
    # non-final item - a CRLF would leave '\r' in the token and be treated
    # as malformed (a cancel). Two items need decisions for a 3-item
    # manifest; a spare third line is harmless (never read).
    # =====================================================================
    Write-Host "--- PB8: --pbar + --batch-progress-control coexist ---" -ForegroundColor Cyan
    Run-TestCase -TestId "PB8" -Step "pbar+control: continues complete the batch" -Body {
        $decisionsFile = Join-Path $script:tmpDir "pbar_decisions.txt"
        [System.IO.File]::WriteAllText($decisionsFile, "continue`ncontinue`ncontinue`n", [System.Text.Encoding]::ASCII)

        $cli = Invoke-Cli -Arguments @("--pbar", "--batch-progress-control", "batch-delete", $script:manifestFile) `
                          -StdinFile $decisionsFile
        Assert-NoPopup $cli
        Assert-True (-not $cli.TimedOut) "control batch must not hit the wait timeout (wedged control read)"
        # The manifest items are already gone after PB5, so every delete is
        # a not-found failure -> the batch ran to completion with failures.
        Assert-True ($cli.ExitCode -eq 2 -or $cli.ExitCode -eq 0) `
            "exit code: expected 2 (or 0) for a completed batch, got $($cli.ExitCode); stderr: $($cli.Stderr)"

        # stdout: EMPTY without --json (the results array is emitted only
        # with --json, and --pbar + --json is a usage error).
        Assert-True ($cli.Stdout.Trim() -eq '') "stdout must be EMPTY without --json; got: $($cli.Stdout)"

        Assert-BarBytes $cli "PB8"
        Assert-True ($cli.Stderr.Contains("] 3/3")) "stderr bar frame must reach '] 3/3'; got: $($cli.Stderr)"
        # All items fail not-found, so BATCH FAILED markers scroll above
        # the bar and the non-clean run ends with BATCH COMPLETE.
        Assert-True ($cli.Stderr.Contains("BATCH FAILED")) `
            "stderr must contain BATCH FAILED markers above the bar; got: $($cli.Stderr)"
        $lines = @($cli.Stderr -split "`n" | Where-Object { $_.Trim() -ne '' })
        $lastLine = $lines[$lines.Count - 1]
        Assert-True ($lastLine.StartsWith("BATCH COMPLETE")) `
            "last stderr line must be BATCH COMPLETE; got: $lastLine"
        Assert-True ($lastLine.Contains("cancelled=false")) `
            "BATCH COMPLETE must show cancelled=false (all 'continue' decisions honored); got: $lastLine"
    }

    # =====================================================================
    # PB9: --pbar insert function <new item> <srcfile> - exit 0; the human
    # "SUCCESS: ..." stdout line is suppressed under --pbar, so stdout is
    # EMPTY (not merely lacking the line); the post-hoc 0->1 bar on stderr
    # is the only output - '\r'-redrawn frames reaching 1/1 100%, with no
    # "SUCCESS:" text anywhere on stderr.
    # =====================================================================
    Write-Host "--- PB9: --pbar insert suppresses the SUCCESS line ---" -ForegroundColor Cyan
    Run-TestCase -TestId "PB9" -Step "pbar insert: exit 0, stdout EMPTY, bar to 1/1 100%" -Body {
        $insertSrc = Write-SourceFile "pbar_insert_src" "$($script:InsertName) = 1;"
        $cli = Invoke-Cli -Arguments @("--pbar", "insert", "function", $script:InsertName, $insertSrc)
        Assert-NoPopup $cli
        Assert-True ($cli.ExitCode -eq 0) "exit code: expected 0, got $($cli.ExitCode); stderr: $($cli.Stderr)"
        # The key assertion: mutation success stdout ("SUCCESS: ...") is
        # suppressed under --pbar - stdout must be EMPTY.
        Assert-True ($cli.Stdout.Trim() -eq '') `
            "stdout must be EMPTY (SUCCESS line suppressed under --pbar); got: $($cli.Stdout)"
        Assert-BarBytes $cli "PB9"
        Assert-True ($cli.Stderr.Contains("1/1")) "stderr bar must reach '1/1'; got: $($cli.Stderr)"
        Assert-True ($cli.Stderr.Contains("100%")) "stderr bar must reach '100%'; got: $($cli.Stderr)"
        Assert-True (-not $cli.Stderr.Contains("SUCCESS:")) `
            "stderr must contain no 'SUCCESS:' text (the line is suppressed, not redirected); got: $($cli.Stderr)"
    }

    # =====================================================================
    # PB10: --pbar batch-delete <manifest of 2, all existing> - a fully
    # clean run: exit 0; stdout is EMPTY (no results array without --json);
    # stderr is ONLY the pinned bar - '\r' frames ending at ] 2/2 100%,
    # no " - <n> failed" suffix, and no BATCH lines at all (no
    # START/SUCCESS/FAILED/COMPLETE). A fully clean run ends with the
    # committed bar frame as the last stderr line - BATCH COMPLETE prints
    # only when the run was not clean.
    #
    # The two real manifest items are throwaway fixtures created inside
    # this case via --json insert (validated success/prog_id, same as
    # SETUP) and removed by the batch-delete under test; their names are
    # tracked in $CreatedItems from SETUP so a partial insert cannot leak.
    # =====================================================================
    Write-Host "--- PB10: --pbar batch-delete all-success manifest ---" -ForegroundColor Cyan
    Run-TestCase -TestId "PB10" -Step "pbar batch-delete clean: bar only, ends on committed frame" -Body {
        $src1 = Write-SourceFile "pbar_clean_src1" "$($script:CleanDelName1) = 1;"
        $src2 = Write-SourceFile "pbar_clean_src2" "$($script:CleanDelName2) = 1;"
        foreach ($fx in @(@{ Name = $script:CleanDelName1; File = $src1 },
                          @{ Name = $script:CleanDelName2; File = $src2 })) {
            $ins = Invoke-Cli -Arguments @("--json", "insert", "function", $fx.Name, $fx.File)
            Assert-NoPopup $ins
            Assert-True ($ins.ExitCode -eq 0) `
                "fixture insert $($fx.Name): expected exit 0, got $($ins.ExitCode); stderr: $($ins.Stderr)"
            $jIns = $null
            try { $jIns = $ins.Stdout | ConvertFrom-Json -ErrorAction Stop } catch {}
            Assert-True ($null -ne $jIns) "fixture insert $($fx.Name) stdout must be JSON; got: $($ins.Stdout)"
            Assert-True ($jIns.success -eq $true -and $jIns.prog_id -gt 0) `
                "fixture insert $($fx.Name) must report success=true and prog_id>0; stdout: $($ins.Stdout)"
        }
        $cleanManifest = Write-ManifestFile "pbar_clean_manifest" @(
            "function $($script:CleanDelName1)",
            "function $($script:CleanDelName2)"
        )

        $cli = Invoke-Cli -Arguments @("--pbar", "batch-delete", $cleanManifest)
        Assert-NoPopup $cli
        Assert-True ($cli.ExitCode -eq 0) "exit code: expected 0 for a fully clean batch, got $($cli.ExitCode); stderr: $($cli.Stderr)"

        # stdout: EMPTY without --json (the results array is emitted only
        # with --json, and --pbar + --json is a usage error).
        Assert-True ($cli.Stdout.Trim() -eq '') "stdout must be EMPTY without --json; got: $($cli.Stdout)"

        # stderr: ONLY the pinned bar - '\r'-redrawn frames ending at
        # ] 2/2 100%, no failure suffix, no BATCH lines whatsoever.
        Assert-BarBytes $cli "PB10"
        Assert-True ($cli.Stderr.Contains("] 2/2")) "stderr bar frame must reach '] 2/2'; got: $($cli.Stderr)"
        Assert-True ($cli.Stderr.Contains("100%")) "stderr bar frame must reach '100%'; got: $($cli.Stderr)"
        Assert-True (-not ($cli.Stderr -match "$($script:FailSep) \d+ failed")) `
            "stderr bar must carry no ' - <n> failed' suffix on a clean run; got: $($cli.Stderr)"
        $batchLines = @($cli.Stderr -split "`n" | Where-Object { $_ -match 'BATCH' })
        Assert-True ($batchLines.Count -eq 0) `
            "stderr must contain no BATCH lines at all (no START/SUCCESS/FAILED/COMPLETE) under --pbar; got: $($batchLines -join ' | ')"

        # The last stderr line is the committed bar frame itself - no
        # BATCH COMPLETE on a clean run. The final '\n' may pin a chunk
        # that still holds earlier '\r'-redrawn frames, so the committed
        # frame is the last '\r'-separated segment of that line.
        $lines = @($cli.Stderr -split "`n" | Where-Object { $_.Trim() -ne '' })
        $segments = @($lines[$lines.Count - 1] -split "`r" | Where-Object { $_.Trim() -ne '' })
        $committed = $segments[$segments.Count - 1]
        Assert-True (-not $committed.StartsWith("BATCH")) `
            "last stderr line must be the committed bar frame, not BATCH COMPLETE; got: $committed"
        Assert-True ($committed.StartsWith("delete")) `
            "committed bar line must start with the 'delete' op label; got: $committed"
        Assert-True ($committed.Contains("[") -and $committed.Contains("] 2/2")) `
            "committed bar line must contain '[' + '] 2/2'; got: $committed"
    }

    }  # end: fixture-ready branch (PB1-PB10 run only against existing fixtures)
}
finally {
    # =====================================================================
    # CLEANUP (always runs, even when cases fail): delete every tracked
    # item, verify the created fixtures are gone (exists=0), then remove the
    # temp directory.
    # =====================================================================
    Write-Host ""
    Write-Host "===== CLEANUP =====" -ForegroundColor Cyan

    foreach ($item in $CreatedItems) {
        $cli = Invoke-Cli -Arguments @("delete", $item.Type, $item.Name)
        if ($cli.ExitCode -eq 0) {
            Write-Host "  Cleaned up: $($item.Type) '$($item.Name)'" -ForegroundColor DarkGray
        }
        elseif ($cli.ExitCode -eq 5) {
            Write-Host "  Already gone: $($item.Type) '$($item.Name)' (exit 5)" -ForegroundColor DarkGray
        }
        else {
            Write-Host "  WARN: delete $($item.Type) '$($item.Name)' returned exit $($cli.ExitCode)" -ForegroundColor Yellow
        }
        if ($cli.Popup -ne '') {
            Write-Host "  WARN: crash/error popup during cleanup delete of '$($item.Name)': $($cli.Popup)" -ForegroundColor Yellow
        }
    }

    # Verify the created fixtures are really gone (exists=0 after delete).
    foreach ($name in @($ProbeName, $BatchProbeName)) {
        $chk = Invoke-Cli -Arguments @("exists", "function", $name)
        if ($chk.ExitCode -eq 0 -and $chk.Stdout -match 'exists=0') {
            Write-Host "  Verified gone: function '$name' (exists=0)" -ForegroundColor DarkGray
        }
        else {
            Write-Host "  WARN: post-cleanup exists for '$name' did not report exists=0: exit $($chk.ExitCode); stdout: $($chk.Stdout)" -ForegroundColor Yellow
        }
    }

    # Remove temp directory.
    if (Test-Path -Path $tmpDir -PathType Container) {
        Remove-Item -Path $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
    }
    Write-Host "CLEANUP: OK (fixtures deleted; temp files removed)" -ForegroundColor Green
}

# ===========================================================================
# SUMMARY
# ===========================================================================
$failureCount = Print-Summary -Results $Results

Write-Host ""
if ($failureCount -eq 0) {
    Write-Host "ALL PBAR E2E TESTS PASSED" -ForegroundColor Green
    exit 0
}
else {
    Write-Host "$failureCount TEST(S) FAILED" -ForegroundColor Red
    exit 1
}
