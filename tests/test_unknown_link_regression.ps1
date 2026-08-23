<#
.SYNOPSIS
    Opt-in desired-state E2E harness for the Unknown Link defect
    (TVS-L.1 + TVS-L.4 + TVS-L.5 + TVS-L.6 + TVS-L.7) covering all 5
    supported component types: Strategy, Indicator, Function, TradingApp,
    ShowMe. TVS-L.7 (read-only-ELL fault injection) is OPT-IN via
    -IncludeRedFaultInjection and is NOT registered in the always-green
    suite (tests/run_regression.ps1 invokes this script without that
    switch).

.DESCRIPTION
    This is the RED regression/acceptance harness for Workstream L (Unknown
    Link Defect Fix) extended to all supported component types. It is an
    OPT-IN test: it is NOT registered in tests/run_regression.ps1 and is
    invoked directly. It exists to prove that today's production CLI does
    NOT complete the original finalization path, while explicitly
    distinguishing:

      * XML evidence  -- a completion marker / debugger source-map artifact.
                         Presence of <prefix><ProgId>.xml with the expected
                         <symbols file="..." version="{GUID}"> header proves
                         the original finalization path (SymWriter::Save) ran.
      * Runtime linkability -- what TradeStation Desktop actually links at
                         runtime. This harness does NOT assert runtime
                         linkability and does NOT substitute a fake runtime
                         proxy. The existing durable user/runtime evidence
                         (Strategies fail in Desktop with "Unknown link error
                         occurred") is the L.1 runtime-failure baseline and is
                         recorded separately in
                         docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md.

    Test flow (per supported component type):
      1. Generate a unique name (TS_AUTO_L1_<TYPE>_<timestamp>).
      2. Insert a minimal valid component through the production CLI with
         --json. Parse the ProgId from the JSON output.
      3. Desired-state assertions (EXPECTED TO FAIL on current pre-L.3 code,
         which is the RED baseline):
         a. ELL exists at <MyWork>\Bin\<prefix><ProgId>.ell and is fresh
            (mtime after the insert operation).
         b. XML exists at <MyWork>\Bin\<prefix><ProgId>.xml and is fresh.
            Current code is EXPECTED TO FAIL here because the bypass skips
            SymWriter::Save.
         c. Parse the XML as
            <symbols file="<prefix><ProgId>" version="{GUID}"> and extract
            the GUID string.
         d. GUID byte match: convert the GUID string to its 16 Windows
            mixed-endian layout bytes and search the COMPLETE ELL binary for
            those 16 bytes. Assert they occur exactly once. Layout:
              Data1 (4 bytes): little-endian DWORD
              Data2 (2 bytes): little-endian WORD
              Data3 (2 bytes): little-endian WORD
              Data4 (8 bytes): big-endian (as-is)
            Do NOT use "last 16 bytes" -- search the entire ELL. See
            tmp/com_nopatch_guid_analysis.json for the authoritative analysis.
         e. Source round-trips: tsdev_cli get-source <type> <name> --json
            returns source matching the inserted source unchanged.
      4. Cleanup (MANDATORY, even after assertion failure): try/finally
         deletes the warehouse entry via tsdev_cli delete <type> <name>,
         verifies via exists that the entry is gone, and verifies both .ell
         and .xml artifacts are absent afterward.

    The harness also runs L4 persistence validation (failed insert, failed
    modify, successful modify, upsert-new, upsert-existing) per type, and
    L5 type-specific semantic error cases (e.g., Plot in a Strategy, Buy in
    an Indicator).

    Output: PASS/FAIL per assertion, a summary table, and a final exit code
    (0 = all assertions passed, nonzero = one or more failed). On current
    pre-L.3 code the harness is EXPECTED to exit nonzero (the XML/GUID
    assertions fail because the bypass skips SymWriter::Save). That nonzero
    exit is the recorded RED baseline. L.3 turns this GREEN and registers it
    in tests/run_regression.ps1.

    IMPORTANT: XML evidence is NOT runtime acceptance. A passing XML/GUID
    assertion proves the original finalization path ran; it does NOT prove
    Desktop linkability. This harness never fakes runtime success/failure.

.PARAMETER CliPath
    Path to the tsdev_cli.exe executable. Defaults to
    "$PSScriptRoot\..\build\Release\tsdev_cli.exe".

.PARAMETER TsPath
    Optional. TradeStation install directory passed through to the CLI as
    "--ts-path <dir>" BEFORE the subcommand. When supplied, the MyWork\Bin
    directory used for artifact checks is derived as <TsPath>\MyWork\Bin.
    When omitted, the default install path
    "C:\Program Files (x86)\TradeStation 9.5\MyWork\Bin" is used.

.PARAMETER BinPath
    Optional. Explicit override for the MyWork\Bin directory used for .ell
    and .xml artifact checks. Use this only if the CLI auto-discovers a
    different TS root than the default and --ts-path is not sufficient.

.PARAMETER Verbose
    Switch. When set, print full CLI output for every step.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\test_unknown_link_regression.ps1
    Runs the opt-in RED baseline harness with the default CLI path. Must be
    run from an elevated (administrator) terminal.

.NOTES
    Exit codes (returned by this script):
      0 = all desired-state assertions passed (GREEN -- expected only after L.3)
      1 = one or more desired-state assertions failed (RED -- expected on
          current pre-L.3 production code)
      2 = environment/prerequisite error (not elevated, CLI missing, etc.)
#>

param(
    [string]$CliPath = (Join-Path $PSScriptRoot "..\build\Release\tsdev_cli.exe"),
    [string]$TsPath,
    [string]$BinPath,
    [switch]$Verbose,
    # TVS-L.7 (handoff Sec.6): opt-in fault-injection acceptance harness.
    # When set, the L7.* fault-injection cases (read-only-ELL modify /
    # upsert-existing / delete) run AFTER the always-green L1/L4/L5/L6 cases.
    # The registered suite (tests/run_regression.ps1) invokes this script
    # WITHOUT this switch, so the opt-in fault-injection cases are NOT
    # registered in the always-green suite (handoff Sec.6: "Do not register
    # intentionally RED cases in the always-green suite yet"). L6 oracles
    # and all L1/L4/L5 normal-path assertions above are preserved unchanged.
    # Post-L.9 (L.9 APPROVED) status: L7.1/L7.2 desired-state (non-success +
    # retain) is GREEN detect-and-fail. Post-L.12 (L.12 GREEN) status: L7.3
    # desired-state (non-success + residual/error reported) is GREEN
    # detect-and-report. The -IncludeRedFaultInjection gate stays opt-in
    # because the harness still mutates the read-only ELL on disk.
    [switch]$IncludeRedFaultInjection,
    # TVS-L.7 (handoff Sec.6 Evidence): durable evidence summary path. The
    # harness writes the complete RED reproduction evidence (exit codes,
    # hashes, GUID occurrence counts, get-source output, read-only-command
    # capture, invariant failures) to this path.
    #
    # WRITE-GUARD: routine -IncludeRedFaultInjection runs are nondeterministic
    # (ProgIds, hashes, timestamps vary per run), so a content-equality check
    # would still dirty the tracked file every time. To avoid overwriting the
    # tracked durable evidence on routine re-runs, the DEFAULT is a gitignored
    # path under tmp/ (tmp/ is gitignored via .gitignore). Only when the
    # operator explicitly passes -UpdateTrackedEvidence is the durable path
    # overridden back to the tracked docs/RE/ file, signalling an intentional
    # refresh of the committed evidence.
    [string]$RedFaultInjectionDurablePath = (Join-Path (Split-Path -Parent $PSScriptRoot) "tmp\RE_L7_RED_FAULT_INJECTION_EVIDENCE.md"),
    # WRITE-GUARD switch: when set, override $RedFaultInjectionDurablePath back
    # to the tracked docs/RE/RE_L7_RED_FAULT_INJECTION_EVIDENCE.md so the
    # operator explicitly opts in to refreshing the tracked durable evidence.
    # When NOT set, routine runs write only to the gitignored tmp/ default and
    # the tracked docs/RE/ file is left untouched.
    [switch]$UpdateTrackedEvidence
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

# WRITE-GUARD: -UpdateTrackedEvidence overrides the durable evidence path back
# to the tracked docs/RE/ file so the operator explicitly opts in to refreshing
# the committed evidence. Without it, routine -IncludeRedFaultInjection runs
# write only to the gitignored tmp/ default and never touch the tracked file.
if ($UpdateTrackedEvidence) {
    $RedFaultInjectionDurablePath = (Join-Path $ProjectRoot "docs\RE\RE_L7_RED_FAULT_INJECTION_EVIDENCE.md")
}

# Resolve the MyWork\Bin directory used for .ell/.xml artifact checks.
# Precedence: -BinPath > -TsPath > default install path.
if ($BinPath) {
    $MyWorkBin = $BinPath
} elseif ($TsPath) {
    $MyWorkBin = Join-Path $TsPath "MyWork\Bin"
} else {
    $MyWorkBin = "C:\Program Files (x86)\TradeStation 9.5\MyWork\Bin"
}

$runId  = Get-Date -Format "yyyyMMdd_HHmmss"
$tmpDir = Join-Path $ProjectRoot "tmp\e2e_unknown_link_$runId"

# Results collector: one object per assertion / step.
$Results = New-Object System.Collections.Generic.List[object]

# ---------------------------------------------------------------------------
# Component type mappings for the 5 supported CLI types.
# ---------------------------------------------------------------------------
$TypeInfo = @{
    strategy   = @{ cli='strategy';   whType=0;    prefix='elmodule_0_'    }
    indicator  = @{ cli='indicator';  whType=2;    prefix='elmodule_2_'    }
    function   = @{ cli='function';   whType=4;    prefix='elmodule_4_'    }
    tradingapp = @{ cli='tradingapp'; whType=5122; prefix='elmodule_5122_' }
    showme     = @{ cli='showme';     whType=1282; prefix='elmodule_1282_' }
}
$TypeCode = @{
    strategy   = 'STRAT'
    indicator  = 'IND'
    function   = 'FUNC'
    tradingapp = 'TA'
    showme     = 'SHOWME'
}

# ---------------------------------------------------------------------------
# Is-Elevated check. The CLI has a requireAdministrator manifest; running it
# from a non-elevated shell would either UAC-prompt or fail with exit 11
# (not_elevated). Fail fast with a clear message instead.
# ---------------------------------------------------------------------------
function Test-IsElevated {
    $id = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object System.Security.Principal.WindowsPrincipal($id)
    return $principal.IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)
}

# ---------------------------------------------------------------------------
# Write-SourceFile: writes EasyLanguage source to a temp file in ASCII
# encoding (UTF-8 BOM breaks the EasyLanguage compiler). Returns the path.
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
# Invoke-Cli: runs tsdev_cli.exe with the given argument array, capturing
# stdout and stderr SEPARATELY (stdout via the pipeline, stderr via 2>file).
# Returns a hashtable with Stdout (string), Stderr (string), ExitCode (int).
# ---------------------------------------------------------------------------
function Invoke-Cli {
    param(
        [Parameter(Mandatory)] [string[]]$Arguments
    )
    $fullArgs = @()
    if ($script:TsPath) {
        $fullArgs += @("--ts-path", $script:TsPath)
    }
    $fullArgs += $Arguments

    $stderrFile = Join-Path $script:tmpDir "stderr_$([System.Guid]::NewGuid().ToString('N')).txt"

    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $stdout = & $script:CliPath @fullArgs 2>$stderrFile
        $exitCode = $LASTEXITCODE
    }
    catch {
        $stdout = $_.Exception.Message
        $exitCode = -2
    }
    finally {
        $ErrorActionPreference = $prevEAP
    }

    $stdoutStr = if ($stdout -is [array]) { ($stdout -join "`n") } else { "$stdout" }
    $stdoutStr = $stdoutStr.TrimEnd()

    $stderrStr = ""
    if (Test-Path -Path $stderrFile -PathType Leaf) {
        $stderrStr = ([System.IO.File]::ReadAllText($stderrFile)).TrimEnd()
        Remove-Item -Path $stderrFile -Force -ErrorAction SilentlyContinue
    }

    return @{
        Stdout   = $stdoutStr
        Stderr   = $stderrStr
        ExitCode = $exitCode
    }
}

# ---------------------------------------------------------------------------
# Record-Result: records one assertion/step result and prints PASS/FAIL.
# Returns the result object.
# ---------------------------------------------------------------------------
function Record-Result {
    param(
        [Parameter(Mandatory)] [string]$StepId,
        [Parameter(Mandatory)] [string]$Description,
        [Parameter(Mandatory)] [bool]$Passed,
        [string]$Detail = ""
    )
    $status = if ($Passed) { 'PASS' } else { 'FAIL' }
    $color  = if ($Passed) { 'Green' } else { 'Red' }
    Write-Host ("[{0}] {1}: {2}" -f $status, $StepId, $Description) -ForegroundColor $color
    if (-not $Passed -or $Verbose) {
        if ($Detail) {
            Write-Host "  => $Detail" -ForegroundColor DarkGray
        }
    }
    $result = [pscustomobject]@{
        StepId      = $StepId
        Description = $Description
        Result      = $status
        Detail      = $Detail
    }
    $Results.Add($result) | Out-Null
    return $result
}

# ---------------------------------------------------------------------------
# Convert-GuidToWindowsBytes: converts a GUID string like
# "{314164A7-DCB4-45D9-BE2D-B6086227CA2E}" to its 16 Windows mixed-endian
# layout bytes (Guid.ToByteArray order):
#   Data1 (4 bytes): little-endian DWORD
#   Data2 (2 bytes): little-endian WORD
#   Data3 (2 bytes): little-endian WORD
#   Data4 (8 bytes): big-endian (as-is)
# Returns a 16-byte array.
# ---------------------------------------------------------------------------
function Convert-GuidToWindowsBytes {
    param([Parameter(Mandatory)] [string]$GuidString)
    # Strip braces/whitespace.
    $g = $GuidString.Trim().TrimStart('{').TrimEnd('}')
    $parts = $g -split '-'
    if ($parts.Count -ne 5) {
        throw "Malformed GUID string: '$GuidString'"
    }
    $d1 = [Convert]::ToUInt32($parts[0], 16)
    $d2 = [Convert]::ToUInt16($parts[1], 16)
    $d3 = [Convert]::ToUInt16($parts[2], 16)
    # Data4 is two groups: 2 bytes then 6 bytes, both big-endian as-is.
    $d4a = $parts[3]  # 2 bytes (4 hex chars)
    $d4b = $parts[4]  # 6 bytes (12 hex chars)

    $bytes = New-Object byte[] 16
    # Data1 little-endian DWORD
    $bytes[0] = ($d1 -band 0xFF)
    $bytes[1] = (($d1 -shr 8)  -band 0xFF)
    $bytes[2] = (($d1 -shr 16) -band 0xFF)
    $bytes[3] = (($d1 -shr 24) -band 0xFF)
    # Data2 little-endian WORD
    $bytes[4] = ($d2 -band 0xFF)
    $bytes[5] = (($d2 -shr 8) -band 0xFF)
    # Data3 little-endian WORD
    $bytes[6] = ($d3 -band 0xFF)
    $bytes[7] = (($d3 -shr 8) -band 0xFF)
    # Data4 big-endian (as-is): 2 bytes from $d4a, 6 bytes from $d4b
    $bytes[8]  = [Convert]::ToByte($d4a.Substring(0,2), 16)
    $bytes[9]  = [Convert]::ToByte($d4a.Substring(2,2), 16)
    for ($i = 0; $i -lt 6; $i++) {
        $bytes[10 + $i] = [Convert]::ToByte($d4b.Substring($i*2, 2), 16)
    }
    return $bytes
}

# ---------------------------------------------------------------------------
# Find-BytePattern: returns an array of offsets at which $Pattern (byte[])
# occurs in $Data (byte[]). Searches the COMPLETE buffer.
# ---------------------------------------------------------------------------
function Find-BytePattern {
    param(
        [Parameter(Mandatory)] [byte[]]$Data,
        [Parameter(Mandatory)] [byte[]]$Pattern
    )
    $offsets = New-Object System.Collections.Generic.List[int]
    if ($Pattern.Length -eq 0 -or $Data.Length -lt $Pattern.Length) {
        return $offsets
    }
    $last = $Data.Length - $Pattern.Length
    for ($i = 0; $i -le $last; $i++) {
        $match = $true
        for ($j = 0; $j -lt $Pattern.Length; $j++) {
            if ($Data[$i + $j] -ne $Pattern[$j]) { $match = $false; break }
        }
        if ($match) { $offsets.Add($i) | Out-Null }
    }
    return $offsets
}

# ---------------------------------------------------------------------------
# Helper: Get-ExistsFromJson -- returns $true if the exists JSON reports the
# entry exists.
# ---------------------------------------------------------------------------
function Get-ExistsFromJson {
    param([Parameter(Mandatory)] [string]$Json)
    try {
        $j = $Json | ConvertFrom-Json -ErrorAction Stop
        return [bool]$j.exists
    } catch { }
    return $false
}

# ---------------------------------------------------------------------------
# Pure helper: Test-DeleteSucceeded -- classifies a CLI delete exit code as
# successful cleanup vs genuine failure. A delete exit 0 is always success.
# A delete exit 5 (not_found) is a successful ALREADY-ABSENT cleanup when
# exists confirms the entry is gone (ExistsGone=true); without exists
# confirmation, exit 5 is ambiguous and treated as failure. Any other exit
# code (e.g. 6) is a genuine delete failure. This is the ACTUAL
# production-test-script path used by Invoke-CleanupForName and the inline L1
# cleanup, and is shared by the L6.4 self-check.
# ---------------------------------------------------------------------------
function Test-DeleteSucceeded {
    param(
        [Parameter(Mandatory)] [int]$ExitCode,
        [Parameter(Mandatory)] [bool]$ExistsGone
    )
    if ($ExitCode -eq 0) { return $true }
    if ($ExitCode -eq 5 -and $ExistsGone) { return $true }
    return $false
}

# ---------------------------------------------------------------------------
# Pure helper: Get-ArtifactSanitationState -- captures PRE-SANITATION
# production artifact state (whether .ell/.xml still exist AFTER the CLI
# delete but BEFORE any best-effort Remove-Item). Returns a hashtable with
# EllExistedBeforeSanitation, XmlExistedBeforeSanitation,
# OrphanExistedBeforeSanitation, EllGone (pre-sanitation absence),
# XmlGone (pre-sanitation absence). This is the ACTUAL production-test-script
# path used by Invoke-CleanupForName, and is shared by the L6.3 self-check so
# the self-check exercises the real state-capture contract (not a replicated
# formula).
# ---------------------------------------------------------------------------
function Get-ArtifactSanitationState {
    param(
        [string]$EllPath = "",
        [string]$XmlPath = ""
    )
    $ellExisted = $false
    $xmlExisted = $false
    if ($EllPath -and (Test-Path -Path $EllPath -PathType Leaf)) { $ellExisted = $true }
    if ($XmlPath -and (Test-Path -Path $XmlPath -PathType Leaf)) { $xmlExisted = $true }
    return @{
        EllExistedBeforeSanitation    = $ellExisted
        XmlExistedBeforeSanitation    = $xmlExisted
        OrphanExistedBeforeSanitation = ($ellExisted -or $xmlExisted)
        EllGone                       = -not $ellExisted
        XmlGone                       = -not $xmlExisted
    }
}

# ---------------------------------------------------------------------------
# Pure helper: Get-CleanupFailureEvidence -- builds the production failure
# evidence string from the delete outcome and pre-sanitation orphan state.
# The evidence string is consistent across Invoke-CleanupForName and the
# inline L1 cleanup, and always includes the delete exit code. This is the
# ACTUAL production-test-script path, shared by the L6.4 self-check.
# ---------------------------------------------------------------------------
function Get-CleanupFailureEvidence {
    param(
        [Parameter(Mandatory)] [bool]$DeletedEntry,
        [Parameter(Mandatory)] [int]$DeleteExitCode,
        [Parameter(Mandatory)] [bool]$OrphanExistedBeforeSanitation,
        [Parameter(Mandatory)] [bool]$EllExistedBeforeSanitation,
        [Parameter(Mandatory)] [bool]$XmlExistedBeforeSanitation
    )
    $evidence = ""
    if (-not $DeletedEntry) {
        $evidence = "cli_delete_failed(exit=$DeleteExitCode)"
    }
    if ($DeletedEntry -and $OrphanExistedBeforeSanitation) {
        $evidence += " orphan_after_successful_delete(ell=$EllExistedBeforeSanitation xml=$XmlExistedBeforeSanitation)"
    }
    return $evidence.Trim()
}

# ---------------------------------------------------------------------------
# Pure helper: Get-CleanupVerdict -- the cleanup verdict formula shared by
# Invoke-CleanupForName, the inline L1 cleanup, and the L6.4 self-check. A
# PASS requires the delete to have succeeded (DeletedEntry), the entry to be
# gone (ExistsGone), and NO orphan artifacts to have been left by the
# production delete (pre-sanitation EllGone/XmlGone).
# ---------------------------------------------------------------------------
function Get-CleanupVerdict {
    param(
        [Parameter(Mandatory)] [bool]$DeletedEntry,
        [Parameter(Mandatory)] [bool]$ExistsGone,
        [Parameter(Mandatory)] [bool]$EllGone,
        [Parameter(Mandatory)] [bool]$XmlGone
    )
    return ($DeletedEntry -and $ExistsGone -and $EllGone -and $XmlGone)
}

# ===========================================================================
# MAIN EXECUTION
# ===========================================================================

Write-Host "================================================" -ForegroundColor Cyan
Write-Host "TSDev Automation - Unknown Link RED Harness"      -ForegroundColor Cyan
Write-Host "TVS-L.1 + TVS-L.4 + L5 (all 5 component types)"   -ForegroundColor Cyan
Write-Host "Opt-in; NOT in run_regression.ps1"                -ForegroundColor Cyan
Write-Host "================================================" -ForegroundColor Cyan
Write-Host "Run ID       : $runId"
Write-Host "Project root : $ProjectRoot"
Write-Host "CLI path     : $CliPath"
Write-Host "TS path      : $(if ($TsPath) { $TsPath } else { 'auto-discovered' })"
Write-Host "MyWork\Bin   : $MyWorkBin"
Write-Host "Verbose      : $Verbose"
Write-Host "Temp dir     : $tmpDir"
Write-Host ""
Write-Host "NOTE: XML evidence is NOT runtime acceptance."    -ForegroundColor Yellow
Write-Host "      This harness proves artifact-completion failure, not runtime" -ForegroundColor Yellow
Write-Host "      failure. It does not fake runtime success/failure."            -ForegroundColor Yellow
Write-Host ""

# ---------------------------------------------------------------------------
# Prerequisite checks.
# ---------------------------------------------------------------------------
if (-not (Test-IsElevated)) {
    Write-Host "ERROR: This harness must be run from an elevated (administrator)" -ForegroundColor Red
    Write-Host "       terminal. The CLI has a requireAdministrator manifest."    -ForegroundColor Red
    exit 2
}

if (-not (Test-Path -Path $CliPath -PathType Leaf)) {
    Write-Host "ERROR: CLI executable not found: $CliPath" -ForegroundColor Red
    Write-Host "       Build the project first: cmake --build build --config Release" -ForegroundColor Yellow
    exit 2
}

if (-not (Test-Path -Path $MyWorkBin -PathType Container)) {
    Write-Host "ERROR: MyWork\Bin directory not found: $MyWorkBin" -ForegroundColor Red
    Write-Host "       Override with -BinPath or -TsPath." -ForegroundColor Yellow
    exit 2
}

# Create temp directory.
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null

# Unique probe name for this run.
$probeName = "TS_AUTO_L1_RED_${runId}"

# Minimal valid EasyLanguage strategy source. The leading "{ name }" comment
# block is the EasyLanguage convention for naming a strategy/function; it is
# required for enumerateRecords to extract the verbatim stored name.
$probeSource = "{ $probeName }`r`nInputs: int x(0);`r`nif x > 0 then Buy next bar at market;"

# State carried across the try/finally so cleanup always knows what to remove.
$progId        = 0
$ellPath       = ""
$xmlPath       = ""
$insertOk      = $false
$cleanupStatus = @()

try {
    # =====================================================================
    # STEP 1: Insert a minimal valid Strategy via the production CLI.
    # =====================================================================
    Write-Host "--- Step 1: insert minimal Strategy via production CLI ---" -ForegroundColor Cyan

    $srcFile = Write-SourceFile "probe_src" $probeSource

    # Capture a pre-insert listing of elmodule_0_* files in MyWork\Bin so we
    # can verify post-insert that the target ELL/XML for the assigned ProgId
    # did NOT pre-exist (spec: "assert neither target ELL nor XML exists for
    # the assigned test entry"). Defense-in-depth against recycled ProgIds.
    $preInsertEllFiles = @()
    if (Test-Path -Path $MyWorkBin -PathType Container) {
        $preInsertEllFiles = @(Get-ChildItem -Path $MyWorkBin -Filter 'elmodule_0_*' -File -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name)
    }

    $insertStart = Get-Date
    $cliIns = Invoke-Cli -Arguments @("--json","insert","strategy",$probeName,$srcFile)
    $insertEnd = Get-Date

    $insertExitOk = ($cliIns.ExitCode -eq 0)
    $insertJsonOk = $false
    $insertDetail = "exit=$($cliIns.ExitCode)"
    try {
        $jIns = $cliIns.Stdout | ConvertFrom-Json -ErrorAction Stop
        if ($jIns.success -and $jIns.prog_id -and $jIns.prog_id -gt 0) {
            $insertJsonOk = $true
            $progId = [int]$jIns.prog_id
            $insertDetail += " prog_id=$progId"
        } else {
            $insertDetail += " (JSON success=false or prog_id missing)"
        }
    } catch {
        $insertDetail += " (JSON parse failed: $($_.Exception.Message))"
    }

    Record-Result -StepId "L1.1" -Description "insert strategy via production CLI (exit 0, JSON success=true, prog_id>0)" `
        -Passed ($insertExitOk -and $insertJsonOk) -Detail $insertDetail | Out-Null

    if (-not $insertExitOk -or -not $insertJsonOk -or $progId -le 0) {
        Write-Host "FATAL: insert did not succeed; cannot run desired-state assertions." -ForegroundColor Red
        # Skip to finally for cleanup (nothing to clean if insert failed).
        throw "insert failed; aborting desired-state assertions"
    }

    $insertOk = $true
    $ellPath = Join-Path $MyWorkBin "elmodule_0_$progId.ell"
    $xmlPath = Join-Path $MyWorkBin "elmodule_0_$progId.xml"
    Write-Host "  ProgId  : $progId"
    Write-Host "  ELL path: $ellPath"
    Write-Host "  XML path: $xmlPath"
    Write-Host "  Insert window: $insertStart -> $insertEnd"

    # =====================================================================
    # STEP 1b: Pre-existence check (spec: "assert neither target ELL nor XML
    # exists for the assigned test entry"). Verify the target ELL/XML for the
    # discovered ProgId were NOT present in the pre-insert listing. A match
    # indicates a recycled ProgId / stale artifacts and is a FAIL.
    # =====================================================================
    Write-Host "--- Step 1b: target ELL/XML did not pre-exist before insert ---" -ForegroundColor Cyan

    $targetEllName = "elmodule_0_$progId.ell"
    $targetXmlName = "elmodule_0_$progId.xml"
    $ellPreExisted = $preInsertEllFiles -contains $targetEllName
    $xmlPreExisted = $preInsertEllFiles -contains $targetXmlName
    $noPreExist = (-not $ellPreExisted -and -not $xmlPreExisted)
    $preExistDetail = "pre_insert_count=$($preInsertEllFiles.Count) ell_preexisted=$ellPreExisted xml_preexisted=$xmlPreExisted"
    Record-Result -StepId "L1.0_PREEXIST" -Description "target elmodule_0_${progId}.ell/.xml did not pre-exist before insert" `
        -Passed $noPreExist -Detail $preExistDetail | Out-Null

    # =====================================================================
    # STEP 2a: ELL exists and is fresh.
    # =====================================================================
    Write-Host "--- Step 2a: ELL exists and is fresh ---" -ForegroundColor Cyan

    $ellExists = Test-Path -Path $ellPath -PathType Leaf
    $ellFresh  = $false
    $ellDetail = "exists=$ellExists path=$ellPath"
    if ($ellExists) {
        $ellMtime = (Get-Item -Path $ellPath).LastWriteTime
        $ellFresh = ($ellMtime -ge $insertStart)
        $ellDetail += " mtime=$($ellMtime.ToString('o')) fresh=$ellFresh"
    }
    Record-Result -StepId "L1.2a" -Description "ELL exists at elmodule_0_${progId}.ell and is fresh (mtime >= insert start)" `
        -Passed ($ellExists -and $ellFresh) -Detail $ellDetail | Out-Null

    # =====================================================================
    # STEP 2b: XML exists and is fresh (EXPECTED RED on current code).
    # =====================================================================
    Write-Host "--- Step 2b: XML exists and is fresh (EXPECTED RED on pre-L.3) ---" -ForegroundColor Cyan

    $xmlExists = Test-Path -Path $xmlPath -PathType Leaf
    $xmlFresh  = $false
    $xmlDetail = "exists=$xmlExists path=$xmlPath"
    if ($xmlExists) {
        $xmlMtime = (Get-Item -Path $xmlPath).LastWriteTime
        $xmlFresh = ($xmlMtime -ge $insertStart)
        $xmlDetail += " mtime=$($xmlMtime.ToString('o')) fresh=$xmlFresh"
    }
    Record-Result -StepId "L1.2b" -Description "XML exists at elmodule_0_${progId}.xml and is fresh (SymWriter::Save ran)" `
        -Passed ($xmlExists -and $xmlFresh) -Detail $xmlDetail | Out-Null

    # =====================================================================
    # STEP 2c: Parse the XML and extract the GUID from the
    # <symbols file="elmodule_0_<ProgId>" version="{GUID}"> header.
    # =====================================================================
    Write-Host "--- Step 2c: parse XML <symbols ... version=`"{GUID}`"> ---" -ForegroundColor Cyan

    $guidString = $null
    $xmlParseOk = $false
    $xmlParseDetail = ""
    if ($xmlExists) {
        try {
            $xmlText = [System.IO.File]::ReadAllText($xmlPath)
            # Match the <symbols ...> opening tag. The version attribute holds
            # the GUID in braces, e.g. version="{314164A7-DCB4-45D9-BE2D-B6086227CA2E}".
            # Bind the <symbols> header to the specific ProgId under test
            # (defense-in-depth: do not match any numeric ProgId).
            $progIdFileAttr = [regex]::Escape("elmodule_0_$progId")
            $m = [regex]::Match($xmlText,
                '<symbols\s+[^>]*file="' + $progIdFileAttr + '"\s+version="\{([0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12})\}"')
            if (-not $m.Success) {
                # Try alternate attribute order (version before file).
                $m = [regex]::Match($xmlText,
                    '<symbols\s+[^>]*version="\{([0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12})\}"[^>]*file="' + $progIdFileAttr + '"')
            }
            if ($m.Success) {
                $guidString = "{" + $m.Groups[1].Value.ToUpper() + "}"
                $xmlParseOk = $true
                $xmlParseDetail = "guid=$guidString"
            } else {
                $xmlParseDetail = "no <symbols ... version=`"{GUID}`"> header matched"
            }
        } catch {
            $xmlParseDetail = "XML read/parse failed: $($_.Exception.Message)"
        }
    } else {
        $xmlParseDetail = "XML file absent; cannot parse"
    }
    Record-Result -StepId "L1.2c" -Description "parse XML and extract version GUID from <symbols> header" `
        -Passed $xmlParseOk -Detail $xmlParseDetail | Out-Null

    # =====================================================================
    # STEP 2d: GUID byte match -- search the COMPLETE ELL for the 16 Windows
    # mixed-endian layout bytes of the GUID; assert exactly one occurrence.
    # (EXPECTED RED on current code, since XML is absent.)
    # =====================================================================
    Write-Host "--- Step 2d: GUID byte match in COMPLETE ELL (Windows mixed-endian) ---" -ForegroundColor Cyan

    $guidMatchOk = $false
    $guidMatchDetail = ""
    if ($ellExists -and $xmlParseOk -and $guidString) {
        try {
            $ellBytes = [System.IO.File]::ReadAllBytes($ellPath)
            $guidBytes = Convert-GuidToWindowsBytes -GuidString $guidString
            $offsets = Find-BytePattern -Data $ellBytes -Pattern $guidBytes
            $count = $offsets.Count
            $guidMatchDetail = "occurrences=$count offsets=$($offsets -join ',') ellLen=$($ellBytes.Length)"
            $guidMatchOk = ($count -eq 1)
            if (-not $guidMatchOk) {
                $guidMatchDetail += " (expected exactly 1 occurrence)"
            }
        } catch {
            $guidMatchDetail = "GUID byte match failed: $($_.Exception.Message)"
        }
    } else {
        $guidMatchDetail = "skipped (ELL absent or GUID not parsed)"
    }
    Record-Result -StepId "L1.2d" -Description "GUID 16 Windows mixed-endian bytes occur exactly once in COMPLETE ELL" `
        -Passed $guidMatchOk -Detail $guidMatchDetail | Out-Null

    # =====================================================================
    # STEP 2e: Source round-trips unchanged via get-source.
    # =====================================================================
    Write-Host "--- Step 2e: source round-trips via get-source ---" -ForegroundColor Cyan

    $rtOk = $false
    $rtDetail = ""
    $cliGs = Invoke-Cli -Arguments @("--json","get-source","strategy",$probeName)
    if ($cliGs.ExitCode -ne 0) {
        $rtDetail = "get-source exit=$($cliGs.ExitCode)"
    } else {
        try {
            $jGs = $cliGs.Stdout | ConvertFrom-Json -ErrorAction Stop
            if (-not $jGs.success) {
                $rtDetail = "get-source JSON success=false"
            } else {
                $returned = [string]$jGs.source
                # Compare against the exact source we wrote. Use ordinal compare.
                if ($returned -eq $probeSource) {
                    $rtOk = $true
                    $rtDetail = "source matches inserted source (length=$($returned.Length))"
                } else {
                    $rtDetail = "source mismatch (inserted len=$($probeSource.Length), returned len=$($returned.Length))"
                }
            }
        } catch {
            $rtDetail = "get-source JSON parse failed: $($_.Exception.Message)"
        }
    }
    Record-Result -StepId "L1.2e" -Description "get-source returns source matching inserted source unchanged" `
        -Passed $rtOk -Detail $rtDetail | Out-Null
}
finally {
    # =====================================================================
    # CLEANUP (MANDATORY, always runs even after assertion failure).
    # =====================================================================
    Write-Host ""
    Write-Host "===== CLEANUP =====" -ForegroundColor Cyan

    # 1. Delete the warehouse entry via the CLI.
    $cliDel = $null
    if ($probeName) {
        $cliDel = Invoke-Cli -Arguments @("--json","delete","strategy",$probeName)
        $cleanupStatus += "delete(strategy $probeName): exit=$($cliDel.ExitCode)"
        Write-Host "  delete(strategy $probeName): exit=$($cliDel.ExitCode)" -ForegroundColor DarkGray
    }

    # 2. Verify the entry is gone via exists.
    $existsGone = $false
    if ($probeName) {
        $cliEx = Invoke-Cli -Arguments @("--json","exists","strategy",$probeName)
        if ($cliEx.ExitCode -eq 0) {
            $existsGone = -not (Get-ExistsFromJson -Json $cliEx.Stdout)
        }
        $cleanupStatus += "exists(strategy $probeName) after delete: exit=$($cliEx.ExitCode) gone=$existsGone"
        Write-Host "  exists after delete: gone=$existsGone" -ForegroundColor $(if ($existsGone) { 'DarkGray' } else { 'Yellow' })
    }

    # 3. Classify delete success: exit 0 is success; exit 5 (not_found) with
    #    ExistsGone=true is a successful already-absent cleanup; any other
    #    exit code (e.g. 6) is a genuine delete failure. Uses the same shared
    #    Test-DeleteSucceeded helper as Invoke-CleanupForName.
    $deleteExitCode = if ($cliDel) { $cliDel.ExitCode } else { -1 }
    $deletedEntry = if ($cliDel) { Test-DeleteSucceeded -ExitCode $deleteExitCode -ExistsGone $existsGone } else { $false }

    # 4. Capture PRE-SANITATION production state via the shared pure helper
    #    (same as Invoke-CleanupForName). A production delete that left an
    #    orphan must not be mutated into a PASS by the best-effort safety-net
    #    removal.
    $state = Get-ArtifactSanitationState -EllPath $ellPath -XmlPath $xmlPath
    $ellExistedBeforeSanitation = $state.EllExistedBeforeSanitation
    $xmlExistedBeforeSanitation = $state.XmlExistedBeforeSanitation
    $orphanExistedBeforeSanitation = $state.OrphanExistedBeforeSanitation
    $ellGone = $state.EllGone
    $xmlGone = $state.XmlGone

    # 5. Build production failure evidence via the shared pure helper. The
    #    evidence string always includes the delete exit code and is
    #    consistent with Invoke-CleanupForName.
    $productionFailureEvidence = Get-CleanupFailureEvidence `
        -DeletedEntry $deletedEntry `
        -DeleteExitCode $deleteExitCode `
        -OrphanExistedBeforeSanitation $orphanExistedBeforeSanitation `
        -EllExistedBeforeSanitation $ellExistedBeforeSanitation `
        -XmlExistedBeforeSanitation $xmlExistedBeforeSanitation

    # 6. NOTE: the shared temp directory ($tmpDir) is removed AFTER all L4
    #    sections complete (see the final cleanup block before SUMMARY). The
    #    L4 sections reuse $tmpDir for their own source files, so it must
    #    survive the L1 finally block.

    # 7. Cleanup verdict: escalate cleanup failure to a test failure so the
    #    final exit code reflects warehouse pollution, not just assertion
    #    outcomes. The verdict is computed BEFORE sanitation via the shared
    #    Get-CleanupVerdict helper (same formula as Invoke-CleanupForName and
    #    the L6.4 self-check), so a failed CLI delete or an orphan left by a
    #    successful delete cannot pass.
    $cleanupOk = Get-CleanupVerdict -DeletedEntry $deletedEntry -ExistsGone $existsGone -EllGone $ellGone -XmlGone $xmlGone
    $cleanupDetail = "deleted=$deletedEntry delete_exit=$deleteExitCode exists_gone=$existsGone ell_gone=$ellGone xml_gone=$xmlGone orphan_before=$orphanExistedBeforeSanitation evidence='$productionFailureEvidence'"

    # 8. Best-effort sanitation (test hygiene only; does not affect the
    #    verdict). Removes any orphan the production delete left behind.
    $sanitizedEllGone = $true
    $sanitizedXmlGone = $true
    if ($ellExistedBeforeSanitation) {
        Remove-Item -Path $ellPath -Force -ErrorAction SilentlyContinue
        $sanitizedEllGone = -not (Test-Path -Path $ellPath -PathType Leaf)
    }
    if ($xmlExistedBeforeSanitation) {
        Remove-Item -Path $xmlPath -Force -ErrorAction SilentlyContinue
        $sanitizedXmlGone = -not (Test-Path -Path $xmlPath -PathType Leaf)
    }
    $cleanupStatus += "artifacts after cleanup: ell_gone(pre-san)=$ellGone xml_gone(pre-san)=$xmlGone sanitized_ell=$sanitizedEllGone sanitized_xml=$sanitizedXmlGone"
    Write-Host "  artifacts after cleanup: ell_gone(pre-san)=$ellGone xml_gone(pre-san)=$xmlGone orphan_before=$orphanExistedBeforeSanitation sanitized_ell=$sanitizedEllGone sanitized_xml=$sanitizedXmlGone" -ForegroundColor $(if ($cleanupOk) { 'DarkGray' } else { 'Yellow' })

    Record-Result -StepId "L1.CLEANUP" -Description "cleanup removed warehouse entry and ELL/XML artifacts" `
        -Passed $cleanupOk -Detail $cleanupDetail | Out-Null
}

# ===========================================================================
# TVS-L.4 EXTENSIONS -- persistence validation + partial-artifact
# cleanup/rollback hardening (RED baseline).
#
# Each L4.* section is a self-contained try/finally with its own cleanup.
# Cleanup is MANDATORY and escalates cleanup failure to a test failure
# (mirrors L1.CLEANUP). Pre-existence checks remove any stale artifacts for
# the target name before the test begins.
# ===========================================================================

# ---------------------------------------------------------------------------
# Helper: Get-ProgIdFromJson -- parse prog_id from a CLI JSON result. Returns
# 0 if parsing fails or prog_id is missing/non-positive.
#
# TVS-L.6 GREEN repair (handoff Sec.3 defect 1): the CLI emits the assigned
# ProgId at the TOP LEVEL (`prog_id`) on success, AND at `error.prog_id` on a
# compile-error failure (exit 8), because createEntry allocates the ProgId /
# warehouse entry before the compile runs. The prior helper gated only on
# `$j.success -and $j.prog_id`, so a compile-error response (success=false,
# ProgId at error.prog_id) returned 0; downstream L4.1c/L4.1d "no .ell/.xml"
# assertions then checked an empty path and passed vacuously. The repaired
# helper consumes BOTH the success top-level prog_id and the failure nested
# error.prog_id.
# ---------------------------------------------------------------------------
function Get-ProgIdFromJson {
    param([Parameter(Mandatory)] [string]$Json)
    try {
        $j = $Json | ConvertFrom-Json -ErrorAction Stop
        # Success path: top-level prog_id.
        if ($j.success -and $j.prog_id -and [int]$j.prog_id -gt 0) {
            return [int]$j.prog_id
        }
        # Failure path: compile-error responses carry the assigned ProgId at
        # error.prog_id (the warehouse entry was allocated before the compile
        # failed). Reading only the top-level prog_id caused failed-insert
        # ELL/XML assertions to check an empty path and pass vacuously.
        if (-not $j.success -and $j.error -and $j.error.prog_id -and [int]$j.error.prog_id -gt 0) {
            return [int]$j.error.prog_id
        }
    } catch { }
    return 0
}

# ---------------------------------------------------------------------------
# Pure helper: Get-ExistsProgIdFromJson -- extracts prog_id (snake_case) from
# an exists JSON result. Returns 0 if parsing fails, exists=false, or prog_id
# is missing/non-positive. This is the ACTUAL production-test-script path used
# by Remove-PreExistingArtifacts, and is shared by the L6.2 self-check so the
# self-check exercises the real extraction contract (not a replicated formula).
# ---------------------------------------------------------------------------
function Get-ExistsProgIdFromJson {
    param([Parameter(Mandatory)] [string]$Json)
    try {
        $j = $Json | ConvertFrom-Json -ErrorAction Stop
        if ($j.exists -and $j.prog_id) { return [int]$j.prog_id }
    } catch { }
    return 0
}

# ---------------------------------------------------------------------------
# Helper: Get-SourceFromJson -- returns the source string from a get-source
# JSON result, or $null on failure.
# ---------------------------------------------------------------------------
function Get-SourceFromJson {
    param([Parameter(Mandatory)] [string]$Json)
    try {
        $j = $Json | ConvertFrom-Json -ErrorAction Stop
        if ($j.success) { return [string]$j.source }
    } catch { }
    return $null
}

# ---------------------------------------------------------------------------
# Helper: Extract-XmlGuid -- reads an <prefix><ProgId>.xml file and extracts
# the version GUID from the <symbols file="<prefix><ProgId>" version="{GUID}">
# header. Returns the GUID string (with braces) or $null.
# ---------------------------------------------------------------------------
function Extract-XmlGuid {
    param(
        [Parameter(Mandatory)] [string]$XmlPath,
        [Parameter(Mandatory)] [int]$ProgId,
        [string]$Prefix = "elmodule_0_"
    )
    if (-not (Test-Path -Path $XmlPath -PathType Leaf)) { return $null }
    try {
        $xmlText = [System.IO.File]::ReadAllText($XmlPath)
        $progIdFileAttr = [regex]::Escape("$Prefix$ProgId")
        $m = [regex]::Match($xmlText,
            '<symbols\s+[^>]*file="' + $progIdFileAttr + '"\s+version="\{([0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12})\}"')
        if (-not $m.Success) {
            $m = [regex]::Match($xmlText,
                '<symbols\s+[^>]*version="\{([0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12})\}"[^>]*file="' + $progIdFileAttr + '"')
        }
        if ($m.Success) { return "{" + $m.Groups[1].Value.ToUpper() + "}" }
    } catch { }
    return $null
}

# ---------------------------------------------------------------------------
# Helper: Test-GuidBytesInEll -- returns $true if the GUID's 16 Windows
# mixed-endian bytes occur exactly once in the ELL file.
# ---------------------------------------------------------------------------
function Test-GuidBytesInEll {
    param(
        [Parameter(Mandatory)] [string]$EllPath,
        [Parameter(Mandatory)] [string]$GuidString
    )
    if (-not (Test-Path -Path $EllPath -PathType Leaf)) { return $false }
    if (-not $GuidString) { return $false }
    try {
        $ellBytes = [System.IO.File]::ReadAllBytes($EllPath)
        $guidBytes = Convert-GuidToWindowsBytes -GuidString $GuidString
        $offsets = Find-BytePattern -Data $ellBytes -Pattern $guidBytes
        return ($offsets.Count -eq 1)
    } catch { }
    return $false
}

# ---------------------------------------------------------------------------
# Helper: Invoke-CleanupForName -- deletes a warehouse entry by name via CLI,
# verifies via exists, and removes any orphan .ell/.xml for the given ProgId.
#
# TVS-L.6 GREEN repair (handoff Sec.3 defects 3 + 4): the prior helper removed
# the .ell/.xml via Remove-Item and ONLY THEN reported EllGone/XmlGone, so a
# production delete that left an orphan artifact was mutated away before the
# verdict was fixed -- the cleanup verdict passed and the production failure
# evidence was gone. The repaired helper:
#   1. Captures PRE-SANITATION production state (whether the .ell/.xml still
#      existed AFTER the CLI delete but BEFORE any best-effort Remove-Item).
#   2. Computes the verdict BEFORE sanitation, so orphan evidence cannot be
#      erased. EllGone/XmlGone now mean "the production delete already
#      removed the artifact (no orphan)" -- i.e. pre-sanitation absence.
#   3. Records OrphanExistedBeforeSanitation and a ProductionFailureEvidence
#      string (failed CLI delete, or orphan left after a successful delete).
#   4. Performs best-effort sanitation AFTER fixing the verdict, and reports
#      SanitizedEllGone/SanitizedXmlGone for test-hygiene only.
#   5. Distinguishes a CLI delete exit 5 (not_found) with confirmed
#      ExistsGone=true as a successful already-absent cleanup from a genuine
#      delete failure (e.g. exit 6), via the shared Test-DeleteSucceeded
#      helper.
# All state capture, evidence, and verdict computation delegate to the shared
# pure helpers (Get-ArtifactSanitationState, Get-CleanupFailureEvidence,
# Get-CleanupVerdict, Test-DeleteSucceeded) so the L6.3/L6.4 self-checks
# exercise the REAL production-test-script path, not a replicated formula.
# Returns a hashtable with DeletedEntry, DeleteExitCode, ExistsGone, EllGone,
# XmlGone, EllExistedBeforeSanitation, XmlExistedBeforeSanitation,
# OrphanExistedBeforeSanitation, ProductionFailureEvidence, VerdictOk,
# SanitizedEllGone, SanitizedXmlGone, Detail.
# ---------------------------------------------------------------------------
function Invoke-CleanupForName {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [string]$Type = 'strategy',
        [int]$WhType = 0,
        [int]$ProgId = 0,
        [string]$EllPath = "",
        [string]$XmlPath = ""
    )
    if ($WhType -eq 0 -and $Type -and $script:TypeInfo[$Type]) {
        $WhType = $script:TypeInfo[$Type].whType
    }
    if (-not $EllPath -and $ProgId -gt 0 -and $Type -and $script:TypeInfo[$Type]) {
        $prefix = $script:TypeInfo[$Type].prefix
        $EllPath = Join-Path $script:MyWorkBin "$prefix$ProgId.ell"
    }
    if (-not $XmlPath -and $ProgId -gt 0 -and $Type -and $script:TypeInfo[$Type]) {
        $prefix = $script:TypeInfo[$Type].prefix
        $XmlPath = Join-Path $script:MyWorkBin "$prefix$ProgId.xml"
    }

    # 1. Delete the warehouse entry via the CLI.
    $cliDel = Invoke-Cli -Arguments @("--json","delete",$Type,$Name)
    Write-Host "  cleanup delete($Type $Name): exit=$($cliDel.ExitCode)" -ForegroundColor DarkGray

    # 2. Verify the entry is gone via exists.
    $existsGone = $false
    $cliEx = Invoke-Cli -Arguments @("--json","exists",$Type,$Name)
    if ($cliEx.ExitCode -eq 0) {
        $existsGone = -not (Get-ExistsFromJson -Json $cliEx.Stdout)
    }
    Write-Host "  cleanup exists after delete: gone=$existsGone" -ForegroundColor $(if ($existsGone) { 'DarkGray' } else { 'Yellow' })

    # 3. Classify delete success: exit 0 is success; exit 5 (not_found) with
    #    ExistsGone=true is a successful already-absent cleanup; any other
    #    exit code (e.g. 6) is a genuine delete failure.
    $deletedEntry = Test-DeleteSucceeded -ExitCode $cliDel.ExitCode -ExistsGone $existsGone

    # 4. Capture PRE-SANITATION production state via the shared pure helper.
    $state = Get-ArtifactSanitationState -EllPath $EllPath -XmlPath $XmlPath
    $ellExistedBeforeSanitation = $state.EllExistedBeforeSanitation
    $xmlExistedBeforeSanitation = $state.XmlExistedBeforeSanitation
    $orphanExistedBeforeSanitation = $state.OrphanExistedBeforeSanitation
    $ellGone = $state.EllGone
    $xmlGone = $state.XmlGone

    # 5. Build production failure evidence via the shared pure helper. The
    #    evidence string always includes the delete exit code and is
    #    consistent with the inline L1 cleanup.
    $productionFailureEvidence = Get-CleanupFailureEvidence `
        -DeletedEntry $deletedEntry `
        -DeleteExitCode $cliDel.ExitCode `
        -OrphanExistedBeforeSanitation $orphanExistedBeforeSanitation `
        -EllExistedBeforeSanitation $ellExistedBeforeSanitation `
        -XmlExistedBeforeSanitation $xmlExistedBeforeSanitation

    # 6. Compute the verdict BEFORE sanitation via the shared pure helper.
    #    A PASS requires the delete to have succeeded, the entry to be gone,
    #    and NO orphan to have been left by the production delete.
    $verdictOk = Get-CleanupVerdict -DeletedEntry $deletedEntry -ExistsGone $existsGone -EllGone $ellGone -XmlGone $xmlGone

    # 7. Best-effort sanitation (test hygiene only; does not affect the
    #    verdict). Removes any orphan the production delete left behind so the
    #    test run does not pollute MyWork\Bin.
    $sanitizedEllGone = $true
    $sanitizedXmlGone = $true
    if ($ellExistedBeforeSanitation) {
        Remove-Item -Path $EllPath -Force -ErrorAction SilentlyContinue
        $sanitizedEllGone = -not (Test-Path -Path $EllPath -PathType Leaf)
    }
    if ($xmlExistedBeforeSanitation) {
        Remove-Item -Path $XmlPath -Force -ErrorAction SilentlyContinue
        $sanitizedXmlGone = -not (Test-Path -Path $XmlPath -PathType Leaf)
    }
    Write-Host "  cleanup artifacts: ell_gone(pre-san)=$ellGone xml_gone(pre-san)=$xmlGone orphan_before=$orphanExistedBeforeSanitation sanitized_ell=$sanitizedEllGone sanitized_xml=$sanitizedXmlGone" -ForegroundColor $(if ($verdictOk) { 'DarkGray' } else { 'Yellow' })

    return @{
        DeletedEntry                  = $deletedEntry
        DeleteExitCode                = $cliDel.ExitCode
        ExistsGone                    = $existsGone
        EllGone                       = $ellGone
        XmlGone                       = $xmlGone
        EllExistedBeforeSanitation    = $ellExistedBeforeSanitation
        XmlExistedBeforeSanitation    = $xmlExistedBeforeSanitation
        OrphanExistedBeforeSanitation = $orphanExistedBeforeSanitation
        ProductionFailureEvidence     = $productionFailureEvidence
        VerdictOk                     = $verdictOk
        SanitizedEllGone              = $sanitizedEllGone
        SanitizedXmlGone              = $sanitizedXmlGone
        Detail                        = "deleted=$deletedEntry delete_exit=$($cliDel.ExitCode) exists_gone=$existsGone ell_gone=$ellGone xml_gone=$xmlGone orphan_before=$orphanExistedBeforeSanitation evidence='$productionFailureEvidence' sanitized_ell=$sanitizedEllGone sanitized_xml=$sanitizedXmlGone"
    }
}

# ---------------------------------------------------------------------------
# Helper: Remove-PreExistingArtifacts -- if an entry already exists for the
# name, delete it and remove any orphan .ell/.xml. Used before each test to
# guarantee a clean starting state.
#
# TVS-L.6 GREEN repair (handoff Sec.3 defect 2): the prior helper read
# `$j.progId` (camelCase), but the CLI exists JSON emits the ProgId at
# `prog_id` (snake_case) -- see src/tsdev_cli.cpp emitJson (success branch:
# j["prog_id"] = r.progId). The camelCase read was always $null, so
# $existingProgId stayed 0 and pre-existing artifacts for a recycled ProgId
# were never cleaned. The repaired helper delegates to the shared pure
# helper Get-ExistsProgIdFromJson, which reads `prog_id` (snake_case).
# ---------------------------------------------------------------------------
function Remove-PreExistingArtifacts {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [string]$Type = 'strategy',
        [int]$WhType = 0
    )
    if ($WhType -eq 0 -and $Type -and $script:TypeInfo[$Type]) {
        $WhType = $script:TypeInfo[$Type].whType
    }
    $cliEx = Invoke-Cli -Arguments @("--json","exists",$Type,$Name)
    $existingProgId = 0
    if ($cliEx.ExitCode -eq 0) {
        # Uses the shared pure helper (Get-ExistsProgIdFromJson) that reads
        # `prog_id` (snake_case), the field the CLI exists JSON actually emits.
        $existingProgId = Get-ExistsProgIdFromJson -Json $cliEx.Stdout
    }
    if ($existingProgId -gt 0) {
        $prefix = $script:TypeInfo[$Type].prefix
        $ell = Join-Path $MyWorkBin "$prefix$existingProgId.ell"
        $xml = Join-Path $MyWorkBin "$prefix$existingProgId.xml"
        Invoke-CleanupForName -Name $Name -Type $Type -WhType $WhType -ProgId $existingProgId -EllPath $ell -XmlPath $xml | Out-Null
    }
}

# ===========================================================================
# TVS-L.6 GREEN SELF-CHECKS -- verify the REPAIRED helpers/contracts (handoff
# Sec.3 "Reproduced test-oracle defects" / Sec.5 GREEN). These are DETERMINISTIC,
# CLI-free self-checks that exercise the ACTUAL repaired helpers/contracts
# (not stale replicated bad formulas) with synthetic inputs and assert the
# CORRECT desired-state behavior. Each MUST PASS (GREEN) and is non-vacuous:
# every check asserts a concrete expected value AND a guard that proves the
# exercised path actually ran (e.g. extracted ProgId > 0, evidence string
# non-empty, both success and failure branches covered). L6.2/L6.3/L6.4 call
# the SAME shared pure helpers (Get-ExistsProgIdFromJson,
# Get-ArtifactSanitationState, Get-CleanupVerdict, Test-DeleteSucceeded,
# Get-CleanupFailureEvidence) that the production-test-script path
# (Invoke-CleanupForName, Remove-PreExistingArtifacts, inline L1 cleanup)
# calls, so a regression in any helper is caught here. These checks do not
# alter any L.4.3-L.4.5 normal-path assertion or any production behavior.
# ===========================================================================
Write-Host ""
Write-Host "===== TVS-L.6: GREEN self-checks for repaired oracle contracts =====" -ForegroundColor Cyan

# ---------------------------------------------------------------------------
# L6.1 -- Get-ProgIdFromJson now handles BOTH success top-level prog_id AND
# failure nested error.prog_id (handoff Sec.3 defect 1).
# This self-check exercises the REAL Get-ProgIdFromJson helper (pure, no CLI)
# on BOTH branches and asserts each extracts the correct ProgId. Non-vacuous:
# the failure branch asserts the extracted value equals 12345 AND is > 0
# (proves error.prog_id was read, not the vacuous 0 the old helper returned),
# and the success branch asserts 67890 AND that the failure branch and
# success branch return DIFFERENT inputs' values (proves both paths ran).
# ---------------------------------------------------------------------------
$l61FailJson = '{"success":false,"error":{"code":"compile_error","prog_id":12345,"compile_errors":["This word not allowed in a Strategy."]}}'
$l61FailExtracted = Get-ProgIdFromJson -Json $l61FailJson
$l61FailExpected = 12345

$l61OkJson = '{"success":true,"prog_id":67890,"ell_path":"C:\\\\x\\\\elmodule_0_67890.ell"}'
$l61OkExtracted = Get-ProgIdFromJson -Json $l61OkJson
$l61OkExpected = 67890

# Malformed / missing-prog_id inputs must still return 0 (no false positive).
$l61EmptyExtracted = Get-ProgIdFromJson -Json '{"success":false,"error":{"code":"compile_error","compile_errors":["x"]}}'
$l61GarbageExtracted = Get-ProgIdFromJson -Json 'not-json'

$l61FailOk = ($l61FailExtracted -eq $l61FailExpected -and $l61FailExtracted -gt 0)
$l61OkOk   = ($l61OkExtracted   -eq $l61OkExpected   -and $l61OkExtracted   -gt 0)
$l61GuardOk = ($l61FailExtracted -ne $l61OkExtracted) -and ($l61EmptyExtracted -eq 0) -and ($l61GarbageExtracted -eq 0)
Record-Result -StepId "L6.1" -Description "Get-ProgIdFromJson extracts ProgId from BOTH success top-level prog_id AND failure nested error.prog_id" `
    -Passed ($l61FailOk -and $l61OkOk -and $l61GuardOk) `
    -Detail "fail_extracted=$l61FailExtracted (expected $l61FailExpected from error.prog_id) ok_extracted=$l61OkExtracted (expected $l61OkExpected from top-level prog_id) empty=$l61EmptyExtracted garbage=$l61GarbageExtracted" | Out-Null

# ---------------------------------------------------------------------------
# L6.2 -- Remove-PreExistingArtifacts now consumes prog_id (snake_case), the
# field the CLI exists JSON actually emits (handoff Sec.3 defect 2).
# Remove-PreExistingArtifacts itself calls the CLI, so this self-check
# exercises the ACTUAL production extraction helper (Get-ExistsProgIdFromJson)
# -- the same helper Remove-PreExistingArtifacts now calls -- against a
# synthetic exists JSON shaped like the real CLI output (src/tsdev_cli.cpp
# emitJson success branch emits prog_id, exists, stored_name). Non-vacuous:
# it asserts the helper yields 777 AND > 0 for a found entry, 0 for a
# not-found entry (no false cleanup of an unrelated ProgId), and 0 for a
# malformed exists=true JSON with no prog_id field (no false positive). This
# is NOT a replicated formula -- it calls the real production-test-script
# helper, so a regression in the helper is caught here.
# ---------------------------------------------------------------------------
$l62FoundJson = '{"exists":true,"prog_id":777,"stored_name":"TS_AUTO_L6_PROBE"}'
$l62NotFoundJson = '{"exists":false,"stored_name":""}'
$l62MalformedJson = '{"exists":true,"stored_name":"X"}'

# Exercise the ACTUAL production extraction helper (Get-ExistsProgIdFromJson),
# the same helper Remove-PreExistingArtifacts calls.
$l62Extracted = Get-ExistsProgIdFromJson -Json $l62FoundJson
$l62NotFoundExtracted = Get-ExistsProgIdFromJson -Json $l62NotFoundJson
$l62MalformedExtracted = Get-ExistsProgIdFromJson -Json $l62MalformedJson

$l62Expected = 777
$l62Ok = ($l62Extracted -eq $l62Expected -and $l62Extracted -gt 0)
$l62GuardOk = ($l62NotFoundExtracted -eq 0) -and ($l62MalformedExtracted -eq 0)
Record-Result -StepId "L6.2" -Description "Get-ExistsProgIdFromJson (production helper) extracts prog_id from exists JSON (found=777, not_found=0, malformed=0)" `
    -Passed ($l62Ok -and $l62GuardOk) `
    -Detail "found_extracted=$l62Extracted (expected $l62Expected) not_found_extracted=$l62NotFoundExtracted malformed_extracted=$l62MalformedExtracted (real helper; no replicated formula)" | Out-Null

# ---------------------------------------------------------------------------
# L6.3 -- Invoke-CleanupForName now captures PRE-SANITATION production state
# and computes the verdict BEFORE sanitation, so orphan evidence cannot be
# erased (handoff Sec.3 defect 3).
# Invoke-CleanupForName calls the CLI, so this self-check cannot call it
# directly without elevation. Instead it reuses the ACTUAL state/verdict
# helper logic (Get-ArtifactSanitationState + Get-CleanupVerdict -- the same
# pure helpers Invoke-CleanupForName calls) on a real orphan file
# deterministically. It creates a real orphan, runs the shared state-capture
# and verdict helpers (the same path the repaired helper uses), and asserts:
# (a) OrphanExistedBeforeSanitation is captured as true, (b) the pre-sanitation
# EllGone is FALSE (orphan present before sanitation), (c) the pre-sanitation
# verdict is FAIL (orphan evidence cannot be erased), THEN (d) sanitation
# removes the file for hygiene. Non-vacuous: it also runs the same helpers on
# a NON-orphan (file absent) and asserts OrphanExistedBeforeSanitation=false
# and verdict PASS-eligible, proving the contract distinguishes the two.
# ---------------------------------------------------------------------------
$l63Orphan = Join-Path $tmpDir "l63_orphan_elmodule_0_999.ell"
$l63Clean = Join-Path $tmpDir "l63_clean_elmodule_0_998.ell"
[System.IO.File]::WriteAllText($l63Orphan, "dummy", [System.Text.Encoding]::ASCII)

# --- Repaired contract on an ORPHAN (file present before sanitation) ---
# Reuses the ACTUAL Get-ArtifactSanitationState + Get-CleanupVerdict helpers
# (the same helpers Invoke-CleanupForName calls), not a replicated formula.
$l63OrphanState = Get-ArtifactSanitationState -EllPath $l63Orphan -XmlPath ""
$l63OrphanExisted = $l63OrphanState.EllExistedBeforeSanitation
$l63EllGonePreSan = $l63OrphanState.EllGone
$l63DeletedEntry = $true   # simulate a CLI delete that reported success...
$l63ExistsGone = $true     # ...and the entry is gone, BUT an orphan was left.
$l63VerdictPreSan = Get-CleanupVerdict -DeletedEntry $l63DeletedEntry -ExistsGone $l63ExistsGone -EllGone $l63EllGonePreSan -XmlGone $true
# Sanitation happens AFTER the verdict is fixed:
$l63SanitizedEllGone = $true
if ($l63OrphanState.EllExistedBeforeSanitation) {
    Remove-Item -Path $l63Orphan -Force -ErrorAction SilentlyContinue
    $l63SanitizedEllGone = -not (Test-Path -Path $l63Orphan -PathType Leaf)
}

# --- Repaired contract on a NON-ORPHAN (file absent before sanitation) ---
$l63CleanState = Get-ArtifactSanitationState -EllPath $l63Clean -XmlPath ""
$l63CleanOrphanExisted = $l63CleanState.OrphanExistedBeforeSanitation
$l63CleanEllGonePreSan = $l63CleanState.EllGone
$l63CleanVerdictArtifacts = Get-CleanupVerdict -DeletedEntry $true -ExistsGone $true -EllGone $l63CleanEllGonePreSan -XmlGone $true

$l63OrphanOk = ($l63OrphanExisted -eq $true) -and ($l63EllGonePreSan -eq $false) -and ($l63VerdictPreSan -eq $false) -and ($l63SanitizedEllGone -eq $true)
$l63CleanOk = ($l63CleanOrphanExisted -eq $false) -and ($l63CleanEllGonePreSan -eq $true) -and ($l63CleanVerdictArtifacts -eq $true)
$l63DistinguishOk = ($l63OrphanExisted -ne $l63CleanOrphanExisted) -and ($l63VerdictPreSan -ne $l63CleanVerdictArtifacts)
Record-Result -StepId "L6.3" -Description "Get-ArtifactSanitationState + Get-CleanupVerdict (production helpers) capture pre-sanitation orphan evidence and fix verdict before sanitation" `
    -Passed ($l63OrphanOk -and $l63CleanOk -and $l63DistinguishOk) `
    -Detail "orphan: existed_before=$l63OrphanExisted ell_gone_pre_san=$l63EllGonePreSan verdict_pre_san=$l63VerdictPreSan sanitized=$l63SanitizedEllGone | clean: existed_before=$l63CleanOrphanExisted ell_gone_pre_san=$l63CleanEllGonePreSan verdict_artifacts=$l63CleanVerdictArtifacts (real helpers; verdict fixed before sanitation)" | Out-Null
# Safety sanitation in case the contract block did not remove the file.
if (Test-Path -Path $l63Orphan -PathType Leaf) { Remove-Item -Path $l63Orphan -Force -ErrorAction SilentlyContinue }

# ---------------------------------------------------------------------------
# L6.4 -- every generic cleanup verdict now includes DeletedEntry AND
# production failure evidence (handoff Sec.3 defect 4).
# This self-check exercises the ACTUAL production verdict path -- the shared
# pure helpers (Test-DeleteSucceeded + Get-CleanupVerdict +
# Get-CleanupFailureEvidence) used by Invoke-CleanupForName, the inline L1
# cleanup, and every L4/L5/L1-per-type finally block. It asserts:
#   (a) a genuine failed CLI delete (exit 6, DeletedEntry=false) -> FAIL even
#       when the other flags are true,
#   (b) a successful delete that left an orphan (EllGone=false pre-sanitation)
#       -> FAIL,
#   (c) a fully successful cleanup -> PASS (non-vacuous: proves the formula is
#       not just always-false),
#   (d) an already-absent cleanup -- delete exit 5 (not_found) with exists
#       confirming absent + no orphan -> PASS (distinct from a genuine delete
#       failure like case a).
# The ProductionFailureEvidence string is asserted non-empty for both failure
# cases and empty for both pass cases. This is NOT a replicated formula -- it
# calls the real production-test-script helpers, so a regression in any of
# them is caught here.
# ---------------------------------------------------------------------------
# Case (a): genuine failed CLI delete (exit 6), no orphan -> must FAIL.
$l64aDeleteExit = 6
$l64aExistsGone = $true
$l64aDeletedEntry = Test-DeleteSucceeded -ExitCode $l64aDeleteExit -ExistsGone $l64aExistsGone
$l64aEvidence = Get-CleanupFailureEvidence -DeletedEntry $l64aDeletedEntry -DeleteExitCode $l64aDeleteExit -OrphanExistedBeforeSanitation $false -EllExistedBeforeSanitation $false -XmlExistedBeforeSanitation $false
$l64aVerdict = Get-CleanupVerdict -DeletedEntry $l64aDeletedEntry -ExistsGone $l64aExistsGone -EllGone $true -XmlGone $true

# Case (b): successful delete but orphan left (EllGone=false pre-sanitation) -> must FAIL.
$l64bDeleteExit = 0
$l64bExistsGone = $true
$l64bDeletedEntry = Test-DeleteSucceeded -ExitCode $l64bDeleteExit -ExistsGone $l64bExistsGone
$l64bEvidence = Get-CleanupFailureEvidence -DeletedEntry $l64bDeletedEntry -DeleteExitCode $l64bDeleteExit -OrphanExistedBeforeSanitation $true -EllExistedBeforeSanitation $true -XmlExistedBeforeSanitation $false
$l64bVerdict = Get-CleanupVerdict -DeletedEntry $l64bDeletedEntry -ExistsGone $l64bExistsGone -EllGone $false -XmlGone $true

# Case (c): fully successful cleanup -> must PASS (non-vacuous guard).
$l64cDeleteExit = 0
$l64cExistsGone = $true
$l64cDeletedEntry = Test-DeleteSucceeded -ExitCode $l64cDeleteExit -ExistsGone $l64cExistsGone
$l64cEvidence = Get-CleanupFailureEvidence -DeletedEntry $l64cDeletedEntry -DeleteExitCode $l64cDeleteExit -OrphanExistedBeforeSanitation $false -EllExistedBeforeSanitation $false -XmlExistedBeforeSanitation $false
$l64cVerdict = Get-CleanupVerdict -DeletedEntry $l64cDeletedEntry -ExistsGone $l64cExistsGone -EllGone $true -XmlGone $true

# Case (d): already-absent cleanup -- delete exit 5 (not_found) + exists
# confirms absent + no orphan -> must PASS. This is the already-absent
# success path, distinct from a genuine delete failure (case a, exit 6).
$l64dDeleteExit = 5
$l64dExistsGone = $true
$l64dDeletedEntry = Test-DeleteSucceeded -ExitCode $l64dDeleteExit -ExistsGone $l64dExistsGone
$l64dEvidence = Get-CleanupFailureEvidence -DeletedEntry $l64dDeletedEntry -DeleteExitCode $l64dDeleteExit -OrphanExistedBeforeSanitation $false -EllExistedBeforeSanitation $false -XmlExistedBeforeSanitation $false
$l64dVerdict = Get-CleanupVerdict -DeletedEntry $l64dDeletedEntry -ExistsGone $l64dExistsGone -EllGone $true -XmlGone $true

$l64aOk = ($l64aVerdict -eq $false) -and ($l64aDeletedEntry -eq $false) -and ($l64aEvidence.Length -gt 0)
$l64bOk = ($l64bVerdict -eq $false) -and ($l64bDeletedEntry -eq $true)  -and ($l64bEvidence.Length -gt 0)
$l64cOk = ($l64cVerdict -eq $true)  -and ($l64cDeletedEntry -eq $true)  -and ($l64cEvidence.Length -eq 0)
$l64dOk = ($l64dVerdict -eq $true)  -and ($l64dDeletedEntry -eq $true)  -and ($l64dEvidence.Length -eq 0)
$l64GuardOk = ($l64aVerdict -ne $l64cVerdict) -and ($l64bVerdict -ne $l64cVerdict) -and ($l64aVerdict -ne $l64dVerdict) -and ($l64dVerdict -eq $l64cVerdict)
Record-Result -StepId "L6.4" -Description "Get-CleanupVerdict + Test-DeleteSucceeded (production helpers): FAIL on genuine delete failure (exit 6) / orphan; PASS on full success / already-absent (exit 5)" `
    -Passed ($l64aOk -and $l64bOk -and $l64cOk -and $l64dOk -and $l64GuardOk) `
    -Detail "case_a(genuine_delete_exit=6)=$l64aVerdict deleted=$l64aDeletedEntry evidence='$l64aEvidence' | case_b(orphan_after_delete)=$l64bVerdict evidence='$l64bEvidence' | case_c(full_success)=$l64cVerdict evidence='$l64cEvidence' | case_d(already_absent_exit=5)=$l64dVerdict deleted=$l64dDeletedEntry evidence='$l64dEvidence' (real helpers: Test-DeleteSucceeded + Get-CleanupVerdict + Get-CleanupFailureEvidence)" | Out-Null

# ---------------------------------------------------------------------------
# Source fixture helpers for the 5 supported component types.
# All source files are written as ASCII via Write-SourceFile.
# ---------------------------------------------------------------------------
function Get-ValidInsertSource {
    param([Parameter(Mandatory)] [string]$Type, [Parameter(Mandatory)] [string]$Name)
    switch ($Type.ToLower()) {
        'strategy'   { return "Inputs: int x(0);`r`nif x > 0 then Buy next bar at market;" }
        'indicator'  { return "Inputs: int x(0);`r`nPlot1(x, `"MyPlot`");" }
        'function'   { return "$Name = 1;" }
        'tradingapp' { return "using elsystem;" }
        'showme'     { return "Inputs: int x(0);`r`nPlot1(Close, `"MyPlot`");" }
        default { throw "Get-ValidInsertSource: unknown type $Type" }
    }
}

function Get-ValidModifySource {
    param([Parameter(Mandatory)] [string]$Type, [Parameter(Mandatory)] [string]$Name)
    switch ($Type.ToLower()) {
        'strategy'   { return "Inputs: int x(1);`r`nif x > 0 then Sell next bar at market;" }
        'indicator'  { return "Inputs: int x(1);`r`nPlot1(x * 2, `"MyPlot2`");" }
        'function'   { return "$Name = 42;" }
        'tradingapp' { return "using elsystem;`r`nusing elsystem.windows.forms;" }
        'showme'     { return "Inputs: int x(1);`r`nPlot1(Close * 2, `"MyPlot2`");" }
        default { throw "Get-ValidModifySource: unknown type $Type" }
    }
}

function Get-InvalidSemanticSource {
    param([Parameter(Mandatory)] [string]$Type, [Parameter(Mandatory)] [string]$Case)
    $c = $Case.ToUpper()
    switch ($Type.ToLower()) {
        'strategy'   { if ($c -eq 'PLOT') { return 'Plot1(Close, "Test");' } }
        'indicator'  { if ($c -eq 'BUY')  { return 'Buy next bar at market;' } }
        'function'   {
            if ($c -eq 'PLOT') { return 'Plot1(Close, "Test");' }
            if ($c -eq 'BUY')  { return 'Buy next bar at market;' }
        }
        'tradingapp' {
            if ($c -eq 'BUY')  { return 'Buy next bar at market;' }
            if ($c -eq 'PLOT') { return 'Plot1(Close, "Test");' }
        }
        'showme'     { if ($c -eq 'BUY')  { return 'Buy next bar at market;' } }
    }
    throw "Get-InvalidSemanticSource: no fixture for type=$Type case=$Case"
}

# ---------------------------------------------------------------------------
# L1 per-type harness: insert + pre-existence + 5 invariants + cleanup.
# ---------------------------------------------------------------------------
function Run-L1ForType {
    param(
        [Parameter(Mandatory)] [string]$Type,
        [Parameter(Mandatory)] [string]$Name
    )
    $info = $script:TypeInfo[$Type]
    $prefix = $info.prefix
    $cliType = $info.cli
    $whType = $info.whType
    $typeCode = $script:TypeCode[$Type]

    $idBase = "L1.$typeCode"
    $source = Get-ValidInsertSource -Type $Type -Name $Name
    $srcFile = Write-SourceFile "$($idBase)_src" $source

    $preInsertFiles = @()
    if (Test-Path -Path $script:MyWorkBin -PathType Container) {
        $preInsertFiles = @(Get-ChildItem -Path $script:MyWorkBin -Filter "$prefix*" -File -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name)
    }

    $progId = 0
    $ellPath = ""
    $xmlPath = ""
    try {
        $insertStart = Get-Date
        $cliIns = Invoke-Cli -Arguments @("--json","insert",$cliType,$Name,$srcFile)
        $insertEnd = Get-Date

        $insertExitOk = ($cliIns.ExitCode -eq 0)
        $insertJsonOk = $false
        $insertDetail = "exit=$($cliIns.ExitCode)"
        try {
            $jIns = $cliIns.Stdout | ConvertFrom-Json -ErrorAction Stop
            if ($jIns.success -and $jIns.prog_id -and $jIns.prog_id -gt 0) {
                $insertJsonOk = $true
                $progId = [int]$jIns.prog_id
                $insertDetail += " prog_id=$progId"
            } else {
                $insertDetail += " (JSON success=false or prog_id missing)"
            }
        } catch {
            $insertDetail += " (JSON parse failed: $($_.Exception.Message))"
        }
        Record-Result -StepId "$($idBase).1" -Description "insert $Type via production CLI (exit 0, JSON success=true, prog_id>0)" `
            -Passed ($insertExitOk -and $insertJsonOk) -Detail $insertDetail | Out-Null

        if (-not $insertExitOk -or -not $insertJsonOk -or $progId -le 0) {
            throw "insert failed for $Type; cannot run desired-state assertions"
        }

        $ellPath = Join-Path $script:MyWorkBin "$prefix$progId.ell"
        $xmlPath = Join-Path $script:MyWorkBin "$prefix$progId.xml"

        # Pre-existence check
        $targetEllName = "$prefix$progId.ell"
        $targetXmlName = "$prefix$progId.xml"
        $ellPreExisted = $preInsertFiles -contains $targetEllName
        $xmlPreExisted = $preInsertFiles -contains $targetXmlName
        $noPreExist = (-not $ellPreExisted -and -not $xmlPreExisted)
        $preExistDetail = "pre_insert_count=$($preInsertFiles.Count) ell_preexisted=$ellPreExisted xml_preexisted=$xmlPreExisted"
        Record-Result -StepId "$($idBase).0_PREEXIST" -Description "target $targetEllName/.xml did not pre-exist before insert" `
            -Passed $noPreExist -Detail $preExistDetail | Out-Null

        # 2a: ELL exists and is fresh
        $ellExists = Test-Path -Path $ellPath -PathType Leaf
        $ellFresh = $false
        $ellDetail = "exists=$ellExists path=$ellPath"
        if ($ellExists) {
            $ellMtime = (Get-Item -Path $ellPath).LastWriteTime
            $ellFresh = ($ellMtime -ge $insertStart)
            $ellDetail += " mtime=$($ellMtime.ToString('o')) fresh=$ellFresh"
        }
        Record-Result -StepId "$($idBase).2a" -Description "ELL exists at $targetEllName and is fresh (mtime >= insert start)" `
            -Passed ($ellExists -and $ellFresh) -Detail $ellDetail | Out-Null

        # 2b: XML exists and is fresh
        $xmlExists = Test-Path -Path $xmlPath -PathType Leaf
        $xmlFresh = $false
        $xmlDetail = "exists=$xmlExists path=$xmlPath"
        if ($xmlExists) {
            $xmlMtime = (Get-Item -Path $xmlPath).LastWriteTime
            $xmlFresh = ($xmlMtime -ge $insertStart)
            $xmlDetail += " mtime=$($xmlMtime.ToString('o')) fresh=$xmlFresh"
        }
        Record-Result -StepId "$($idBase).2b" -Description "XML exists at $targetXmlName and is fresh (SymWriter::Save ran)" `
            -Passed ($xmlExists -and $xmlFresh) -Detail $xmlDetail | Out-Null

        # 2c: Parse XML GUID
        $guidString = $null
        $xmlParseOk = $false
        $xmlParseDetail = ""
        if ($xmlExists) {
            try {
                $guidString = Extract-XmlGuid -XmlPath $xmlPath -ProgId $progId -Prefix $prefix
                if ($guidString) {
                    $xmlParseOk = $true
                    $xmlParseDetail = "guid=$guidString"
                } else {
                    $xmlParseDetail = "no <symbols ... version=`"{GUID}`"> header matched"
                }
            } catch {
                $xmlParseDetail = "XML read/parse failed: $($_.Exception.Message)"
            }
        } else {
            $xmlParseDetail = "XML file absent; cannot parse"
        }
        Record-Result -StepId "$($idBase).2c" -Description "parse XML and extract version GUID from <symbols> header" `
            -Passed $xmlParseOk -Detail $xmlParseDetail | Out-Null

        # 2d: GUID byte match
        $guidMatchOk = $false
        $guidMatchDetail = ""
        if ($ellExists -and $xmlParseOk -and $guidString) {
            try {
                $ellBytes = [System.IO.File]::ReadAllBytes($ellPath)
                $guidBytes = Convert-GuidToWindowsBytes -GuidString $guidString
                $offsets = Find-BytePattern -Data $ellBytes -Pattern $guidBytes
                $count = $offsets.Count
                $guidMatchDetail = "occurrences=$count offsets=$($offsets -join ',') ellLen=$($ellBytes.Length)"
                $guidMatchOk = ($count -eq 1)
                if (-not $guidMatchOk) { $guidMatchDetail += " (expected exactly 1 occurrence)" }
            } catch {
                $guidMatchDetail = "GUID byte match failed: $($_.Exception.Message)"
            }
        } else {
            $guidMatchDetail = "skipped (ELL absent or GUID not parsed)"
        }
        Record-Result -StepId "$($idBase).2d" -Description "GUID 16 Windows mixed-endian bytes occur exactly once in COMPLETE ELL" `
            -Passed $guidMatchOk -Detail $guidMatchDetail | Out-Null

        # 2e: Source round-trip
        $rtOk = $false
        $rtDetail = ""
        $cliGs = Invoke-Cli -Arguments @("--json","get-source",$cliType,$Name)
        if ($cliGs.ExitCode -ne 0) {
            $rtDetail = "get-source exit=$($cliGs.ExitCode)"
        } else {
            $returned = Get-SourceFromJson -Json $cliGs.Stdout
            if ($returned -eq $null) {
                $rtDetail = "get-source JSON parse failed or success=false"
            } else {
                if ($returned -eq $source) {
                    $rtOk = $true
                    $rtDetail = "source matches inserted source (length=$($returned.Length))"
                } else {
                    $rtDetail = "source mismatch (inserted len=$($source.Length), returned len=$($returned.Length))"
                }
            }
        }
        Record-Result -StepId "$($idBase).2e" -Description "get-source returns source matching inserted source unchanged" `
            -Passed $rtOk -Detail $rtDetail | Out-Null
    }
    finally {
        Write-Host "--- $idBase cleanup ---" -ForegroundColor Cyan
        if ($srcFile -and (Test-Path -Path $srcFile -PathType Leaf)) {
            Remove-Item -Path $srcFile -Force -ErrorAction SilentlyContinue
        }
        $cln = Invoke-CleanupForName -Name $Name -Type $Type -WhType $whType -ProgId $progId -EllPath $ellPath -XmlPath $xmlPath
        # TVS-L.6 GREEN: verdict includes DeletedEntry and uses pre-sanitation
        # EllGone/XmlGone (from the repaired Invoke-CleanupForName), so a failed
        # CLI delete or an orphan left by a successful delete cannot pass.
        $cleanupOk = ($cln.DeletedEntry -and $cln.ExistsGone -and $cln.EllGone -and $cln.XmlGone)
        Record-Result -StepId "$($idBase).CLEANUP" -Description "cleanup removed warehouse entry and artifacts" `
            -Passed $cleanupOk -Detail $cln.Detail | Out-Null
    }
}

# ---------------------------------------------------------------------------
# L4 per-type helper functions (L4.1-L4.5).
# ---------------------------------------------------------------------------
function Run-L4-InvalidCase {
    param([Parameter(Mandatory)] [string]$Type)
    switch ($Type.ToLower()) {
        'strategy'   { return 'PLOT' }
        'indicator'  { return 'BUY' }
        'function'   { return 'PLOT' }
        'tradingapp' { return 'BUY' }
        'showme'     { return 'BUY' }
        default { throw "Run-L4-InvalidCase: unknown type $Type" }
    }
}

function Run-L4-1 {
    param(
        [Parameter(Mandatory)] [string]$Type,
        [Parameter(Mandatory)] [string]$IdBase,
        [Parameter(Mandatory)] [string]$Name
    )
    $info = $script:TypeInfo[$Type]
    $prefix = $info.prefix
    $cliType = $info.cli
    $whType = $info.whType
    $invalidCase = Run-L4-InvalidCase -Type $Type

    $srcFile = $null
    $progId = 0
    $ellPath = ""
    $xmlPath = ""
    try {
        $srcFile = Write-SourceFile "$($IdBase)_invalid" (Get-InvalidSemanticSource -Type $Type -Case $invalidCase)

        Remove-PreExistingArtifacts -Name $Name -Type $Type -WhType $whType
        $preCli = Invoke-Cli -Arguments @("--json","exists",$cliType,$Name)
        $preExists = $false
        if ($preCli.ExitCode -eq 0) { $preExists = Get-ExistsFromJson -Json $preCli.Stdout }
        Record-Result -StepId "$($IdBase)a" -Description "pre-existence: entry does not exist before failed insert" `
            -Passed (-not $preExists) -Detail "exists_before=$preExists exit=$($preCli.ExitCode)" | Out-Null

        $insertStart = Get-Date
        $cliIns = Invoke-Cli -Arguments @("--json","insert",$cliType,$Name,$srcFile)
        $insertEnd = Get-Date

        $progId = Get-ProgIdFromJson -Json $cliIns.Stdout
        if ($progId -gt 0) {
            $ellPath = Join-Path $script:MyWorkBin "$prefix$progId.ell"
            $xmlPath = Join-Path $script:MyWorkBin "$prefix$progId.xml"
        }

        $failedAsExpected = ($cliIns.ExitCode -eq 8)
        Record-Result -StepId "$($IdBase)b" -Description "insert FAILED with compile error (exit 8)" `
            -Passed $failedAsExpected -Detail "exit=$($cliIns.ExitCode) (expected 8) prog_id=$progId stderr=$($cliIns.Stderr)" | Out-Null

        $ellAbsent = $true
        if ($ellPath) { $ellAbsent = -not (Test-Path -Path $ellPath -PathType Leaf) }
        Record-Result -StepId "$($IdBase)c" -Description "no .ell file exists after failed insert" `
            -Passed $ellAbsent -Detail "ell_path=$ellPath absent=$ellAbsent" | Out-Null

        $xmlAbsent = $true
        if ($xmlPath) { $xmlAbsent = -not (Test-Path -Path $xmlPath -PathType Leaf) }
        Record-Result -StepId "$($IdBase)d" -Description "no .xml file exists after failed insert" `
            -Passed $xmlAbsent -Detail "xml_path=$xmlPath absent=$xmlAbsent" | Out-Null

        $postCli = Invoke-Cli -Arguments @("--json","exists",$cliType,$Name)
        $postExists = $false
        if ($postCli.ExitCode -eq 0) { $postExists = Get-ExistsFromJson -Json $postCli.Stdout }
        Record-Result -StepId "$($IdBase)e" -Description "warehouse entry does NOT exist after failed insert" `
            -Passed (-not $postExists) -Detail "exists_after=$postExists exit=$($postCli.ExitCode)" | Out-Null
    }
    finally {
        Write-Host "--- $IdBase cleanup ---" -ForegroundColor Cyan
        if ($srcFile -and (Test-Path -Path $srcFile -PathType Leaf)) {
            Remove-Item -Path $srcFile -Force -ErrorAction SilentlyContinue
        }
        $cln = Invoke-CleanupForName -Name $Name -Type $Type -WhType $whType -ProgId $progId -EllPath $ellPath -XmlPath $xmlPath
        # TVS-L.6 GREEN: verdict includes DeletedEntry and uses pre-sanitation
        # EllGone/XmlGone (from the repaired Invoke-CleanupForName), so a failed
        # CLI delete or an orphan left by a successful delete cannot pass.
        $cleanupOk = ($cln.DeletedEntry -and $cln.ExistsGone -and $cln.EllGone -and $cln.XmlGone)
        Record-Result -StepId "$($IdBase).CLEANUP" -Description "$IdBase cleanup removed warehouse entry and artifacts" `
            -Passed $cleanupOk -Detail $cln.Detail | Out-Null
    }
}

function Run-L4-2 {
    param(
        [Parameter(Mandatory)] [string]$Type,
        [Parameter(Mandatory)] [string]$IdBase,
        [Parameter(Mandatory)] [string]$Name
    )
    $info = $script:TypeInfo[$Type]
    $prefix = $info.prefix
    $cliType = $info.cli
    $whType = $info.whType
    $invalidCase = Run-L4-InvalidCase -Type $Type

    $insertSrcFile = $null
    $modifySrcFile = $null
    $progId = 0
    $ellPath = ""
    $xmlPath = ""
    $preEllHash = $null
    $preXmlHash = $null
    try {
        $insertSrcFile = Write-SourceFile "$($IdBase)_insert_valid" (Get-ValidInsertSource -Type $Type -Name $Name)

        Remove-PreExistingArtifacts -Name $Name -Type $Type -WhType $whType
        $preCli = Invoke-Cli -Arguments @("--json","exists",$cliType,$Name)
        $preExists = $false
        if ($preCli.ExitCode -eq 0) { $preExists = Get-ExistsFromJson -Json $preCli.Stdout }
        Record-Result -StepId "$($IdBase)a" -Description "pre-existence: entry does not exist before L4.2 insert" `
            -Passed (-not $preExists) -Detail "exists_before=$preExists" | Out-Null

        $cliIns = Invoke-Cli -Arguments @("--json","insert",$cliType,$Name,$insertSrcFile)
        $insertOk = ($cliIns.ExitCode -eq 0)
        $progId = Get-ProgIdFromJson -Json $cliIns.Stdout
        Record-Result -StepId "$($IdBase)b" -Description "insert valid $Type succeeded (exit 0, prog_id>0)" `
            -Passed ($insertOk -and $progId -gt 0) -Detail "exit=$($cliIns.ExitCode) prog_id=$progId" | Out-Null

        if (-not $insertOk -or $progId -le 0) { throw "$IdBase insert failed; cannot run modify assertions" }

        $ellPath = Join-Path $script:MyWorkBin "$prefix$progId.ell"
        $xmlPath = Join-Path $script:MyWorkBin "$prefix$progId.xml"

        $ellExistsPre = Test-Path -Path $ellPath -PathType Leaf
        $xmlExistsPre = Test-Path -Path $xmlPath -PathType Leaf
        if ($ellExistsPre) { $preEllHash = (Get-FileHash -Path $ellPath -Algorithm SHA256).Hash }
        if ($xmlExistsPre) { $preXmlHash = (Get-FileHash -Path $xmlPath -Algorithm SHA256).Hash }
        Record-Result -StepId "$($IdBase)c" -Description "pre-modify artifacts exist (ELL+XML) for hashing" `
            -Passed ($ellExistsPre -and $xmlExistsPre) -Detail "ell_exists=$ellExistsPre xml_exists=$xmlExistsPre ell_hash=$preEllHash xml_hash=$preXmlHash" | Out-Null

        $modifySrcFile = Write-SourceFile "$($IdBase)_modify_invalid" (Get-InvalidSemanticSource -Type $Type -Case $invalidCase)
        $cliMod = Invoke-Cli -Arguments @("--json","modify",$cliType,$Name,$modifySrcFile)

        $modFailedAsExpected = ($cliMod.ExitCode -eq 8)
        Record-Result -StepId "$($IdBase)d" -Description "modify FAILED with compile error (exit 8)" `
            -Passed $modFailedAsExpected -Detail "exit=$($cliMod.ExitCode) (expected 8) stderr=$($cliMod.Stderr)" | Out-Null

        $ellStillExists = Test-Path -Path $ellPath -PathType Leaf
        Record-Result -StepId "$($IdBase)e" -Description ".ell file STILL EXISTS after failed modify" `
            -Passed $ellStillExists -Detail "ell_path=$ellPath exists=$ellStillExists" | Out-Null

        $xmlStillExists = Test-Path -Path $xmlPath -PathType Leaf
        Record-Result -StepId "$($IdBase)f" -Description ".xml file STILL EXISTS after failed modify" `
            -Passed $xmlStillExists -Detail "xml_path=$xmlPath exists=$xmlStillExists" | Out-Null

        $ellHashUnchanged = $false
        if ($ellStillExists -and $preEllHash) {
            $postEllHash = (Get-FileHash -Path $ellPath -Algorithm SHA256).Hash
            $ellHashUnchanged = ($postEllHash -eq $preEllHash)
        }
        Record-Result -StepId "$($IdBase)g" -Description ".ell SHA-256 hash UNCHANGED after failed modify" `
            -Passed $ellHashUnchanged -Detail "pre=$preEllHash post=$(if ($ellStillExists) { (Get-FileHash -Path $ellPath -Algorithm SHA256).Hash } else { 'absent' })" | Out-Null

        $xmlHashUnchanged = $false
        if ($xmlStillExists -and $preXmlHash) {
            $postXmlHash = (Get-FileHash -Path $xmlPath -Algorithm SHA256).Hash
            $xmlHashUnchanged = ($postXmlHash -eq $preXmlHash)
        }
        Record-Result -StepId "$($IdBase)h" -Description ".xml SHA-256 hash UNCHANGED after failed modify" `
            -Passed $xmlHashUnchanged -Detail "pre=$preXmlHash post=$(if ($xmlStillExists) { (Get-FileHash -Path $xmlPath -Algorithm SHA256).Hash } else { 'absent' })" | Out-Null

        $postCli = Invoke-Cli -Arguments @("--json","exists",$cliType,$Name)
        $entryStillExists = $false
        if ($postCli.ExitCode -eq 0) { $entryStillExists = Get-ExistsFromJson -Json $postCli.Stdout }
        Record-Result -StepId "$($IdBase)i" -Description "warehouse entry STILL EXISTS after failed modify" `
            -Passed $entryStillExists -Detail "exists_after=$entryStillExists exit=$($postCli.ExitCode)" | Out-Null
    }
    finally {
        Write-Host "--- $IdBase cleanup ---" -ForegroundColor Cyan
        if ($insertSrcFile -and (Test-Path -Path $insertSrcFile -PathType Leaf)) { Remove-Item -Path $insertSrcFile -Force -ErrorAction SilentlyContinue }
        if ($modifySrcFile -and (Test-Path -Path $modifySrcFile -PathType Leaf)) { Remove-Item -Path $modifySrcFile -Force -ErrorAction SilentlyContinue }
        $cln = Invoke-CleanupForName -Name $Name -Type $Type -WhType $whType -ProgId $progId -EllPath $ellPath -XmlPath $xmlPath
        # TVS-L.6 GREEN: verdict includes DeletedEntry and uses pre-sanitation
        # EllGone/XmlGone (from the repaired Invoke-CleanupForName), so a failed
        # CLI delete or an orphan left by a successful delete cannot pass.
        $cleanupOk = ($cln.DeletedEntry -and $cln.ExistsGone -and $cln.EllGone -and $cln.XmlGone)
        Record-Result -StepId "$($IdBase).CLEANUP" -Description "$IdBase cleanup removed warehouse entry and artifacts" `
            -Passed $cleanupOk -Detail $cln.Detail | Out-Null
    }
}

function Run-L4-3 {
    param(
        [Parameter(Mandatory)] [string]$Type,
        [Parameter(Mandatory)] [string]$IdBase,
        [Parameter(Mandatory)] [string]$Name
    )
    $info = $script:TypeInfo[$Type]
    $prefix = $info.prefix
    $cliType = $info.cli
    $whType = $info.whType

    $insertSrcFile = $null
    $modifySrcFile = $null
    $progId = 0
    $ellPath = ""
    $xmlPath = ""
    try {
        $insertSrcFile = Write-SourceFile "$($IdBase)_insert_valid" (Get-ValidInsertSource -Type $Type -Name $Name)

        Remove-PreExistingArtifacts -Name $Name -Type $Type -WhType $whType
        $preCli = Invoke-Cli -Arguments @("--json","exists",$cliType,$Name)
        $preExists = $false
        if ($preCli.ExitCode -eq 0) { $preExists = Get-ExistsFromJson -Json $preCli.Stdout }
        Record-Result -StepId "$($IdBase)a" -Description "pre-existence: entry does not exist before L4.3 insert" `
            -Passed (-not $preExists) -Detail "exists_before=$preExists" | Out-Null

        $cliIns = Invoke-Cli -Arguments @("--json","insert",$cliType,$Name,$insertSrcFile)
        $insertOk = ($cliIns.ExitCode -eq 0)
        $progId = Get-ProgIdFromJson -Json $cliIns.Stdout
        Record-Result -StepId "$($IdBase)b" -Description "insert valid $Type succeeded (exit 0, prog_id>0)" `
            -Passed ($insertOk -and $progId -gt 0) -Detail "exit=$($cliIns.ExitCode) prog_id=$progId" | Out-Null

        if (-not $insertOk -or $progId -le 0) { throw "$IdBase insert failed; cannot run modify assertions" }

        $ellPath = Join-Path $script:MyWorkBin "$prefix$progId.ell"
        $xmlPath = Join-Path $script:MyWorkBin "$prefix$progId.xml"

        $modifySrcFile = Write-SourceFile "$($IdBase)_modify_valid" (Get-ValidModifySource -Type $Type -Name $Name)
        $modifyStart = Get-Date
        $cliMod = Invoke-Cli -Arguments @("--json","modify",$cliType,$Name,$modifySrcFile)
        $modifyEnd = Get-Date

        $modOk = ($cliMod.ExitCode -eq 0)
        Record-Result -StepId "$($IdBase)c" -Description "modify succeeded (exit 0)" `
            -Passed $modOk -Detail "exit=$($cliMod.ExitCode) stderr=$($cliMod.Stderr)" | Out-Null

        $ellExists = Test-Path -Path $ellPath -PathType Leaf
        $ellFresh = $false
        if ($ellExists) { $ellFresh = ((Get-Item -Path $ellPath).LastWriteTime -ge $modifyStart) }
        Record-Result -StepId "$($IdBase)d" -Description ".ell exists and is fresh (mtime >= modify start)" `
            -Passed ($ellExists -and $ellFresh) -Detail "exists=$ellExists fresh=$ellFresh path=$ellPath" | Out-Null

        $xmlExists = Test-Path -Path $xmlPath -PathType Leaf
        $xmlFresh = $false
        if ($xmlExists) { $xmlFresh = ((Get-Item -Path $xmlPath).LastWriteTime -ge $modifyStart) }
        Record-Result -StepId "$($IdBase)e" -Description ".xml exists and is fresh (mtime >= modify start)" `
            -Passed ($xmlExists -and $xmlFresh) -Detail "exists=$xmlExists fresh=$xmlFresh path=$xmlPath" | Out-Null

        $guid = Extract-XmlGuid -XmlPath $xmlPath -ProgId $progId -Prefix $prefix
        $guidOk = Test-GuidBytesInEll -EllPath $ellPath -GuidString $guid
        Record-Result -StepId "$($IdBase)f" -Description "XML GUID bytes occur exactly once in .ell (GUID agreement)" `
            -Passed $guidOk -Detail "guid=$guid guid_bytes_in_ell=$guidOk" | Out-Null

        $cliGs = Invoke-Cli -Arguments @("--json","get-source",$cliType,$Name)
        $returnedSrc = $null
        if ($cliGs.ExitCode -eq 0) { $returnedSrc = Get-SourceFromJson -Json $cliGs.Stdout }
        $expectedModSrc = Get-Content -Path $modifySrcFile -Raw -ErrorAction SilentlyContinue
        $rtOk = ($returnedSrc -ne $null -and $expectedModSrc -ne $null -and $returnedSrc -eq $expectedModSrc)
        Record-Result -StepId "$($IdBase)g" -Description "get-source returns source matching modified source" `
            -Passed $rtOk -Detail "exit=$($cliGs.ExitCode) returned_len=$(if ($returnedSrc) { $returnedSrc.Length } else { 0 }) expected_len=$(if ($expectedModSrc) { $expectedModSrc.Length } else { 0 })" | Out-Null
    }
    finally {
        Write-Host "--- $IdBase cleanup ---" -ForegroundColor Cyan
        if ($insertSrcFile -and (Test-Path -Path $insertSrcFile -PathType Leaf)) { Remove-Item -Path $insertSrcFile -Force -ErrorAction SilentlyContinue }
        if ($modifySrcFile -and (Test-Path -Path $modifySrcFile -PathType Leaf)) { Remove-Item -Path $modifySrcFile -Force -ErrorAction SilentlyContinue }
        $cln = Invoke-CleanupForName -Name $Name -Type $Type -WhType $whType -ProgId $progId -EllPath $ellPath -XmlPath $xmlPath
        # TVS-L.6 GREEN: verdict includes DeletedEntry and uses pre-sanitation
        # EllGone/XmlGone (from the repaired Invoke-CleanupForName), so a failed
        # CLI delete or an orphan left by a successful delete cannot pass.
        $cleanupOk = ($cln.DeletedEntry -and $cln.ExistsGone -and $cln.EllGone -and $cln.XmlGone)
        Record-Result -StepId "$($IdBase).CLEANUP" -Description "$IdBase cleanup removed warehouse entry and artifacts" `
            -Passed $cleanupOk -Detail $cln.Detail | Out-Null
    }
}

function Run-L4-4 {
    param(
        [Parameter(Mandatory)] [string]$Type,
        [Parameter(Mandatory)] [string]$IdBase,
        [Parameter(Mandatory)] [string]$Name
    )
    $info = $script:TypeInfo[$Type]
    $prefix = $info.prefix
    $cliType = $info.cli
    $whType = $info.whType

    $srcFile = $null
    $progId = 0
    $ellPath = ""
    $xmlPath = ""
    try {
        $srcFile = Write-SourceFile "$($IdBase)_upsert_new" (Get-ValidInsertSource -Type $Type -Name $Name)

        Remove-PreExistingArtifacts -Name $Name -Type $Type -WhType $whType
        $preCli = Invoke-Cli -Arguments @("--json","exists",$cliType,$Name)
        $preExists = $false
        if ($preCli.ExitCode -eq 0) { $preExists = Get-ExistsFromJson -Json $preCli.Stdout }
        Record-Result -StepId "$($IdBase)a" -Description "pre-existence: entry does not exist before upsert-new" `
            -Passed (-not $preExists) -Detail "exists_before=$preExists" | Out-Null

        $upsertStart = Get-Date
        $cliUps = Invoke-Cli -Arguments @("--json","upsert",$cliType,$Name,$srcFile)
        $upsertEnd = Get-Date

        $upsOk = ($cliUps.ExitCode -eq 0)
        $progId = Get-ProgIdFromJson -Json $cliUps.Stdout
        Record-Result -StepId "$($IdBase)b" -Description "upsert-new succeeded (exit 0, prog_id>0)" `
            -Passed ($upsOk -and $progId -gt 0) -Detail "exit=$($cliUps.ExitCode) prog_id=$progId" | Out-Null

        if (-not $upsOk -or $progId -le 0) { throw "$IdBase upsert failed; cannot run artifact assertions" }

        $ellPath = Join-Path $script:MyWorkBin "$prefix$progId.ell"
        $xmlPath = Join-Path $script:MyWorkBin "$prefix$progId.xml"

        $ellExists = Test-Path -Path $ellPath -PathType Leaf
        $ellFresh = $false
        if ($ellExists) { $ellFresh = ((Get-Item -Path $ellPath).LastWriteTime -ge $upsertStart) }
        Record-Result -StepId "$($IdBase)c" -Description ".ell exists and is fresh (mtime >= upsert start)" `
            -Passed ($ellExists -and $ellFresh) -Detail "exists=$ellExists fresh=$ellFresh path=$ellPath" | Out-Null

        $xmlExists = Test-Path -Path $xmlPath -PathType Leaf
        $xmlFresh = $false
        if ($xmlExists) { $xmlFresh = ((Get-Item -Path $xmlPath).LastWriteTime -ge $upsertStart) }
        Record-Result -StepId "$($IdBase)d" -Description ".xml exists and is fresh (mtime >= upsert start)" `
            -Passed ($xmlExists -and $xmlFresh) -Detail "exists=$xmlExists fresh=$xmlFresh path=$xmlPath" | Out-Null

        $guid = Extract-XmlGuid -XmlPath $xmlPath -ProgId $progId -Prefix $prefix
        $guidOk = Test-GuidBytesInEll -EllPath $ellPath -GuidString $guid
        Record-Result -StepId "$($IdBase)e" -Description "XML GUID bytes occur exactly once in .ell (GUID agreement)" `
            -Passed $guidOk -Detail "guid=$guid guid_bytes_in_ell=$guidOk" | Out-Null

        $cliGs = Invoke-Cli -Arguments @("--json","get-source",$cliType,$Name)
        $returnedSrc = $null
        if ($cliGs.ExitCode -eq 0) { $returnedSrc = Get-SourceFromJson -Json $cliGs.Stdout }
        $expectedSrc = Get-Content -Path $srcFile -Raw -ErrorAction SilentlyContinue
        $rtOk = ($returnedSrc -ne $null -and $expectedSrc -ne $null -and $returnedSrc -eq $expectedSrc)
        Record-Result -StepId "$($IdBase)f" -Description "get-source returns source matching upserted source" `
            -Passed $rtOk -Detail "exit=$($cliGs.ExitCode) returned_len=$(if ($returnedSrc) { $returnedSrc.Length } else { 0 }) expected_len=$(if ($expectedSrc) { $expectedSrc.Length } else { 0 })" | Out-Null
    }
    finally {
        Write-Host "--- $IdBase cleanup ---" -ForegroundColor Cyan
        if ($srcFile -and (Test-Path -Path $srcFile -PathType Leaf)) { Remove-Item -Path $srcFile -Force -ErrorAction SilentlyContinue }
        $cln = Invoke-CleanupForName -Name $Name -Type $Type -WhType $whType -ProgId $progId -EllPath $ellPath -XmlPath $xmlPath
        # TVS-L.6 GREEN: verdict includes DeletedEntry and uses pre-sanitation
        # EllGone/XmlGone (from the repaired Invoke-CleanupForName), so a failed
        # CLI delete or an orphan left by a successful delete cannot pass.
        $cleanupOk = ($cln.DeletedEntry -and $cln.ExistsGone -and $cln.EllGone -and $cln.XmlGone)
        Record-Result -StepId "$($IdBase).CLEANUP" -Description "$IdBase cleanup removed warehouse entry and artifacts" `
            -Passed $cleanupOk -Detail $cln.Detail | Out-Null
    }
}

function Run-L4-5 {
    param(
        [Parameter(Mandatory)] [string]$Type,
        [Parameter(Mandatory)] [string]$IdBase,
        [Parameter(Mandatory)] [string]$Name
    )
    $info = $script:TypeInfo[$Type]
    $prefix = $info.prefix
    $cliType = $info.cli
    $whType = $info.whType

    $insertSrcFile = $null
    $upsertSrcFile = $null
    $progId = 0
    $ellPath = ""
    $xmlPath = ""
    try {
        $insertSrcFile = Write-SourceFile "$($IdBase)_insert_valid" (Get-ValidInsertSource -Type $Type -Name $Name)

        Remove-PreExistingArtifacts -Name $Name -Type $Type -WhType $whType
        $preCli = Invoke-Cli -Arguments @("--json","exists",$cliType,$Name)
        $preExists = $false
        if ($preCli.ExitCode -eq 0) { $preExists = Get-ExistsFromJson -Json $preCli.Stdout }
        Record-Result -StepId "$($IdBase)a" -Description "pre-existence: entry does not exist before L4.5 insert" `
            -Passed (-not $preExists) -Detail "exists_before=$preExists" | Out-Null

        $cliIns = Invoke-Cli -Arguments @("--json","insert",$cliType,$Name,$insertSrcFile)
        $insertOk = ($cliIns.ExitCode -eq 0)
        $progId = Get-ProgIdFromJson -Json $cliIns.Stdout
        Record-Result -StepId "$($IdBase)b" -Description "insert valid $Type succeeded (exit 0, prog_id>0)" `
            -Passed ($insertOk -and $progId -gt 0) -Detail "exit=$($cliIns.ExitCode) prog_id=$progId" | Out-Null

        if (-not $insertOk -or $progId -le 0) { throw "$IdBase insert failed; cannot run upsert-existing assertions" }

        $ellPath = Join-Path $script:MyWorkBin "$prefix$progId.ell"
        $xmlPath = Join-Path $script:MyWorkBin "$prefix$progId.xml"

        $preEllHash = $null
        $preXmlHash = $null
        if (Test-Path -Path $ellPath -PathType Leaf) { $preEllHash = (Get-FileHash -Path $ellPath -Algorithm SHA256).Hash }
        if (Test-Path -Path $xmlPath -PathType Leaf) { $preXmlHash = (Get-FileHash -Path $xmlPath -Algorithm SHA256).Hash }

        $upsertSrcFile = Write-SourceFile "$($IdBase)_upsert_modified" (Get-ValidModifySource -Type $Type -Name $Name)
        $upsertStart = Get-Date
        $cliUps = Invoke-Cli -Arguments @("--json","upsert",$cliType,$Name,$upsertSrcFile)
        $upsertEnd = Get-Date

        $upsOk = ($cliUps.ExitCode -eq 0)
        Record-Result -StepId "$($IdBase)c" -Description "upsert-existing succeeded (exit 0)" `
            -Passed $upsOk -Detail "exit=$($cliUps.ExitCode) stderr=$($cliUps.Stderr)" | Out-Null

        $ellExists = Test-Path -Path $ellPath -PathType Leaf
        $ellFresh = $false
        if ($ellExists) { $ellFresh = ((Get-Item -Path $ellPath).LastWriteTime -ge $upsertStart) }
        Record-Result -StepId "$($IdBase)d" -Description ".ell exists and is fresh (mtime >= upsert start)" `
            -Passed ($ellExists -and $ellFresh) -Detail "exists=$ellExists fresh=$ellFresh path=$ellPath" | Out-Null

        $xmlExists = Test-Path -Path $xmlPath -PathType Leaf
        $xmlFresh = $false
        if ($xmlExists) { $xmlFresh = ((Get-Item -Path $xmlPath).LastWriteTime -ge $upsertStart) }
        Record-Result -StepId "$($IdBase)e" -Description ".xml exists and is fresh (mtime >= upsert start)" `
            -Passed ($xmlExists -and $xmlFresh) -Detail "exists=$xmlExists fresh=$xmlFresh path=$xmlPath" | Out-Null

        $guid = Extract-XmlGuid -XmlPath $xmlPath -ProgId $progId -Prefix $prefix
        $guidOk = Test-GuidBytesInEll -EllPath $ellPath -GuidString $guid
        Record-Result -StepId "$($IdBase)f" -Description "XML GUID bytes occur exactly once in .ell (GUID agreement)" `
            -Passed $guidOk -Detail "guid=$guid guid_bytes_in_ell=$guidOk" | Out-Null

        $cliGs = Invoke-Cli -Arguments @("--json","get-source",$cliType,$Name)
        $returnedSrc = $null
        if ($cliGs.ExitCode -eq 0) { $returnedSrc = Get-SourceFromJson -Json $cliGs.Stdout }
        $expectedSrc = Get-Content -Path $upsertSrcFile -Raw -ErrorAction SilentlyContinue
        $rtOk = ($returnedSrc -ne $null -and $expectedSrc -ne $null -and $returnedSrc -eq $expectedSrc)
        Record-Result -StepId "$($IdBase)g" -Description "get-source returns source matching upserted (modified) source" `
            -Passed $rtOk -Detail "exit=$($cliGs.ExitCode) returned_len=$(if ($returnedSrc) { $returnedSrc.Length } else { 0 }) expected_len=$(if ($expectedSrc) { $expectedSrc.Length } else { 0 })" | Out-Null
    }
    finally {
        Write-Host "--- $IdBase cleanup ---" -ForegroundColor Cyan
        if ($insertSrcFile -and (Test-Path -Path $insertSrcFile -PathType Leaf)) { Remove-Item -Path $insertSrcFile -Force -ErrorAction SilentlyContinue }
        if ($upsertSrcFile -and (Test-Path -Path $upsertSrcFile -PathType Leaf)) { Remove-Item -Path $upsertSrcFile -Force -ErrorAction SilentlyContinue }
        $cln = Invoke-CleanupForName -Name $Name -Type $Type -WhType $whType -ProgId $progId -EllPath $ellPath -XmlPath $xmlPath
        # TVS-L.6 GREEN: verdict includes DeletedEntry and uses pre-sanitation
        # EllGone/XmlGone (from the repaired Invoke-CleanupForName), so a failed
        # CLI delete or an orphan left by a successful delete cannot pass.
        $cleanupOk = ($cln.DeletedEntry -and $cln.ExistsGone -and $cln.EllGone -and $cln.XmlGone)
        Record-Result -StepId "$($IdBase).CLEANUP" -Description "$IdBase cleanup removed warehouse entry and artifacts" `
            -Passed $cleanupOk -Detail $cln.Detail | Out-Null
    }
}

function Run-L4ForType {
    param(
        [Parameter(Mandatory)] [string]$Type,
        [Parameter(Mandatory)] [string]$IdPrefix,
        [Parameter(Mandatory)] [string]$NameBase
    )
    Run-L4-1 -Type $Type -IdBase "$($IdPrefix)1" -Name "$($NameBase)_FAIL_INSERT_$runId"
    Run-L4-2 -Type $Type -IdBase "$($IdPrefix)2" -Name "$($NameBase)_FAIL_MODIFY_$runId"
    Run-L4-3 -Type $Type -IdBase "$($IdPrefix)3" -Name "$($NameBase)_MODIFY_OK_$runId"
    Run-L4-4 -Type $Type -IdBase "$($IdPrefix)4" -Name "$($NameBase)_UPSERT_NEW_$runId"
    Run-L4-5 -Type $Type -IdBase "$($IdPrefix)5" -Name "$($NameBase)_UPSERT_EXIST_$runId"
}

# ---------------------------------------------------------------------------
# L5 semantic-error helper.
# ---------------------------------------------------------------------------
function Run-SemanticCase {
    param(
        [Parameter(Mandatory)] [string]$Type,
        [Parameter(Mandatory)] [string]$Case,
        [Parameter(Mandatory)] [string]$Id,
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string]$ExpectedSubstr
    )
    $info = $script:TypeInfo[$Type]
    $cliType = $info.cli
    $whType = $info.whType

    $src = Get-InvalidSemanticSource -Type $Type -Case $Case
    $srcFile = $null
    $progId = 0
    try {
        $srcFile = Write-SourceFile "$($Id -replace '\.','_')_src" $src
        Remove-PreExistingArtifacts -Name $Name -Type $Type -WhType $whType
        $cli = Invoke-Cli -Arguments @("--json","insert",$cliType,$Name,$srcFile)

        $jsonObj = $null
        $jsonParseError = $null
        try {
            $jsonObj = $cli.Stdout | ConvertFrom-Json -ErrorAction Stop
        } catch {
            $jsonParseError = $_.Exception.Message
        }

        $progId = 0
        if ($jsonObj -and $jsonObj.prog_id -and [int]$jsonObj.prog_id -gt 0) {
            $progId = [int]$jsonObj.prog_id
        } else {
            $progId = Get-ProgIdFromJson -Json $cli.Stdout
        }

        $exitOk = ($cli.ExitCode -eq 8)
        $successFalse = ($jsonObj -and ($jsonObj.success -eq $false))
        $errorCodeCompile = ($jsonObj -and $jsonObj.error -and ($jsonObj.error.code -eq 'compile_error'))

        $compileErrors = ''
        if ($jsonObj -and $jsonObj.error -and $jsonObj.error.compile_errors) {
            if ($jsonObj.error.compile_errors -is [array]) {
                $compileErrors = $jsonObj.error.compile_errors -join "`n"
            } else {
                $compileErrors = [string]$jsonObj.error.compile_errors
            }
        }
        $textOk = ($compileErrors -and ($compileErrors -match [regex]::Escape($ExpectedSubstr)))

        $passed = $exitOk -and $successFalse -and $errorCodeCompile -and $textOk
        $detail = if ($jsonParseError) {
            "exit=$($cli.ExitCode) json_parse_error=`"$jsonParseError`""
        } else {
            $sanitizedCompileErrors = $compileErrors -replace '\r?\n', '; '
            "exit=$($cli.ExitCode) success=$($jsonObj.success) error_code=$($jsonObj.error.code) expected_substr=`"$ExpectedSubstr`" found_in_compile_errors=$textOk compile_errors=`"$sanitizedCompileErrors`""
        }

        Record-Result -StepId $Id -Description "insert invalid $Type ($Case) fails with exit 8 and expected error text" `
            -Passed $passed -Detail $detail | Out-Null
    }
    finally {
        Write-Host "--- $Id cleanup ---" -ForegroundColor Cyan
        if ($srcFile -and (Test-Path -Path $srcFile -PathType Leaf)) { Remove-Item -Path $srcFile -Force -ErrorAction SilentlyContinue }
        $cln = Invoke-CleanupForName -Name $Name -Type $Type -WhType $whType -ProgId $progId
        # TVS-L.6 GREEN: verdict includes DeletedEntry and uses pre-sanitation
        # EllGone/XmlGone (from the repaired Invoke-CleanupForName), so a failed
        # CLI delete or an orphan left by a successful delete cannot pass.
        $cleanupOk = ($cln.DeletedEntry -and $cln.ExistsGone -and $cln.EllGone -and $cln.XmlGone)
        Record-Result -StepId "$($Id).CLEANUP" -Description "cleanup for $Id" `
            -Passed $cleanupOk -Detail $cln.Detail | Out-Null
    }
}

# ===========================================================================
# L1 EXTENSIONS -- artifact invariants for Indicator, Function, TradingApp, ShowMe.
# ===========================================================================
Write-Host ""
Write-Host "===== L1: Artifact invariants for remaining component types =====" -ForegroundColor Cyan

$indNameL1 = "TS_AUTO_L1_IND_${runId}"
$funcNameL1 = "TS_AUTO_L1_FUNC_${runId}"
$taNameL1 = "TS_AUTO_L1_TA_${runId}"
$showmeNameL1 = "TS_AUTO_L1_SHOWME_${runId}"

Run-L1ForType -Type 'indicator' -Name $indNameL1
Run-L1ForType -Type 'function' -Name $funcNameL1
Run-L1ForType -Type 'tradingapp' -Name $taNameL1
Run-L1ForType -Type 'showme' -Name $showmeNameL1

# ===========================================================================
# L4.1 -- Failed insert (compile error) leaves no artifacts.
# ===========================================================================
Write-Host ""
Write-Host "===== L4.1: Failed insert (compile error) leaves no artifacts =====" -ForegroundColor Cyan

$l41Name = "TS_AUTO_L4_FAIL_INSERT_${runId}"
$l41ProgId = 0
$l41EllPath = ""
$l41XmlPath = ""
$l41SrcFile = ""
try {
    $l41SrcFile = Write-SourceFile "l41_invalid" (Get-InvalidSemanticSource -Type 'strategy' -Case 'PLOT')

    # Pre-existence check: assert neither entry nor artifacts exist.
    Remove-PreExistingArtifacts -Name $l41Name
    $preCli = Invoke-Cli -Arguments @("--json","exists","strategy",$l41Name)
    $preExists = $false
    if ($preCli.ExitCode -eq 0) { $preExists = Get-ExistsFromJson -Json $preCli.Stdout }
    Record-Result -StepId "L4.1a" -Description "pre-existence: entry does not exist before failed insert" `
        -Passed (-not $preExists) -Detail "exists_before=$preExists exit=$($preCli.ExitCode)" | Out-Null

    $insertStart = Get-Date
    $cliIns = Invoke-Cli -Arguments @("--json","insert","strategy",$l41Name,$l41SrcFile)
    $insertEnd = Get-Date

    # Discover ProgId if assigned (some implementations assign before compile).
    $l41ProgId = Get-ProgIdFromJson -Json $cliIns.Stdout
    if ($l41ProgId -gt 0) {
        $l41EllPath = Join-Path $MyWorkBin "elmodule_0_$l41ProgId.ell"
        $l41XmlPath = Join-Path $MyWorkBin "elmodule_0_$l41ProgId.xml"
    }

    $failedAsExpected = ($cliIns.ExitCode -ne 0 -and $cliIns.ExitCode -eq 8)
    Record-Result -StepId "L4.1b" -Description "insert FAILED with compile error (exit 8)" `
        -Passed $failedAsExpected -Detail "exit=$($cliIns.ExitCode) (expected 8) prog_id=$l41ProgId stderr=$($cliIns.Stderr)" | Out-Null

    # After failed insert: assert NO .ell file exists for the ProgId.
    $ellAbsent = $true
    if ($l41EllPath) { $ellAbsent = -not (Test-Path -Path $l41EllPath -PathType Leaf) }
    Record-Result -StepId "L4.1c" -Description "no .ell file exists after failed insert" `
        -Passed $ellAbsent -Detail "ell_path=$l41EllPath absent=$ellAbsent" | Out-Null

    # After failed insert: assert NO .xml file exists for the ProgId.
    $xmlAbsent = $true
    if ($l41XmlPath) { $xmlAbsent = -not (Test-Path -Path $l41XmlPath -PathType Leaf) }
    Record-Result -StepId "L4.1d" -Description "no .xml file exists after failed insert" `
        -Passed $xmlAbsent -Detail "xml_path=$l41XmlPath absent=$xmlAbsent" | Out-Null

    # After failed insert: assert warehouse entry does NOT exist.
    $postCli = Invoke-Cli -Arguments @("--json","exists","strategy",$l41Name)
    $postExists = $false
    if ($postCli.ExitCode -eq 0) { $postExists = Get-ExistsFromJson -Json $postCli.Stdout }
    Record-Result -StepId "L4.1e" -Description "warehouse entry does NOT exist after failed insert" `
        -Passed (-not $postExists) -Detail "exists_after=$postExists exit=$($postCli.ExitCode)" | Out-Null
}
finally {
    Write-Host "--- L4.1 cleanup ---" -ForegroundColor Cyan
    if ($l41SrcFile -and (Test-Path -Path $l41SrcFile -PathType Leaf)) {
        Remove-Item -Path $l41SrcFile -Force -ErrorAction SilentlyContinue
    }
    $cln = Invoke-CleanupForName -Name $l41Name -ProgId $l41ProgId -EllPath $l41EllPath -XmlPath $l41XmlPath
    # TVS-L.6 GREEN: verdict includes DeletedEntry and uses pre-sanitation
    # EllGone/XmlGone (from the repaired Invoke-CleanupForName), so a failed
    # CLI delete or an orphan left by a successful delete cannot pass.
    $cleanupOk = ($cln.DeletedEntry -and $cln.ExistsGone -and $cln.EllGone -and $cln.XmlGone)
    Record-Result -StepId "L4.1.CLEANUP" -Description "L4.1 cleanup removed warehouse entry and ELL/XML artifacts" `
        -Passed $cleanupOk -Detail $cln.Detail | Out-Null
}

# ===========================================================================
# L4.2 -- Failed modify (compile error) leaves old artifacts intact.
# ===========================================================================
Write-Host ""
Write-Host "===== L4.2: Failed modify (compile error) leaves old artifacts intact =====" -ForegroundColor Cyan

$l42Name = "TS_AUTO_L4_FAIL_MODIFY_${runId}"
$l42ProgId = 0
$l42EllPath = ""
$l42XmlPath = ""
$l42InsertSrcFile = ""
$l42ModifySrcFile = ""
$l42PreEllHash = $null
$l42PreXmlHash = $null
try {
    $l42InsertSrcFile = Write-SourceFile "l42_insert_valid" (Get-ValidInsertSource -Type 'strategy' -Name $l42Name)

    Remove-PreExistingArtifacts -Name $l42Name
    $preCli = Invoke-Cli -Arguments @("--json","exists","strategy",$l42Name)
    $preExists = $false
    if ($preCli.ExitCode -eq 0) { $preExists = Get-ExistsFromJson -Json $preCli.Stdout }
    Record-Result -StepId "L4.2a" -Description "pre-existence: entry does not exist before L4.2 insert" `
        -Passed (-not $preExists) -Detail "exists_before=$preExists" | Out-Null

    $cliIns = Invoke-Cli -Arguments @("--json","insert","strategy",$l42Name,$l42InsertSrcFile)
    $insertOk = ($cliIns.ExitCode -eq 0)
    $l42ProgId = Get-ProgIdFromJson -Json $cliIns.Stdout
    Record-Result -StepId "L4.2b" -Description "insert valid strategy succeeded (exit 0, prog_id>0)" `
        -Passed ($insertOk -and $l42ProgId -gt 0) -Detail "exit=$($cliIns.ExitCode) prog_id=$l42ProgId" | Out-Null

    if (-not $insertOk -or $l42ProgId -le 0) {
        throw "L4.2 insert failed; cannot run modify assertions"
    }

    $l42EllPath = Join-Path $MyWorkBin "elmodule_0_$l42ProgId.ell"
    $l42XmlPath = Join-Path $MyWorkBin "elmodule_0_$l42ProgId.xml"

    # Record SHA-256 hashes of the .ell and .xml files (pre-modify).
    $ellExistsPre = Test-Path -Path $l42EllPath -PathType Leaf
    $xmlExistsPre = Test-Path -Path $l42XmlPath -PathType Leaf
    if ($ellExistsPre) { $l42PreEllHash = (Get-FileHash -Path $l42EllPath -Algorithm SHA256).Hash }
    if ($xmlExistsPre) { $l42PreXmlHash = (Get-FileHash -Path $l42XmlPath -Algorithm SHA256).Hash }
    Record-Result -StepId "L4.2c" -Description "pre-modify artifacts exist (ELL+XML) for hashing" `
        -Passed ($ellExistsPre -and $xmlExistsPre) -Detail "ell_exists=$ellExistsPre xml_exists=$xmlExistsPre ell_hash=$l42PreEllHash xml_hash=$l42PreXmlHash" | Out-Null

    # Modify with INVALID source (Plot in Strategy -- compile error).
    $l42ModifySrcFile = Write-SourceFile "l42_modify_invalid" (Get-InvalidSemanticSource -Type 'strategy' -Case 'PLOT')
    $cliMod = Invoke-Cli -Arguments @("--json","modify","strategy",$l42Name,$l42ModifySrcFile)

    $modFailedAsExpected = ($cliMod.ExitCode -eq 8)
    Record-Result -StepId "L4.2d" -Description "modify FAILED with compile error (exit 8)" `
        -Passed $modFailedAsExpected -Detail "exit=$($cliMod.ExitCode) (expected 8) stderr=$($cliMod.Stderr)" | Out-Null

    # After failed modify: assert .ell STILL EXISTS.
    $ellStillExists = Test-Path -Path $l42EllPath -PathType Leaf
    Record-Result -StepId "L4.2e" -Description ".ell file STILL EXISTS after failed modify" `
        -Passed $ellStillExists -Detail "ell_path=$l42EllPath exists=$ellStillExists" | Out-Null

    # After failed modify: assert .xml STILL EXISTS.
    $xmlStillExists = Test-Path -Path $l42XmlPath -PathType Leaf
    Record-Result -StepId "L4.2f" -Description ".xml file STILL EXISTS after failed modify" `
        -Passed $xmlStillExists -Detail "xml_path=$l42XmlPath exists=$xmlStillExists" | Out-Null

    # After failed modify: assert .ell SHA-256 hash UNCHANGED.
    $ellHashUnchanged = $false
    if ($ellStillExists -and $l42PreEllHash) {
        $postEllHash = (Get-FileHash -Path $l42EllPath -Algorithm SHA256).Hash
        $ellHashUnchanged = ($postEllHash -eq $l42PreEllHash)
    }
    Record-Result -StepId "L4.2g" -Description ".ell SHA-256 hash UNCHANGED after failed modify" `
        -Passed $ellHashUnchanged -Detail "pre=$l42PreEllHash post=$(if ($ellStillExists) { (Get-FileHash -Path $l42EllPath -Algorithm SHA256).Hash } else { 'absent' })" | Out-Null

    # After failed modify: assert .xml SHA-256 hash UNCHANGED.
    $xmlHashUnchanged = $false
    if ($xmlStillExists -and $l42PreXmlHash) {
        $postXmlHash = (Get-FileHash -Path $l42XmlPath -Algorithm SHA256).Hash
        $xmlHashUnchanged = ($postXmlHash -eq $l42PreXmlHash)
    }
    Record-Result -StepId "L4.2h" -Description ".xml SHA-256 hash UNCHANGED after failed modify" `
        -Passed $xmlHashUnchanged -Detail "pre=$l42PreXmlHash post=$(if ($xmlStillExists) { (Get-FileHash -Path $l42XmlPath -Algorithm SHA256).Hash } else { 'absent' })" | Out-Null

    # After failed modify: assert warehouse entry STILL EXISTS.
    $postCli = Invoke-Cli -Arguments @("--json","exists","strategy",$l42Name)
    $entryStillExists = $false
    if ($postCli.ExitCode -eq 0) { $entryStillExists = Get-ExistsFromJson -Json $postCli.Stdout }
    Record-Result -StepId "L4.2i" -Description "warehouse entry STILL EXISTS after failed modify" `
        -Passed $entryStillExists -Detail "exists_after=$entryStillExists exit=$($postCli.ExitCode)" | Out-Null
}
finally {
    Write-Host "--- L4.2 cleanup ---" -ForegroundColor Cyan
    if ($l42InsertSrcFile -and (Test-Path -Path $l42InsertSrcFile -PathType Leaf)) {
        Remove-Item -Path $l42InsertSrcFile -Force -ErrorAction SilentlyContinue
    }
    if ($l42ModifySrcFile -and (Test-Path -Path $l42ModifySrcFile -PathType Leaf)) {
        Remove-Item -Path $l42ModifySrcFile -Force -ErrorAction SilentlyContinue
    }
    $cln = Invoke-CleanupForName -Name $l42Name -ProgId $l42ProgId -EllPath $l42EllPath -XmlPath $l42XmlPath
    # TVS-L.6 GREEN: verdict includes DeletedEntry and uses pre-sanitation
    # EllGone/XmlGone (from the repaired Invoke-CleanupForName), so a failed
    # CLI delete or an orphan left by a successful delete cannot pass.
    $cleanupOk = ($cln.DeletedEntry -and $cln.ExistsGone -and $cln.EllGone -and $cln.XmlGone)
    Record-Result -StepId "L4.2.CLEANUP" -Description "L4.2 cleanup removed warehouse entry and ELL/XML artifacts" `
        -Passed $cleanupOk -Detail $cln.Detail | Out-Null
}

# ===========================================================================
# L4.3 -- Successful modify produces both .ell and .xml with GUID agreement.
# ===========================================================================
Write-Host ""
Write-Host "===== L4.3: Successful modify produces both .ell and .xml with GUID agreement =====" -ForegroundColor Cyan

$l43Name = "TS_AUTO_L4_MODIFY_OK_${runId}"
$l43ProgId = 0
$l43EllPath = ""
$l43XmlPath = ""
$l43InsertSrcFile = ""
$l43ModifySrcFile = ""
try {
    $l43InsertSrcFile = Write-SourceFile "l43_insert_valid" (Get-ValidInsertSource -Type 'strategy' -Name $l43Name)

    Remove-PreExistingArtifacts -Name $l43Name
    $preCli = Invoke-Cli -Arguments @("--json","exists","strategy",$l43Name)
    $preExists = $false
    if ($preCli.ExitCode -eq 0) { $preExists = Get-ExistsFromJson -Json $preCli.Stdout }
    Record-Result -StepId "L4.3a" -Description "pre-existence: entry does not exist before L4.3 insert" `
        -Passed (-not $preExists) -Detail "exists_before=$preExists" | Out-Null

    $cliIns = Invoke-Cli -Arguments @("--json","insert","strategy",$l43Name,$l43InsertSrcFile)
    $insertOk = ($cliIns.ExitCode -eq 0)
    $l43ProgId = Get-ProgIdFromJson -Json $cliIns.Stdout
    Record-Result -StepId "L4.3b" -Description "insert valid strategy succeeded (exit 0, prog_id>0)" `
        -Passed ($insertOk -and $l43ProgId -gt 0) -Detail "exit=$($cliIns.ExitCode) prog_id=$l43ProgId" | Out-Null

    if (-not $insertOk -or $l43ProgId -le 0) {
        throw "L4.3 insert failed; cannot run modify assertions"
    }

    $l43EllPath = Join-Path $MyWorkBin "elmodule_0_$l43ProgId.ell"
    $l43XmlPath = Join-Path $MyWorkBin "elmodule_0_$l43ProgId.xml"

    # Modify with a DIFFERENT valid strategy (Inputs: int x(1); if x > 0 then Sell next bar at market;).
    $l43ModifySrcFile = Write-SourceFile "l43_modify_valid" (Get-ValidModifySource -Type 'strategy' -Name $l43Name)
    $modifyStart = Get-Date
    $cliMod = Invoke-Cli -Arguments @("--json","modify","strategy",$l43Name,$l43ModifySrcFile)
    $modifyEnd = Get-Date

    $modOk = ($cliMod.ExitCode -eq 0)
    Record-Result -StepId "L4.3c" -Description "modify succeeded (exit 0)" `
        -Passed $modOk -Detail "exit=$($cliMod.ExitCode) stderr=$($cliMod.Stderr)" | Out-Null

    # After successful modify: assert .ell exists and is fresh.
    $ellExists = Test-Path -Path $l43EllPath -PathType Leaf
    $ellFresh = $false
    if ($ellExists) {
        $ellFresh = ((Get-Item -Path $l43EllPath).LastWriteTime -ge $modifyStart)
    }
    Record-Result -StepId "L4.3d" -Description ".ell exists and is fresh (mtime >= modify start)" `
        -Passed ($ellExists -and $ellFresh) -Detail "exists=$ellExists fresh=$ellFresh path=$l43EllPath" | Out-Null

    # After successful modify: assert .xml exists and is fresh.
    $xmlExists = Test-Path -Path $l43XmlPath -PathType Leaf
    $xmlFresh = $false
    if ($xmlExists) {
        $xmlFresh = ((Get-Item -Path $l43XmlPath).LastWriteTime -ge $modifyStart)
    }
    Record-Result -StepId "L4.3e" -Description ".xml exists and is fresh (mtime >= modify start)" `
        -Passed ($xmlExists -and $xmlFresh) -Detail "exists=$xmlExists fresh=$xmlFresh path=$l43XmlPath" | Out-Null

    # Parse XML GUID, assert GUID bytes occur exactly once in .ell.
    $guid = Extract-XmlGuid -XmlPath $l43XmlPath -ProgId $l43ProgId
    $guidOk = Test-GuidBytesInEll -EllPath $l43EllPath -GuidString $guid
    Record-Result -StepId "L4.3f" -Description "XML GUID bytes occur exactly once in .ell (GUID agreement)" `
        -Passed $guidOk -Detail "guid=$guid guid_bytes_in_ell=$guidOk" | Out-Null

    # Assert source round-trips (matches the modified source).
    $cliGs = Invoke-Cli -Arguments @("--json","get-source","strategy",$l43Name)
    $returnedSrc = $null
    if ($cliGs.ExitCode -eq 0) { $returnedSrc = Get-SourceFromJson -Json $cliGs.Stdout }
    $expectedModSrc = Get-Content -Path $l43ModifySrcFile -Raw -ErrorAction SilentlyContinue
    $rtOk = ($returnedSrc -ne $null -and $expectedModSrc -ne $null -and $returnedSrc -eq $expectedModSrc)
    Record-Result -StepId "L4.3g" -Description "get-source returns source matching modified source" `
        -Passed $rtOk -Detail "exit=$($cliGs.ExitCode) returned_len=$(if ($returnedSrc) { $returnedSrc.Length } else { 0 }) expected_len=$(if ($expectedModSrc) { $expectedModSrc.Length } else { 0 })" | Out-Null
}
finally {
    Write-Host "--- L4.3 cleanup ---" -ForegroundColor Cyan
    if ($l43InsertSrcFile -and (Test-Path -Path $l43InsertSrcFile -PathType Leaf)) {
        Remove-Item -Path $l43InsertSrcFile -Force -ErrorAction SilentlyContinue
    }
    if ($l43ModifySrcFile -and (Test-Path -Path $l43ModifySrcFile -PathType Leaf)) {
        Remove-Item -Path $l43ModifySrcFile -Force -ErrorAction SilentlyContinue
    }
    $cln = Invoke-CleanupForName -Name $l43Name -ProgId $l43ProgId -EllPath $l43EllPath -XmlPath $l43XmlPath
    # TVS-L.6 GREEN: verdict includes DeletedEntry and uses pre-sanitation
    # EllGone/XmlGone (from the repaired Invoke-CleanupForName), so a failed
    # CLI delete or an orphan left by a successful delete cannot pass.
    $cleanupOk = ($cln.DeletedEntry -and $cln.ExistsGone -and $cln.EllGone -and $cln.XmlGone)
    Record-Result -StepId "L4.3.CLEANUP" -Description "L4.3 cleanup removed warehouse entry and ELL/XML artifacts" `
        -Passed $cleanupOk -Detail $cln.Detail | Out-Null
}

# ===========================================================================
# L4.4 -- Successful upsert-new produces both .ell and .xml.
# ===========================================================================
Write-Host ""
Write-Host "===== L4.4: Successful upsert-new produces both .ell and .xml =====" -ForegroundColor Cyan

$l44Name = "TS_AUTO_L4_UPSERT_NEW_${runId}"
$l44ProgId = 0
$l44EllPath = ""
$l44XmlPath = ""
$l44SrcFile = ""
try {
    $l44SrcFile = Write-SourceFile "l44_upsert_new" (Get-ValidInsertSource -Type 'strategy' -Name $l44Name)

    Remove-PreExistingArtifacts -Name $l44Name
    $preCli = Invoke-Cli -Arguments @("--json","exists","strategy",$l44Name)
    $preExists = $false
    if ($preCli.ExitCode -eq 0) { $preExists = Get-ExistsFromJson -Json $preCli.Stdout }
    Record-Result -StepId "L4.4a" -Description "pre-existence: entry does not exist before upsert-new" `
        -Passed (-not $preExists) -Detail "exists_before=$preExists" | Out-Null

    $upsertStart = Get-Date
    $cliUps = Invoke-Cli -Arguments @("--json","upsert","strategy",$l44Name,$l44SrcFile)
    $upsertEnd = Get-Date

    $upsOk = ($cliUps.ExitCode -eq 0)
    $l44ProgId = Get-ProgIdFromJson -Json $cliUps.Stdout
    Record-Result -StepId "L4.4b" -Description "upsert-new succeeded (exit 0, prog_id>0)" `
        -Passed ($upsOk -and $l44ProgId -gt 0) -Detail "exit=$($cliUps.ExitCode) prog_id=$l44ProgId" | Out-Null

    if (-not $upsOk -or $l44ProgId -le 0) {
        throw "L4.4 upsert failed; cannot run artifact assertions"
    }

    $l44EllPath = Join-Path $MyWorkBin "elmodule_0_$l44ProgId.ell"
    $l44XmlPath = Join-Path $MyWorkBin "elmodule_0_$l44ProgId.xml"

    # Assert .ell exists and is fresh.
    $ellExists = Test-Path -Path $l44EllPath -PathType Leaf
    $ellFresh = $false
    if ($ellExists) {
        $ellFresh = ((Get-Item -Path $l44EllPath).LastWriteTime -ge $upsertStart)
    }
    Record-Result -StepId "L4.4c" -Description ".ell exists and is fresh (mtime >= upsert start)" `
        -Passed ($ellExists -and $ellFresh) -Detail "exists=$ellExists fresh=$ellFresh path=$l44EllPath" | Out-Null

    # Assert .xml exists and is fresh.
    $xmlExists = Test-Path -Path $l44XmlPath -PathType Leaf
    $xmlFresh = $false
    if ($xmlExists) {
        $xmlFresh = ((Get-Item -Path $l44XmlPath).LastWriteTime -ge $upsertStart)
    }
    Record-Result -StepId "L4.4d" -Description ".xml exists and is fresh (mtime >= upsert start)" `
        -Passed ($xmlExists -and $xmlFresh) -Detail "exists=$xmlExists fresh=$xmlFresh path=$l44XmlPath" | Out-Null

    # Parse XML GUID, assert GUID bytes occur exactly once in .ell.
    $guid = Extract-XmlGuid -XmlPath $l44XmlPath -ProgId $l44ProgId
    $guidOk = Test-GuidBytesInEll -EllPath $l44EllPath -GuidString $guid
    Record-Result -StepId "L4.4e" -Description "XML GUID bytes occur exactly once in .ell (GUID agreement)" `
        -Passed $guidOk -Detail "guid=$guid guid_bytes_in_ell=$guidOk" | Out-Null

    # Assert source round-trips.
    $cliGs = Invoke-Cli -Arguments @("--json","get-source","strategy",$l44Name)
    $returnedSrc = $null
    if ($cliGs.ExitCode -eq 0) { $returnedSrc = Get-SourceFromJson -Json $cliGs.Stdout }
    $expectedSrc = Get-Content -Path $l44SrcFile -Raw -ErrorAction SilentlyContinue
    $rtOk = ($returnedSrc -ne $null -and $expectedSrc -ne $null -and $returnedSrc -eq $expectedSrc)
    Record-Result -StepId "L4.4f" -Description "get-source returns source matching upserted source" `
        -Passed $rtOk -Detail "exit=$($cliGs.ExitCode) returned_len=$(if ($returnedSrc) { $returnedSrc.Length } else { 0 }) expected_len=$(if ($expectedSrc) { $expectedSrc.Length } else { 0 })" | Out-Null
}
finally {
    Write-Host "--- L4.4 cleanup ---" -ForegroundColor Cyan
    if ($l44SrcFile -and (Test-Path -Path $l44SrcFile -PathType Leaf)) {
        Remove-Item -Path $l44SrcFile -Force -ErrorAction SilentlyContinue
    }
    $cln = Invoke-CleanupForName -Name $l44Name -ProgId $l44ProgId -EllPath $l44EllPath -XmlPath $l44XmlPath
    # TVS-L.6 GREEN: verdict includes DeletedEntry and uses pre-sanitation
    # EllGone/XmlGone (from the repaired Invoke-CleanupForName), so a failed
    # CLI delete or an orphan left by a successful delete cannot pass.
    $cleanupOk = ($cln.DeletedEntry -and $cln.ExistsGone -and $cln.EllGone -and $cln.XmlGone)
    Record-Result -StepId "L4.4.CLEANUP" -Description "L4.4 cleanup removed warehouse entry and ELL/XML artifacts" `
        -Passed $cleanupOk -Detail $cln.Detail | Out-Null
}

# ===========================================================================
# L4.5 -- Successful upsert-existing produces both .ell and .xml with GUID
# agreement.
# ===========================================================================
Write-Host ""
Write-Host "===== L4.5: Successful upsert-existing produces both .ell and .xml with GUID agreement =====" -ForegroundColor Cyan

$l45Name = "TS_AUTO_L4_UPSERT_EXIST_${runId}"
$l45ProgId = 0
$l45EllPath = ""
$l45XmlPath = ""
$l45InsertSrcFile = ""
$l45UpsertSrcFile = ""
try {
    $l45InsertSrcFile = Write-SourceFile "l45_insert_valid" (Get-ValidInsertSource -Type 'strategy' -Name $l45Name)

    Remove-PreExistingArtifacts -Name $l45Name
    $preCli = Invoke-Cli -Arguments @("--json","exists","strategy",$l45Name)
    $preExists = $false
    if ($preCli.ExitCode -eq 0) { $preExists = Get-ExistsFromJson -Json $preCli.Stdout }
    Record-Result -StepId "L4.5a" -Description "pre-existence: entry does not exist before L4.5 insert" `
        -Passed (-not $preExists) -Detail "exists_before=$preExists" | Out-Null

    $cliIns = Invoke-Cli -Arguments @("--json","insert","strategy",$l45Name,$l45InsertSrcFile)
    $insertOk = ($cliIns.ExitCode -eq 0)
    $l45ProgId = Get-ProgIdFromJson -Json $cliIns.Stdout
    Record-Result -StepId "L4.5b" -Description "insert valid strategy succeeded (exit 0, prog_id>0)" `
        -Passed ($insertOk -and $l45ProgId -gt 0) -Detail "exit=$($cliIns.ExitCode) prog_id=$l45ProgId" | Out-Null

    if (-not $insertOk -or $l45ProgId -le 0) {
        throw "L4.5 insert failed; cannot run upsert-existing assertions"
    }

    $l45EllPath = Join-Path $MyWorkBin "elmodule_0_$l45ProgId.ell"
    $l45XmlPath = Join-Path $MyWorkBin "elmodule_0_$l45ProgId.xml"

    # Record pre-upsert hashes (for reference / freshness comparison).
    $preEllHash = $null
    $preXmlHash = $null
    if (Test-Path -Path $l45EllPath -PathType Leaf) { $preEllHash = (Get-FileHash -Path $l45EllPath -Algorithm SHA256).Hash }
    if (Test-Path -Path $l45XmlPath -PathType Leaf) { $preXmlHash = (Get-FileHash -Path $l45XmlPath -Algorithm SHA256).Hash }

    # Upsert with modified valid source (Inputs: int x(1); if x > 0 then Sell next bar at market;).
    $l45UpsertSrcFile = Write-SourceFile "l45_upsert_modified" (Get-ValidModifySource -Type 'strategy' -Name $l45Name)
    $upsertStart = Get-Date
    $cliUps = Invoke-Cli -Arguments @("--json","upsert","strategy",$l45Name,$l45UpsertSrcFile)
    $upsertEnd = Get-Date

    $upsOk = ($cliUps.ExitCode -eq 0)
    Record-Result -StepId "L4.5c" -Description "upsert-existing succeeded (exit 0)" `
        -Passed $upsOk -Detail "exit=$($cliUps.ExitCode) stderr=$($cliUps.Stderr)" | Out-Null

    # After upsert: assert .ell exists and is fresh.
    $ellExists = Test-Path -Path $l45EllPath -PathType Leaf
    $ellFresh = $false
    if ($ellExists) {
        $ellFresh = ((Get-Item -Path $l45EllPath).LastWriteTime -ge $upsertStart)
    }
    Record-Result -StepId "L4.5d" -Description ".ell exists and is fresh (mtime >= upsert start)" `
        -Passed ($ellExists -and $ellFresh) -Detail "exists=$ellExists fresh=$ellFresh path=$l45EllPath" | Out-Null

    # After upsert: assert .xml exists and is fresh.
    $xmlExists = Test-Path -Path $l45XmlPath -PathType Leaf
    $xmlFresh = $false
    if ($xmlExists) {
        $xmlFresh = ((Get-Item -Path $l45XmlPath).LastWriteTime -ge $upsertStart)
    }
    Record-Result -StepId "L4.5e" -Description ".xml exists and is fresh (mtime >= upsert start)" `
        -Passed ($xmlExists -and $xmlFresh) -Detail "exists=$xmlExists fresh=$xmlFresh path=$l45XmlPath" | Out-Null

    # Parse XML GUID, assert GUID bytes occur exactly once in .ell.
    $guid = Extract-XmlGuid -XmlPath $l45XmlPath -ProgId $l45ProgId
    $guidOk = Test-GuidBytesInEll -EllPath $l45EllPath -GuidString $guid
    Record-Result -StepId "L4.5f" -Description "XML GUID bytes occur exactly once in .ell (GUID agreement)" `
        -Passed $guidOk -Detail "guid=$guid guid_bytes_in_ell=$guidOk" | Out-Null

    # Assert source round-trips (matches the modified/upserted source).
    $cliGs = Invoke-Cli -Arguments @("--json","get-source","strategy",$l45Name)
    $returnedSrc = $null
    if ($cliGs.ExitCode -eq 0) { $returnedSrc = Get-SourceFromJson -Json $cliGs.Stdout }
    $expectedSrc = Get-Content -Path $l45UpsertSrcFile -Raw -ErrorAction SilentlyContinue
    $rtOk = ($returnedSrc -ne $null -and $expectedSrc -ne $null -and $returnedSrc -eq $expectedSrc)
    Record-Result -StepId "L4.5g" -Description "get-source returns source matching upserted (modified) source" `
        -Passed $rtOk -Detail "exit=$($cliGs.ExitCode) returned_len=$(if ($returnedSrc) { $returnedSrc.Length } else { 0 }) expected_len=$(if ($expectedSrc) { $expectedSrc.Length } else { 0 })" | Out-Null
}
finally {
    Write-Host "--- L4.5 cleanup ---" -ForegroundColor Cyan
    if ($l45InsertSrcFile -and (Test-Path -Path $l45InsertSrcFile -PathType Leaf)) {
        Remove-Item -Path $l45InsertSrcFile -Force -ErrorAction SilentlyContinue
    }
    if ($l45UpsertSrcFile -and (Test-Path -Path $l45UpsertSrcFile -PathType Leaf)) {
        Remove-Item -Path $l45UpsertSrcFile -Force -ErrorAction SilentlyContinue
    }
    $cln = Invoke-CleanupForName -Name $l45Name -ProgId $l45ProgId -EllPath $l45EllPath -XmlPath $l45XmlPath
    # TVS-L.6 GREEN: verdict includes DeletedEntry and uses pre-sanitation
    # EllGone/XmlGone (from the repaired Invoke-CleanupForName), so a failed
    # CLI delete or an orphan left by a successful delete cannot pass.
    $cleanupOk = ($cln.DeletedEntry -and $cln.ExistsGone -and $cln.EllGone -and $cln.XmlGone)
    Record-Result -StepId "L4.5.CLEANUP" -Description "L4.5 cleanup removed warehouse entry and ELL/XML artifacts" `
        -Passed $cleanupOk -Detail $cln.Detail | Out-Null
}

# ===========================================================================
# L4 EXTENSIONS -- persistence validation for Indicator, Function,
# TradingApp, ShowMe.
# ===========================================================================
Write-Host ""
Write-Host "===== L4: Persistence validation for remaining component types =====" -ForegroundColor Cyan

Run-L4ForType -Type 'indicator' -IdPrefix 'L4.IND.' -NameBase 'TS_AUTO_L4_IND'
Run-L4ForType -Type 'function' -IdPrefix 'L4.FUNC.' -NameBase 'TS_AUTO_L4_FUNC'
Run-L4ForType -Type 'tradingapp' -IdPrefix 'L4.TA.' -NameBase 'TS_AUTO_L4_TA'
Run-L4ForType -Type 'showme' -IdPrefix 'L4.SHOWME.' -NameBase 'TS_AUTO_L4_SHOWME'

# ===========================================================================
# L5 SEMANTIC ERROR CASES
# ===========================================================================
Write-Host ""
Write-Host "===== L5: Type-specific semantic error cases =====" -ForegroundColor Cyan

Run-SemanticCase -Type 'strategy' -Case 'PLOT' -Id 'L5.SEM.STRAT_PLOT' -Name "TS_AUTO_L5_SEM_STRAT_PLOT_${runId}" -ExpectedSubstr 'This word not allowed in a Strategy.'
Run-SemanticCase -Type 'indicator' -Case 'BUY' -Id 'L5.SEM.IND_BUY' -Name "TS_AUTO_L5_SEM_IND_BUY_${runId}" -ExpectedSubstr 'This word not allowed in a study.'
Run-SemanticCase -Type 'function' -Case 'PLOT' -Id 'L5.SEM.FUNC_PLOT' -Name "TS_AUTO_L5_SEM_FUNC_PLOT_${runId}" -ExpectedSubstr 'This word not allowed in a function.'
Run-SemanticCase -Type 'function' -Case 'BUY' -Id 'L5.SEM.FUNC_BUY' -Name "TS_AUTO_L5_SEM_FUNC_BUY_${runId}" -ExpectedSubstr 'This word not allowed in a function.'
Run-SemanticCase -Type 'tradingapp' -Case 'BUY' -Id 'L5.SEM.TA_BUY' -Name "TS_AUTO_L5_SEM_TA_BUY_${runId}" -ExpectedSubstr 'This word not allowed in a study.'
Run-SemanticCase -Type 'tradingapp' -Case 'PLOT' -Id 'L5.SEM.TA_PLOT' -Name "TS_AUTO_L5_SEM_TA_PLOT_${runId}" -ExpectedSubstr 'This word only allowed in an Indicator, ShowMe or PaintBar study.'
Run-SemanticCase -Type 'showme' -Case 'BUY' -Id 'L5.SEM.SHOWME_BUY' -Name "TS_AUTO_L5_SEM_SHOWME_BUY_${runId}" -ExpectedSubstr 'This word not allowed in a study.'

# ===========================================================================
# TVS-L.7 -- OPT-IN RED FAULT-INJECTION ACCEPTANCE HARNESS (handoff Sec.6).
#
# Gated by -IncludeRedFaultInjection. The registered always-green suite
# (tests/run_regression.ps1) invokes this script WITHOUT that switch, so the
# opt-in fault-injection cases below are NOT registered in the always-green
# suite (handoff Sec.6: "Do not register intentionally RED cases in the
# always-green suite yet"). All L6 oracles and L1/L4/L5 normal-path
# assertions above are preserved unchanged; this block only APPENDS opt-in
# fault-injection cases.
#
# Required fault-injection cases (handoff Sec.6):
#   L7.1 Modify / read-only ELL            -- Fault A (stale-ELL modify)
#   L7.2 Upsert-existing / read-only ELL   -- Fault A variant
#   L7.3 Delete / read-only ELL            -- Fault B (orphan ELL false success)
#   L7.4 Sanitation confirmation           -- all disposable items, attributes,
#                                            and artifacts sanitized even when
#                                            assertions fail.
#
# Each case captures (handoff Sec.6 Evidence):
#   - exact disposable item identifier and component type
#   - exact read-only-attribute command sequence + captured output
#   - pre/post-operation ELL/XML SHA-256 hashes
#   - new XML GUID value + occurrence count in post-op ELL (Fault A cases)
#   - get-source return value (Fault A cases)
#   - CLI exit code, JSON success field, stderr/stdout
#   - for Fault B: warehouse-entry presence, XML presence, ELL presence
#     (orphan), and the exact "failed to delete ... error=5" line
#   - production vs sanitation state before cleanup
#
# Desired-state (handoff Sec.6):
#   L7.1/L7.2: non-success, no false verified success, existing component
#              retained. Cross-store recovery-state assertions deferred to L.9.
#   L7.3: non-success, no false success, residual artifact + Windows error
#         REPORTED (error=5 / path). Cross-store assertions deferred to L.12.
# Post-L.9 (L.9 APPROVED) desired-state status:
#   - L7.1/L7.2: GREEN detect-and-fail. Production now returns non-success
#     (exit 9 / success=false, artifact_missing) and retains the existing
#     component (ELL hash unchanged), so the desired-state (non-success AND
#     retain) PASSES. These cases are no longer a RED baseline.
# Post-L.12 (L.12 GREEN) desired-state status:
#   - L7.3: GREEN detect-and-report. Production now returns non-success
#     (deleteEntry returns false when DeleteFileA fails with a real removal
#     error such as ERROR_ACCESS_DENIED=5) AND surfaces the residual path +
#     Windows error via the "failed to delete <path> (error=5)" stderr
#     WARNING plus doDelete's error.message JSON field (exit 6 /
#     success=false) carrying the residual path. The desired-state
#     (non-success AND residual/error reported) PASSES. This is NOT "the
#     read-only ELL was physically removed" -- the read-only ELL is still
#     present at verdict time because DeleteFileA cannot remove a +R file
#     and L.12 deliberately does NOT invent attrib -R recovery; the L7.4
#     sanitation in finally (which clears +R and deletes the orphan) runs
#     AFTER the verdict is recorded. Full physical orphan removal is
#     therefore NOT the L.12 success criterion; detect-and-report is.
#
# Durable evidence: a markdown summary is written to
# $RedFaultInjectionDurablePath (default gitignored
# tmp/RE_L7_RED_FAULT_INJECTION_EVIDENCE.md; overridden to the tracked
# docs/RE/RE_L7_RED_FAULT_INJECTION_EVIDENCE.md only when -UpdateTrackedEvidence
# is set) capturing exit codes, hashes, GUID occurrence counts, get-source
# output, the read-only-command capture, and invariant failures (handoff Sec.6
# Evidence).
# ===========================================================================

# Script-scoped registry of all L7 disposable items created, so the final
# sanitation sweep (L7.4) can clean them even if a case crashes mid-flight.
# Each entry: @{ Name; Type; ProgId; EllPath; XmlPath; ReadOnlySet=$bool }
if (-not (Get-Variable -Name L7TrackedItems -Scope Script -ErrorAction SilentlyContinue)) {
    $script:L7TrackedItems = New-Object System.Collections.Generic.List[hashtable]
}
# Script-scoped accumulator for the durable evidence write.
if (-not (Get-Variable -Name L7Evidence -Scope Script -ErrorAction SilentlyContinue)) {
    $script:L7Evidence = New-Object System.Collections.Generic.List[hashtable]
}

# ---------------------------------------------------------------------------
# TVS-L.7 hardening: safe recovery-marker mechanism.
#
# Before marking a disposable item's ELL read-only, the harness atomically
# persists the EXACT disposable item identifier (name/type/ProgId/ELL/XML
# paths) under a project tmp recovery directory. At L7 startup, stale markers
# from prior crashed runs are processed: each marker is strictly validated
# (TS_AUTO_L7_* name, expected elmodule_0_<ProgId>.ell/.xml basename, path
# under the configured MyWork\Bin), then read-only is cleared, the warehouse
# entry is deleted via the CLI if it still exists, the EXACT orphan artifacts
# are removed (no directory scanning), absence is verified, and the marker is
# deleted ONLY on success. The current run's finally removes its own marker
# only after verified sanitation. The recovery directory is the ONLY path
# scanned, and only for *.json marker files; MyWork\Bin and any other
# directory are NEVER scanned or bulk-deleted.
# ---------------------------------------------------------------------------
$script:L7RecoveryDir = Join-Path $ProjectRoot "tmp\l7_recovery"

# Helper: New-L7RecoveryMarker -- atomically persists a recovery marker for
# one disposable L7 item. Writes to a .tmp sibling then renames (atomic on a
# single volume / same filesystem). Returns the final marker path. The marker
# is created BEFORE the read-only attribute is set so a crash between marker
# creation and attrib is still recoverable.
function New-L7RecoveryMarker {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string]$Type,
        [Parameter(Mandatory)] [int]$ProgId,
        [Parameter(Mandatory)] [string]$EllPath,
        [Parameter(Mandatory)] [string]$XmlPath,
        [Parameter(Mandatory)] [string]$CaseId,
        [Parameter(Mandatory)] [string]$RunId
    )
    if (-not (Test-Path -Path $script:L7RecoveryDir -PathType Container)) {
        New-Item -ItemType Directory -Force -Path $script:L7RecoveryDir | Out-Null
    }
    $marker = [pscustomobject]@{
        RunId     = $RunId
        CaseId    = $CaseId
        Name      = $Name
        Type      = $Type
        ProgId    = $ProgId
        EllPath   = $EllPath
        XmlPath   = $XmlPath
        CreatedAt = (Get-Date -Format 'o')
    }
    $json = $marker | ConvertTo-Json -Compress
    # Sanitize the marker filename (Name is already TS_AUTO_L7_* validated, but
    # defend against any path separators just in case).
    $safeName = ($Name -replace '[^A-Za-z0-9_]', '_')
    $markerPath = Join-Path $script:L7RecoveryDir "${RunId}_${CaseId}_${safeName}.json"
    $tmpPath = "$markerPath.tmp"
    [System.IO.File]::WriteAllText($tmpPath, $json, [System.Text.Encoding]::UTF8)
    # Atomic move on the same volume. Use .NET Move so a stale .tmp from a
    # prior crash is overwritten cleanly only after the new write succeeds.
    if (Test-Path -Path $markerPath -PathType Leaf) {
        Remove-Item -Path $markerPath -Force -ErrorAction SilentlyContinue
    }
    [System.IO.File]::Move($tmpPath, $markerPath)
    return $markerPath
}

# Helper: Remove-L7RecoveryMarker -- removes a marker file. Only called after
# verified sanitation (per-case finally) so a marker survives a crash that
# leaves artifacts behind for the next run's stale-marker recovery.
function Remove-L7RecoveryMarker {
    param([Parameter(Mandatory)] [string]$MarkerPath)
    if ($MarkerPath -and (Test-Path -Path $MarkerPath -PathType Leaf)) {
        Remove-Item -Path $MarkerPath -Force -ErrorAction SilentlyContinue
    }
}

# Helper: Test-L7MarkerValid -- strict validation of a parsed marker object.
# Returns $true only when EVERY field is present and matches the strict
# contracts:
#   - Name matches ^TS_AUTO_L7_[A-Z0-9_]+$
#   - Type is one of the 5 supported CLI types in $script:TypeInfo
#   - ProgId is a positive integer
#   - EllPath basename is exactly elmodule_0_<ProgId>.ell
#   - XmlPath basename is exactly elmodule_0_<ProgId>.xml
#   - EllPath and XmlPath parent directories resolve to the configured
#     $script:MyWorkBin (defense-in-depth: never act on a path outside the
#     configured bin, even if the marker was tampered with).
function Test-L7MarkerValid {
    param([Parameter(Mandatory)] $Marker)
    if ($null -eq $Marker) { return $false }
    if (-not ($Marker.Name -is [string])) { return $false }
    if ($Marker.Name -notmatch '^TS_AUTO_L7_[A-Z0-9_]+$') { return $false }
    if (-not ($Marker.Type -is [string])) { return $false }
    if (-not $script:TypeInfo.ContainsKey($Marker.Type)) { return $false }
    $progId = 0
    try { $progId = [int]$Marker.ProgId } catch { return $false }
    if ($progId -le 0) { return $false }
    if (-not ($Marker.EllPath -is [string])) { return $false }
    if (-not ($Marker.XmlPath -is [string])) { return $false }
    $expectedEllBasename = "elmodule_0_${progId}.ell"
    $expectedXmlBasename = "elmodule_0_${progId}.xml"
    if ((Split-Path -Leaf $Marker.EllPath) -ne $expectedEllBasename) { return $false }
    if ((Split-Path -Leaf $Marker.XmlPath) -ne $expectedXmlBasename) { return $false }
    # Verify both paths resolve under the configured MyWork\Bin.
    $myWorkBinFull = $null
    try { $myWorkBinFull = (Get-Item -Path $script:MyWorkBin -ErrorAction Stop).FullName.TrimEnd('\') } catch { return $false }
    foreach ($p in @($Marker.EllPath, $Marker.XmlPath)) {
        $parent = Split-Path -Parent $p
        if (-not $parent) { return $false }
        $parentFull = $null
        try { $parentFull = (Get-Item -Path $parent -ErrorAction Stop).FullName.TrimEnd('\') } catch { return $false }
        if ($parentFull -ne $myWorkBinFull) { return $false }
    }
    return $true
}

# Helper: Invoke-L7StaleMarkerRecovery -- processes stale recovery markers
# from prior crashed runs at L7 startup. Scans ONLY $script:L7RecoveryDir for
# *.json files (never MyWork\Bin or any other directory). For each valid
# marker: clears read-only on the ELL, deletes the warehouse entry via the CLI
# if it still exists, removes the EXACT orphan ELL/XML artifacts (no glob,
# no recursion), verifies absence, and deletes the marker ONLY on success.
# Invalid / unparseable / out-of-bin markers are skipped (left in place for
# manual inspection) and reported. Returns @{ Recovered; Skipped } arrays.
function Invoke-L7StaleMarkerRecovery {
    $recovered = @()
    $skipped = @()
    if (-not (Test-Path -Path $script:L7RecoveryDir -PathType Container)) {
        return @{ Recovered = $recovered; Skipped = $skipped }
    }
    # Enumerate ONLY marker JSON files in the recovery directory.
    $markerFiles = @(Get-ChildItem -Path $script:L7RecoveryDir -Filter '*.json' -File -ErrorAction SilentlyContinue)
    foreach ($mf in $markerFiles) {
        $markerPath = $mf.FullName
        $marker = $null
        try {
            $marker = ([System.IO.File]::ReadAllText($markerPath)) | ConvertFrom-Json -ErrorAction Stop
        } catch {
            $skipped += "unparseable_marker:$markerPath"
            continue
        }
        if (-not (Test-L7MarkerValid -Marker $marker)) {
            $skipped += "invalid_marker:$markerPath (strict validation failed; left for manual inspection)"
            continue
        }
        $detail = ""
        $success = $false
        try {
            # 1. Clear read-only on the ELL if it still exists.
            if (Test-Path -Path $marker.EllPath -PathType Leaf) {
                Clear-ReadOnlyAttributeExact -Path $marker.EllPath | Out-Null
            }
            # 2. Delete the warehouse entry via the CLI if it still exists.
            $entryExisted = $false
            $cliEx = Invoke-Cli -Arguments @("--json","exists",$marker.Type,$marker.Name)
            if ($cliEx.ExitCode -eq 0) { $entryExisted = Get-ExistsFromJson -Json $cliEx.Stdout }
            if ($entryExisted) {
                Invoke-Cli -Arguments @("--json","delete",$marker.Type,$marker.Name) | Out-Null
            }
            # 3. Remove the EXACT orphan artifacts (no scanning, no glob).
            if (Test-Path -Path $marker.EllPath -PathType Leaf) {
                Remove-Item -Path $marker.EllPath -Force -ErrorAction SilentlyContinue
            }
            if (Test-Path -Path $marker.XmlPath -PathType Leaf) {
                Remove-Item -Path $marker.XmlPath -Force -ErrorAction SilentlyContinue
            }
            # 4. Verify absence of the entry and both artifacts.
            $ellGone = -not (Test-Path -Path $marker.EllPath -PathType Leaf)
            $xmlGone = -not (Test-Path -Path $marker.XmlPath -PathType Leaf)
            $entryGone = $true
            if ($entryExisted) {
                $cliEx2 = Invoke-Cli -Arguments @("--json","exists",$marker.Type,$marker.Name)
                if ($cliEx2.ExitCode -eq 0) {
                    $entryGone = -not (Get-ExistsFromJson -Json $cliEx2.Stdout)
                }
            }
            if ($ellGone -and $xmlGone -and $entryGone) {
                # 5. Delete the marker ONLY on verified success.
                Remove-Item -Path $markerPath -Force -ErrorAction SilentlyContinue
                $success = $true
                $detail = "ell_gone=$ellGone xml_gone=$xmlGone entry_gone=$entryGone entry_existed=$entryExisted"
            } else {
                $detail = "incomplete ell_gone=$ellGone xml_gone=$xmlGone entry_gone=$entryGone entry_existed=$entryExisted"
            }
        } catch {
            $detail = "exception: $($_.Exception.Message)"
        }
        if ($success) {
            $recovered += "$($marker.Name):$detail"
        } else {
            $skipped += "incomplete_recovery:$markerPath $detail"
        }
    }
    return @{ Recovered = $recovered; Skipped = $skipped }
}

# ---------------------------------------------------------------------------
# Helper: Set-ReadOnlyAttributeExact -- sets the ReadOnly attribute on a file
# using the canonical Windows `attrib +R` command and CAPTURES the exact
# command sequence + stdout/stderr (handoff Sec.6: "the exact read-only-attribute
# command sequence and its captured output ... must be re-established").
# Also verifies via Get-Item .IsReadOnly that the attribute took effect.
#
# TVS-L.7 hardening: invokes attrib.exe DIRECTLY as a native command with
# separate native arguments (`& attrib +R $Path`) instead of the fragile
# `cmd /c "attrib +R \"<path>\""` string round-trip (which re-parses the
# quoted path through cmd.exe and breaks on paths containing characters cmd
# treats specially). The captured Command string is still the canonical
# `attrib +R "<path>"` form for the durable evidence record, and stdout /
# exit code / stderr are still captured exactly as before.
# Returns a hashtable with Command, Stdout, Stderr, ExitCode, IsReadOnlyAfter.
# ---------------------------------------------------------------------------
function Set-ReadOnlyAttributeExact {
    param([Parameter(Mandatory)] [string]$Path)
    $cmd = "attrib +R `"$Path`""
    $roStderrFile = Join-Path $script:tmpDir "ro_set_stderr_$([System.Guid]::NewGuid().ToString('N')).txt"
    $roStdout = ""
    $roStderr = ""
    $roExit = 0
    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        # Direct native invocation: attrib.exe receives '+R' and $Path as
        # separate native argv entries; PowerShell handles path quoting.
        $roStdout = & attrib +R $Path 2>$roStderrFile
        $roExit = $LASTEXITCODE
    } catch {
        $roStdout = "EXCEPTION: $($_.Exception.Message)"
        $roExit = -2
    } finally {
        $ErrorActionPreference = $prevEAP
    }
    if (Test-Path -Path $roStderrFile -PathType Leaf) {
        $roStderr = ([System.IO.File]::ReadAllText($roStderrFile)).TrimEnd()
        Remove-Item -Path $roStderrFile -Force -ErrorAction SilentlyContinue
    }
    $isReadOnlyAfter = $false
    if (Test-Path -Path $Path -PathType Leaf) {
        $isReadOnlyAfter = (Get-Item -Path $Path).IsReadOnly
    }
    return @{
        Command           = $cmd
        Stdout            = "$roStdout".TrimEnd()
        Stderr            = $roStderr
        ExitCode          = $roExit
        IsReadOnlyAfter   = $isReadOnlyAfter
    }
}

# ---------------------------------------------------------------------------
# Helper: Clear-ReadOnlyAttributeExact -- clears the ReadOnly attribute using
# `attrib -R` and captures the command + output. ALWAYS used in finally blocks
# to restore the file attribute (handoff Sec.6: "always restore the attribute in
# finally"). Returns the same hashtable shape as Set-ReadOnlyAttributeExact.
#
# TVS-L.7 hardening: invokes attrib.exe DIRECTLY as a native command (see
# Set-ReadOnlyAttributeExact header for rationale) instead of the fragile
# `cmd /c` string round-trip.
# ---------------------------------------------------------------------------
function Clear-ReadOnlyAttributeExact {
    param([Parameter(Mandatory)] [string]$Path)
    if (-not (Test-Path -Path $Path -PathType Leaf)) {
        return @{ Command = "attrib -R `"$Path`" (skipped: file absent)"; Stdout = ""; Stderr = ""; ExitCode = 0; IsReadOnlyAfter = $false }
    }
    $cmd = "attrib -R `"$Path`""
    $roStderrFile = Join-Path $script:tmpDir "ro_clr_stderr_$([System.Guid]::NewGuid().ToString('N')).txt"
    $roStdout = ""
    $roStderr = ""
    $roExit = 0
    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        # Direct native invocation: attrib.exe receives '-R' and $Path as
        # separate native argv entries; PowerShell handles path quoting.
        $roStdout = & attrib -R $Path 2>$roStderrFile
        $roExit = $LASTEXITCODE
    } catch {
        $roStdout = "EXCEPTION: $($_.Exception.Message)"
        $roExit = -2
    } finally {
        $ErrorActionPreference = $prevEAP
    }
    if (Test-Path -Path $roStderrFile -PathType Leaf) {
        $roStderr = ([System.IO.File]::ReadAllText($roStderrFile)).TrimEnd()
        Remove-Item -Path $roStderrFile -Force -ErrorAction SilentlyContinue
    }
    $isReadOnlyAfter = $false
    if (Test-Path -Path $Path -PathType Leaf) {
        $isReadOnlyAfter = (Get-Item -Path $Path).IsReadOnly
    }
    return @{
        Command         = $cmd
        Stdout          = "$roStdout".TrimEnd()
        Stderr          = $roStderr
        ExitCode        = $roExit
        IsReadOnlyAfter = $isReadOnlyAfter
    }
}

# ---------------------------------------------------------------------------
# Helper: Get-FileHashOrNull -- returns the SHA-256 hash of a file or $null if
# the file is absent. Avoids exceptions in hash-capture paths.
# ---------------------------------------------------------------------------
function Get-FileHashOrNull {
    param([string]$Path)
    if ($Path -and (Test-Path -Path $Path -PathType Leaf)) {
        return (Get-FileHash -Path $Path -Algorithm SHA256).Hash
    }
    return $null
}

# ---------------------------------------------------------------------------
# Helper: Get-GuidOccurrenceCountInEll -- returns the number of occurrences of
# the GUID's 16 Windows mixed-endian bytes in the ELL file (0 if ELL or GUID
# absent). Reuses the existing Convert-GuidToWindowsBytes / Find-BytePattern
# helpers (L6 oracles preserved).
# ---------------------------------------------------------------------------
function Get-GuidOccurrenceCountInEll {
    param([string]$EllPath, [string]$GuidString)
    if (-not $EllPath -or -not (Test-Path -Path $EllPath -PathType Leaf)) { return 0 }
    if (-not $GuidString) { return 0 }
    try {
        $ellBytes = [System.IO.File]::ReadAllBytes($EllPath)
        $guidBytes = Convert-GuidToWindowsBytes -GuidString $GuidString
        $offsets = Find-BytePattern -Data $ellBytes -Pattern $guidBytes
        return $offsets.Count
    } catch { }
    return 0
}

# ---------------------------------------------------------------------------
# Helper: Invoke-L7Sanitation -- the per-case finally cleanup for L7. ALWAYS
# restores the read-only attribute first (handoff Sec.6: "always restore the
# attribute in finally"), then deletes the warehouse entry via the CLI,
# verifies via exists, captures PRE-SANITATION production state (so the
# durable record shows production vs sanitation state before cleanup), and
# performs best-effort artifact removal. Escalates cleanup failure to a test
# failure. Returns a hashtable with the full cleanup + state capture.
# ---------------------------------------------------------------------------
function Invoke-L7Sanitation {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string]$Type,
        [int]$ProgId = 0,
        [string]$EllPath = "",
        [string]$XmlPath = ""
    )
    # 1. ALWAYS restore the read-only attribute first, even on failure/crash.
    $roRestore = $null
    if ($EllPath -and (Test-Path -Path $EllPath -PathType Leaf)) {
        $roRestore = Clear-ReadOnlyAttributeExact -Path $EllPath
        Write-Host "  L7 sanitation: restored read-only attribute on $EllPath (exit=$($roRestore.ExitCode) isReadOnly=$($roRestore.IsReadOnlyAfter))" -ForegroundColor DarkGray
    }

    # 2. Delete the warehouse entry via the CLI.
    $cliDel = Invoke-Cli -Arguments @("--json","delete",$Type,$Name)
    Write-Host "  L7 sanitation delete($Type $Name): exit=$($cliDel.ExitCode)" -ForegroundColor DarkGray

    # 3. Verify the entry is gone via exists.
    $existsGone = $false
    $cliEx = Invoke-Cli -Arguments @("--json","exists",$Type,$Name)
    if ($cliEx.ExitCode -eq 0) {
        $existsGone = -not (Get-ExistsFromJson -Json $cliEx.Stdout)
    }
    Write-Host "  L7 sanitation exists after delete: gone=$existsGone" -ForegroundColor $(if ($existsGone) { 'DarkGray' } else { 'Yellow' })

    # 4. Capture PRE-SANITATION production state (production vs sanitation
    #    state BEFORE any best-effort Remove-Item). Reuses the shared pure
    #    helper (L6 oracle preserved).
    $state = Get-ArtifactSanitationState -EllPath $EllPath -XmlPath $XmlPath
    $ellExistedBeforeSanitation = $state.EllExistedBeforeSanitation
    $xmlExistedBeforeSanitation = $state.XmlExistedBeforeSanitation
    $orphanExistedBeforeSanitation = $state.OrphanExistedBeforeSanitation
    $ellGonePreSan = $state.EllGone
    $xmlGonePreSan = $state.XmlGone

    # 5. Best-effort sanitation (test hygiene only; does not affect the
    #    verdict, which is fixed above by the pre-sanitation state).
    $sanitizedEllGone = $true
    $sanitizedXmlGone = $true
    if ($ellExistedBeforeSanitation) {
        Remove-Item -Path $EllPath -Force -ErrorAction SilentlyContinue
        $sanitizedEllGone = -not (Test-Path -Path $EllPath -PathType Leaf)
    }
    if ($xmlExistedBeforeSanitation) {
        Remove-Item -Path $XmlPath -Force -ErrorAction SilentlyContinue
        $sanitizedXmlGone = -not (Test-Path -Path $XmlPath -PathType Leaf)
    }
    Write-Host "  L7 sanitation artifacts: ell_gone(pre-san)=$ellGonePreSan xml_gone(pre-san)=$xmlGonePreSan orphan_before=$orphanExistedBeforeSanitation sanitized_ell=$sanitizedEllGone sanitized_xml=$sanitizedXmlGone" -ForegroundColor $(if ($ellGonePreSan -and $xmlGonePreSan) { 'DarkGray' } else { 'Yellow' })

    $deletedEntry = Test-DeleteSucceeded -ExitCode $cliDel.ExitCode -ExistsGone $existsGone
    $verdictOk = Get-CleanupVerdict -DeletedEntry $deletedEntry -ExistsGone $existsGone -EllGone $ellGonePreSan -XmlGone $xmlGonePreSan

    return @{
        DeleteExitCode                = $cliDel.ExitCode
        DeleteStderr                  = $cliDel.Stderr
        DeleteStdout                  = $cliDel.Stdout
        DeletedEntry                  = $deletedEntry
        ExistsGone                    = $existsGone
        EllExistedBeforeSanitation    = $ellExistedBeforeSanitation
        XmlExistedBeforeSanitation    = $xmlExistedBeforeSanitation
        OrphanExistedBeforeSanitation = $orphanExistedBeforeSanitation
        EllGonePreSanitation          = $ellGonePreSan
        XmlGonePreSanitation          = $xmlGonePreSan
        SanitizedEllGone              = $sanitizedEllGone
        SanitizedXmlGone              = $sanitizedXmlGone
        ReadOnlyRestored              = if ($roRestore) { -not $roRestore.IsReadOnlyAfter } else { $true }
        ReadOnlyRestoreCommand        = if ($roRestore) { $roRestore.Command } else { "" }
        ReadOnlyRestoreExitCode       = if ($roRestore) { $roRestore.ExitCode } else { 0 }
        VerdictOk                     = $verdictOk
        Detail                        = "deleted=$deletedEntry delete_exit=$($cliDel.ExitCode) exists_gone=$existsGone ell_gone_pre_san=$ellGonePreSan xml_gone_pre_san=$xmlGonePreSan orphan_before=$orphanExistedBeforeSanitation sanitized_ell=$sanitizedEllGone sanitized_xml=$sanitizedXmlGone ro_restored=$(if ($roRestore) { -not $roRestore.IsReadOnlyAfter } else { $true })"
    }
}

# ---------------------------------------------------------------------------
# Helper: Write-L7DurableEvidence -- writes the complete RED reproduction
# evidence (handoff Sec.6 Evidence) to the supplied $Path. The default path is the
# gitignored tmp/ copy; the tracked docs/RE/ file is written only when the
# operator passes -UpdateTrackedEvidence (see the param-block write-guard).
# Raw logs remain transient; this summary includes exit codes, hashes, GUID
# occurrence counts, get-source output, the read-only-command capture, and
# invariant failures. Overwrites the destination on each run with the latest
# complete evidence.
# ---------------------------------------------------------------------------
function Write-L7DurableEvidence {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [System.Collections.Generic.List[hashtable]]$Evidence
    )
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine("# TVS-L.7 RED Fault-Injection Evidence")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("> Tracked durable summary produced by ``tests/test_unknown_link_regression.ps1 -IncludeRedFaultInjection``.")
    [void]$sb.AppendLine("> Handoff Sec.6 Evidence: exit codes, hashes, GUID occurrence counts, get-source output,")
    [void]$sb.AppendLine("> read-only-command capture, and invariant failures. Raw logs remain transient.")
    [void]$sb.AppendLine("> Generated: $(Get-Date -Format 'o')")
    [void]$sb.AppendLine("")
    foreach ($e in $Evidence) {
        [void]$sb.AppendLine("## $($e.CaseId) -- $($e.CaseTitle)")
        [void]$sb.AppendLine("")
        [void]$sb.AppendLine("- **Disposable item identifier:** $($e.ItemIdentifier)")
        [void]$sb.AppendLine("- **Component type:** $($e.ComponentType)")
        [void]$sb.AppendLine("- **ProgId:** $($e.ProgId)")
        [void]$sb.AppendLine("- **Operation:** $($e.Operation)")
        [void]$sb.AppendLine("")
        [void]$sb.AppendLine("### Read-only-attribute command capture")
        [void]$sb.AppendLine("- Command: ``$($e.ReadOnlyCommand)``")
        [void]$sb.AppendLine("- Exit code: $($e.ReadOnlyExitCode)")
        [void]$sb.AppendLine("- IsReadOnly after: $($e.ReadOnlyIsReadOnlyAfter)")
        [void]$sb.AppendLine("- Stdout: ``$($e.ReadOnlyStdout)``")
        [void]$sb.AppendLine("- Stderr: ``$($e.ReadOnlyStderr)``")
        [void]$sb.AppendLine("")
        [void]$sb.AppendLine("### Pre/post-operation ELL/XML SHA-256 hashes")
        [void]$sb.AppendLine("| Artifact | Pre SHA-256 | Post SHA-256 | Changed |")
        [void]$sb.AppendLine("|----------|-------------|--------------|---------|")
        $ellChanged = if ($e.PreEllHash -and $e.PostEllHash) { ($e.PreEllHash -ne $e.PostEllHash) } else { 'n/a' }
        $xmlChanged = if ($e.PreXmlHash -and $e.PostXmlHash) { ($e.PreXmlHash -ne $e.PostXmlHash) } else { 'n/a' }
        [void]$sb.AppendLine("| ELL | $($e.PreEllHash) | $($e.PostEllHash) | $ellChanged |")
        [void]$sb.AppendLine("| XML | $($e.PreXmlHash) | $($e.PostXmlHash) | $xmlChanged |")
        [void]$sb.AppendLine("")
        if ($e.ContainsKey('NewXmlGuid')) {
            [void]$sb.AppendLine("### New XML GUID and occurrence count in post-operation ELL (Fault A)")
            [void]$sb.AppendLine("- New XML GUID: ``$($e.NewXmlGuid)``")
            [void]$sb.AppendLine("- GUID occurrence count in post-op ELL: $($e.GuidOccurrenceCountInEll)")
            [void]$sb.AppendLine("")
        }
        if ($e.ContainsKey('GetSourceExitCode')) {
            [void]$sb.AppendLine("### get-source return value (Fault A)")
            [void]$sb.AppendLine("- get-source exit code: $($e.GetSourceExitCode)")
            [void]$sb.AppendLine("- get-source JSON success: $($e.GetSourceSuccess)")
            [void]$sb.AppendLine("- get-source returned source length: $($e.GetSourceLength)")
            [void]$sb.AppendLine("- get-source returned source matches NEW (modified) source: $($e.GetSourceMatchesNew)")
            [void]$sb.AppendLine("- get-source returned source matches OLD (pre-modify) source: $($e.GetSourceMatchesOld)")
            [void]$sb.AppendLine("")
        }
        [void]$sb.AppendLine("### CLI exit / JSON success / stderr / stdout")
        [void]$sb.AppendLine("- Exit code: $($e.OpExitCode)")
        [void]$sb.AppendLine("- JSON success: $($e.OpJsonSuccess)")
        [void]$sb.AppendLine("- Stderr: ``$($e.OpStderr)``")
        [void]$sb.AppendLine("- Stdout: ``$($e.OpStdout)``")
        [void]$sb.AppendLine("")
        if ($e.ContainsKey('WarehouseEntryPresent')) {
            [void]$sb.AppendLine("### Warehouse / XML / ELL presence after delete (Fault B)")
            [void]$sb.AppendLine("- Warehouse entry present after delete: $($e.WarehouseEntryPresent)")
            [void]$sb.AppendLine("- XML present after delete: $($e.XmlPresentAfter)")
            [void]$sb.AppendLine("- ELL present after delete (orphan): $($e.EllPresentAfter)")
            [void]$sb.AppendLine("- Exact ``failed to delete ... error=5`` line captured: ``$($e.DeleteError5Line)``")
            [void]$sb.AppendLine("")
        }
        [void]$sb.AppendLine("### Desired-state invariant result")
        [void]$sb.AppendLine("- Desired-state passed: **$($e.DesiredStatePassed)**")
        [void]$sb.AppendLine("- Invariant failures: $($e.InvariantFailures)")
        [void]$sb.AppendLine("")
        [void]$sb.AppendLine("### Production vs sanitation state before cleanup")
        [void]$sb.AppendLine("- ELL existed before sanitation: $($e.EllExistedBeforeSanitation)")
        [void]$sb.AppendLine("- XML existed before sanitation: $($e.XmlExistedBeforeSanitation)")
        [void]$sb.AppendLine("- Orphan existed before sanitation: $($e.OrphanExistedBeforeSanitation)")
        [void]$sb.AppendLine("- Read-only attribute restored in finally: $($e.ReadOnlyRestored)")
        [void]$sb.AppendLine("- Cleanup verdict (pre-sanitation): $($e.CleanupVerdictOk)")
        [void]$sb.AppendLine("")
        [void]$sb.AppendLine("---")
        [void]$sb.AppendLine("")
    }
    $dir = Split-Path -Parent $Path
    if ($dir -and -not (Test-Path -Path $dir -PathType Container)) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
    }
    [System.IO.File]::WriteAllText($Path, $sb.ToString(), [System.Text.Encoding]::UTF8)
    Write-Host "  L7 durable evidence written to: $Path" -ForegroundColor Cyan
}

# ---------------------------------------------------------------------------
# L7.1 -- Modify / read-only ELL (Fault A: stale-ELL modify).
# Desired-state (handoff Sec.6 case 1):
#   - non-success and no false verified success
#   - existing component retained (ELL hash unchanged)
#   - cross-store recovery-state assertions deferred to L.9
# Post-L.9 (L.9 APPROVED): GREEN detect-and-fail. Production now returns
# non-success (exit 9 / success=false, artifact_missing) and retains the
# existing component (ELL hash unchanged), so the desired-state PASSES.
# ---------------------------------------------------------------------------
function Run-L7-ModifyReadOnly {
    param([Parameter(Mandatory)] [string]$Name)
    $Type = 'strategy'
    $info = $script:TypeInfo[$Type]
    $prefix = $info.prefix
    $cliType = $info.cli
    $whType = $info.whType
    $idBase = 'L7.1'

    $insertSrcFile = $null
    $modifySrcFile = $null
    $progId = 0
    $ellPath = ""
    $xmlPath = ""
    $preEllHash = $null
    $preXmlHash = $null
    $preGuid = $null
    $readOnlyCapture = $null
    $evidence = $null
    $markerPath = $null
    try {
        $insertSrcFile = Write-SourceFile "l71_insert_valid" (Get-ValidInsertSource -Type $Type -Name $Name)
        $modifySrcFile = Write-SourceFile "l71_modify_valid" (Get-ValidModifySource -Type $Type -Name $Name)

        Remove-PreExistingArtifacts -Name $Name -Type $Type -WhType $whType
        $preCli = Invoke-Cli -Arguments @("--json","exists",$cliType,$Name)
        $preExists = $false
        if ($preCli.ExitCode -eq 0) { $preExists = Get-ExistsFromJson -Json $preCli.Stdout }
        Record-Result -StepId "$($idBase)a" -Description "pre-existence: entry does not exist before L7.1 insert" `
            -Passed (-not $preExists) -Detail "exists_before=$preExists exit=$($preCli.ExitCode)" | Out-Null

        $cliIns = Invoke-Cli -Arguments @("--json","insert",$cliType,$Name,$insertSrcFile)
        $insertOk = ($cliIns.ExitCode -eq 0)
        $progId = Get-ProgIdFromJson -Json $cliIns.Stdout
        Record-Result -StepId "$($idBase)b" -Description "insert valid Strategy succeeded (exit 0, prog_id>0)" `
            -Passed ($insertOk -and $progId -gt 0) -Detail "exit=$($cliIns.ExitCode) prog_id=$progId" | Out-Null
        if (-not $insertOk -or $progId -le 0) { throw "$idBase insert failed; cannot run read-only-ELL assertions" }

        $ellPath = Join-Path $script:MyWorkBin "$prefix$progId.ell"
        $xmlPath = Join-Path $script:MyWorkBin "$prefix$progId.xml"
        $script:L7TrackedItems.Add(@{ Name=$Name; Type=$Type; ProgId=$progId; EllPath=$ellPath; XmlPath=$xmlPath; ReadOnlySet=$false }) | Out-Null

        # Pre-modify artifacts + hashes + GUID.
        $preEllHash = Get-FileHashOrNull -Path $ellPath
        $preXmlHash = Get-FileHashOrNull -Path $xmlPath
        $preGuid = Extract-XmlGuid -XmlPath $xmlPath -ProgId $progId -Prefix $prefix
        Record-Result -StepId "$($idBase)c" -Description "pre-modify ELL/XML exist with SHA-256 hashes + GUID captured" `
            -Passed ($preEllHash -ne $null -and $preXmlHash -ne $null) -Detail "ell_hash=$preEllHash xml_hash=$preXmlHash pre_guid=$preGuid" | Out-Null

        # TVS-L.7 hardening: atomically persist a recovery marker BEFORE
        # setting the ELL read-only, so a crash between marker creation and
        # the attrib call (or any later crash) is recoverable at the next
        # L7 startup via Invoke-L7StaleMarkerRecovery.
        $markerPath = New-L7RecoveryMarker -Name $Name -Type $Type -ProgId $progId -EllPath $ellPath -XmlPath $xmlPath -CaseId $idBase -RunId $runId

        # Mark ONLY the ELL read-only (handoff Sec.6 case 1: "mark only its ELL read-only").
        $readOnlyCapture = Set-ReadOnlyAttributeExact -Path $ellPath
        $roOk = ($readOnlyCapture.ExitCode -eq 0 -and $readOnlyCapture.IsReadOnlyAfter -eq $true)
        Record-Result -StepId "$($idBase)d" -Description "read-only attribute set on ELL only (attrib +R captured)" `
            -Passed $roOk -Detail "cmd='$($readOnlyCapture.Command)' exit=$($readOnlyCapture.ExitCode) isReadOnly=$($readOnlyCapture.IsReadOnlyAfter) stdout='$($readOnlyCapture.Stdout)' stderr='$($readOnlyCapture.Stderr)' marker=$markerPath" | Out-Null
        # Mark tracked item as read-only-set so the final sweep can clear it.
        for ($i = 0; $i -lt $script:L7TrackedItems.Count; $i++) {
            if ($script:L7TrackedItems[$i].ProgId -eq $progId) {
                $item = $script:L7TrackedItems[$i]
                $item.ReadOnlySet = $true
                $script:L7TrackedItems[$i] = $item
            }
        }

        # Modify with valid changed source.
        $cliMod = Invoke-Cli -Arguments @("--json","modify",$cliType,$Name,$modifySrcFile)
        $opExit = $cliMod.ExitCode
        $opJsonSuccess = $null
        try {
            $jMod = $cliMod.Stdout | ConvertFrom-Json -ErrorAction Stop
            $opJsonSuccess = [bool]$jMod.success
        } catch { $opJsonSuccess = $null }
        $reportedSuccess = ($opExit -eq 0 -and $opJsonSuccess -eq $true)
        Record-Result -StepId "$($idBase)e" -Description "modify executed; captured exit/JSON success/stderr/stdout" `
            -Passed $true -Detail "exit=$opExit json_success=$opJsonSuccess reported_success=$reportedSuccess stderr='$($cliMod.Stderr)' stdout='$($cliMod.Stdout)'" | Out-Null

        # Post-modify hashes + new XML GUID + GUID occurrence count in ELL.
        $postEllHash = Get-FileHashOrNull -Path $ellPath
        $postXmlHash = Get-FileHashOrNull -Path $xmlPath
        $newGuid = Extract-XmlGuid -XmlPath $xmlPath -ProgId $progId -Prefix $prefix
        $guidOccInEll = Get-GuidOccurrenceCountInEll -EllPath $ellPath -GuidString $newGuid
        Record-Result -StepId "$($idBase)f" -Description "post-modify ELL/XML hashes + new XML GUID + GUID occurrence count in ELL captured" `
            -Passed $true -Detail "post_ell_hash=$postEllHash post_xml_hash=$postXmlHash new_guid=$newGuid guid_occ_in_ell=$guidOccInEll" | Out-Null

        # get-source return value (handoff Sec.6: must be re-established for Fault A).
        $cliGs = Invoke-Cli -Arguments @("--json","get-source",$cliType,$Name)
        $gsSuccess = $null; $gsSrc = $null
        try {
            $jGs = $cliGs.Stdout | ConvertFrom-Json -ErrorAction Stop
            $gsSuccess = [bool]$jGs.success
            $gsSrc = [string]$jGs.source
        } catch { $gsSuccess = $null; $gsSrc = $null }
        $oldSrc = Get-Content -Path $insertSrcFile -Raw -ErrorAction SilentlyContinue
        $newSrc = Get-Content -Path $modifySrcFile -Raw -ErrorAction SilentlyContinue
        $gsMatchesNew = ($gsSrc -ne $null -and $newSrc -ne $null -and $gsSrc -eq $newSrc)
        $gsMatchesOld = ($gsSrc -ne $null -and $oldSrc -ne $null -and $gsSrc -eq $oldSrc)
        Record-Result -StepId "$($idBase)g" -Description "get-source return value captured (Fault A evidence; cross-store assertions deferred to L.9)" `
            -Passed $true -Detail "exit=$($cliGs.ExitCode) success=$gsSuccess src_len=$(if ($gsSrc) { $gsSrc.Length } else { 0 }) matches_new=$gsMatchesNew matches_old=$gsMatchesOld" | Out-Null

        # DESIRED-STATE ASSERTIONS (handoff Sec.6 case 1). Post-L.9 (L.9
        # APPROVED): GREEN detect-and-fail -- production returns non-success
        # (exit 9 / success=false) and retains the existing component.
        $nonSuccess = (-not $reportedSuccess)
        $ellHashUnchanged = ($preEllHash -ne $null -and $postEllHash -ne $null -and $preEllHash -eq $postEllHash)
        $existingComponentRetained = ($ellHashUnchanged -and (Test-Path -Path $ellPath -PathType Leaf))

        Record-Result -StepId "$($idBase)h" -Description "DESIRED-STATE: modify did NOT report verified success (non-success)" `
            -Passed $nonSuccess -Detail "reported_success=$reportedSuccess (exit=$opExit json_success=$opJsonSuccess) -- GREEN detect-and-fail after L.9" | Out-Null

        Record-Result -StepId "$($idBase)i" -Description "DESIRED-STATE: existing component retained (ELL hash unchanged, ELL present)" `
            -Passed $existingComponentRetained -Detail "pre_ell=$preEllHash post_ell=$postEllHash unchanged=$ellHashUnchanged ell_present=$(Test-Path -Path $ellPath -PathType Leaf)" | Out-Null

        $desiredStatePassed = ($nonSuccess -and $existingComponentRetained)
        $invariantFailures = @()
        if (-not $nonSuccess) { $invariantFailures += "false_verified_success(exit=$opExit,json_success=$opJsonSuccess)" }
        if (-not $existingComponentRetained) { $invariantFailures += "existing_component_not_retained(ell_hash_changed_or_absent)" }
        Record-Result -StepId "$($idBase)j" -Description "DESIRED-STATE combined: non-success AND existing component retained (Fault A desired-state)" `
            -Passed $desiredStatePassed -Detail "desired_passed=$desiredStatePassed invariant_failures=$($invariantFailures -join '; ') -- GREEN detect-and-fail after L.9" | Out-Null

        $evidence = @{
            CaseId                = $idBase
            CaseTitle             = "Modify / read-only ELL (Fault A: stale-ELL modify; detect-and-fail; GREEN after L.9)"
            ItemIdentifier        = $Name
            ComponentType         = $Type
            ProgId                = $progId
            Operation             = "modify"
            ReadOnlyCommand       = $readOnlyCapture.Command
            ReadOnlyExitCode      = $readOnlyCapture.ExitCode
            ReadOnlyIsReadOnlyAfter = $readOnlyCapture.IsReadOnlyAfter
            ReadOnlyStdout        = $readOnlyCapture.Stdout
            ReadOnlyStderr        = $readOnlyCapture.Stderr
            PreEllHash            = $preEllHash
            PreXmlHash            = $preXmlHash
            PostEllHash           = $postEllHash
            PostXmlHash           = $postXmlHash
            NewXmlGuid            = $newGuid
            GuidOccurrenceCountInEll = $guidOccInEll
            GetSourceExitCode     = $cliGs.ExitCode
            GetSourceSuccess      = $gsSuccess
            GetSourceLength       = if ($gsSrc) { $gsSrc.Length } else { 0 }
            GetSourceMatchesNew   = $gsMatchesNew
            GetSourceMatchesOld   = $gsMatchesOld
            OpExitCode            = $opExit
            OpJsonSuccess         = $opJsonSuccess
            OpStderr              = $cliMod.Stderr
            OpStdout              = $cliMod.Stdout
            DesiredStatePassed    = $desiredStatePassed
            InvariantFailures     = ($invariantFailures -join '; ')
        }
    }
    finally {
        Write-Host "--- $idBase cleanup ---" -ForegroundColor Cyan
        if ($insertSrcFile -and (Test-Path -Path $insertSrcFile -PathType Leaf)) { Remove-Item -Path $insertSrcFile -Force -ErrorAction SilentlyContinue }
        if ($modifySrcFile -and (Test-Path -Path $modifySrcFile -PathType Leaf)) { Remove-Item -Path $modifySrcFile -Force -ErrorAction SilentlyContinue }
        $cln = Invoke-L7Sanitation -Name $Name -Type $Type -ProgId $progId -EllPath $ellPath -XmlPath $xmlPath
        Record-Result -StepId "$($idBase).CLEANUP" -Description "$idBase cleanup restored read-only attribute, removed warehouse entry and artifacts" `
            -Passed ($cln.VerdictOk -and $cln.ReadOnlyRestored) -Detail $cln.Detail | Out-Null
        # TVS-L.7 hardening: remove this run's recovery marker ONLY after
        # verified sanitation (entry gone, both artifacts gone, read-only
        # restored). If sanitation did not verify, the marker is left in place
        # for the next run's Invoke-L7StaleMarkerRecovery to process.
        $markerSanitized = ($cln.ExistsGone -and $cln.SanitizedEllGone -and $cln.SanitizedXmlGone -and $cln.ReadOnlyRestored)
        if ($markerPath -and $markerSanitized) {
            Remove-L7RecoveryMarker -MarkerPath $markerPath
        }
        if ($evidence) {
            $evidence.EllExistedBeforeSanitation    = $cln.EllExistedBeforeSanitation
            $evidence.XmlExistedBeforeSanitation    = $cln.XmlExistedBeforeSanitation
            $evidence.OrphanExistedBeforeSanitation = $cln.OrphanExistedBeforeSanitation
            $evidence.ReadOnlyRestored              = $cln.ReadOnlyRestored
            $evidence.CleanupVerdictOk              = $cln.VerdictOk
            $script:L7Evidence.Add($evidence) | Out-Null
        }
    }
}

# ---------------------------------------------------------------------------
# L7.2 -- Upsert-existing / read-only ELL (Fault A variant).
# Desired-state (handoff Sec.6 case 2): same as case 1 -- non-success, no false
# verified success, existing component retained. Cross-store assertions
# deferred to L.9. Post-L.9 (L.9 APPROVED): GREEN detect-and-fail --
# production returns non-success (exit 9 / success=false, artifact_missing)
# and retains the existing component (ELL hash unchanged), so the
# desired-state PASSES.
# ---------------------------------------------------------------------------
function Run-L7-UpsertExistingReadOnly {
    param([Parameter(Mandatory)] [string]$Name)
    $Type = 'strategy'
    $info = $script:TypeInfo[$Type]
    $prefix = $info.prefix
    $cliType = $info.cli
    $whType = $info.whType
    $idBase = 'L7.2'

    $insertSrcFile = $null
    $upsertSrcFile = $null
    $progId = 0
    $ellPath = ""
    $xmlPath = ""
    $preEllHash = $null
    $preXmlHash = $null
    $readOnlyCapture = $null
    $evidence = $null
    $markerPath = $null
    try {
        $insertSrcFile = Write-SourceFile "l72_insert_valid" (Get-ValidInsertSource -Type $Type -Name $Name)
        $upsertSrcFile = Write-SourceFile "l72_upsert_modified" (Get-ValidModifySource -Type $Type -Name $Name)

        Remove-PreExistingArtifacts -Name $Name -Type $Type -WhType $whType
        $preCli = Invoke-Cli -Arguments @("--json","exists",$cliType,$Name)
        $preExists = $false
        if ($preCli.ExitCode -eq 0) { $preExists = Get-ExistsFromJson -Json $preCli.Stdout }
        Record-Result -StepId "$($idBase)a" -Description "pre-existence: entry does not exist before L7.2 insert" `
            -Passed (-not $preExists) -Detail "exists_before=$preExists exit=$($preCli.ExitCode)" | Out-Null

        $cliIns = Invoke-Cli -Arguments @("--json","insert",$cliType,$Name,$insertSrcFile)
        $insertOk = ($cliIns.ExitCode -eq 0)
        $progId = Get-ProgIdFromJson -Json $cliIns.Stdout
        Record-Result -StepId "$($idBase)b" -Description "insert valid Strategy succeeded (exit 0, prog_id>0)" `
            -Passed ($insertOk -and $progId -gt 0) -Detail "exit=$($cliIns.ExitCode) prog_id=$progId" | Out-Null
        if (-not $insertOk -or $progId -le 0) { throw "$idBase insert failed; cannot run read-only-ELL assertions" }

        $ellPath = Join-Path $script:MyWorkBin "$prefix$progId.ell"
        $xmlPath = Join-Path $script:MyWorkBin "$prefix$progId.xml"
        $script:L7TrackedItems.Add(@{ Name=$Name; Type=$Type; ProgId=$progId; EllPath=$ellPath; XmlPath=$xmlPath; ReadOnlySet=$false }) | Out-Null

        $preEllHash = Get-FileHashOrNull -Path $ellPath
        $preXmlHash = Get-FileHashOrNull -Path $xmlPath
        Record-Result -StepId "$($idBase)c" -Description "pre-upsert ELL/XML exist with SHA-256 hashes captured" `
            -Passed ($preEllHash -ne $null -and $preXmlHash -ne $null) -Detail "ell_hash=$preEllHash xml_hash=$preXmlHash" | Out-Null

        # TVS-L.7 hardening: atomically persist a recovery marker BEFORE
        # setting the ELL read-only (see L7.1 header for rationale).
        $markerPath = New-L7RecoveryMarker -Name $Name -Type $Type -ProgId $progId -EllPath $ellPath -XmlPath $xmlPath -CaseId $idBase -RunId $runId

        # Mark ONLY the ELL read-only (handoff Sec.6 case 2: same invariant as case 1).
        $readOnlyCapture = Set-ReadOnlyAttributeExact -Path $ellPath
        $roOk = ($readOnlyCapture.ExitCode -eq 0 -and $readOnlyCapture.IsReadOnlyAfter -eq $true)
        Record-Result -StepId "$($idBase)d" -Description "read-only attribute set on ELL only (attrib +R captured)" `
            -Passed $roOk -Detail "cmd='$($readOnlyCapture.Command)' exit=$($readOnlyCapture.ExitCode) isReadOnly=$($readOnlyCapture.IsReadOnlyAfter) stdout='$($readOnlyCapture.Stdout)' stderr='$($readOnlyCapture.Stderr)' marker=$markerPath" | Out-Null
        for ($i = 0; $i -lt $script:L7TrackedItems.Count; $i++) {
            if ($script:L7TrackedItems[$i].ProgId -eq $progId) {
                $item = $script:L7TrackedItems[$i]
                $item.ReadOnlySet = $true
                $script:L7TrackedItems[$i] = $item
            }
        }

        # Upsert-existing with valid changed source.
        $cliUps = Invoke-Cli -Arguments @("--json","upsert",$cliType,$Name,$upsertSrcFile)
        $opExit = $cliUps.ExitCode
        $opJsonSuccess = $null
        try {
            $jUps = $cliUps.Stdout | ConvertFrom-Json -ErrorAction Stop
            $opJsonSuccess = [bool]$jUps.success
        } catch { $opJsonSuccess = $null }
        $reportedSuccess = ($opExit -eq 0 -and $opJsonSuccess -eq $true)
        Record-Result -StepId "$($idBase)e" -Description "upsert-existing executed; captured exit/JSON success/stderr/stdout" `
            -Passed $true -Detail "exit=$opExit json_success=$opJsonSuccess reported_success=$reportedSuccess stderr='$($cliUps.Stderr)' stdout='$($cliUps.Stdout)'" | Out-Null

        $postEllHash = Get-FileHashOrNull -Path $ellPath
        $postXmlHash = Get-FileHashOrNull -Path $xmlPath
        $newGuid = Extract-XmlGuid -XmlPath $xmlPath -ProgId $progId -Prefix $prefix
        $guidOccInEll = Get-GuidOccurrenceCountInEll -EllPath $ellPath -GuidString $newGuid
        Record-Result -StepId "$($idBase)f" -Description "post-upsert ELL/XML hashes + new XML GUID + GUID occurrence count in ELL captured" `
            -Passed $true -Detail "post_ell_hash=$postEllHash post_xml_hash=$postXmlHash new_guid=$newGuid guid_occ_in_ell=$guidOccInEll" | Out-Null

        $cliGs = Invoke-Cli -Arguments @("--json","get-source",$cliType,$Name)
        $gsSuccess = $null; $gsSrc = $null
        try {
            $jGs = $cliGs.Stdout | ConvertFrom-Json -ErrorAction Stop
            $gsSuccess = [bool]$jGs.success
            $gsSrc = [string]$jGs.source
        } catch { $gsSuccess = $null; $gsSrc = $null }
        $oldSrc = Get-Content -Path $insertSrcFile -Raw -ErrorAction SilentlyContinue
        $newSrc = Get-Content -Path $upsertSrcFile -Raw -ErrorAction SilentlyContinue
        $gsMatchesNew = ($gsSrc -ne $null -and $newSrc -ne $null -and $gsSrc -eq $newSrc)
        $gsMatchesOld = ($gsSrc -ne $null -and $oldSrc -ne $null -and $gsSrc -eq $oldSrc)
        Record-Result -StepId "$($idBase)g" -Description "get-source return value captured (Fault A evidence; cross-store assertions deferred to L.9)" `
            -Passed $true -Detail "exit=$($cliGs.ExitCode) success=$gsSuccess src_len=$(if ($gsSrc) { $gsSrc.Length } else { 0 }) matches_new=$gsMatchesNew matches_old=$gsMatchesOld" | Out-Null

        # DESIRED-STATE ASSERTIONS (handoff Sec.6 case 2). Post-L.9 (L.9
        # APPROVED): GREEN detect-and-fail -- production returns non-success
        # and retains the existing component.
        $nonSuccess = (-not $reportedSuccess)
        $ellHashUnchanged = ($preEllHash -ne $null -and $postEllHash -ne $null -and $preEllHash -eq $postEllHash)
        $existingComponentRetained = ($ellHashUnchanged -and (Test-Path -Path $ellPath -PathType Leaf))

        Record-Result -StepId "$($idBase)h" -Description "DESIRED-STATE: upsert-existing did NOT report verified success (non-success)" `
            -Passed $nonSuccess -Detail "reported_success=$reportedSuccess (exit=$opExit json_success=$opJsonSuccess) -- GREEN detect-and-fail after L.9" | Out-Null

        Record-Result -StepId "$($idBase)i" -Description "DESIRED-STATE: existing component retained (ELL hash unchanged, ELL present)" `
            -Passed $existingComponentRetained -Detail "pre_ell=$preEllHash post_ell=$postEllHash unchanged=$ellHashUnchanged ell_present=$(Test-Path -Path $ellPath -PathType Leaf)" | Out-Null

        $desiredStatePassed = ($nonSuccess -and $existingComponentRetained)
        $invariantFailures = @()
        if (-not $nonSuccess) { $invariantFailures += "false_verified_success(exit=$opExit,json_success=$opJsonSuccess)" }
        if (-not $existingComponentRetained) { $invariantFailures += "existing_component_not_retained(ell_hash_changed_or_absent)" }
        Record-Result -StepId "$($idBase)j" -Description "DESIRED-STATE combined: non-success AND existing component retained (Fault A desired-state)" `
            -Passed $desiredStatePassed -Detail "desired_passed=$desiredStatePassed invariant_failures=$($invariantFailures -join '; ') -- GREEN detect-and-fail after L.9" | Out-Null

        $evidence = @{
            CaseId                = $idBase
            CaseTitle             = "Upsert-existing / read-only ELL (Fault A variant; detect-and-fail; GREEN after L.9)"
            ItemIdentifier        = $Name
            ComponentType         = $Type
            ProgId                = $progId
            Operation             = "upsert"
            ReadOnlyCommand       = $readOnlyCapture.Command
            ReadOnlyExitCode      = $readOnlyCapture.ExitCode
            ReadOnlyIsReadOnlyAfter = $readOnlyCapture.IsReadOnlyAfter
            ReadOnlyStdout        = $readOnlyCapture.Stdout
            ReadOnlyStderr        = $readOnlyCapture.Stderr
            PreEllHash            = $preEllHash
            PreXmlHash            = $preXmlHash
            PostEllHash           = $postEllHash
            PostXmlHash           = $postXmlHash
            NewXmlGuid            = $newGuid
            GuidOccurrenceCountInEll = $guidOccInEll
            GetSourceExitCode     = $cliGs.ExitCode
            GetSourceSuccess      = $gsSuccess
            GetSourceLength       = if ($gsSrc) { $gsSrc.Length } else { 0 }
            GetSourceMatchesNew   = $gsMatchesNew
            GetSourceMatchesOld   = $gsMatchesOld
            OpExitCode            = $opExit
            OpJsonSuccess         = $opJsonSuccess
            OpStderr              = $cliUps.Stderr
            OpStdout              = $cliUps.Stdout
            DesiredStatePassed    = $desiredStatePassed
            InvariantFailures     = ($invariantFailures -join '; ')
        }
    }
    finally {
        Write-Host "--- $idBase cleanup ---" -ForegroundColor Cyan
        if ($insertSrcFile -and (Test-Path -Path $insertSrcFile -PathType Leaf)) { Remove-Item -Path $insertSrcFile -Force -ErrorAction SilentlyContinue }
        if ($upsertSrcFile -and (Test-Path -Path $upsertSrcFile -PathType Leaf)) { Remove-Item -Path $upsertSrcFile -Force -ErrorAction SilentlyContinue }
        $cln = Invoke-L7Sanitation -Name $Name -Type $Type -ProgId $progId -EllPath $ellPath -XmlPath $xmlPath
        Record-Result -StepId "$($idBase).CLEANUP" -Description "$idBase cleanup restored read-only attribute, removed warehouse entry and artifacts" `
            -Passed ($cln.VerdictOk -and $cln.ReadOnlyRestored) -Detail $cln.Detail | Out-Null
        # TVS-L.7 hardening: remove this run's recovery marker ONLY after
        # verified sanitation (see L7.1 finally for rationale).
        $markerSanitized = ($cln.ExistsGone -and $cln.SanitizedEllGone -and $cln.SanitizedXmlGone -and $cln.ReadOnlyRestored)
        if ($markerPath -and $markerSanitized) {
            Remove-L7RecoveryMarker -MarkerPath $markerPath
        }
        if ($evidence) {
            $evidence.EllExistedBeforeSanitation    = $cln.EllExistedBeforeSanitation
            $evidence.XmlExistedBeforeSanitation    = $cln.XmlExistedBeforeSanitation
            $evidence.OrphanExistedBeforeSanitation = $cln.OrphanExistedBeforeSanitation
            $evidence.ReadOnlyRestored              = $cln.ReadOnlyRestored
            $evidence.CleanupVerdictOk              = $cln.VerdictOk
            $script:L7Evidence.Add($evidence) | Out-Null
        }
    }
}

# ---------------------------------------------------------------------------
# L7.3 -- Delete / read-only ELL (Fault B: orphan ELL false success).
# Desired-state (handoff Sec.6 case 3): non-success, no false success, AND the
# residual artifact + Windows error REPORTED (error=5 / path). Cross-store
# assertions deferred to L.12.
# Post-L.12 (L.12 GREEN): detect-and-report. Production now returns
# non-success (deleteEntry returns false when DeleteFileA fails with a real
# removal error such as ERROR_ACCESS_DENIED=5) and surfaces the residual path
# + Windows error via the "failed to delete <path> (error=5)" stderr WARNING
# plus doDelete's error.message JSON field (exit 6 / success=false). The
# desired-state (non-success AND residual/error reported) PASSES. This is NOT
# physical orphan removal: the read-only ELL is still present at verdict time
# (DeleteFileA cannot remove a +R file; L.12 does NOT invent attrib -R
# recovery). The L7.4 sanitation in finally clears +R and deletes the orphan
# AFTER the verdict is recorded, so full physical orphan removal is NOT the
# L.12 success criterion.
# ---------------------------------------------------------------------------
function Run-L7-DeleteReadOnly {
    param([Parameter(Mandatory)] [string]$Name)
    $Type = 'strategy'
    $info = $script:TypeInfo[$Type]
    $prefix = $info.prefix
    $cliType = $info.cli
    $whType = $info.whType
    $idBase = 'L7.3'

    $insertSrcFile = $null
    $progId = 0
    $ellPath = ""
    $xmlPath = ""
    $preEllHash = $null
    $preXmlHash = $null
    $readOnlyCapture = $null
    $evidence = $null
    $markerPath = $null
    try {
        $insertSrcFile = Write-SourceFile "l73_insert_valid" (Get-ValidInsertSource -Type $Type -Name $Name)

        Remove-PreExistingArtifacts -Name $Name -Type $Type -WhType $whType
        $preCli = Invoke-Cli -Arguments @("--json","exists",$cliType,$Name)
        $preExists = $false
        if ($preCli.ExitCode -eq 0) { $preExists = Get-ExistsFromJson -Json $preCli.Stdout }
        Record-Result -StepId "$($idBase)a" -Description "pre-existence: entry does not exist before L7.3 insert" `
            -Passed (-not $preExists) -Detail "exists_before=$preExists exit=$($preCli.ExitCode)" | Out-Null

        $cliIns = Invoke-Cli -Arguments @("--json","insert",$cliType,$Name,$insertSrcFile)
        $insertOk = ($cliIns.ExitCode -eq 0)
        $progId = Get-ProgIdFromJson -Json $cliIns.Stdout
        Record-Result -StepId "$($idBase)b" -Description "insert valid Strategy succeeded (exit 0, prog_id>0)" `
            -Passed ($insertOk -and $progId -gt 0) -Detail "exit=$($cliIns.ExitCode) prog_id=$progId" | Out-Null
        if (-not $insertOk -or $progId -le 0) { throw "$idBase insert failed; cannot run read-only-ELL assertions" }

        $ellPath = Join-Path $script:MyWorkBin "$prefix$progId.ell"
        $xmlPath = Join-Path $script:MyWorkBin "$prefix$progId.xml"
        $script:L7TrackedItems.Add(@{ Name=$Name; Type=$Type; ProgId=$progId; EllPath=$ellPath; XmlPath=$xmlPath; ReadOnlySet=$false }) | Out-Null

        $preEllHash = Get-FileHashOrNull -Path $ellPath
        $preXmlHash = Get-FileHashOrNull -Path $xmlPath
        Record-Result -StepId "$($idBase)c" -Description "pre-delete ELL/XML exist with SHA-256 hashes captured" `
            -Passed ($preEllHash -ne $null -and $preXmlHash -ne $null) -Detail "ell_hash=$preEllHash xml_hash=$preXmlHash" | Out-Null

        # TVS-L.7 hardening: atomically persist a recovery marker BEFORE
        # setting the ELL read-only (see L7.1 header for rationale).
        $markerPath = New-L7RecoveryMarker -Name $Name -Type $Type -ProgId $progId -EllPath $ellPath -XmlPath $xmlPath -CaseId $idBase -RunId $runId

        # Mark ONLY the ELL read-only (handoff Sec.6 case 3).
        $readOnlyCapture = Set-ReadOnlyAttributeExact -Path $ellPath
        $roOk = ($readOnlyCapture.ExitCode -eq 0 -and $readOnlyCapture.IsReadOnlyAfter -eq $true)
        Record-Result -StepId "$($idBase)d" -Description "read-only attribute set on ELL only (attrib +R captured)" `
            -Passed $roOk -Detail "cmd='$($readOnlyCapture.Command)' exit=$($readOnlyCapture.ExitCode) isReadOnly=$($readOnlyCapture.IsReadOnlyAfter) stdout='$($readOnlyCapture.Stdout)' stderr='$($readOnlyCapture.Stderr)' marker=$markerPath" | Out-Null
        for ($i = 0; $i -lt $script:L7TrackedItems.Count; $i++) {
            if ($script:L7TrackedItems[$i].ProgId -eq $progId) {
                $item = $script:L7TrackedItems[$i]
                $item.ReadOnlySet = $true
                $script:L7TrackedItems[$i] = $item
            }
        }

        # Delete through the production CLI.
        $cliDel = Invoke-Cli -Arguments @("--json","delete",$cliType,$Name)
        $opExit = $cliDel.ExitCode
        $opJsonSuccess = $null
        try {
            $jDel = $cliDel.Stdout | ConvertFrom-Json -ErrorAction Stop
            $opJsonSuccess = [bool]$jDel.success
        } catch { $opJsonSuccess = $null }
        $reportedSuccess = ($opExit -eq 0 -and $opJsonSuccess -eq $true)
        # Capture the exact "failed to delete ... error=5" line from stderr.
        $deleteError5Line = ""
        if ($cliDel.Stderr -and ($cliDel.Stderr -match 'error=5')) {
            $deleteError5Line = ($cliDel.Stderr -split "`n" | Where-Object { $_ -match 'error=5' } | Select-Object -First 1)
        }
        Record-Result -StepId "$($idBase)e" -Description "delete executed; captured exit/JSON success/stderr/stdout + error=5 line" `
            -Passed $true -Detail "exit=$opExit json_success=$opJsonSuccess reported_success=$reportedSuccess stderr='$($cliDel.Stderr)' stdout='$($cliDel.Stdout)' error5_line='$deleteError5Line'" | Out-Null

        # Post-delete warehouse / XML / ELL presence (Fault B evidence).
        $postExCli = Invoke-Cli -Arguments @("--json","exists",$cliType,$Name)
        $warehouseEntryPresent = $false
        if ($postExCli.ExitCode -eq 0) { $warehouseEntryPresent = Get-ExistsFromJson -Json $postExCli.Stdout }
        $xmlPresentAfter = (Test-Path -Path $xmlPath -PathType Leaf)
        $ellPresentAfter = (Test-Path -Path $ellPath -PathType Leaf)
        $postEllHash = Get-FileHashOrNull -Path $ellPath
        Record-Result -StepId "$($idBase)f" -Description "post-delete warehouse/XML/ELL presence + orphan ELL hash captured (Fault B evidence)" `
            -Passed $true -Detail "warehouse_present=$warehouseEntryPresent xml_present=$xmlPresentAfter ell_present=$ellPresentAfter orphan=$ellPresentAfter post_ell_hash=$postEllHash" | Out-Null

        # DESIRED-STATE ASSERTIONS (handoff Sec.6 case 3). Post-L.12 (L.12 GREEN):
        # detect-and-report. The desired-state is NON-SUCCESS PLUS the residual
        # artifact + Windows error being REPORTED (error=5 / path) -- NOT that
        # the read-only ELL was physically removed. DeleteFileA cannot remove a
        # +R file and L.12 deliberately does NOT invent attrib -R recovery, so
        # the orphan ELL is EXPECTED to still be present at verdict time; the
        # L7.4 sanitation in finally clears +R and deletes the orphan AFTER the
        # verdict is recorded. Full physical orphan removal is therefore NOT
        # the L.12 success criterion.
        $nonSuccess = (-not $reportedSuccess)
        # residualReported: the L.12 detect-and-report signal. deleteEntry step
        # 5 emits "tsdev: WARNING -- failed to delete <path> (error=5)" to
        # stderr AND doDelete surfaces the residual path + Windows error in the
        # JSON error.message (exit 6 / success=false). The error=5 line carries
        # BOTH the residual path and the Windows error, so its presence is the
        # single evidence that the residual was reported (not silently treated
        # as success, as it was before L.12).
        $residualReported = ($deleteError5Line -ne "")
        # The orphan ELL being present is EXPECTED post-L.12 (sanitation is in
        # finally AFTER verdict). It is captured as evidence (L7.3f) but is NOT
        # a desired-state pass/fail criterion -- asserting its absence would
        # claim full physical orphan removal is L.12 success, which it is not.
        $orphanExpectedPresent = $ellPresentAfter

        Record-Result -StepId "$($idBase)g" -Description "DESIRED-STATE: delete did NOT report success (non-success)" `
            -Passed $nonSuccess -Detail "reported_success=$reportedSuccess (exit=$opExit json_success=$opJsonSuccess) -- GREEN detect-and-report after L.12" | Out-Null

        Record-Result -StepId "$($idBase)h" -Description "DESIRED-STATE: residual artifact + Windows error REPORTED (error=5 / path captured in stderr)" `
            -Passed $residualReported -Detail "error5_line='$deleteError5Line' orphan_ell_present=$ellPresentAfter (expected present: sanitation is in finally AFTER verdict) -- GREEN detect-and-report after L.12" | Out-Null

        $desiredStatePassed = ($nonSuccess -and $residualReported)
        $invariantFailures = @()
        if (-not $nonSuccess) { $invariantFailures += "false_success(exit=$opExit,json_success=$opJsonSuccess)" }
        if (-not $residualReported) { $invariantFailures += "residual_not_reported(error5_line_empty=$($deleteError5Line -eq ''))" }
        Record-Result -StepId "$($idBase)i" -Description "DESIRED-STATE combined: non-success AND residual/error reported (Fault B desired-state; detect-and-report, NOT physical orphan removal)" `
            -Passed $desiredStatePassed -Detail "desired_passed=$desiredStatePassed invariant_failures=$($invariantFailures -join '; ') orphan_ell_present=$orphanExpectedPresent (expected: sanitation in finally AFTER verdict) -- GREEN detect-and-report after L.12" | Out-Null

        $evidence = @{
            CaseId                = $idBase
            CaseTitle             = "Delete / read-only ELL (Fault B: orphan ELL false success; detect-and-report; GREEN after L.12)"
            ItemIdentifier        = $Name
            ComponentType         = $Type
            ProgId                = $progId
            Operation             = "delete"
            ReadOnlyCommand       = $readOnlyCapture.Command
            ReadOnlyExitCode      = $readOnlyCapture.ExitCode
            ReadOnlyIsReadOnlyAfter = $readOnlyCapture.IsReadOnlyAfter
            ReadOnlyStdout        = $readOnlyCapture.Stdout
            ReadOnlyStderr        = $readOnlyCapture.Stderr
            PreEllHash            = $preEllHash
            PreXmlHash            = $preXmlHash
            PostEllHash           = $postEllHash
            PostXmlHash           = $null
            OpExitCode            = $opExit
            OpJsonSuccess         = $opJsonSuccess
            OpStderr              = $cliDel.Stderr
            OpStdout              = $cliDel.Stdout
            WarehouseEntryPresent = $warehouseEntryPresent
            XmlPresentAfter       = $xmlPresentAfter
            EllPresentAfter       = $ellPresentAfter
            DeleteError5Line      = $deleteError5Line
            ResidualReported      = $residualReported
            OrphanExpectedPresent = $orphanExpectedPresent
            DesiredStatePassed    = $desiredStatePassed
            InvariantFailures     = ($invariantFailures -join '; ')
        }
    }
    finally {
        Write-Host "--- $idBase cleanup ---" -ForegroundColor Cyan
        if ($insertSrcFile -and (Test-Path -Path $insertSrcFile -PathType Leaf)) { Remove-Item -Path $insertSrcFile -Force -ErrorAction SilentlyContinue }
        $cln = Invoke-L7Sanitation -Name $Name -Type $Type -ProgId $progId -EllPath $ellPath -XmlPath $xmlPath
        # L7.3.CLEANUP predicate (L.12 detect-and-report alignment): the
        # pre-sanitation orphan ELL is EXPECTED to still be present at verdict
        # time (DeleteFileA cannot remove a +R file; L.12 does NOT invent
        # attrib -R recovery), so the shared Get-CleanupVerdict formula
        # ($cln.VerdictOk, which requires pre-sanitation EllGone) does NOT
        # apply here. Instead, CLEANUP passes when post-verdict hygiene
        # succeeded: the read-only attribute was restored, the warehouse entry
        # is gone, and the post-sanitation artifacts are gone (the orphan is
        # removed by this finally, not by L.12 production code). This does NOT
        # weaken L7.1/L7.2 cleanup (which still use $cln.VerdictOk) or any
        # production code.
        $l73CleanupPassed = ($cln.ReadOnlyRestored -and $cln.ExistsGone -and $cln.SanitizedEllGone -and $cln.SanitizedXmlGone)
        Record-Result -StepId "$($idBase).CLEANUP" -Description "$idBase cleanup restored read-only attribute, removed warehouse entry and post-sanitation artifacts; L7.3.g/h/i verdict (detect-and-report) was recorded BEFORE this sanitation -- the pre-sanitation orphan ELL is expected to still be present at verdict time (allowed under detect-and-report) and is removed here in finally, NOT by L.12" `
            -Passed $l73CleanupPassed -Detail $cln.Detail | Out-Null
        # TVS-L.7 hardening: remove this run's recovery marker ONLY after
        # verified sanitation (see L7.1 finally for rationale).
        $markerSanitized = ($cln.ExistsGone -and $cln.SanitizedEllGone -and $cln.SanitizedXmlGone -and $cln.ReadOnlyRestored)
        if ($markerPath -and $markerSanitized) {
            Remove-L7RecoveryMarker -MarkerPath $markerPath
        }
        if ($evidence) {
            $evidence.EllExistedBeforeSanitation    = $cln.EllExistedBeforeSanitation
            $evidence.XmlExistedBeforeSanitation    = $cln.XmlExistedBeforeSanitation
            $evidence.OrphanExistedBeforeSanitation = $cln.OrphanExistedBeforeSanitation
            $evidence.ReadOnlyRestored              = $cln.ReadOnlyRestored
            $evidence.CleanupVerdictOk              = $cln.VerdictOk
            $script:L7Evidence.Add($evidence) | Out-Null
        }
    }
}

# ---------------------------------------------------------------------------
# L7.4 -- Sanitation confirmation (handoff Sec.6 case 4): confirm all test
# items, attributes, and artifacts are sanitized even when assertions fail.
# This is a final sweep over $L7TrackedItems that:
#   1. clears any remaining read-only attribute on tracked ELL paths;
#   2. deletes any remaining warehouse entry by name;
#   3. removes any remaining ELL/XML artifacts;
#   4. verifies no tracked warehouse entries remain and no tracked
#      artifacts remain.
# Runs in the L7 outer try/finally so it executes even if a case crashes.
# ---------------------------------------------------------------------------
function Invoke-L7FinalSanitation {
    $allClean = $true
    $details = @()
    foreach ($item in $script:L7TrackedItems) {
        # 1. Clear read-only attribute if still set.
        if ($item.EllPath -and (Test-Path -Path $item.EllPath -PathType Leaf)) {
            $fi = Get-Item -Path $item.EllPath -ErrorAction SilentlyContinue
            if ($fi -and $fi.IsReadOnly) {
                Clear-ReadOnlyAttributeExact -Path $item.EllPath | Out-Null
                $details += "cleared RO on $($item.EllPath)"
            }
        }
        # 2. Delete remaining warehouse entry by name (idempotent).
        $cliDel = Invoke-Cli -Arguments @("--json","delete",$item.Type,$item.Name)
        $cliEx = Invoke-Cli -Arguments @("--json","exists",$item.Type,$item.Name)
        $stillExists = $false
        if ($cliEx.ExitCode -eq 0) { $stillExists = Get-ExistsFromJson -Json $cliEx.Stdout }
        # 3. Remove remaining artifacts.
        $ellStill = $false; $xmlStill = $false
        if ($item.EllPath -and (Test-Path -Path $item.EllPath -PathType Leaf)) {
            Remove-Item -Path $item.EllPath -Force -ErrorAction SilentlyContinue
            $ellStill = (Test-Path -Path $item.EllPath -PathType Leaf)
        }
        if ($item.XmlPath -and (Test-Path -Path $item.XmlPath -PathType Leaf)) {
            Remove-Item -Path $item.XmlPath -Force -ErrorAction SilentlyContinue
            $xmlStill = (Test-Path -Path $item.XmlPath -PathType Leaf)
        }
        if ($stillExists -or $ellStill -or $xmlStill) {
            $allClean = $false
            $details += "RESIDUAL $($item.Name): entry=$stillExists ell=$ellStill xml=$xmlStill"
        }
    }
    return @{ AllClean = $allClean; Details = ($details -join '; ') }
}

# ---------------------------------------------------------------------------
# L7 OPT-IN EXECUTION (handoff Sec.6). Gated by -IncludeRedFaultInjection; the
# registered always-green suite never sets this switch.
# ---------------------------------------------------------------------------
if ($IncludeRedFaultInjection) {
    Write-Host ""
    Write-Host "===== TVS-L.7: OPT-IN RED fault-injection harness (read-only ELL) =====" -ForegroundColor Magenta
    Write-Host "Opt-in via -IncludeRedFaultInjection; NOT registered in run_regression.ps1" -ForegroundColor Magenta
    Write-Host "Durable evidence path: $RedFaultInjectionDurablePath" -ForegroundColor Magenta
    Write-Host "Recovery marker dir : $script:L7RecoveryDir" -ForegroundColor Magenta
    Write-Host "Post-L.9 (L.9 APPROVED): L7.1/L7.2 desired-state is GREEN detect-and-fail" -ForegroundColor Yellow
    Write-Host "Post-L.12 (L.12 GREEN): L7.3 desired-state is GREEN detect-and-report (non-success + residual/error reported; NOT physical orphan removal)" -ForegroundColor Yellow
    Write-Host ""

    # TVS-L.7 hardening: process stale recovery markers from prior crashed
    # L7 runs BEFORE starting any new case. This clears read-only, deletes
    # any still-existing warehouse entry, removes the EXACT orphan ELL/XML
    # artifacts (never scans MyWork\Bin), verifies absence, and deletes each
    # marker only on success. Strict validation rejects any marker whose
    # name/type/ProgId/ELL/XML paths do not match the TS_AUTO_L7_* /
    # elmodule_0_<ProgId>.ell/.xml / under-MyWork\Bin contracts.
    Write-Host "--- L7.0: stale recovery-marker sweep (prior crashed runs) ---" -ForegroundColor Cyan
    $staleRecovery = Invoke-L7StaleMarkerRecovery
    $staleRecovered = $staleRecovery.Recovered
    $staleSkipped = $staleRecovery.Skipped
    $staleOk = ($staleSkipped.Count -eq 0)
    $staleDetail = "recovered=$($staleRecovered.Count) skipped=$($staleSkipped.Count)"
    if ($staleRecovered.Count -gt 0) {
        $staleDetail += " | recovered: $($staleRecovered -join '; ')"
    }
    if ($staleSkipped.Count -gt 0) {
        $staleDetail += " | skipped: $($staleSkipped -join '; ')"
    }
    Record-Result -StepId "L7.0" -Description "stale recovery-marker sweep: strictly-validated markers processed; no unrelated files scanned" `
        -Passed $staleOk -Detail $staleDetail | Out-Null
    Write-Host "  $staleDetail" -ForegroundColor $(if ($staleOk) { 'DarkGray' } else { 'Yellow' })

    $l7Name1 = "TS_AUTO_L7_MOD_RO_${runId}"
    $l7Name2 = "TS_AUTO_L7_UPS_RO_${runId}"
    $l7Name3 = "TS_AUTO_L7_DEL_RO_${runId}"

    try {
        Run-L7-ModifyReadOnly -Name $l7Name1
        Run-L7-UpsertExistingReadOnly -Name $l7Name2
        Run-L7-DeleteReadOnly -Name $l7Name3
    }
    finally {
        # L7.4: final sanitation confirmation (handoff Sec.6 case 4). Runs even
        # if a case crashes mid-flight.
        Write-Host ""
        Write-Host "--- L7.4: final sanitation confirmation ---" -ForegroundColor Cyan
        $finalSan = Invoke-L7FinalSanitation
        Record-Result -StepId "L7.4" -Description "all L7 disposable items, attributes, and artifacts sanitized even when assertions fail" `
            -Passed $finalSan.AllClean -Detail $finalSan.Details | Out-Null

        # TVS-L.7 hardening: any markers left by cases whose per-case finally
        # could not verify sanitation are reported (NOT silently deleted) so
        # the next run's L7.0 sweep can attempt recovery again. This is a
        # visibility-only check; it does not delete markers itself.
        $leftoverMarkers = @()
        if (Test-Path -Path $script:L7RecoveryDir -PathType Container) {
            $leftoverMarkers = @(Get-ChildItem -Path $script:L7RecoveryDir -Filter '*.json' -File -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name)
        }
        if ($leftoverMarkers.Count -gt 0) {
            Write-Host "  NOTE: $($leftoverMarkers.Count) recovery marker(s) left for next-run sweep: $($leftoverMarkers -join ', ')" -ForegroundColor Yellow
        }

        # Durable evidence write (handoff Sec.6 Evidence).
        # WRITE-GUARD: $RedFaultInjectionDurablePath defaults to the gitignored
        # tmp/ path; only -UpdateTrackedEvidence overrides it to the tracked
        # docs/RE/ file. Routine runs do NOT touch the tracked evidence.
        try {
            Write-L7DurableEvidence -Path $RedFaultInjectionDurablePath -Evidence $script:L7Evidence
            Record-Result -StepId "L7.EVIDENCE" -Description "durable RED evidence summary written" `
                -Passed $true -Detail "path=$RedFaultInjectionDurablePath cases=$($script:L7Evidence.Count)" | Out-Null
        } catch {
            Record-Result -StepId "L7.EVIDENCE" -Description "durable RED evidence summary write FAILED" `
                -Passed $false -Detail "path=$RedFaultInjectionDurablePath error=$($_.Exception.Message)" | Out-Null
        }
    }
}

# ---------------------------------------------------------------------------
# Final temp-directory cleanup (after all L1 + L4 + L5 + L7 sections). The L1
# finally block intentionally leaves $tmpDir in place so the L4 / L5 / L7
# sections can reuse it.
# ---------------------------------------------------------------------------
if (Test-Path -Path $tmpDir -PathType Container) {
    Remove-Item -Path $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
}

# ===========================================================================
# SUMMARY
# ===========================================================================
Write-Host ""
Write-Host "==================== UNKNOWN LINK / PERSISTENCE / SEMANTIC SUMMARY ====================" -ForegroundColor Cyan
$header = "{0,-12} {1,-70} {2,-6}" -f "Step", "Description", "Result"
Write-Host $header
Write-Host ("{0,-12} {1,-70} {2,-6}" -f ("-" * 12), ("-" * 70), ("-" * 6))

$passed = 0
$failed = 0
$l1Passed = 0; $l1Failed = 0
$l4Passed = 0; $l4Failed = 0
$l5Passed = 0; $l5Failed = 0
$l6Passed = 0; $l6Failed = 0
$l7Passed = 0; $l7Failed = 0
foreach ($r in $Results) {
    $line = "{0,-12} {1,-70} {2,-6}" -f $r.StepId, $r.Description, $r.Result
    $color = if ($r.Result -eq 'PASS') { 'Green' } else { 'Red' }
    Write-Host $line -ForegroundColor $color
    if ($r.Result -eq 'PASS') {
        $passed++
        if ($r.StepId -like "L4.*") { $l4Passed++ } elseif ($r.StepId -like "L5.*") { $l5Passed++ } elseif ($r.StepId -like "L6.*") { $l6Passed++ } elseif ($r.StepId -like "L7.*") { $l7Passed++ } else { $l1Passed++ }
    } else {
        $failed++
        if ($r.StepId -like "L4.*") { $l4Failed++ } elseif ($r.StepId -like "L5.*") { $l5Failed++ } elseif ($r.StepId -like "L6.*") { $l6Failed++ } elseif ($r.StepId -like "L7.*") { $l7Failed++ } else { $l1Failed++ }
    }
}

Write-Host ""
Write-Host ("Totals: {0}/{1} passed, {2} failed" -f $passed, $Results.Count, $failed) -ForegroundColor Cyan
Write-Host ("  L1.* : {0} passed, {1} failed" -f $l1Passed, $l1Failed) -ForegroundColor Cyan
Write-Host ("  L4.* : {0} passed, {1} failed" -f $l4Passed, $l4Failed) -ForegroundColor Cyan
Write-Host ("  L5.* : {0} passed, {1} failed" -f $l5Passed, $l5Failed) -ForegroundColor Cyan
Write-Host ("  L6.* : {0} passed, {1} failed" -f $l6Passed, $l6Failed) -ForegroundColor Cyan
Write-Host ("  L7.* : {0} passed, {1} failed (opt-in fault-injection; L7.1/L7.2 GREEN detect-and-fail after L.9, L7.3 GREEN detect-and-report after L.12)" -f $l7Passed, $l7Failed) -ForegroundColor Cyan

Write-Host ""
Write-Host "Cleanup status:" -ForegroundColor Cyan
foreach ($s in $cleanupStatus) {
    Write-Host "  $s" -ForegroundColor DarkGray
}

Write-Host ""
Write-Host "XML evidence is NOT runtime acceptance." -ForegroundColor Yellow
Write-Host "A passing XML/GUID assertion proves the original finalization path" -ForegroundColor Yellow
Write-Host "ran; it does NOT prove Desktop linkability. The existing durable"  -ForegroundColor Yellow
Write-Host "runtime-failure baseline is recorded separately in"               -ForegroundColor Yellow
Write-Host "docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md."                            -ForegroundColor Yellow

Write-Host ""
if ($failed -eq 0) {
    Write-Host "ALL DESIRED-STATE ASSERTIONS PASSED (GREEN -- L1 + L4 + L5 + L6$(if ($IncludeRedFaultInjection) { ' + L7' }))" -ForegroundColor Green
    exit 0
} else {
    Write-Host "$failed ASSERTION(S) FAILED (RED -- L1 + L4 + L5 + L6$(if ($IncludeRedFaultInjection) { ' + L7' }))" -ForegroundColor Red
    Write-Host "  L1 failures: $l1Failed | L4 failures: $l4Failed | L5 failures: $l5Failed | L6 failures: $l6Failed | L7 failures: $l7Failed" -ForegroundColor Red
    if ($IncludeRedFaultInjection) {
        Write-Host "  L7 is the opt-in fault-injection harness; post-L.9 L7.1/L7.2 desired-state is GREEN detect-and-fail, and post-L.12 L7.3 desired-state is GREEN detect-and-report (non-success + residual/error reported; NOT physical orphan removal)." -ForegroundColor Yellow
    }
    exit 1
}
