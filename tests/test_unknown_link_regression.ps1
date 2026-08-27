<#
.SYNOPSIS
    Desired-state E2E harness for the Unknown Link defect
    (TVS-L.1 + TVS-L.4 + TVS-L.5 + TVS-L.6 + TVS-L.7) covering all 5
    supported component types: Strategy, Indicator, Function, TradingApp,
    ShowMe. TVS-L.7 (read-only-ELL fault injection) is OPT-IN via
    -IncludeRedFaultInjection and is NOT registered in the default regression
    suite (tests/run_regression.ps1 invokes this script without that
    switch).

.DESCRIPTION
    This harness retains the historical RED baseline and exercises the
    Workstream L artifact and fault-injection checks across all supported
    component types. The L7 read-only-ELL fault-injection cases are opt-in;
    the normal L1/L4/L5/L6 cases are invoked by tests/run_regression.ps1.
    It explicitly distinguishes:

      * XML evidence  -- a completion marker / debugger source-map artifact.
                         Presence of <prefix><ProgId>.xml with the expected
                         <symbols file="..." version="{GUID}"> header proves
                         the original finalization path (SymWriter::Save) ran.
      * Runtime linkability -- what TradeStation Desktop actually links at
                         runtime. This harness does NOT assert runtime
                         linkability and does NOT substitute a fake runtime
                         proxy. The durable user/runtime evidence that
                         Strategies failed in Desktop with "Unknown link error
                         occurred" is a historical L.1 runtime-failure baseline
                         and is recorded separately in
                         docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md.

    Test flow (per supported component type):
      1. Generate a unique name (TS_AUTO_L1_<TYPE>_<timestamp>).
      2. Insert a minimal valid component through the production CLI with
         --json. Parse the ProgId from the JSON output.
      3. Desired-state assertions:
         a. ELL exists at <MyWork>\Bin\<prefix><ProgId>.ell and is fresh
            (mtime after the insert operation).
         b. XML exists at <MyWork>\Bin\<prefix><ProgId>.xml and is fresh.
            The pre-L.3 RED baseline recorded XML absence because the bypass
            skipped SymWriter::Save; that is historical, not current expected
            behavior.
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
            the historical/transient tmp/com_nopatch_guid_analysis.json for the
           recorded analysis (the file is not tracked and may be unavailable).
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
    (0 = all assertions passed, nonzero = one or more failed). The pre-L.3
    nonzero run is retained as the historical RED baseline. The current
    default regression runner invokes the normal L1/L4/L5/L6 cases; the L7
    fault-injection cases remain opt-in via -IncludeRedFaultInjection. A run
    result is not implied by this header.

    IMPORTANT: XML evidence is NOT runtime acceptance. A passing XML/GUID
    assertion proves the original finalization path ran; it does NOT prove
    Desktop linkability. This harness never fakes runtime success/failure.

.PARAMETER CliPath
    Path to the tsdev_cli.exe executable. Defaults to
    "$PSScriptRoot\..\build\Release\tsdev_cli.exe".

.PARAMETER TsPath
    Optional. TradeStation install directory passed through to the CLI as
    "--ts-path <dir>" BEFORE each subcommand. The MyWork\Bin directory used
    for artifact checks is always derived as <effective TsPath>\MyWork\Bin.
    When omitted, the existing default install root
    "C:\Program Files (x86)\TradeStation 9.5" is selected and passed explicitly.

.PARAMETER BinPath
    Compatibility parameter. If supplied, it must resolve exactly to the
    derived <effective TsPath>\MyWork\Bin path. It cannot select a separate
    artifact directory; omit it unless it names that canonical path.

.PARAMETER Verbose
    Switch. When set, print full CLI output for every step.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\test_unknown_link_regression.ps1
    Runs the normal L1/L4/L5/L6 harness with the default CLI path. The direct
    CLI requires an elevated (administrator) process; L7 fault injection is
    opt-in via -IncludeRedFaultInjection.

.NOTES
    Exit codes (returned by this script):
      0 = all executed desired-state assertions passed
      1 = one or more executed desired-state assertions failed
      2 = environment/prerequisite error (not elevated, CLI missing, etc.)

    Historical L.1 RED results and any recorded aggregate counts are
    unverified historical/transient evidence, not a current acceptance result.
#>

param(
    [string]$CliPath = (Join-Path $PSScriptRoot "..\build\Release\tsdev_cli.exe"),
    [string]$TsPath,
    [string]$BinPath,
    [switch]$Verbose,
    # TVS-L.7 (handoff Sec.6): opt-in fault-injection acceptance harness.
    # When set, the L7.* fault-injection cases (read-only-ELL modify /
    # upsert-existing / delete) run AFTER the default regression L1/L4/L5/L6 cases.
    # The registered suite (tests/run_regression.ps1) invokes this script
    # WITHOUT this switch, so the opt-in fault-injection cases are NOT
    # registered in the default regression suite (handoff Sec.6: "Do not register
    # intentionally RED cases in the default regression suite yet"). L6 oracles
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
    # (ProgIds, hashes, timestamps vary per run). An omitted destination is
    # assigned a run-specific gitignored tmp/ path after the run ID is created,
    # so FileMode.CreateNew remains safe on the second opt-in run. A caller-
    # supplied destination is preserved and is always written exclusively.
    [string]$RedFaultInjectionDurablePath,
    # WRITE-GUARD switch: when set, permit the tracked
    # docs/RE/RE_L7_RED_FAULT_INJECTION_EVIDENCE.md destination. This switch
    # authorizes selecting the tracked path; it never authorizes overwriting an
    # existing destination.
    [switch]$UpdateTrackedEvidence
)

$ErrorActionPreference = 'Stop'

# WRITE-GUARD: Windows does not expose hard-link count through FileAttributes.
# Use the native file-information query for existing regular files so an exact
# artifact/evidence path that is also linked elsewhere fails closed rather than
# deleting or replacing the other link target.
if (-not ('TsDevL7.NativeMethods' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

namespace TsDevL7 {
    [StructLayout(LayoutKind.Sequential)]
    public struct ByHandleFileInformation {
        public uint FileAttributes;
        public System.Runtime.InteropServices.ComTypes.FILETIME CreationTime;
        public System.Runtime.InteropServices.ComTypes.FILETIME LastAccessTime;
        public System.Runtime.InteropServices.ComTypes.FILETIME LastWriteTime;
        public uint VolumeSerialNumber;
        public uint FileSizeHigh;
        public uint FileSizeLow;
        public uint NumberOfLinks;
        public uint FileIndexHigh;
        public uint FileIndexLow;
    }

    public static class NativeMethods {
        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool GetFileInformationByHandle(
            IntPtr hFile, out ByHandleFileInformation lpFileInformation);
    }
}
'@
}

# ---------------------------------------------------------------------------
# Resolve paths and run ID.
# ---------------------------------------------------------------------------
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$runId = Get-Date -Format "yyyyMMdd_HHmmss"
$resolved = Resolve-Path -Path $CliPath -ErrorAction SilentlyContinue
if ($resolved) {
    $CliPath = $resolved.Path
} elseif (-not (Test-Path -Path $CliPath -PathType Leaf)) {
    $CliPath = Join-Path $ProjectRoot "build\Release\tsdev_cli.exe"
}

# WRITE-GUARD: -UpdateTrackedEvidence is the only switch that may select the
# tracked docs/RE/ file. Without it, canonicalize the caller-provided (or
# run-specific default) path and reject anything outside the project tmp
# subtree (including traversal paths) before any evidence write can occur. This
# lexical guard works for paths whose destination file does not exist yet on
# both Windows PowerShell 5.1 and pwsh 7.
$trackedEvidencePath = Join-Path $ProjectRoot "docs\RE\RE_L7_RED_FAULT_INJECTION_EVIDENCE.md"
$tmpEvidenceRoot = Join-Path $ProjectRoot "tmp"
try {
    $tmpEvidenceRootFull = [System.IO.Path]::GetFullPath($tmpEvidenceRoot)
    $trackedEvidencePathFull = [System.IO.Path]::GetFullPath($trackedEvidencePath)
    $script:trackedEvidencePathFull = $trackedEvidencePathFull
} catch {
    throw "Unable to resolve the L7 evidence roots: $($_.Exception.Message)"
}

$evidenceDestinationSupplied = -not [string]::IsNullOrWhiteSpace($RedFaultInjectionDurablePath)
if (-not $evidenceDestinationSupplied) {
    if ($UpdateTrackedEvidence) {
        # With no destination supplied, the explicit switch selects the tracked
        # file. A caller-supplied tmp destination is never replaced by this
        # switch.
        $RedFaultInjectionDurablePath = $trackedEvidencePathFull
    } else {
        # Include both the visible run ID and a per-invocation token so two
        # opt-in runs started within one clock second still cannot collide.
        $evidenceRunToken = [System.Guid]::NewGuid().ToString('N')
        $RedFaultInjectionDurablePath = Join-Path $tmpEvidenceRoot ("RE_L7_RED_FAULT_INJECTION_EVIDENCE_{0}_{1}.md" -f $runId, $evidenceRunToken)
    }
}

try {
    $providerPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($RedFaultInjectionDurablePath)
    $candidateEvidencePath = [System.IO.Path]::GetFullPath($providerPath)
} catch {
    throw "Unsafe L7 evidence path '$RedFaultInjectionDurablePath': it could not be resolved under the project evidence roots."
}

$tmpEvidencePrefix = $tmpEvidenceRootFull.TrimEnd([char[]]@('\', '/')) + [System.IO.Path]::DirectorySeparatorChar
$isUnderProjectTmp = $candidateEvidencePath.StartsWith($tmpEvidencePrefix, [System.StringComparison]::OrdinalIgnoreCase)
$isTrackedEvidencePath = [System.String]::Equals($candidateEvidencePath, $trackedEvidencePathFull, [System.StringComparison]::OrdinalIgnoreCase)
if ($isTrackedEvidencePath) {
    if (-not $UpdateTrackedEvidence) {
        throw "Unsafe L7 evidence path '$RedFaultInjectionDurablePath' (resolved to '$candidateEvidencePath'). Selecting the tracked evidence file requires -UpdateTrackedEvidence."
    }
} elseif (-not $isUnderProjectTmp) {
    throw "Unsafe L7 evidence path '$RedFaultInjectionDurablePath' (resolved to '$candidateEvidencePath'). Without a tracked-evidence authorization, L7 evidence must remain under '$tmpEvidenceRootFull'."
}
$RedFaultInjectionDurablePath = $candidateEvidencePath

# WRITE-GUARD: reject any existing reparse-point component in the selected
# destination before creating its parent or writing the evidence. File.GetAttributes
# reports the reparse-point bit without following the link and is available in
# both Windows PowerShell 5.1 and pwsh 7. Inspection failures are unsafe and fail
# closed. This applies to both the tracked destination and the tmp destination.
function Test-L7EvidencePathHasReparsePoint {
    param([Parameter(Mandatory)] [string]$Path)
    try {
        $current = [System.IO.Path]::GetFullPath($Path)
        while ($null -ne $current) {
            $attributes = $null
            $componentExists = $true
            try {
                # GetAttributes examines the component itself, so a link is
                # identified by ReparsePoint rather than followed to its target.
                $attributes = [System.IO.File]::GetAttributes($current)
            } catch [System.IO.FileNotFoundException] {
                $componentExists = $false
            } catch [System.IO.DirectoryNotFoundException] {
                $componentExists = $false
            } catch {
                throw "Unable to inspect evidence path component '$current': $($_.Exception.Message)"
            }
            if ($componentExists -and (($attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)) {
                throw "Evidence destination contains a reparse-point component: '$current'"
            }

            $parent = [System.IO.Directory]::GetParent($current)
            if ($null -eq $parent) { break }
            $parentPath = $parent.FullName
            if ([System.String]::Equals($parentPath, $current, [System.StringComparison]::OrdinalIgnoreCase)) { break }
            $current = $parentPath
        }
        return $false
    } catch {
        throw "Unsafe L7 evidence path '$Path': reparse-point validation failed. $($_.Exception.Message)"
    }
}

Test-L7EvidencePathHasReparsePoint -Path $RedFaultInjectionDurablePath | Out-Null

function Get-L7PathState {
    param([Parameter(Mandatory)] [string]$Path)
    $state = @{
        QuerySucceeded = $false
        Exists         = $false
        IsRegularFile  = $false
        IsDirectory    = $false
        IsReadOnly     = $false
        Error          = ""
    }
    try {
        Test-L7EvidencePathHasReparsePoint -Path $Path | Out-Null
        try {
            $attributes = [System.IO.File]::GetAttributes($Path)
        } catch [System.IO.FileNotFoundException] {
            $state.QuerySucceeded = $true
            return $state
        } catch [System.IO.DirectoryNotFoundException] {
            $state.QuerySucceeded = $true
            return $state
        }
        $state.Exists = $true
        $state.IsDirectory = (($attributes -band [System.IO.FileAttributes]::Directory) -ne 0)
        $state.IsRegularFile = -not $state.IsDirectory
        $state.IsReadOnly = (($attributes -band [System.IO.FileAttributes]::ReadOnly) -ne 0)
        $state.QuerySucceeded = $true
    } catch {
        $state.Error = $_.Exception.Message
    }
    return $state
}

# WRITE-GUARD: returns whether an existing regular file has exactly one hard
# link. Opening the path read-only and querying its handle avoids following a
# reparse point (the caller has already run the reparse guard); failures are
# unknown and therefore unsafe.
function Get-L7HardLinkState {
    param([Parameter(Mandatory)] [string]$Path)
    $stream = $null
    try {
        $share = [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete
        $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, $share)
        $info = New-Object TsDevL7.ByHandleFileInformation
        $ok = [TsDevL7.NativeMethods]::GetFileInformationByHandle($stream.SafeFileHandle.DangerousGetHandle(), [ref]$info)
        if (-not $ok) {
            throw "GetFileInformationByHandle failed with Win32 error $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error())"
        }
        return @{ QuerySucceeded = $true; LinkCount = [uint32]$info.NumberOfLinks; IsSingleLink = ([uint32]$info.NumberOfLinks -eq 1); Error = "" }
    } catch {
        return @{ QuerySucceeded = $false; LinkCount = $null; IsSingleLink = $false; Error = $_.Exception.Message }
    } finally {
        if ($null -ne $stream) { $stream.Dispose() }
    }
}

# Remove-L7ExactFile -- removes one already-validated single-link regular file.
# With Type/ProgId/Extension supplied, both the pre-delete and post-delete state
# checks use Get-L7ArtifactState; without them it retains the generic safe-file
# behavior used by the L6.3 temporary self-check and recovery-marker cleanup.
# Hard-linked files are refused before deletion.
function Remove-L7ExactFile {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [string]$Type = "",
        [int]$ProgId = 0,
        [ValidateSet('ell','xml')]
        [string]$Extension = ""
    )
    $hasArtifactIdentity = ($Type -or $ProgId -ne 0 -or $Extension)
    if ($hasArtifactIdentity -and (-not $Type -or $ProgId -le 0 -or -not $Extension)) {
        return @{ Succeeded = $false; ExistsAfter = $null; Detail = "refused_incomplete_artifact_identity path=$Path type=$Type prog_id=$ProgId extension=$Extension" }
    }

    $getState = {
        param([string]$StatePath)
        if ($hasArtifactIdentity) {
            return (Get-L7ArtifactState -Path $StatePath -Type $Type -ProgId $ProgId -Extension $Extension)
        }
        return (Get-L7PathState -Path $StatePath)
    }

    $before = & $getState $Path
    if (-not $before.QuerySucceeded) {
        return @{ Succeeded = $false; ExistsAfter = $null; Detail = "inspection_failed path=$Path error=$($before.Error)" }
    }
    if ($hasArtifactIdentity -and -not $before.PathValidated) {
        return @{ Succeeded = $false; ExistsAfter = $null; Detail = "refused_unvalidated_artifact_path path=$Path error=$($before.Error)" }
    }
    if (-not $before.Exists) {
        return @{ Succeeded = $true; ExistsAfter = $false; Detail = "absent path=$Path" }
    }
    if (-not $before.IsRegularFile) {
        return @{ Succeeded = $false; ExistsAfter = $true; Detail = "refused_non_file path=$Path" }
    }
    $hardLinkState = Get-L7HardLinkState -Path $Path
    if (-not $hardLinkState.QuerySucceeded) {
        return @{ Succeeded = $false; ExistsAfter = $null; Detail = "hard_link_inspection_failed path=$Path error=$($hardLinkState.Error)" }
    }
    if (-not $hardLinkState.IsSingleLink) {
        return @{ Succeeded = $false; ExistsAfter = $true; Detail = "refused_hard_link path=$Path link_count=$($hardLinkState.LinkCount)" }
    }
    try {
        [System.IO.File]::Delete($Path)
    } catch {
        return @{ Succeeded = $false; ExistsAfter = $null; Detail = "delete_failed path=$Path error=$($_.Exception.Message)" }
    }
    $after = & $getState $Path
    if (-not $after.QuerySucceeded) {
        return @{ Succeeded = $false; ExistsAfter = $null; Detail = "post_delete_inspection_failed path=$Path error=$($after.Error)" }
    }
    if ($hasArtifactIdentity -and -not $after.PathValidated) {
        return @{ Succeeded = $false; ExistsAfter = $null; Detail = "post_delete_unvalidated_artifact_path path=$Path error=$($after.Error)" }
    }
    if ($after.Exists) {
        return @{ Succeeded = $false; ExistsAfter = $true; Detail = "still_exists path=$Path" }
    }
    return @{ Succeeded = $true; ExistsAfter = $false; Detail = "deleted path=$Path" }
}

function Get-L7ExistsState {
    param(
        [Parameter(Mandatory)] [string]$Type,
        [Parameter(Mandatory)] [string]$Name
    )
    $state = @{
        QuerySucceeded = $false
        Exists         = $false
        ExitCode       = -1
        Detail         = ""
    }
    try {
        $cliEx = Invoke-Cli -Arguments @("--json","exists",$Type,$Name)
    } catch {
        $state.Detail = "exists_exception=$($_.Exception.Message)"
        return $state
    }
    $state.ExitCode = $cliEx.ExitCode
    if ($cliEx.ExitCode -ne 0) {
        $state.Detail = "exists_exit=$($cliEx.ExitCode) stderr='$($cliEx.Stderr)' stdout='$($cliEx.Stdout)'"
        return $state
    }
    try {
        $json = $cliEx.Stdout | ConvertFrom-Json -ErrorAction Stop
        if ($null -eq $json.PSObject.Properties['exists']) {
            $state.Detail = "exists_json_missing_exists_field"
            return $state
        }
        if ($json.exists -isnot [bool]) {
            $state.Detail = "exists_json_exists_field_not_boolean"
            return $state
        }
        $state.Exists = $json.exists
        $state.QuerySucceeded = $true
        $state.Detail = "exists_exit=0 exists=$($state.Exists)"
    } catch {
        $state.Detail = "exists_json_parse_failed=$($_.Exception.Message)"
    }
    return $state
}

function Invoke-L7WarehouseCleanup {
    param(
        [Parameter(Mandatory)] [string]$Type,
        [Parameter(Mandatory)] [string]$Name
    )
    $result = @{
        CleanupSucceeded = $false
        ExistsGone       = $false
        EntryExisted     = $false
        DeleteAttempted  = $false
        DeleteSucceeded  = $false
        DeleteExitCode   = -1
        DeleteJsonSuccess = $false
        DeleteStderr     = ""
        DeleteStdout     = ""
        Detail           = ""
    }
    $before = Get-L7ExistsState -Type $Type -Name $Name
    if (-not $before.QuerySucceeded) {
        $result.Detail = "warehouse_before_unknown $($before.Detail)"
        return $result
    }
    $requiresDelete = $before.Exists
    $result.EntryExisted = $before.Exists
    if (-not $requiresDelete) {
        $confirmed = Get-L7ExistsState -Type $Type -Name $Name
        if (-not $confirmed.QuerySucceeded) {
            $result.Detail = "warehouse_confirmation_unknown $($confirmed.Detail)"
            return $result
        }
        if (-not $confirmed.Exists) {
            $result.CleanupSucceeded = $true
            $result.ExistsGone = $true
            $result.DeleteSucceeded = $true
            $result.Detail = "warehouse_absent delete_not_needed"
            return $result
        }
        $result.EntryExisted = $true
        $requiresDelete = $true
    }

    $result.DeleteAttempted = $true
    try {
        $cliDel = Invoke-Cli -Arguments @("--json","delete",$Type,$Name)
        $result.DeleteExitCode = $cliDel.ExitCode
        $result.DeleteStderr = $cliDel.Stderr
        $result.DeleteStdout = $cliDel.Stdout
    } catch {
        $result.Detail = "warehouse_delete_exception=$($_.Exception.Message)"
        return $result
    }
    if ($result.DeleteExitCode -ne 0) {
        $result.Detail = "warehouse_delete_failed exit=$($result.DeleteExitCode)"
        return $result
    }
    try {
        $deleteJson = $cliDel.Stdout | ConvertFrom-Json -ErrorAction Stop
        if ($null -eq $deleteJson.PSObject.Properties['success'] -or $deleteJson.success -isnot [bool] -or -not $deleteJson.success) {
            $result.Detail = "warehouse_delete_json_not_success"
            return $result
        }
        $result.DeleteJsonSuccess = $true
    } catch {
        $result.Detail = "warehouse_delete_json_parse_failed=$($_.Exception.Message)"
        return $result
    }
    $after = Get-L7ExistsState -Type $Type -Name $Name
    if (-not $after.QuerySucceeded) {
        $result.Detail = "warehouse_after_unknown $($after.Detail)"
        return $result
    }
    if ($after.Exists) {
        $result.Detail = "warehouse_entry_still_exists after_delete"
        return $result
    }
    $result.CleanupSucceeded = $true
    $result.ExistsGone = $true
    $result.DeleteSucceeded = $true
    $result.Detail = "warehouse_deleted exit=$($result.DeleteExitCode) exists_gone=true"
    return $result
}

# Resolve the effective TradeStation root and the one MyWork\Bin directory
# used for all artifact checks/cleanup. -BinPath is retained as a compatibility
# parameter, but it cannot select a second artifact root: when supplied it must
# resolve exactly to <effective TsPath>\MyWork\Bin.
$defaultTsPath = "C:\Program Files (x86)\TradeStation 9.5"
$effectiveTsPath = if ($TsPath) { $TsPath } else { $defaultTsPath }
try {
    $effectiveTsPathProvider = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($effectiveTsPath)
    $effectiveTsPathFull = [System.IO.Path]::GetFullPath($effectiveTsPathProvider)
    $canonicalBinPath = [System.IO.Path]::GetFullPath((Join-Path $effectiveTsPathFull "MyWork\Bin"))

    if ($BinPath) {
        $binPathProvider = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BinPath)
        $binPathFull = [System.IO.Path]::GetFullPath($binPathProvider)
        if (-not [System.String]::Equals($binPathFull, $canonicalBinPath, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "-BinPath '$BinPath' resolves to '$binPathFull', but the CLI artifact root for the effective TsPath is '$canonicalBinPath'. -BinPath must be omitted or exactly equal to that canonical path; it cannot select a separate artifact directory."
        }
    }

    $script:EffectiveTsPath = $effectiveTsPathFull
    $script:MyWorkBin = $canonicalBinPath
    $MyWorkBin = $canonicalBinPath
} catch {
    Write-Host "ERROR: Invalid TradeStation artifact path configuration: $($_.Exception.Message)" -ForegroundColor Red
    exit 2
}
try {
    Test-L7EvidencePathHasReparsePoint -Path $script:MyWorkBin | Out-Null
} catch {
    Write-Host "ERROR: Unsafe TradeStation artifact root '$script:MyWorkBin': $($_.Exception.Message)" -ForegroundColor Red
    exit 2
}

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
# Get-L7ArtifactState -- validates one exact, type-specific artifact path and
# then inspects that path without following reparse points. Cleanup callers pass
# the warehouse type, ProgId, and extension so a path outside the canonical
# MyWork\Bin, a mismatched basename, a directory, a reparse point, or a
# multi-link file fails closed before any delete is attempted. The optional
# generic Get-L7PathState
# path remains available for non-artifact self-check fixtures and marker files.
# ---------------------------------------------------------------------------
function Get-L7ArtifactState {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Type,
        [Parameter(Mandatory)] [int]$ProgId,
        [Parameter(Mandatory)] [ValidateSet('ell','xml')] [string]$Extension
    )
    $state = @{
        QuerySucceeded = $false
        Exists         = $false
        IsRegularFile  = $false
        IsDirectory    = $false
        IsReadOnly     = $false
        PathValidated  = $false
        Error          = ""
    }
    try {
        if ($ProgId -le 0) { throw "artifact ProgId must be positive" }
        $typeKey = $Type.ToLowerInvariant()
        if (-not $script:TypeInfo.ContainsKey($typeKey)) { throw "unsupported artifact type '$Type'" }
        $typeInfo = $script:TypeInfo[$typeKey]
        $expectedLeaf = "$($typeInfo.prefix)$ProgId.$Extension"

        $pathFull = [System.IO.Path]::GetFullPath($Path)
        $parent = Split-Path -Parent $pathFull
        if (-not $parent) { throw "artifact path has no parent directory" }
        $parentFull = [System.IO.Path]::GetFullPath($parent).TrimEnd([char[]]@('\', '/'))
        $binFull = [System.IO.Path]::GetFullPath($script:MyWorkBin).TrimEnd([char[]]@('\', '/'))
        if (-not [System.String]::Equals($parentFull, $binFull, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "artifact parent '$parentFull' is not the canonical MyWork\Bin '$binFull'"
        }
        if (-not [System.String]::Equals((Split-Path -Leaf $pathFull), $expectedLeaf, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "artifact leaf '$(Split-Path -Leaf $pathFull)' is not the expected '$expectedLeaf'"
        }

        Test-L7EvidencePathHasReparsePoint -Path $pathFull | Out-Null
        $state.PathValidated = $true
        $observed = Get-L7PathState -Path $pathFull
        $state.QuerySucceeded = $observed.QuerySucceeded
        $state.Exists = $observed.Exists
        $state.IsRegularFile = $observed.IsRegularFile
        $state.IsDirectory = $observed.IsDirectory
        $state.IsReadOnly = $observed.IsReadOnly
        $state.Error = $observed.Error
        if ($state.QuerySucceeded -and $state.Exists -and $state.IsRegularFile) {
            $hardLinkState = Get-L7HardLinkState -Path $pathFull
            if (-not $hardLinkState.QuerySucceeded) {
                $state.QuerySucceeded = $false
                $state.IsRegularFile = $false
                $state.Error = "hard-link inspection failed: $($hardLinkState.Error)"
            } elseif (-not $hardLinkState.IsSingleLink) {
                $state.QuerySucceeded = $false
                $state.IsRegularFile = $false
                $state.Error = "refused hard-linked artifact path (link_count=$($hardLinkState.LinkCount))"
            }
        }
    } catch {
        $state.Error = $_.Exception.Message
    }
    return $state
}

# ---------------------------------------------------------------------------
# Helper: Get-L7SanitationSnapshot -- captures one guarded production snapshot
# before any L7 sanitation action. The snapshot includes exact ELL/XML path
# validation, existence, regular-file/read-only state, SHA-256 hashes, and
# warehouse existence/query state. Missing paths are an incomplete snapshot,
# not a clean/absent assertion. Hash capture is successful for a verified
# absence (there is no hash to capture) and fails closed for an existing file
# whose hash cannot be read.
# ---------------------------------------------------------------------------
function Get-L7SanitationSnapshot {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string]$Type,
        [int]$ProgId = 0,
        [string]$EllPath = "",
        [string]$XmlPath = ""
    )
    $invalidArtifactState = @{
        QuerySucceeded = $false
        Exists         = $false
        IsRegularFile  = $false
        IsDirectory    = $false
        IsReadOnly     = $null
        PathValidated  = $false
        Error          = "artifact path not supplied"
    }
    $ellState = if ($EllPath -and $ProgId -gt 0) {
        Get-L7ArtifactState -Path $EllPath -Type $Type -ProgId $ProgId -Extension 'ell'
    } else {
        $invalidArtifactState.Clone()
    }
    $xmlState = if ($XmlPath -and $ProgId -gt 0) {
        Get-L7ArtifactState -Path $XmlPath -Type $Type -ProgId $ProgId -Extension 'xml'
    } else {
        $invalidArtifactState.Clone()
    }
    $warehouseState = Get-L7ExistsState -Type $Type -Name $Name

    $ellStateKnown = ($ellState.QuerySucceeded -and $ellState.PathValidated -and (-not $ellState.Exists -or $ellState.IsRegularFile))
    $xmlStateKnown = ($xmlState.QuerySucceeded -and $xmlState.PathValidated -and (-not $xmlState.Exists -or $xmlState.IsRegularFile))
    $warehouseStateKnown = [bool]$warehouseState.QuerySucceeded

    $ellHash = $null
    $xmlHash = $null
    $ellHashCaptured = $false
    $xmlHashCaptured = $false
    if ($ellStateKnown) {
        if (-not $ellState.Exists) {
            $ellHashCaptured = $true
        } else {
            try {
                $ellHash = (Get-FileHash -Path $EllPath -Algorithm SHA256 -ErrorAction Stop).Hash
                $ellHashCaptured = ($null -ne $ellHash)
            } catch {
                $ellHashCaptured = $false
            }
        }
    }
    if ($xmlStateKnown) {
        if (-not $xmlState.Exists) {
            $xmlHashCaptured = $true
        } else {
            try {
                $xmlHash = (Get-FileHash -Path $XmlPath -Algorithm SHA256 -ErrorAction Stop).Hash
                $xmlHashCaptured = ($null -ne $xmlHash)
            } catch {
                $xmlHashCaptured = $false
            }
        }
    }

    return @{
        EllState             = $ellState
        XmlState             = $xmlState
        WarehouseState       = $warehouseState
        EllStateKnown        = $ellStateKnown
        XmlStateKnown        = $xmlStateKnown
        WarehouseStateKnown  = $warehouseStateKnown
        EllExists             = if ($ellStateKnown) { [bool]$ellState.Exists } else { $null }
        XmlExists             = if ($xmlStateKnown) { [bool]$xmlState.Exists } else { $null }
        EllReadOnly           = if ($ellStateKnown -and $ellState.Exists) { [bool]$ellState.IsReadOnly } else { $null }
        XmlReadOnly           = if ($xmlStateKnown -and $xmlState.Exists) { [bool]$xmlState.IsReadOnly } else { $null }
        WarehouseEntryExists  = if ($warehouseStateKnown) { [bool]$warehouseState.Exists } else { $null }
        EllHash               = $ellHash
        EllHashCaptured       = $ellHashCaptured
        XmlHash               = $xmlHash
        XmlHashCaptured       = $xmlHashCaptured
        StateComplete         = ($ellStateKnown -and $xmlStateKnown -and $warehouseStateKnown -and $ellHashCaptured -and $xmlHashCaptured)
    }
}

# ---------------------------------------------------------------------------
# Is-Elevated check. Every direct CLI subcommand requires an elevated
# administrator process. With the default requireAdministrator manifest, a
# non-elevated launch can prompt before main(); if the elevation guard executes
# (for example in a manifest-disabled build), the CLI returns exit 11
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
    $fullArgs = @("--ts-path", $script:EffectiveTsPath)
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
# delete but BEFORE any best-effort sanitation). When Type/ProgId are supplied,
# both paths must pass exact basename, canonical MyWork\Bin, regular-file, and
# reparse-safe validation. Returns a hashtable with
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
        [string]$XmlPath = "",
        [string]$Type = "",
        [int]$ProgId = 0
    )
    $strictArtifactIdentity = ($Type -or $ProgId -ne 0)
    $ellState = @{ QuerySucceeded = $true; Exists = $false; IsRegularFile = $false; PathValidated = $true; Error = "" }
    $xmlState = @{ QuerySucceeded = $true; Exists = $false; IsRegularFile = $false; PathValidated = $true; Error = "" }

    if ($EllPath) {
        if ($strictArtifactIdentity) {
            if (-not $Type -or $ProgId -le 0) {
                $ellState = @{ QuerySucceeded = $false; Exists = $false; IsRegularFile = $false; PathValidated = $false; Error = "exact ELL artifact identity was not supplied" }
            } else {
                $ellState = Get-L7ArtifactState -Path $EllPath -Type $Type -ProgId $ProgId -Extension 'ell'
            }
        } else {
            $ellState = Get-L7PathState -Path $EllPath
            $ellState.PathValidated = $true
        }
    }
    if ($XmlPath) {
        if ($strictArtifactIdentity) {
            if (-not $Type -or $ProgId -le 0) {
                $xmlState = @{ QuerySucceeded = $false; Exists = $false; IsRegularFile = $false; PathValidated = $false; Error = "exact XML artifact identity was not supplied" }
            } else {
                $xmlState = Get-L7ArtifactState -Path $XmlPath -Type $Type -ProgId $ProgId -Extension 'xml'
            }
        } else {
            $xmlState = Get-L7PathState -Path $XmlPath
            $xmlState.PathValidated = $true
        }
    }

    $ellKnown = ($ellState.QuerySucceeded -and $ellState.PathValidated)
    $xmlKnown = ($xmlState.QuerySucceeded -and $xmlState.PathValidated)
    $ellExisted = ($ellKnown -and [bool]$ellState.Exists)
    $xmlExisted = ($xmlKnown -and [bool]$xmlState.Exists)
    return @{
        EllExistedBeforeSanitation    = $ellExisted
        XmlExistedBeforeSanitation    = $xmlExisted
        OrphanExistedBeforeSanitation = ((-not $ellKnown) -or (-not $xmlKnown) -or $ellExisted -or $xmlExisted)
        EllGone                       = ($ellKnown -and -not $ellExisted)
        XmlGone                       = ($xmlKnown -and -not $xmlExisted)
        EllStateKnown                 = $ellKnown
        XmlStateKnown                 = $xmlKnown
        EllStateError                 = $ellState.Error
        XmlStateError                 = $xmlState.Error
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
Write-Host "TSDev Automation - Unknown Link Regression Harness" -ForegroundColor Cyan
Write-Host "TVS-L.1 + TVS-L.4 + L5 (all 5 component types)"   -ForegroundColor Cyan
Write-Host "L7 fault injection is opt-in; normal cases run in run_regression.ps1" -ForegroundColor Cyan
Write-Host "================================================" -ForegroundColor Cyan
Write-Host "Run ID       : $runId"
Write-Host "Project root : $ProjectRoot"
Write-Host "CLI path     : $CliPath"
Write-Host "TS path      : $script:EffectiveTsPath"
Write-Host "MyWork\Bin   : $MyWorkBin"
Write-Host "Verbose      : $Verbose"
Write-Host "Temp dir     : $tmpDir"
Write-Host ""
Write-Host "NOTE: XML evidence is NOT runtime acceptance."    -ForegroundColor Yellow
Write-Host "      This harness checks artifact completion and fault handling, not" -ForegroundColor Yellow
Write-Host "      runtime linkability. It does not fake runtime success/failure."  -ForegroundColor Yellow
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

$myWorkBinState = Get-L7PathState -Path $MyWorkBin
if (-not $myWorkBinState.QuerySucceeded -or -not $myWorkBinState.Exists -or -not $myWorkBinState.IsDirectory) {
    $rootDetail = if (-not $myWorkBinState.QuerySucceeded) { $myWorkBinState.Error } elseif (-not $myWorkBinState.Exists) { "directory not found" } else { "path is not a directory" }
    Write-Host "ERROR: MyWork\Bin root is not a verified directory: $MyWorkBin ($rootDetail)" -ForegroundColor Red
    Write-Host "       Set -TsPath to the intended installation root (and omit -BinPath or pass its canonical MyWork\Bin path)." -ForegroundColor Yellow
    exit 2
}

# Create temp directory.
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null

# Unique probe name for this run.
$probeName = "TS_AUTO_L1_RED_${runId}"

# Minimal valid EasyLanguage strategy source. The leading "{ name }" text is
# arbitrary fixture source content only; it is not a metadata convention or a
# lookup mechanism. The CLI supplies the item name, and stored metadata comes
# from orlib20 warehouse records/APIs.
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
    # STEP 2b: XML exists and is fresh (historical pre-L.3 RED baseline was missing XML).
    # =====================================================================
    Write-Host "--- Step 2b: XML exists and is fresh ---" -ForegroundColor Cyan

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
    # (The missing-XML result belongs to the historical pre-L.3 RED baseline.)
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
    $state = Get-ArtifactSanitationState -EllPath $ellPath -XmlPath $xmlPath -Type 'strategy' -ProgId $progId
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
    $sanitizedEllGone = [bool]$ellGone
    $sanitizedXmlGone = [bool]$xmlGone
    if ($ellExistedBeforeSanitation) {
        $ellRemoval = Remove-L7ExactFile -Path $ellPath -Type 'strategy' -ProgId $progId -Extension 'ell'
        $sanitizedEllGone = ($ellRemoval.Succeeded -and $ellRemoval.ExistsAfter -eq $false)
    }
    if ($xmlExistedBeforeSanitation) {
        $xmlRemoval = Remove-L7ExactFile -Path $xmlPath -Type 'strategy' -ProgId $progId -Extension 'xml'
        $sanitizedXmlGone = ($xmlRemoval.Succeeded -and $xmlRemoval.ExistsAfter -eq $false)
    }
    $cleanupStatus += "artifacts after cleanup: ell_gone(pre-san)=$ellGone xml_gone(pre-san)=$xmlGone sanitized_ell=$sanitizedEllGone sanitized_xml=$sanitizedXmlGone"
    Write-Host "  artifacts after cleanup: ell_gone(pre-san)=$ellGone xml_gone(pre-san)=$xmlGone orphan_before=$orphanExistedBeforeSanitation sanitized_ell=$sanitizedEllGone sanitized_xml=$sanitizedXmlGone" -ForegroundColor $(if ($cleanupOk) { 'DarkGray' } else { 'Yellow' })

    Record-Result -StepId "L1.CLEANUP" -Description "cleanup removed warehouse entry and ELL/XML artifacts" `
        -Passed $cleanupOk -Detail $cleanupDetail | Out-Null
}

# ===========================================================================
# TVS-L.4 EXTENSIONS -- persistence validation + partial-artifact
# cleanup/rollback hardening (historical baseline; current checks are retained).
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
    $xmlState = Get-L7PathState -Path $XmlPath
    if (-not $xmlState.QuerySucceeded -or -not $xmlState.Exists -or -not $xmlState.IsRegularFile) { return $null }
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
    $ellState = Get-L7PathState -Path $EllPath
    if (-not $ellState.QuerySucceeded -or -not $ellState.Exists -or -not $ellState.IsRegularFile) { return $false }
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
# the .ell/.xml and ONLY THEN reported EllGone/XmlGone, so a production delete
# that left an orphan artifact was mutated away before the verdict was fixed --
# the cleanup verdict passed and the production failure evidence was gone. The
# repaired helper:
#   1. Captures PRE-SANITATION production state (whether the .ell/.xml still
#      existed AFTER the CLI delete but BEFORE any best-effort sanitation).
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

    # 4. Capture PRE-SANITATION production state via the shared exact-path,
    #    type-aware, reparse-safe artifact helper.
    $state = Get-ArtifactSanitationState -EllPath $EllPath -XmlPath $XmlPath -Type $Type -ProgId $ProgId
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
    #    verdict). Removes any orphan the production delete left behind through
    #    the same exact-path, type-aware, reparse-safe helper used by L7.
    $sanitizedEllGone = [bool]$ellGone
    $sanitizedXmlGone = [bool]$xmlGone
    if ($ellExistedBeforeSanitation) {
        $ellRemoval = Remove-L7ExactFile -Path $EllPath -Type $Type -ProgId $ProgId -Extension 'ell'
        $sanitizedEllGone = ($ellRemoval.Succeeded -and $ellRemoval.ExistsAfter -eq $false)
    }
    if ($xmlExistedBeforeSanitation) {
        $xmlRemoval = Remove-L7ExactFile -Path $XmlPath -Type $Type -ProgId $ProgId -Extension 'xml'
        $sanitizedXmlGone = ($xmlRemoval.Succeeded -and $xmlRemoval.ExistsAfter -eq $false)
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
        $cleanup = Invoke-CleanupForName -Name $Name -Type $Type -WhType $WhType -ProgId $existingProgId -EllPath $ell -XmlPath $xml
        if (-not $cleanup.VerdictOk) {
            throw "Pre-existing cleanup refused to proceed for $Type '$Name': $($cleanup.Detail)"
        }
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
    $l63OrphanRemoval = Remove-L7ExactFile -Path $l63Orphan
    $l63SanitizedEllGone = ($l63OrphanRemoval.Succeeded -and $l63OrphanRemoval.ExistsAfter -eq $false)
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
$l63SafetyRemoval = Remove-L7ExactFile -Path $l63Orphan

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
# Gated by -IncludeRedFaultInjection. The registered default regression suite
# (tests/run_regression.ps1) invokes this script WITHOUT that switch, so the
# opt-in fault-injection cases below are NOT registered in the default regression
# suite (handoff Sec.6: "Do not register intentionally RED cases in the
# default regression suite yet"). All L6 oracles and L1/L4/L5 normal-path
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
#   - production state captured before sanitation, sanitation actions/results,
#     and post-sanitation hygiene state (kept as separate evidence fields)
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
# $RedFaultInjectionDurablePath (an omitted destination becomes a run-specific
# gitignored tmp/RE_L7_RED_FAULT_INJECTION_EVIDENCE_<run-id>_<token>.md; the tracked
# docs/RE/RE_L7_RED_FAULT_INJECTION_EVIDENCE.md may be selected only when
# -UpdateTrackedEvidence is set). Every destination is exclusive-create; an
# existing destination is never overwritten. The summary captures exit codes,
# hashes, GUID occurrence counts, get-source output, the read-only-command
# capture, and invariant failures (handoff Sec.6 Evidence).
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
# (TS_AUTO_L7_* name, type-specific <prefix><ProgId>.ell/.xml basenames from
# $script:TypeInfo, path under the configured MyWork\Bin), then read-only is
# cleared, the warehouse entry is deleted via the CLI if it still exists, the
# EXACT orphan artifacts are removed (no directory scanning), absence is
# verified, and the marker is deleted ONLY on success. The current run's finally
# removes its own marker only after verified sanitation. The recovery directory is
# the ONLY path
# scanned, and only for *.json marker files; MyWork\Bin and any other
# directory are NEVER scanned or bulk-deleted.
# ---------------------------------------------------------------------------
$script:L7RecoveryDir = Join-Path $ProjectRoot "tmp\l7_recovery"
Test-L7EvidencePathHasReparsePoint -Path $script:L7RecoveryDir | Out-Null

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
    Test-L7EvidencePathHasReparsePoint -Path $script:L7RecoveryDir | Out-Null
    $recoveryState = Get-L7PathState -Path $script:L7RecoveryDir
    if (-not $recoveryState.QuerySucceeded) {
        throw "Unable to inspect L7 recovery directory '$script:L7RecoveryDir': $($recoveryState.Error)"
    }
    if (-not $recoveryState.Exists) {
        New-Item -ItemType Directory -Force -Path $script:L7RecoveryDir | Out-Null
        Test-L7EvidencePathHasReparsePoint -Path $script:L7RecoveryDir | Out-Null
        $recoveryState = Get-L7PathState -Path $script:L7RecoveryDir
    }
    if (-not $recoveryState.QuerySucceeded -or -not $recoveryState.Exists -or -not $recoveryState.IsDirectory) {
        throw "L7 recovery directory is not a verified directory: '$script:L7RecoveryDir'"
    }
    $ellArtifactState = Get-L7ArtifactState -Path $EllPath -Type $Type -ProgId $ProgId -Extension 'ell'
    $xmlArtifactState = Get-L7ArtifactState -Path $XmlPath -Type $Type -ProgId $ProgId -Extension 'xml'
    if (-not $ellArtifactState.QuerySucceeded -or -not $ellArtifactState.PathValidated -or -not $ellArtifactState.Exists -or -not $ellArtifactState.IsRegularFile) {
        throw "ELL recovery-marker path is not a verified exact regular artifact: '$EllPath' ($($ellArtifactState.Error))"
    }
    if (-not $xmlArtifactState.QuerySucceeded -or -not $xmlArtifactState.PathValidated -or -not $xmlArtifactState.Exists -or -not $xmlArtifactState.IsRegularFile) {
        throw "XML recovery-marker path is not a verified exact regular artifact: '$XmlPath' ($($xmlArtifactState.Error))"
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
    $safeName = ($Name -replace '[^A-Za-z0-9_]', '_')
    $markerPath = Join-Path $script:L7RecoveryDir "${RunId}_${CaseId}_${safeName}.json"
    Test-L7EvidencePathHasReparsePoint -Path $markerPath | Out-Null
    $markerState = Get-L7PathState -Path $markerPath
    if (-not $markerState.QuerySucceeded) {
        throw "Unable to inspect L7 recovery marker path '$markerPath': $($markerState.Error)"
    }
    if ($markerState.Exists) {
        throw "L7 recovery marker already exists; refusing to replace durable state: '$markerPath'"
    }

    $tmpPath = Join-Path $script:L7RecoveryDir (".{0}.{1}.tmp" -f ([System.IO.Path]::GetFileName($markerPath)), ([System.Guid]::NewGuid().ToString('N')))
    Test-L7EvidencePathHasReparsePoint -Path $tmpPath | Out-Null
    $stream = $null
    try {
        $stream = [System.IO.File]::Open($tmpPath, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
        $bytes = (New-Object -TypeName System.Text.UTF8Encoding -ArgumentList $false).GetBytes($json)
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    } finally {
        if ($null -ne $stream) { $stream.Dispose() }
    }
    Test-L7EvidencePathHasReparsePoint -Path $tmpPath | Out-Null
    $tmpState = Get-L7PathState -Path $tmpPath
    if (-not $tmpState.QuerySucceeded -or -not $tmpState.Exists -or -not $tmpState.IsRegularFile) {
        throw "L7 temporary recovery marker could not be verified: '$tmpPath'"
    }
    Test-L7EvidencePathHasReparsePoint -Path $markerPath | Out-Null
    $markerState = Get-L7PathState -Path $markerPath
    if (-not $markerState.QuerySucceeded) {
        throw "Unable to recheck L7 recovery marker path '$markerPath': $($markerState.Error)"
    }
    if ($markerState.Exists) {
        throw "L7 recovery marker appeared before commit; refusing to replace durable state: '$markerPath'"
    }
    [System.IO.File]::Move($tmpPath, $markerPath)
    Test-L7EvidencePathHasReparsePoint -Path $markerPath | Out-Null
    $markerState = Get-L7PathState -Path $markerPath
    if (-not $markerState.QuerySucceeded -or -not $markerState.Exists -or -not $markerState.IsRegularFile) {
        throw "L7 recovery marker commit could not be verified: '$markerPath'"
    }
    return $markerPath
}

# Helper: Remove-L7RecoveryMarker -- removes a marker file. Only called after
# verified sanitation (per-case finally) so a marker survives a crash that
# leaves artifacts behind for the next run's stale-marker recovery.
function Remove-L7RecoveryMarker {
    param([Parameter(Mandatory)] [string]$MarkerPath)
    $state = Get-L7PathState -Path $MarkerPath
    if (-not $state.QuerySucceeded) {
        return @{ Succeeded = $false; Detail = "marker_inspection_failed path=$MarkerPath error=$($state.Error)" }
    }
    if (-not $state.Exists) {
        return @{ Succeeded = $false; Detail = "marker_absence_not_expected path=$MarkerPath" }
    }
    if (-not $state.IsRegularFile) {
        return @{ Succeeded = $false; Detail = "marker_is_not_regular_file path=$MarkerPath" }
    }
    return (Remove-L7ExactFile -Path $MarkerPath)
}

# Helper: Test-L7MarkerValid -- strict validation of a parsed marker object.
# Returns $true only when EVERY field is present and matches the strict
# contracts:
#   - Name matches ^TS_AUTO_L7_[A-Z0-9_]+$
#   - Type is one of the 5 supported CLI types in $script:TypeInfo
#   - ProgId is a positive integer
#   - EllPath basename is exactly <type-prefix><ProgId>.ell
#   - XmlPath basename is exactly <type-prefix><ProgId>.xml
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
    $typeInfo = $script:TypeInfo[$Marker.Type]
    if ($null -eq $typeInfo -or -not ($typeInfo.prefix -is [string]) -or [string]::IsNullOrEmpty($typeInfo.prefix)) { return $false }
    $progId = 0
    try { $progId = [int]$Marker.ProgId } catch { return $false }
    if ($progId -le 0) { return $false }
    if (-not ($Marker.EllPath -is [string])) { return $false }
    if (-not ($Marker.XmlPath -is [string])) { return $false }
    $expectedEllBasename = "$($typeInfo.prefix)$progId.ell"
    $expectedXmlBasename = "$($typeInfo.prefix)$progId.xml"
    if ((Split-Path -Leaf $Marker.EllPath) -ne $expectedEllBasename) { return $false }
    if ((Split-Path -Leaf $Marker.XmlPath) -ne $expectedXmlBasename) { return $false }
    # Verify both paths resolve under the configured MyWork\Bin.
    $myWorkBinFull = $null
    try {
        Test-L7EvidencePathHasReparsePoint -Path $script:MyWorkBin | Out-Null
        $myWorkBinFull = [System.IO.Path]::GetFullPath($script:MyWorkBin).TrimEnd([char[]]@('\', '/'))
    } catch { return $false }
    foreach ($p in @($Marker.EllPath, $Marker.XmlPath)) {
        $parent = Split-Path -Parent $p
        if (-not $parent) { return $false }
        try {
            Test-L7EvidencePathHasReparsePoint -Path $p | Out-Null
            $pathFull = [System.IO.Path]::GetFullPath($p)
            $parentFull = [System.IO.Path]::GetFullPath((Split-Path -Parent $pathFull)).TrimEnd([char[]]@('\', '/'))
        } catch { return $false }
        if (-not [System.String]::Equals($parentFull, $myWorkBinFull, [System.StringComparison]::OrdinalIgnoreCase)) { return $false }
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
# manual inspection) and reported. Returns @{ Recovered; Skipped; Records } arrays;
# Records retain separate pre-sanitation, sanitation-action, and post-hygiene fields.
function Invoke-L7StaleMarkerRecovery {
    $recovered = @()
    $skipped = @()
    $records = @()
    $recoveryState = Get-L7PathState -Path $script:L7RecoveryDir
    if (-not $recoveryState.QuerySucceeded) {
        $skipped += "recovery_directory_inspection_failed:$script:L7RecoveryDir $($recoveryState.Error)"
        return @{ Recovered = $recovered; Skipped = $skipped; Records = $records }
    }
    if (-not $recoveryState.Exists) {
        return @{ Recovered = $recovered; Skipped = $skipped; Records = $records }
    }
    if (-not $recoveryState.IsDirectory) {
        $skipped += "recovery_directory_not_directory:$script:L7RecoveryDir"
        return @{ Recovered = $recovered; Skipped = $skipped; Records = $records }
    }
    try {
        Test-L7EvidencePathHasReparsePoint -Path $script:L7RecoveryDir | Out-Null
        $markerFiles = @(Get-ChildItem -Path $script:L7RecoveryDir -Filter '*.json' -File -ErrorAction Stop)
    } catch {
        $skipped += "recovery_directory_enumeration_failed:$script:L7RecoveryDir $($_.Exception.Message)"
        return @{ Recovered = $recovered; Skipped = $skipped; Records = $records }
    }
    foreach ($mf in $markerFiles) {
        $markerPath = $mf.FullName
        $markerState = Get-L7PathState -Path $markerPath
        if (-not $markerState.QuerySucceeded -or -not $markerState.Exists -or -not $markerState.IsRegularFile) {
            $detail = if (-not $markerState.QuerySucceeded) { $markerState.Error } elseif (-not $markerState.Exists) { "marker_disappeared" } else { "marker_is_not_regular_file" }
            $skipped += "marker_inspection_failed:$markerPath $detail"
            continue
        }
        $marker = $null
        try {
            Test-L7EvidencePathHasReparsePoint -Path $markerPath | Out-Null
            $marker = ([System.IO.File]::ReadAllText($markerPath)) | ConvertFrom-Json -ErrorAction Stop
        } catch {
            $skipped += "unparseable_marker:$markerPath"
            continue
        }
        $markerValid = $false
        try { $markerValid = Test-L7MarkerValid -Marker $marker } catch { $markerValid = $false }
        if (-not $markerValid) {
            $skipped += "invalid_marker:$markerPath (strict validation failed; left for manual inspection)"
            continue
        }

        $record = @{
            CaseId = "L7.0.$($marker.Name)"
            ItemIdentifier = $marker.Name
            ComponentType = $marker.Type
            ProgId = [int]$marker.ProgId
            PreSanitationSnapshotComplete = $false
            PreSanitationWarehouseQuerySucceeded = $false
            PreSanitationWarehouseEntryExists = $null
            PreSanitationEllExists = $null
            PreSanitationEllReadOnly = $null
            PreSanitationEllHash = $null
            PreSanitationEllHashCaptured = $false
            PreSanitationXmlExists = $null
            PreSanitationXmlReadOnly = $null
            PreSanitationXmlHash = $null
            PreSanitationXmlHashCaptured = $false
            SanitationReadOnlyRestored = $false
            SanitationWarehouseCleanupSucceeded = $false
            SanitationArtifactRemovalAttempted = $false
            SanitationArtifactEllGoneBeforeRemoval = $null
            SanitationArtifactXmlGoneBeforeRemoval = $null
            PostSanitationSnapshotComplete = $false
            PostSanitationWarehouseQuerySucceeded = $false
            PostSanitationWarehouseEntryExists = $null
            PostSanitationEllExists = $null
            PostSanitationEllReadOnly = $null
            PostSanitationEllHash = $null
            PostSanitationEllHashCaptured = $false
            PostSanitationXmlExists = $null
            PostSanitationXmlReadOnly = $null
            PostSanitationXmlHash = $null
            PostSanitationXmlHashCaptured = $false
            PostSanitationHygienePassed = $false
            Detail = ""
        }
        $detail = ""
        $success = $false
        try {
            $markerProgId = [int]$marker.ProgId

            # Capture all guarded production state before changing the ELL
            # attribute, invoking the sanitation CLI delete, or removing any
            # exact artifact. Unknown/incomplete state fails closed.
            $preSnapshot = Get-L7SanitationSnapshot -Name $marker.Name -Type $marker.Type -ProgId $markerProgId -EllPath $marker.EllPath -XmlPath $marker.XmlPath
            $record.PreSanitationSnapshotComplete = $preSnapshot.StateComplete
            $record.PreSanitationWarehouseQuerySucceeded = $preSnapshot.WarehouseStateKnown
            $record.PreSanitationWarehouseEntryExists = $preSnapshot.WarehouseEntryExists
            $record.PreSanitationEllExists = $preSnapshot.EllExists
            $record.PreSanitationEllReadOnly = $preSnapshot.EllReadOnly
            $record.PreSanitationEllHash = $preSnapshot.EllHash
            $record.PreSanitationEllHashCaptured = $preSnapshot.EllHashCaptured
            $record.PreSanitationXmlExists = $preSnapshot.XmlExists
            $record.PreSanitationXmlReadOnly = $preSnapshot.XmlReadOnly
            $record.PreSanitationXmlHash = $preSnapshot.XmlHash
            $record.PreSanitationXmlHashCaptured = $preSnapshot.XmlHashCaptured
            if (-not $preSnapshot.StateComplete) {
                throw "incomplete guarded pre-sanitation snapshot"
            }

            $readOnlyRestored = $false
            $roRestore = $null
            if (-not $preSnapshot.EllState.Exists) {
                $readOnlyRestored = $true
            } elseif ($preSnapshot.EllReadOnly) {
                $roRestore = Clear-ReadOnlyAttributeExact -Path $marker.EllPath
                $readOnlyRestored = ($roRestore.ExitCode -eq 0 -and $roRestore.IsReadOnlyAfter -eq $false)
            } else {
                $readOnlyRestored = $true
            }
            $record.SanitationReadOnlyRestored = $readOnlyRestored
            if (-not $readOnlyRestored) {
                throw "read-only restoration failed: exit=$($roRestore.ExitCode) isReadOnly=$($roRestore.IsReadOnlyAfter)"
            }

            $warehouse = Invoke-L7WarehouseCleanup -Type $marker.Type -Name $marker.Name
            $record.SanitationWarehouseCleanupSucceeded = [bool]$warehouse.CleanupSucceeded
            $entryGone = $warehouse.ExistsGone
            if (-not $warehouse.CleanupSucceeded) {
                throw "warehouse cleanup not verified: $($warehouse.Detail)"
            }

            # Separate sanitation-stage observation, after warehouse action and
            # before artifact removal.
            $artifactBeforeRemovalEllState = Get-L7ArtifactState -Path $marker.EllPath -Type $marker.Type -ProgId $markerProgId -Extension 'ell'
            $artifactBeforeRemovalXmlState = Get-L7ArtifactState -Path $marker.XmlPath -Type $marker.Type -ProgId $markerProgId -Extension 'xml'
            $artifactBeforeRemovalEllKnown = ($artifactBeforeRemovalEllState.QuerySucceeded -and $artifactBeforeRemovalEllState.PathValidated -and (-not $artifactBeforeRemovalEllState.Exists -or $artifactBeforeRemovalEllState.IsRegularFile))
            $artifactBeforeRemovalXmlKnown = ($artifactBeforeRemovalXmlState.QuerySucceeded -and $artifactBeforeRemovalXmlState.PathValidated -and (-not $artifactBeforeRemovalXmlState.Exists -or $artifactBeforeRemovalXmlState.IsRegularFile))
            $record.SanitationArtifactEllGoneBeforeRemoval = ($artifactBeforeRemovalEllKnown -and -not $artifactBeforeRemovalEllState.Exists)
            $record.SanitationArtifactXmlGoneBeforeRemoval = ($artifactBeforeRemovalXmlKnown -and -not $artifactBeforeRemovalXmlState.Exists)
            if (-not $artifactBeforeRemovalEllKnown -or -not $artifactBeforeRemovalXmlKnown) {
                throw "artifact state before removal was not verified"
            }

            $record.SanitationArtifactRemovalAttempted = $true
            $ellRemoval = Remove-L7ExactFile -Path $marker.EllPath -Type $marker.Type -ProgId $markerProgId -Extension 'ell'
            $xmlRemoval = Remove-L7ExactFile -Path $marker.XmlPath -Type $marker.Type -ProgId $markerProgId -Extension 'xml'
            $ellGone = ($ellRemoval.Succeeded -and $ellRemoval.ExistsAfter -eq $false)
            $xmlGone = ($xmlRemoval.Succeeded -and $xmlRemoval.ExistsAfter -eq $false)
            if (-not $ellGone -or -not $xmlGone) {
                throw "artifact cleanup not verified: ell=$($ellRemoval.Detail) xml=$($xmlRemoval.Detail)"
            }

            # Post-sanitation production hygiene is separate from the snapshot
            # and action fields. The marker is removed only after this state is
            # verified, so an incomplete recovery remains recoverable.
            $postSnapshot = Get-L7SanitationSnapshot -Name $marker.Name -Type $marker.Type -ProgId $markerProgId -EllPath $marker.EllPath -XmlPath $marker.XmlPath
            $record.PostSanitationSnapshotComplete = $postSnapshot.StateComplete
            $record.PostSanitationWarehouseQuerySucceeded = $postSnapshot.WarehouseStateKnown
            $record.PostSanitationWarehouseEntryExists = $postSnapshot.WarehouseEntryExists
            $record.PostSanitationEllExists = $postSnapshot.EllExists
            $record.PostSanitationEllReadOnly = $postSnapshot.EllReadOnly
            $record.PostSanitationEllHash = $postSnapshot.EllHash
            $record.PostSanitationEllHashCaptured = $postSnapshot.EllHashCaptured
            $record.PostSanitationXmlExists = $postSnapshot.XmlExists
            $record.PostSanitationXmlReadOnly = $postSnapshot.XmlReadOnly
            $record.PostSanitationXmlHash = $postSnapshot.XmlHash
            $record.PostSanitationXmlHashCaptured = $postSnapshot.XmlHashCaptured
            $record.PostSanitationHygienePassed = ($postSnapshot.StateComplete -and -not $postSnapshot.WarehouseEntryExists -and -not $postSnapshot.EllExists -and -not $postSnapshot.XmlExists -and $readOnlyRestored)
            if (-not $record.PostSanitationHygienePassed) {
                throw "post-sanitation hygiene state was not verified"
            }

            $markerRemoval = Remove-L7RecoveryMarker -MarkerPath $markerPath
            if (-not $markerRemoval.Succeeded) {
                throw "recovery marker removal not verified: $($markerRemoval.Detail)"
            }
            $success = $true
            $detail = "pre_complete=$($record.PreSanitationSnapshotComplete) pre_warehouse=$($record.PreSanitationWarehouseEntryExists) pre_ell=$($record.PreSanitationEllExists) pre_xml=$($record.PreSanitationXmlExists) ro_restored=$readOnlyRestored warehouse_cleanup=$($warehouse.CleanupSucceeded) ell_gone=$ellGone xml_gone=$xmlGone post_hygiene=$($record.PostSanitationHygienePassed) entry_gone=$entryGone"
        } catch {
            $detail = "incomplete: $($_.Exception.Message)"
        }
        $record.Detail = $detail
        $records += $record
        if ($success) {
            $recovered += "$($marker.Name):$detail"
        } else {
            $skipped += "incomplete_recovery:$markerPath $detail"
        }
    }
    return @{ Recovered = $recovered; Skipped = $skipped; Records = $records }
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
    $initialState = Get-L7PathState -Path $Path
    if (-not $initialState.QuerySucceeded -or -not $initialState.Exists -or -not $initialState.IsRegularFile) {
        throw "Cannot set read-only on an unverified regular file '$Path'"
    }
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
    $postState = Get-L7PathState -Path $Path
    $isReadOnlyAfter = $null
    if ($postState.QuerySucceeded -and $postState.Exists -and $postState.IsRegularFile) {
        $isReadOnlyAfter = $postState.IsReadOnly
    } elseif ($roExit -eq 0) {
        $roExit = -3
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
    $initialState = Get-L7PathState -Path $Path
    if (-not $initialState.QuerySucceeded) {
        return @{ Command = "attrib -R `"$Path`" (inspection failed)"; Stdout = ""; Stderr = $initialState.Error; ExitCode = -3; IsReadOnlyAfter = $null }
    }
    if (-not $initialState.Exists) {
        return @{ Command = "attrib -R `"$Path`" (skipped: file absent)"; Stdout = ""; Stderr = ""; ExitCode = 0; IsReadOnlyAfter = $false }
    }
    if (-not $initialState.IsRegularFile) {
        return @{ Command = "attrib -R `"$Path`" (refused: not a regular file)"; Stdout = ""; Stderr = ""; ExitCode = -3; IsReadOnlyAfter = $null }
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
    $postState = Get-L7PathState -Path $Path
    $isReadOnlyAfter = $null
    if ($postState.QuerySucceeded -and $postState.Exists -and $postState.IsRegularFile) {
        $isReadOnlyAfter = $postState.IsReadOnly
    } elseif ($roExit -eq 0) {
        $roExit = -3
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
    if ($Path) {
        $pathState = Get-L7PathState -Path $Path
        if (-not $pathState.QuerySucceeded -or -not $pathState.Exists -or -not $pathState.IsRegularFile) { return $null }
        try { return (Get-FileHash -Path $Path -Algorithm SHA256 -ErrorAction Stop).Hash } catch { return $null }
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
    if (-not $EllPath -or -not $GuidString) { return 0 }
    $ellState = Get-L7PathState -Path $EllPath
    if (-not $ellState.QuerySucceeded -or -not $ellState.Exists -or -not $ellState.IsRegularFile) { return 0 }
    try {
        $ellBytes = [System.IO.File]::ReadAllBytes($EllPath)
        $guidBytes = Convert-GuidToWindowsBytes -GuidString $GuidString
        $offsets = Find-BytePattern -Data $ellBytes -Pattern $guidBytes
        return $offsets.Count
    } catch { }
    return 0
}

# ---------------------------------------------------------------------------
# Helper: New-L7EvidenceRecord -- creates an honest evidence skeleton before a
# case's mutable operations begin. If a case throws before its normal evidence
# assignment, the sanitation helper can still attach its guarded pre/action/post
# fields instead of silently dropping the lifecycle record.
# ---------------------------------------------------------------------------
function New-L7EvidenceRecord {
    param(
        [Parameter(Mandatory)] [string]$CaseId,
        [Parameter(Mandatory)] [string]$CaseTitle,
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string]$Type,
        [Parameter(Mandatory)] [int]$ProgId,
        [Parameter(Mandatory)] [string]$Operation,
        [hashtable]$ReadOnlyCapture = $null
    )
    return @{
        CaseId = $CaseId
        CaseTitle = $CaseTitle
        ItemIdentifier = $Name
        ComponentType = $Type
        ProgId = $ProgId
        Operation = $Operation
        ReadOnlyCommand = if ($ReadOnlyCapture) { $ReadOnlyCapture.Command } else { "" }
        ReadOnlyExitCode = if ($ReadOnlyCapture) { $ReadOnlyCapture.ExitCode } else { $null }
        ReadOnlyIsReadOnlyAfter = if ($ReadOnlyCapture) { $ReadOnlyCapture.IsReadOnlyAfter } else { $null }
        ReadOnlyStdout = if ($ReadOnlyCapture) { $ReadOnlyCapture.Stdout } else { "" }
        ReadOnlyStderr = if ($ReadOnlyCapture) { $ReadOnlyCapture.Stderr } else { "" }
        PreEllHash = $null
        PreXmlHash = $null
        PostEllHash = $null
        PostXmlHash = $null
        OpExitCode = $null
        OpJsonSuccess = $null
        OpStderr = ""
        OpStdout = ""
        DesiredStatePassed = $false
        InvariantFailures = "case interrupted before operation evidence was assembled"
    }
}

# ---------------------------------------------------------------------------
# Helper: Invoke-L7Sanitation -- the per-case finally cleanup for L7.
#
# Ordering is deliberate and is part of the evidence contract:
#   1. Capture the guarded production snapshot (ELL/XML state and hashes, plus
#      warehouse state) before changing attributes, invoking the sanitation
#      delete, or removing any artifact.
#   2. Perform sanitation actions: restore ELL read-only, clean the warehouse,
#      then remove only the exact validated artifacts.
#   3. Capture the post-sanitation hygiene state and keep it separate from the
#      production snapshot. In particular, post-cleanup values are never put in
#      a *BeforeSanitation field.
#
# The intermediate artifact state after the sanitation warehouse operation and
# before artifact removal is used only for the sanitation delete verdict. A
# residual observed there remains a failed production/sanitation observation;
# the later hygiene removal does not rewrite that verdict as clean.
# ---------------------------------------------------------------------------
function Invoke-L7Sanitation {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string]$Type,
        [int]$ProgId = 0,
        [string]$EllPath = "",
        [string]$XmlPath = "",
        [hashtable]$Evidence = $null
    )

    # -----------------------------------------------------------------------
    # Production state snapshot. This call must remain before every sanitation
    # action below (Clear-ReadOnlyAttributeExact, Invoke-L7WarehouseCleanup,
    # and Remove-L7ExactFile). It is a guarded snapshot of the production
    # ELL/XML and warehouse state; it is not reconstructed after cleanup.
    # -----------------------------------------------------------------------
    $invalidArtifactState = @{
        QuerySucceeded = $false
        Exists         = $false
        IsRegularFile  = $false
        IsDirectory    = $false
        IsReadOnly     = $null
        PathValidated  = $false
        Error          = "artifact path not supplied"
    }
    $productionSnapshot = Get-L7SanitationSnapshot -Name $Name -Type $Type -ProgId $ProgId -EllPath $EllPath -XmlPath $XmlPath
    $productionEllState = $productionSnapshot.EllState
    $productionXmlState = $productionSnapshot.XmlState
    $productionWarehouseState = $productionSnapshot.WarehouseState
    $productionEllStateKnown = $productionSnapshot.EllStateKnown
    $productionXmlStateKnown = $productionSnapshot.XmlStateKnown
    $productionWarehouseStateKnown = $productionSnapshot.WarehouseStateKnown
    $productionEllExists = $productionSnapshot.EllExists
    $productionXmlExists = $productionSnapshot.XmlExists
    $productionWarehouseEntryExists = $productionSnapshot.WarehouseEntryExists
    $productionEllReadOnly = $productionSnapshot.EllReadOnly
    $productionXmlReadOnly = $productionSnapshot.XmlReadOnly
    $productionEllHash = $productionSnapshot.EllHash
    $productionEllHashCaptured = $productionSnapshot.EllHashCaptured
    $productionXmlHash = $productionSnapshot.XmlHash
    $productionXmlHashCaptured = $productionSnapshot.XmlHashCaptured
    $productionStateComplete = $productionSnapshot.StateComplete

    if ($Evidence) {
        # Populate production evidence while the guarded snapshot is still the
        # current state. Later blocks add sanitation actions and post-hygiene
        # state under separate field names; no post-cleanup value is assigned
        # to a production-before-sanitation field.
        $Evidence.ProductionWarehouseQuerySucceeded = $productionWarehouseState.QuerySucceeded
        $Evidence.ProductionWarehouseEntryExistsBeforeSanitation = $productionWarehouseEntryExists
        $Evidence.ProductionWarehouseDetailBeforeSanitation = $productionWarehouseState.Detail
        $Evidence.ProductionEllExistsBeforeSanitation = $productionEllExists
        $Evidence.ProductionEllReadOnlyBeforeSanitation = $productionEllReadOnly
        $Evidence.ProductionEllHashBeforeSanitation = $productionEllHash
        $Evidence.ProductionEllHashCapturedBeforeSanitation = $productionEllHashCaptured
        $Evidence.ProductionEllStateKnownBeforeSanitation = $productionEllStateKnown
        $Evidence.ProductionXmlExistsBeforeSanitation = $productionXmlExists
        $Evidence.ProductionXmlReadOnlyBeforeSanitation = $productionXmlReadOnly
        $Evidence.ProductionXmlHashBeforeSanitation = $productionXmlHash
        $Evidence.ProductionXmlHashCapturedBeforeSanitation = $productionXmlHashCaptured
        $Evidence.ProductionXmlStateKnownBeforeSanitation = $productionXmlStateKnown
        $Evidence.ProductionStateCompleteBeforeSanitation = $productionStateComplete
    }

    # -----------------------------------------------------------------------
    # Sanitation actions/results: restore the attribute, then clean the
    # warehouse. Artifact deletion remains withheld unless every required
    # state/guard is verified, preserving fail-closed behavior.
    # -----------------------------------------------------------------------
    $roRestore = $null
    $readOnlyRestored = $true
    $readOnlyAction = if ($EllPath) { "not_attempted" } else { "not_attempted_path_not_supplied" }
    if (-not $productionStateComplete) {
        $readOnlyRestored = $false
        $readOnlyAction = "withheld_incomplete_production_snapshot"
    } elseif ($EllPath) {
        if (-not $productionEllStateKnown) {
            $readOnlyRestored = $false
            $readOnlyAction = "withheld_state_unknown"
        } elseif ($productionEllState.Exists) {
            if ($productionEllState.IsReadOnly) {
                $readOnlyAction = "attempted_attrib_minus_R"
                try {
                    $roRestore = Clear-ReadOnlyAttributeExact -Path $EllPath
                    $readOnlyRestored = ($roRestore.ExitCode -eq 0 -and $roRestore.IsReadOnlyAfter -eq $false)
                } catch {
                    $roRestore = @{ Command = "attrib -R `"$EllPath`""; Stdout = ""; Stderr = $_.Exception.Message; ExitCode = -3; IsReadOnlyAfter = $null }
                    $readOnlyRestored = $false
                }
                Write-Host "  L7 sanitation: restored read-only attribute on $EllPath (exit=$($roRestore.ExitCode) isReadOnly=$($roRestore.IsReadOnlyAfter))" -ForegroundColor DarkGray
            } else {
                $readOnlyAction = "not_needed_already_clear"
            }
        } else {
            $readOnlyAction = "not_needed_absent"
        }
    }

    $warehouse = Invoke-L7WarehouseCleanup -Type $Type -Name $Name
    $existsGone = $warehouse.ExistsGone
    $deletedEntry = $warehouse.CleanupSucceeded
    Write-Host "  L7 sanitation warehouse cleanup($Type $Name): delete_attempted=$($warehouse.DeleteAttempted) delete_exit=$($warehouse.DeleteExitCode) exists_gone=$existsGone" -ForegroundColor $(if ($warehouse.CleanupSucceeded) { 'DarkGray' } else { 'Yellow' })

    # This is an explicitly separate sanitation-stage observation: it is after
    # the warehouse action but before any artifact removal. It is never called
    # a production-before-sanitation value.
    $artifactBeforeRemovalEllState = if ($EllPath) { Get-L7ArtifactState -Path $EllPath -Type $Type -ProgId $ProgId -Extension 'ell' } else { $invalidArtifactState.Clone() }
    $artifactBeforeRemovalXmlState = if ($XmlPath) { Get-L7ArtifactState -Path $XmlPath -Type $Type -ProgId $ProgId -Extension 'xml' } else { $invalidArtifactState.Clone() }
    $artifactBeforeRemovalEllKnown = ($artifactBeforeRemovalEllState.QuerySucceeded -and $artifactBeforeRemovalEllState.PathValidated -and (-not $artifactBeforeRemovalEllState.Exists -or $artifactBeforeRemovalEllState.IsRegularFile))
    $artifactBeforeRemovalXmlKnown = ($artifactBeforeRemovalXmlState.QuerySucceeded -and $artifactBeforeRemovalXmlState.PathValidated -and (-not $artifactBeforeRemovalXmlState.Exists -or $artifactBeforeRemovalXmlState.IsRegularFile))
    $ellGoneBeforeArtifactRemoval = ($artifactBeforeRemovalEllKnown -and -not $artifactBeforeRemovalEllState.Exists)
    $xmlGoneBeforeArtifactRemoval = ($artifactBeforeRemovalXmlKnown -and -not $artifactBeforeRemovalXmlState.Exists)
    $orphanBeforeArtifactRemoval = if ($artifactBeforeRemovalEllKnown -and $artifactBeforeRemovalXmlKnown) { ([bool]$artifactBeforeRemovalEllState.Exists -or [bool]$artifactBeforeRemovalXmlState.Exists) } else { $null }

    $verdictOk = Get-CleanupVerdict -DeletedEntry $deletedEntry -ExistsGone $existsGone -EllGone $ellGoneBeforeArtifactRemoval -XmlGone $xmlGoneBeforeArtifactRemoval

    $sanitizedEllGone = $false
    $sanitizedXmlGone = $false
    $ellRemovalDetail = "not_attempted"
    $xmlRemovalDetail = "not_attempted"
    $ellRemoval = $null
    $xmlRemoval = $null
    if ($productionStateComplete -and $warehouse.CleanupSucceeded -and $readOnlyRestored -and $artifactBeforeRemovalEllKnown -and $artifactBeforeRemovalXmlKnown) {
        $ellRemoval = if ($EllPath) { Remove-L7ExactFile -Path $EllPath -Type $Type -ProgId $ProgId -Extension 'ell' } else { @{ Succeeded = $true; ExistsAfter = $false; Detail = "path_not_supplied" } }
        $xmlRemoval = if ($XmlPath) { Remove-L7ExactFile -Path $XmlPath -Type $Type -ProgId $ProgId -Extension 'xml' } else { @{ Succeeded = $true; ExistsAfter = $false; Detail = "path_not_supplied" } }
        $sanitizedEllGone = ($ellRemoval.Succeeded -and $ellRemoval.ExistsAfter -eq $false)
        $sanitizedXmlGone = ($xmlRemoval.Succeeded -and $xmlRemoval.ExistsAfter -eq $false)
        $ellRemovalDetail = $ellRemoval.Detail
        $xmlRemovalDetail = $xmlRemoval.Detail
    } elseif (-not $productionStateComplete) {
        $ellRemovalDetail = "withheld_incomplete_production_snapshot"
        $xmlRemovalDetail = "withheld_incomplete_production_snapshot"
    } elseif (-not $warehouse.CleanupSucceeded) {
        $ellRemovalDetail = "withheld_warehouse_state_unknown"
        $xmlRemovalDetail = "withheld_warehouse_state_unknown"
    } elseif (-not $readOnlyRestored) {
        $ellRemovalDetail = "withheld_read_only_restore_unverified"
        $xmlRemovalDetail = "withheld_read_only_restore_unverified"
    } else {
        $ellRemovalDetail = "withheld_artifact_state_unknown"
        $xmlRemovalDetail = "withheld_artifact_state_unknown"
    }
    Write-Host "  L7 sanitation artifacts: gone_before_removal=$ellGoneBeforeArtifactRemoval/$xmlGoneBeforeArtifactRemoval orphan_before_removal=$orphanBeforeArtifactRemoval sanitized_ell=$sanitizedEllGone sanitized_xml=$sanitizedXmlGone" -ForegroundColor $(if ($sanitizedEllGone -and $sanitizedXmlGone) { 'DarkGray' } else { 'Yellow' })

    # -----------------------------------------------------------------------
    # Post-sanitation hygiene state. These queries are guarded and are kept
    # separate from the production snapshot and sanitation-stage verdict.
    # -----------------------------------------------------------------------
    $postEllState = if ($EllPath) { Get-L7ArtifactState -Path $EllPath -Type $Type -ProgId $ProgId -Extension 'ell' } else { $invalidArtifactState.Clone() }
    $postXmlState = if ($XmlPath) { Get-L7ArtifactState -Path $XmlPath -Type $Type -ProgId $ProgId -Extension 'xml' } else { $invalidArtifactState.Clone() }
    $postWarehouseState = Get-L7ExistsState -Type $Type -Name $Name
    $postEllStateKnown = ($postEllState.QuerySucceeded -and $postEllState.PathValidated -and (-not $postEllState.Exists -or $postEllState.IsRegularFile))
    $postXmlStateKnown = ($postXmlState.QuerySucceeded -and $postXmlState.PathValidated -and (-not $postXmlState.Exists -or $postXmlState.IsRegularFile))
    $postWarehouseStateKnown = [bool]$postWarehouseState.QuerySucceeded
    $postEllExists = if ($postEllStateKnown) { [bool]$postEllState.Exists } else { $null }
    $postXmlExists = if ($postXmlStateKnown) { [bool]$postXmlState.Exists } else { $null }
    $postWarehouseEntryExists = if ($postWarehouseStateKnown) { [bool]$postWarehouseState.Exists } else { $null }
    $postEllHash = if ($postEllStateKnown -and $postEllState.Exists -and $postEllState.IsRegularFile) { Get-FileHashOrNull -Path $EllPath } else { $null }
    $postXmlHash = if ($postXmlStateKnown -and $postXmlState.Exists -and $postXmlState.IsRegularFile) { Get-FileHashOrNull -Path $XmlPath } else { $null }
    $postEllHashCaptured = if ($postEllStateKnown -and -not $postEllState.Exists) { $true } elseif ($postEllStateKnown -and $postEllState.Exists) { $null -ne $postEllHash } else { $false }
    $postXmlHashCaptured = if ($postXmlStateKnown -and -not $postXmlState.Exists) { $true } elseif ($postXmlStateKnown -and $postXmlState.Exists) { $null -ne $postXmlHash } else { $false }
    $postHygieneOk = ($productionStateComplete -and $warehouse.CleanupSucceeded -and $readOnlyRestored -and $postEllStateKnown -and -not $postEllState.Exists -and $postXmlStateKnown -and -not $postXmlState.Exists -and $postWarehouseStateKnown -and -not $postWarehouseState.Exists)

    if ($Evidence) {
        # Sanitation actions/results.
        $Evidence.ReadOnlyRestored = $readOnlyRestored
        $Evidence.ReadOnlyRestoreAction = $readOnlyAction
        $Evidence.ReadOnlyRestoreCommand = if ($roRestore) { $roRestore.Command } else { "" }
        $Evidence.ReadOnlyRestoreExitCode = if ($roRestore) { $roRestore.ExitCode } else { 0 }
        $Evidence.ReadOnlyRestoreStdout = if ($roRestore) { $roRestore.Stdout } else { "" }
        $Evidence.ReadOnlyRestoreStderr = if ($roRestore) { $roRestore.Stderr } else { "" }
        $Evidence.SanitationWarehouseCleanupSucceeded = $warehouse.CleanupSucceeded
        $Evidence.SanitationWarehouseDetail = $warehouse.Detail
        $Evidence.SanitationWarehouseDeleteAttempted = $warehouse.DeleteAttempted
        $Evidence.SanitationWarehouseDeleteExitCode = $warehouse.DeleteExitCode
        $Evidence.SanitationWarehouseDeleteJsonSuccess = $warehouse.DeleteJsonSuccess
        $Evidence.SanitationWarehouseExistsGone = $warehouse.ExistsGone
        $Evidence.SanitationArtifactEllGoneBeforeRemoval = $ellGoneBeforeArtifactRemoval
        $Evidence.SanitationArtifactXmlGoneBeforeRemoval = $xmlGoneBeforeArtifactRemoval
        $Evidence.SanitationArtifactOrphanBeforeRemoval = $orphanBeforeArtifactRemoval
        $Evidence.SanitationEllRemovalSucceeded = if ($ellRemoval) { $ellRemoval.Succeeded } else { $false }
        $Evidence.SanitationXmlRemovalSucceeded = if ($xmlRemoval) { $xmlRemoval.Succeeded } else { $false }
        $Evidence.SanitationEllRemovalDetail = $ellRemovalDetail
        $Evidence.SanitationXmlRemovalDetail = $xmlRemovalDetail
        $Evidence.SanitationDeleteVerdictBeforeArtifactRemoval = $verdictOk

        # Post-sanitation hygiene state.
        $Evidence.PostSanitationWarehouseQuerySucceeded = $postWarehouseState.QuerySucceeded
        $Evidence.PostSanitationWarehouseEntryPresent = $postWarehouseEntryExists
        $Evidence.PostSanitationEllPresent = $postEllExists
        $Evidence.PostSanitationXmlPresent = $postXmlExists
        $Evidence.PostSanitationEllReadOnly = if ($postEllStateKnown -and $postEllState.Exists) { [bool]$postEllState.IsReadOnly } else { $null }
        $Evidence.PostSanitationXmlReadOnly = if ($postXmlStateKnown -and $postXmlState.Exists) { [bool]$postXmlState.IsReadOnly } else { $null }
        $Evidence.PostSanitationEllHash = $postEllHash
        $Evidence.PostSanitationEllHashCaptured = $postEllHashCaptured
        $Evidence.PostSanitationXmlHash = $postXmlHash
        $Evidence.PostSanitationXmlHashCaptured = $postXmlHashCaptured
        $Evidence.PostSanitationHygienePassed = $postHygieneOk
    }

    return @{
        # Production state captured before any sanitation action.
        ProductionWarehouseState          = $productionWarehouseState
        ProductionWarehouseQuerySucceeded  = $productionWarehouseStateKnown
        ProductionWarehouseEntryExists     = $productionWarehouseEntryExists
        ProductionEllState                 = $productionEllState
        ProductionXmlState                 = $productionXmlState
        ProductionEllExists                = $productionEllExists
        ProductionEllReadOnly              = $productionEllReadOnly
        ProductionXmlExists                = $productionXmlExists
        ProductionXmlReadOnly              = $productionXmlReadOnly
        ProductionEllHash                  = $productionEllHash
        ProductionEllHashCaptured          = $productionEllHashCaptured
        ProductionXmlHash                  = $productionXmlHash
        ProductionXmlHashCaptured          = $productionXmlHashCaptured
        ProductionStateComplete            = $productionStateComplete

        # Sanitation actions/results.
        DeleteExitCode                     = $warehouse.DeleteExitCode
        DeleteStderr                       = $warehouse.DeleteStderr
        DeleteStdout                       = $warehouse.DeleteStdout
        DeleteJsonSuccess                  = $warehouse.DeleteJsonSuccess
        WarehouseCleanupSucceeded          = $warehouse.CleanupSucceeded
        WarehouseCleanupDetail             = $warehouse.Detail
        DeletedEntry                       = $deletedEntry
        ExistsGone                         = $existsGone
        ArtifactEllGoneBeforeRemoval       = $ellGoneBeforeArtifactRemoval
        ArtifactXmlGoneBeforeRemoval       = $xmlGoneBeforeArtifactRemoval
        ArtifactOrphanBeforeRemoval        = $orphanBeforeArtifactRemoval
        SanitizedEllGone                   = $sanitizedEllGone
        SanitizedXmlGone                   = $sanitizedXmlGone
        ReadOnlyRestored                   = $readOnlyRestored
        ReadOnlyRestoreAction               = $readOnlyAction
        ReadOnlyRestoreCommand             = if ($roRestore) { $roRestore.Command } else { "" }
        ReadOnlyRestoreExitCode            = if ($roRestore) { $roRestore.ExitCode } else { 0 }
        ReadOnlyRestoreStdout              = if ($roRestore) { $roRestore.Stdout } else { "" }
        ReadOnlyRestoreStderr              = if ($roRestore) { $roRestore.Stderr } else { "" }
        SanitationVerdictOk                = $verdictOk
        VerdictOk                          = $verdictOk

        # Post-sanitation hygiene state.
        PostSanitationWarehouseState       = $postWarehouseState
        PostSanitationWarehouseEntryExists = $postWarehouseEntryExists
        PostSanitationEllState             = $postEllState
        PostSanitationXmlState             = $postXmlState
        PostSanitationEllExists            = $postEllExists
        PostSanitationXmlExists            = $postXmlExists
        PostSanitationEllReadOnly          = if ($postEllStateKnown -and $postEllState.Exists) { [bool]$postEllState.IsReadOnly } else { $null }
        PostSanitationXmlReadOnly          = if ($postXmlStateKnown -and $postXmlState.Exists) { [bool]$postXmlState.IsReadOnly } else { $null }
        PostSanitationEllHash              = $postEllHash
        PostSanitationEllHashCaptured      = $postEllHashCaptured
        PostSanitationXmlHash              = $postXmlHash
        PostSanitationXmlHashCaptured      = $postXmlHashCaptured
        PostSanitationHygieneOk            = $postHygieneOk
        Detail                             = "production_warehouse=$productionWarehouseEntryExists production_ell=$productionEllExists production_xml=$productionXmlExists deleted=$deletedEntry delete_attempted=$($warehouse.DeleteAttempted) delete_exit=$($warehouse.DeleteExitCode) exists_gone=$existsGone artifact_ell_gone_before_removal=$ellGoneBeforeArtifactRemoval artifact_xml_gone_before_removal=$xmlGoneBeforeArtifactRemoval sanitation_verdict=$verdictOk sanitized_ell=$sanitizedEllGone sanitized_xml=$sanitizedXmlGone post_warehouse=$postWarehouseEntryExists post_ell=$postEllExists post_xml=$postXmlExists post_hygiene=$postHygieneOk ell_cleanup='$ellRemovalDetail' xml_cleanup='$xmlRemovalDetail' ro_restored=$readOnlyRestored"
    }
}

# ---------------------------------------------------------------------------
# Helper: Write-L7DurableEvidence -- writes the complete RED reproduction
# evidence (handoff Sec.6 Evidence) to the supplied $Path. The omitted default
# is a run-specific gitignored tmp/ copy; the tracked docs/RE/ file may be
# selected only when the operator passes -UpdateTrackedEvidence (see the
# param-block write-guard).
# Raw logs remain transient; this summary includes exit codes, hashes, GUID
# occurrence counts, get-source output, the read-only-command capture, and
# invariant failures. Selecting the tracked path requires
# -UpdateTrackedEvidence, but that switch does not permit overwriting; every
# destination uses exclusive create and an existing destination fails closed.
# ---------------------------------------------------------------------------
function Write-L7DurableEvidence {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [System.Collections.Generic.List[hashtable]]$Evidence,
        [switch]$AllowTrackedPath
    )
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine("# TVS-L.7 RED Fault-Injection Evidence - Historical/Transient Recorded Run")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("> This evidence records one historical/transient recorded run only; it is not a current run and not current production-acceptance proof.")
    [void]$sb.AppendLine("> Exact ProgIds, hashes, exit codes, and outcomes below are unverified outside that recorded run.")
    [void]$sb.AppendLine("> Routine ``-IncludeRedFaultInjection`` runs write run-specific gitignored ``tmp/`` evidence; ``-UpdateTrackedEvidence`` authorizes the tracked destination, but an existing destination is never overwritten.")
    [void]$sb.AppendLine("> This evidence document is assembled after cleanup. The production-state snapshot is captured before sanitation begins (before attribute restore, the sanitation CLI delete, or artifact removal). Sanitation action/result fields and post-sanitation hygiene fields are reported separately; no post-cleanup value is labeled as production-before-sanitation.")
    [void]$sb.AppendLine("> Textual stdout/stderr in this retained/generated Markdown is PowerShell-rendered and may contain encoding artifacts; it is not a byte-exact transcript. Underlying native bytes are not verified.")
    [void]$sb.AppendLine("> Produced by ``tests/test_unknown_link_regression.ps1 -IncludeRedFaultInjection``.")
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
        [void]$sb.AppendLine("- Stdout (PowerShell-rendered text; encoding artifacts possible; not byte-exact): ``$($e.ReadOnlyStdout)``")
        [void]$sb.AppendLine("- Stderr (PowerShell-rendered text; encoding artifacts possible; not byte-exact): ``$($e.ReadOnlyStderr)``")
        [void]$sb.AppendLine("")
        [void]$sb.AppendLine("### Operation measurements captured before sanitation (pre/post-operation ELL/XML SHA-256 hashes)")
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
        [void]$sb.AppendLine("- Stderr (PowerShell-rendered text; encoding artifacts possible; not byte-exact): ``$($e.OpStderr)``")
        [void]$sb.AppendLine("- Stdout (PowerShell-rendered text; encoding artifacts possible; not byte-exact): ``$($e.OpStdout)``")
        [void]$sb.AppendLine("")
        if ($e.ContainsKey('WarehouseEntryPresent')) {
            [void]$sb.AppendLine("### Fault B production operation state before finally sanitation")
            [void]$sb.AppendLine("- Warehouse entry present after production delete: $($e.WarehouseEntryPresent)")
            [void]$sb.AppendLine("- XML present after production delete: $($e.XmlPresentAfter)")
            [void]$sb.AppendLine("- ELL present after production delete (orphan observation): $($e.EllPresentAfter)")
            [void]$sb.AppendLine("- Exact ``failed to delete ... error=5`` line captured: ``$($e.DeleteError5Line)``")
            [void]$sb.AppendLine("")
        }
        [void]$sb.AppendLine("### Production operation desired-state invariant result (recorded before finally sanitation)")
        [void]$sb.AppendLine("- Production desired-state passed: **$($e.DesiredStatePassed)**")
        [void]$sb.AppendLine("- Invariant failures: $($e.InvariantFailures)")
        [void]$sb.AppendLine("")
        [void]$sb.AppendLine("### Production state captured before sanitation")
        [void]$sb.AppendLine("- Warehouse query succeeded: $($e.ProductionWarehouseQuerySucceeded)")
        [void]$sb.AppendLine("- Warehouse query detail: $($e.ProductionWarehouseDetailBeforeSanitation)")
        [void]$sb.AppendLine("- Warehouse entry existed before sanitation: $($e.ProductionWarehouseEntryExistsBeforeSanitation)")
        [void]$sb.AppendLine("- ELL existed before sanitation: $($e.ProductionEllExistsBeforeSanitation)")
        [void]$sb.AppendLine("- ELL read-only before sanitation: $($e.ProductionEllReadOnlyBeforeSanitation)")
        [void]$sb.AppendLine("- ELL SHA-256 before sanitation: $($e.ProductionEllHashBeforeSanitation)")
        [void]$sb.AppendLine("- ELL hash captured before sanitation: $($e.ProductionEllHashCapturedBeforeSanitation)")
        [void]$sb.AppendLine("- XML existed before sanitation: $($e.ProductionXmlExistsBeforeSanitation)")
        [void]$sb.AppendLine("- XML read-only before sanitation: $($e.ProductionXmlReadOnlyBeforeSanitation)")
        [void]$sb.AppendLine("- XML SHA-256 before sanitation: $($e.ProductionXmlHashBeforeSanitation)")
        [void]$sb.AppendLine("- XML hash captured before sanitation: $($e.ProductionXmlHashCapturedBeforeSanitation)")
        [void]$sb.AppendLine("- Complete guarded production snapshot: $($e.ProductionStateCompleteBeforeSanitation)")
        [void]$sb.AppendLine("")
        [void]$sb.AppendLine("### Sanitation actions/results")
        [void]$sb.AppendLine("- Read-only restoration action: $($e.ReadOnlyRestoreAction)")
        [void]$sb.AppendLine("- Read-only attribute restored: $($e.ReadOnlyRestored)")
        [void]$sb.AppendLine("- Read-only restore command: ``$($e.ReadOnlyRestoreCommand)``")
        [void]$sb.AppendLine("- Read-only restore exit code: $($e.ReadOnlyRestoreExitCode)")
        [void]$sb.AppendLine("- Read-only restore stdout (PowerShell-rendered; not byte-exact): ``$($e.ReadOnlyRestoreStdout)``")
        [void]$sb.AppendLine("- Read-only restore stderr (PowerShell-rendered; not byte-exact): ``$($e.ReadOnlyRestoreStderr)``")
        [void]$sb.AppendLine("- Warehouse cleanup succeeded: $($e.SanitationWarehouseCleanupSucceeded)")
        [void]$sb.AppendLine("- Warehouse cleanup detail: $($e.SanitationWarehouseDetail)")
        [void]$sb.AppendLine("- Warehouse delete attempted: $($e.SanitationWarehouseDeleteAttempted)")
        [void]$sb.AppendLine("- Warehouse delete exit code: $($e.SanitationWarehouseDeleteExitCode)")
        [void]$sb.AppendLine("- Warehouse delete JSON success: $($e.SanitationWarehouseDeleteJsonSuccess)")
        [void]$sb.AppendLine("- Warehouse entry gone after sanitation delete: $($e.SanitationWarehouseExistsGone)")
        [void]$sb.AppendLine("- ELL gone before artifact removal: $($e.SanitationArtifactEllGoneBeforeRemoval)")
        [void]$sb.AppendLine("- XML gone before artifact removal: $($e.SanitationArtifactXmlGoneBeforeRemoval)")
        [void]$sb.AppendLine("- Artifact orphan before artifact removal: $($e.SanitationArtifactOrphanBeforeRemoval)")
        [void]$sb.AppendLine("- ELL removal succeeded: $($e.SanitationEllRemovalSucceeded) ($($e.SanitationEllRemovalDetail))")
        [void]$sb.AppendLine("- XML removal succeeded: $($e.SanitationXmlRemovalSucceeded) ($($e.SanitationXmlRemovalDetail))")
        [void]$sb.AppendLine("- Sanitation delete verdict before artifact removal: $($e.SanitationDeleteVerdictBeforeArtifactRemoval)")
        [void]$sb.AppendLine("- Recovery marker path: $($e.RecoveryMarkerPath)")
        [void]$sb.AppendLine("- Recovery marker sanitation eligible: $($e.RecoveryMarkerSanitized)")
        [void]$sb.AppendLine("- Recovery marker removal succeeded: $($e.RecoveryMarkerRemovalSucceeded)")
        [void]$sb.AppendLine("- Recovery marker removal detail: $($e.RecoveryMarkerRemovalDetail)")
        [void]$sb.AppendLine("")
        [void]$sb.AppendLine("### Post-sanitation hygiene state")
        [void]$sb.AppendLine("- Warehouse query succeeded: $($e.PostSanitationWarehouseQuerySucceeded)")
        [void]$sb.AppendLine("- Warehouse entry present: $($e.PostSanitationWarehouseEntryPresent)")
        [void]$sb.AppendLine("- ELL present: $($e.PostSanitationEllPresent)")
        [void]$sb.AppendLine("- XML present: $($e.PostSanitationXmlPresent)")
        [void]$sb.AppendLine("- ELL read-only: $($e.PostSanitationEllReadOnly)")
        [void]$sb.AppendLine("- ELL SHA-256: $($e.PostSanitationEllHash)")
        [void]$sb.AppendLine("- ELL hash captured: $($e.PostSanitationEllHashCaptured)")
        [void]$sb.AppendLine("- XML read-only: $($e.PostSanitationXmlReadOnly)")
        [void]$sb.AppendLine("- XML SHA-256: $($e.PostSanitationXmlHash)")
        [void]$sb.AppendLine("- XML hash captured: $($e.PostSanitationXmlHashCaptured)")
        [void]$sb.AppendLine("- Hygiene passed: **$($e.PostSanitationHygienePassed)**")
        [void]$sb.AppendLine("")
        [void]$sb.AppendLine("---")
        [void]$sb.AppendLine("")
    }
    $dir = Split-Path -Parent $Path
    $isTrackedPath = [System.String]::Equals($Path, $script:trackedEvidencePathFull, [System.StringComparison]::OrdinalIgnoreCase)
    if (-not $AllowTrackedPath -and $isTrackedPath) {
        throw "Selecting the tracked evidence path requires -UpdateTrackedEvidence: '$Path'"
    }
    Test-L7EvidencePathHasReparsePoint -Path $Path | Out-Null
    if ($dir) {
        $dirState = Get-L7PathState -Path $dir
        if (-not $dirState.QuerySucceeded) {
            throw "Unable to inspect evidence parent '$dir': $($dirState.Error)"
        }
        if (-not $dirState.Exists) {
            New-Item -ItemType Directory -Force -Path $dir | Out-Null
        }
        Test-L7EvidencePathHasReparsePoint -Path $dir | Out-Null
        $dirState = Get-L7PathState -Path $dir
        if (-not $dirState.QuerySucceeded -or -not $dirState.Exists -or -not $dirState.IsDirectory) {
            throw "Evidence parent is not a verified directory: '$dir'"
        }
    }
    Test-L7EvidencePathHasReparsePoint -Path $Path | Out-Null
    $destinationState = Get-L7PathState -Path $Path
    if (-not $destinationState.QuerySucceeded) {
        throw "Unable to inspect evidence destination '$Path': $($destinationState.Error)"
    }
    if ($destinationState.Exists -and -not $destinationState.IsRegularFile) {
        throw "Evidence destination is not a regular file: '$Path'"
    }
    if ($destinationState.Exists) {
        if ($destinationState.IsRegularFile) {
            $hardLinkState = Get-L7HardLinkState -Path $Path
            if (-not $hardLinkState.QuerySucceeded) {
                throw "Unable to inspect evidence destination hard-link state '$Path': $($hardLinkState.Error)"
            }
            if (-not $hardLinkState.IsSingleLink) {
                throw "Refusing hard-linked evidence destination (link_count=$($hardLinkState.LinkCount)): '$Path'"
            }
        }
        # This also rejects an existing hard-link destination; the evidence
        # writer never replaces the file entry or its linked target.
        throw "Refusing to overwrite existing evidence destination: '$Path'"
    }
    # CreateNew is retained for every destination, including the explicitly
    # authorized tracked path. A race that creates/replaces the destination
    # after inspection therefore fails closed instead of overwriting it.
    $fileMode = [System.IO.FileMode]::CreateNew
    $stream = $null
    try {
        $stream = [System.IO.File]::Open($Path, $fileMode, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
        $bytes = (New-Object -TypeName System.Text.UTF8Encoding -ArgumentList $false).GetBytes($sb.ToString())
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    } finally {
        if ($null -ne $stream) { $stream.Dispose() }
    }
    Test-L7EvidencePathHasReparsePoint -Path $Path | Out-Null
    $writtenState = Get-L7PathState -Path $Path
    if (-not $writtenState.QuerySucceeded -or -not $writtenState.Exists -or -not $writtenState.IsRegularFile) {
        throw "L7 durable evidence write could not be verified: '$Path'"
    }
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
        $initialEllState = Get-L7ArtifactState -Path $ellPath -Type $Type -ProgId $progId -Extension 'ell'
        $initialXmlState = Get-L7ArtifactState -Path $xmlPath -Type $Type -ProgId $progId -Extension 'xml'
        if (-not $initialEllState.QuerySucceeded -or -not $initialEllState.PathValidated -or -not $initialXmlState.QuerySucceeded -or -not $initialXmlState.PathValidated) {
            throw "L7 artifact paths failed exact-path validation: ell=$($initialEllState.Error) xml=$($initialXmlState.Error)"
        }
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
        $opJsonValid = $false
        $opJsonSuccess = $null
        $opJsonExitCode = $null
        $opErrorCode = $null
        try {
            $jMod = $cliMod.Stdout | ConvertFrom-Json -ErrorAction Stop
            $successProperty = $jMod.PSObject.Properties['success']
            $jsonExitProperty = $jMod.PSObject.Properties['exit_code']
            $errorProperty = $jMod.PSObject.Properties['error']
            if ($null -ne $successProperty -and $successProperty.Value -is [bool]) {
                $opJsonSuccess = [bool]$successProperty.Value
            }
            if ($null -ne $jsonExitProperty -and ($jsonExitProperty.Value -is [int] -or $jsonExitProperty.Value -is [long])) {
                $opJsonExitCode = [int]$jsonExitProperty.Value
            }
            if ($null -ne $errorProperty -and $null -ne $errorProperty.Value) {
                $errorCodeProperty = $errorProperty.Value.PSObject.Properties['code']
                if ($null -ne $errorCodeProperty) { $opErrorCode = [string]$errorCodeProperty.Value }
            }
            $opJsonValid = ($null -ne $successProperty -and $successProperty.Value -is [bool] -and
                $null -ne $jsonExitProperty -and $null -ne $opJsonExitCode)
        } catch { $opJsonValid = $false }
        $reportedSuccess = ($opExit -eq 0 -and $opJsonSuccess -eq $true)
        Record-Result -StepId "$($idBase)e" -Description "modify executed; captured exit/JSON success/stderr/stdout" `
            -Passed $opJsonValid -Detail "exit=$opExit json_valid=$opJsonValid json_exit=$opJsonExitCode json_success=$opJsonSuccess error_code=$opErrorCode reported_success=$reportedSuccess stderr='$($cliMod.Stderr)' stdout='$($cliMod.Stdout)'" | Out-Null

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
        # APPROVED): GREEN detect-and-fail -- production must return the
        # specific persistence failure contract (exit 9 / success=false /
        # artifact_missing), not merely any unrelated failure, and retain the
        # existing component.
        $operationContractSatisfied = ($opExit -eq 9 -and $opJsonValid -and
            $opJsonExitCode -eq 9 -and $opJsonSuccess -eq $false -and
            $opErrorCode -eq 'artifact_missing')
        $nonSuccess = $operationContractSatisfied
        $ellHashUnchanged = ($preEllHash -ne $null -and $postEllHash -ne $null -and $preEllHash -eq $postEllHash)
        $ellPostState = Get-L7ArtifactState -Path $ellPath -Type $Type -ProgId $progId -Extension 'ell'
        $ellPresentAfterOperation = ($ellPostState.QuerySucceeded -and $ellPostState.PathValidated -and $ellPostState.Exists -and $ellPostState.IsRegularFile)
        $existingComponentRetained = ($ellHashUnchanged -and $ellPresentAfterOperation)

        Record-Result -StepId "$($idBase)h" -Description "DESIRED-STATE: modify returned exit 9, JSON success=false, error.code=artifact_missing" `
            -Passed $operationContractSatisfied -Detail "contract=$operationContractSatisfied exit=$opExit json_valid=$opJsonValid json_exit=$opJsonExitCode json_success=$opJsonSuccess error_code=$opErrorCode" | Out-Null

        Record-Result -StepId "$($idBase)i" -Description "DESIRED-STATE: existing component retained (ELL hash unchanged, ELL present)" `
            -Passed $existingComponentRetained -Detail "pre_ell=$preEllHash post_ell=$postEllHash unchanged=$ellHashUnchanged ell_present=$ellPresentAfterOperation" | Out-Null

        $desiredStatePassed = ($operationContractSatisfied -and $existingComponentRetained)
        $invariantFailures = @()
        if (-not $operationContractSatisfied) {
            $invariantFailures += "operation_contract_not_satisfied(exit=$opExit,json_valid=$opJsonValid,json_exit=$opJsonExitCode,json_success=$opJsonSuccess,error_code=$opErrorCode; expected exit=9,json success=false,error.code=artifact_missing)"
        }
        if (-not $existingComponentRetained) { $invariantFailures += "existing_component_not_retained(ell_hash_changed_or_absent)" }
        Record-Result -StepId "$($idBase)j" -Description "DESIRED-STATE combined: required modify failure contract AND existing component retained (Fault A desired-state)" `
            -Passed $desiredStatePassed -Detail "desired_passed=$desiredStatePassed invariant_failures=$($invariantFailures -join '; ') -- GREEN detect-and-fail after L.9" | Out-Null

        $evidence = @{
            CaseId                = $idBase
            CaseTitle             = "Modify / read-only ELL (Fault A: stale-ELL modify; detect-and-fail; historical/transient recorded run)"
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
        # Invoke-L7Sanitation captures and records the production snapshot before
        # any attribute restore, sanitation delete, or artifact removal. Source
        # fixture files are removed only after that snapshot has been captured.
        if (-not $evidence -and $progId -gt 0) {
            $evidence = New-L7EvidenceRecord -CaseId $idBase -CaseTitle "Modify / read-only ELL (Fault A: stale-ELL modify; detect-and-fail; historical/transient recorded run)" -Name $Name -Type $Type -ProgId $progId -Operation "modify" -ReadOnlyCapture $readOnlyCapture
        }
        $cln = Invoke-L7Sanitation -Name $Name -Type $Type -ProgId $progId -EllPath $ellPath -XmlPath $xmlPath -Evidence $evidence
        if ($insertSrcFile -and (Test-Path -Path $insertSrcFile -PathType Leaf)) { Remove-Item -Path $insertSrcFile -Force -ErrorAction SilentlyContinue }
        if ($modifySrcFile -and (Test-Path -Path $modifySrcFile -PathType Leaf)) { Remove-Item -Path $modifySrcFile -Force -ErrorAction SilentlyContinue }
        # TVS-L.7 hardening: remove this run's recovery marker ONLY after
        # verified sanitation (entry gone, both artifacts gone, read-only
        # restored). If sanitation did not verify, the marker is left in place
        # for the next run's Invoke-L7StaleMarkerRecovery to process.
        $markerSanitized = $cln.PostSanitationHygieneOk
        $markerRemovalOk = $true
        $markerRemovalDetail = ""
        if ($markerPath -and $markerSanitized) {
            $markerRemoval = Remove-L7RecoveryMarker -MarkerPath $markerPath
            $markerRemovalOk = $markerRemoval.Succeeded
            $markerRemovalDetail = $markerRemoval.Detail
        }
        $cleanupPassed = ($cln.PostSanitationHygieneOk -and ($markerRemovalOk -or -not $markerSanitized))
        $cleanupDetail = $cln.Detail
        if ($markerPath -and $markerSanitized -and -not $markerRemovalOk) { $cleanupDetail += " marker_cleanup=$markerRemovalDetail" }
        Record-Result -StepId "$($idBase).CLEANUP" -Description "$idBase cleanup restored read-only attribute, removed warehouse entry and artifacts" `
            -Passed $cleanupPassed -Detail $cleanupDetail | Out-Null
        if ($evidence) {
            # The production-before-sanitation fields were populated inside
            # Invoke-L7Sanitation before the first sanitation action. Only
            # sanitation action and marker results are added here.
            $evidence.RecoveryMarkerPath = $markerPath
            $evidence.RecoveryMarkerSanitized = $markerSanitized
            $evidence.RecoveryMarkerRemovalSucceeded = $markerRemovalOk
            $evidence.RecoveryMarkerRemovalDetail = $markerRemovalDetail
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
        $initialEllState = Get-L7ArtifactState -Path $ellPath -Type $Type -ProgId $progId -Extension 'ell'
        $initialXmlState = Get-L7ArtifactState -Path $xmlPath -Type $Type -ProgId $progId -Extension 'xml'
        if (-not $initialEllState.QuerySucceeded -or -not $initialEllState.PathValidated -or -not $initialXmlState.QuerySucceeded -or -not $initialXmlState.PathValidated) {
            throw "L7 artifact paths failed exact-path validation: ell=$($initialEllState.Error) xml=$($initialXmlState.Error)"
        }
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
        $opJsonValid = $false
        $opJsonSuccess = $null
        $opJsonExitCode = $null
        $opErrorCode = $null
        try {
            $jUps = $cliUps.Stdout | ConvertFrom-Json -ErrorAction Stop
            $successProperty = $jUps.PSObject.Properties['success']
            $jsonExitProperty = $jUps.PSObject.Properties['exit_code']
            $errorProperty = $jUps.PSObject.Properties['error']
            if ($null -ne $successProperty -and $successProperty.Value -is [bool]) {
                $opJsonSuccess = [bool]$successProperty.Value
            }
            if ($null -ne $jsonExitProperty -and ($jsonExitProperty.Value -is [int] -or $jsonExitProperty.Value -is [long])) {
                $opJsonExitCode = [int]$jsonExitProperty.Value
            }
            if ($null -ne $errorProperty -and $null -ne $errorProperty.Value) {
                $errorCodeProperty = $errorProperty.Value.PSObject.Properties['code']
                if ($null -ne $errorCodeProperty) { $opErrorCode = [string]$errorCodeProperty.Value }
            }
            $opJsonValid = ($null -ne $successProperty -and $successProperty.Value -is [bool] -and
                $null -ne $jsonExitProperty -and $null -ne $opJsonExitCode)
        } catch { $opJsonValid = $false }
        $reportedSuccess = ($opExit -eq 0 -and $opJsonSuccess -eq $true)
        Record-Result -StepId "$($idBase)e" -Description "upsert-existing executed; captured exit/JSON success/stderr/stdout" `
            -Passed $opJsonValid -Detail "exit=$opExit json_valid=$opJsonValid json_exit=$opJsonExitCode json_success=$opJsonSuccess error_code=$opErrorCode reported_success=$reportedSuccess stderr='$($cliUps.Stderr)' stdout='$($cliUps.Stdout)'" | Out-Null

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
        # APPROVED): GREEN detect-and-fail -- production must return the
        # specific persistence failure contract (exit 9 / success=false /
        # artifact_missing), not merely any unrelated failure, and retain the
        # existing component.
        $operationContractSatisfied = ($opExit -eq 9 -and $opJsonValid -and
            $opJsonExitCode -eq 9 -and $opJsonSuccess -eq $false -and
            $opErrorCode -eq 'artifact_missing')
        $nonSuccess = $operationContractSatisfied
        $ellHashUnchanged = ($preEllHash -ne $null -and $postEllHash -ne $null -and $preEllHash -eq $postEllHash)
        $ellPostState = Get-L7ArtifactState -Path $ellPath -Type $Type -ProgId $progId -Extension 'ell'
        $ellPresentAfterOperation = ($ellPostState.QuerySucceeded -and $ellPostState.PathValidated -and $ellPostState.Exists -and $ellPostState.IsRegularFile)
        $existingComponentRetained = ($ellHashUnchanged -and $ellPresentAfterOperation)

        Record-Result -StepId "$($idBase)h" -Description "DESIRED-STATE: upsert-existing returned exit 9, JSON success=false, error.code=artifact_missing" `
            -Passed $operationContractSatisfied -Detail "contract=$operationContractSatisfied exit=$opExit json_valid=$opJsonValid json_exit=$opJsonExitCode json_success=$opJsonSuccess error_code=$opErrorCode" | Out-Null

        Record-Result -StepId "$($idBase)i" -Description "DESIRED-STATE: existing component retained (ELL hash unchanged, ELL present)" `
            -Passed $existingComponentRetained -Detail "pre_ell=$preEllHash post_ell=$postEllHash unchanged=$ellHashUnchanged ell_present=$ellPresentAfterOperation" | Out-Null

        $desiredStatePassed = ($operationContractSatisfied -and $existingComponentRetained)
        $invariantFailures = @()
        if (-not $operationContractSatisfied) {
            $invariantFailures += "operation_contract_not_satisfied(exit=$opExit,json_valid=$opJsonValid,json_exit=$opJsonExitCode,json_success=$opJsonSuccess,error_code=$opErrorCode; expected exit=9,json success=false,error.code=artifact_missing)"
        }
        if (-not $existingComponentRetained) { $invariantFailures += "existing_component_not_retained(ell_hash_changed_or_absent)" }
        Record-Result -StepId "$($idBase)j" -Description "DESIRED-STATE combined: required upsert failure contract AND existing component retained (Fault A desired-state)" `
            -Passed $desiredStatePassed -Detail "desired_passed=$desiredStatePassed invariant_failures=$($invariantFailures -join '; ') -- GREEN detect-and-fail after L.9" | Out-Null

        $evidence = @{
            CaseId                = $idBase
            CaseTitle             = "Upsert-existing / read-only ELL (Fault A variant; detect-and-fail; historical/transient recorded run)"
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
        # Invoke-L7Sanitation captures and records the production snapshot before
        # any attribute restore, sanitation delete, or artifact removal. Source
        # fixture files are removed only after that snapshot has been captured.
        if (-not $evidence -and $progId -gt 0) {
            $evidence = New-L7EvidenceRecord -CaseId $idBase -CaseTitle "Upsert-existing / read-only ELL (Fault A variant; detect-and-fail; historical/transient recorded run)" -Name $Name -Type $Type -ProgId $progId -Operation "upsert" -ReadOnlyCapture $readOnlyCapture
        }
        $cln = Invoke-L7Sanitation -Name $Name -Type $Type -ProgId $progId -EllPath $ellPath -XmlPath $xmlPath -Evidence $evidence
        if ($insertSrcFile -and (Test-Path -Path $insertSrcFile -PathType Leaf)) { Remove-Item -Path $insertSrcFile -Force -ErrorAction SilentlyContinue }
        if ($upsertSrcFile -and (Test-Path -Path $upsertSrcFile -PathType Leaf)) { Remove-Item -Path $upsertSrcFile -Force -ErrorAction SilentlyContinue }
        # TVS-L.7 hardening: remove this run's recovery marker ONLY after
        # verified sanitation (see L7.1 finally for rationale).
        $markerSanitized = $cln.PostSanitationHygieneOk
        $markerRemovalOk = $true
        $markerRemovalDetail = ""
        if ($markerPath -and $markerSanitized) {
            $markerRemoval = Remove-L7RecoveryMarker -MarkerPath $markerPath
            $markerRemovalOk = $markerRemoval.Succeeded
            $markerRemovalDetail = $markerRemoval.Detail
        }
        $cleanupPassed = ($cln.PostSanitationHygieneOk -and ($markerRemovalOk -or -not $markerSanitized))
        $cleanupDetail = $cln.Detail
        if ($markerPath -and $markerSanitized -and -not $markerRemovalOk) { $cleanupDetail += " marker_cleanup=$markerRemovalDetail" }
        Record-Result -StepId "$($idBase).CLEANUP" -Description "$idBase cleanup restored read-only attribute, removed warehouse entry and artifacts" `
            -Passed $cleanupPassed -Detail $cleanupDetail | Out-Null
        if ($evidence) {
            # Production-before-sanitation fields were populated by the helper
            # before its first sanitation action; only marker results are added
            # after sanitation here.
            $evidence.RecoveryMarkerPath = $markerPath
            $evidence.RecoveryMarkerSanitized = $markerSanitized
            $evidence.RecoveryMarkerRemovalSucceeded = $markerRemovalOk
            $evidence.RecoveryMarkerRemovalDetail = $markerRemovalDetail
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
        $initialEllState = Get-L7ArtifactState -Path $ellPath -Type $Type -ProgId $progId -Extension 'ell'
        $initialXmlState = Get-L7ArtifactState -Path $xmlPath -Type $Type -ProgId $progId -Extension 'xml'
        if (-not $initialEllState.QuerySucceeded -or -not $initialEllState.PathValidated -or -not $initialXmlState.QuerySucceeded -or -not $initialXmlState.PathValidated) {
            throw "L7 artifact paths failed exact-path validation: ell=$($initialEllState.Error) xml=$($initialXmlState.Error)"
        }
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
        $opJsonValid = $false
        $opJsonSuccess = $null
        $opJsonExitCode = $null
        $opErrorCode = $null
        $opErrorMessage = ""
        $opErrorDetails = ""
        try {
            $jDel = $cliDel.Stdout | ConvertFrom-Json -ErrorAction Stop
            $successProperty = $jDel.PSObject.Properties['success']
            $jsonExitProperty = $jDel.PSObject.Properties['exit_code']
            $errorProperty = $jDel.PSObject.Properties['error']
            if ($null -ne $successProperty -and $successProperty.Value -is [bool]) {
                $opJsonSuccess = [bool]$successProperty.Value
            }
            if ($null -ne $jsonExitProperty -and ($jsonExitProperty.Value -is [int] -or $jsonExitProperty.Value -is [long])) {
                $opJsonExitCode = [int]$jsonExitProperty.Value
            }
            if ($null -ne $errorProperty -and $null -ne $errorProperty.Value) {
                $errorCodeProperty = $errorProperty.Value.PSObject.Properties['code']
                $errorMessageProperty = $errorProperty.Value.PSObject.Properties['message']
                $errorDetailsProperty = $errorProperty.Value.PSObject.Properties['details']
                if ($null -ne $errorCodeProperty) { $opErrorCode = [string]$errorCodeProperty.Value }
                if ($null -ne $errorMessageProperty) { $opErrorMessage = [string]$errorMessageProperty.Value }
                if ($null -ne $errorDetailsProperty) {
                    if ($errorDetailsProperty.Value -is [string]) {
                        $opErrorDetails = [string]$errorDetailsProperty.Value
                    } else {
                        $opErrorDetails = $errorDetailsProperty.Value | ConvertTo-Json -Compress
                    }
                }
            }
            $opJsonValid = ($null -ne $successProperty -and $successProperty.Value -is [bool] -and
                $null -ne $jsonExitProperty -and $null -ne $opJsonExitCode)
        } catch { $opJsonValid = $false }
        $reportedSuccess = ($opExit -eq 0 -and $opJsonSuccess -eq $true)
        # Capture the exact "failed to delete ... error=5" line from stderr.
        $deleteError5Line = ""
        if ($cliDel.Stderr -and ($cliDel.Stderr -match 'error=5')) {
            $deleteError5Line = ($cliDel.Stderr -split "`n" | Where-Object { $_ -match 'error=5' } | Select-Object -First 1)
        }
        Record-Result -StepId "$($idBase)e" -Description "delete executed; captured exit/JSON success/stderr/stdout + error=5 line" `
            -Passed $opJsonValid -Detail "exit=$opExit json_valid=$opJsonValid json_exit=$opJsonExitCode json_success=$opJsonSuccess error_code=$opErrorCode reported_success=$reportedSuccess stderr='$($cliDel.Stderr)' stdout='$($cliDel.Stdout)' error5_line='$deleteError5Line'" | Out-Null

        # Post-delete warehouse / XML / ELL presence (Fault B evidence).
        $postWarehouseState = Get-L7ExistsState -Type $cliType -Name $Name
        $warehouseEntryPresent = if ($postWarehouseState.QuerySucceeded) { $postWarehouseState.Exists } else { $null }
        $postXmlState = Get-L7ArtifactState -Path $xmlPath -Type $Type -ProgId $progId -Extension 'xml'
        $postEllState = Get-L7ArtifactState -Path $ellPath -Type $Type -ProgId $progId -Extension 'ell'
        $xmlPresentAfter = if ($postXmlState.QuerySucceeded) { $postXmlState.Exists } else { $null }
        $ellPresentAfter = if ($postEllState.QuerySucceeded) { $postEllState.Exists } else { $null }
        $postEllHash = Get-FileHashOrNull -Path $ellPath
        Record-Result -StepId "$($idBase)f" -Description "post-delete warehouse/XML/ELL presence + orphan ELL hash captured (Fault B evidence)" `
            -Passed $true -Detail "warehouse_present=$warehouseEntryPresent xml_present=$xmlPresentAfter ell_present=$ellPresentAfter orphan=$ellPresentAfter post_ell_hash=$postEllHash" | Out-Null

        # DESIRED-STATE ASSERTIONS (handoff Sec.6 case 3). Post-L.12 (L.12 GREEN):
        # detect-and-report. The production operation must return the specific
        # delete failure contract (exit 6 / success=false /
        # entry_op_failure), report the exact residual path + Windows error, and
        # leave the expected pre-sanitation state. This is NOT that the
        # read-only ELL was physically removed. DeleteFileA cannot remove a +R
        # file and L.12 deliberately does NOT invent attrib -R recovery, so the
        # orphan ELL is EXPECTED to still be present at verdict time; the L7.4
        # sanitation in finally clears +R and deletes the orphan AFTER the
        # verdict is recorded. Full physical orphan removal is therefore NOT
        # the L.12 success criterion.
        $operationContractSatisfied = ($opExit -eq 6 -and $opJsonValid -and
            $opJsonExitCode -eq 6 -and $opJsonSuccess -eq $false -and
            $opErrorCode -eq 'entry_op_failure')
        $nonSuccess = $operationContractSatisfied
        # residualReported: the L.12 detect-and-report signal. deleteEntry step
        # 5 emits "tsdev: WARNING -- failed to delete <path> (error=5)" to
        # stderr AND doDelete surfaces the residual path + Windows error in the
        # JSON error.message (exit 6 / success=false). Require the exact ELL
        # path in both reports so an unrelated error=5 cannot satisfy the case.
        $jsonResidualPathReported = ($opJsonValid -and $ellPath -and
            (($opErrorMessage -and $opErrorMessage.Contains($ellPath)) -or
             ($opErrorDetails -and $opErrorDetails.Contains($ellPath))))
        $stderrResidualPathReported = ($deleteError5Line -ne "" -and $ellPath -and
            $deleteError5Line -match 'error=5' -and $deleteError5Line.Contains($ellPath))
        $residualReported = ($jsonResidualPathReported -and $stderrResidualPathReported)
        # The orphan ELL being present is EXPECTED post-L.12 (sanitation is in
        # finally AFTER verdict). It is captured as evidence (L7.3f) and is
        # required together with the absent warehouse/XML state below; asserting
        # its absence would claim full physical orphan removal is L.12 success.
        $orphanExpectedPresent = $ellPresentAfter
        $warehouseStateKnown = [bool]$postWarehouseState.QuerySucceeded
        $xmlStateKnown = ($postXmlState.QuerySucceeded -and $postXmlState.PathValidated -and
            (-not $postXmlState.Exists -or $postXmlState.IsRegularFile))
        $ellStateKnown = ($postEllState.QuerySucceeded -and $postEllState.PathValidated -and
            (-not $postEllState.Exists -or $postEllState.IsRegularFile))
        $expectedPreSanitationState = ($warehouseStateKnown -and
            (-not [bool]$postWarehouseState.Exists) -and $xmlStateKnown -and
            (-not [bool]$postXmlState.Exists) -and $ellStateKnown -and
            [bool]$postEllState.Exists -and [bool]$postEllState.IsRegularFile)

        Record-Result -StepId "$($idBase)g" -Description "DESIRED-STATE: delete returned exit 6, JSON success=false, error.code=entry_op_failure" `
            -Passed $operationContractSatisfied -Detail "contract=$operationContractSatisfied exit=$opExit json_valid=$opJsonValid json_exit=$opJsonExitCode json_success=$opJsonSuccess error_code=$opErrorCode" | Out-Null

        Record-Result -StepId "$($idBase)h" -Description "DESIRED-STATE: residual artifact + Windows error REPORTED (error=5 / exact path captured in stderr and JSON error/details)" `
            -Passed $residualReported -Detail "json_path_reported=$jsonResidualPathReported stderr_path_reported=$stderrResidualPathReported ell_path='$ellPath' error_message='$opErrorMessage' error_details='$opErrorDetails' error5_line='$deleteError5Line' orphan_ell_present=$ellPresentAfter (expected present: sanitation is in finally AFTER verdict) -- GREEN detect-and-report after L.12" | Out-Null

        Record-Result -StepId "$($idBase)h.state" -Description "DESIRED-STATE: pre-sanitation warehouse entry absent, XML absent, ELL orphan present" `
            -Passed $expectedPreSanitationState -Detail "warehouse_query=$warehouseStateKnown warehouse_present=$warehouseEntryPresent xml_known=$xmlStateKnown xml_present=$xmlPresentAfter ell_known=$ellStateKnown ell_present=$ellPresentAfter -- production state before sanitation" | Out-Null

        $desiredStatePassed = ($operationContractSatisfied -and $residualReported -and $expectedPreSanitationState)
        $invariantFailures = @()
        if (-not $operationContractSatisfied) {
            $invariantFailures += "operation_contract_not_satisfied(exit=$opExit,json_valid=$opJsonValid,json_exit=$opJsonExitCode,json_success=$opJsonSuccess,error_code=$opErrorCode; expected exit=6,json success=false,error.code=entry_op_failure)"
        }
        if (-not $residualReported) {
            $invariantFailures += "residual_not_reported(json_path=$jsonResidualPathReported,stderr_path=$stderrResidualPathReported,error5_line_empty=$($deleteError5Line -eq ''))"
        }
        if (-not $expectedPreSanitationState) {
            $invariantFailures += "unexpected_or_unknown_pre_sanitation_state(warehouse_query=$warehouseStateKnown,warehouse_present=$warehouseEntryPresent,xml_known=$xmlStateKnown,xml_present=$xmlPresentAfter,ell_known=$ellStateKnown,ell_present=$ellPresentAfter; expected warehouse=false,xml=false,ell=true)"
        }
        Record-Result -StepId "$($idBase)i" -Description "DESIRED-STATE combined: required delete failure contract, exact residual/error report, and expected pre-sanitation state (Fault B desired-state; detect-and-report, NOT physical orphan removal)" `
            -Passed $desiredStatePassed -Detail "desired_passed=$desiredStatePassed invariant_failures=$($invariantFailures -join '; ') orphan_ell_present=$orphanExpectedPresent (expected: sanitation in finally AFTER verdict) -- GREEN detect-and-report after L.12" | Out-Null

        $evidence = @{
            CaseId                = $idBase
            CaseTitle             = "Delete / read-only ELL (Fault B: orphan ELL false success; detect-and-report; historical/transient recorded run)"
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
        # Invoke-L7Sanitation captures and records the production snapshot before
        # any attribute restore, sanitation delete, or artifact removal. Source
        # fixture files are removed only after that snapshot has been captured.
        if (-not $evidence -and $progId -gt 0) {
            $evidence = New-L7EvidenceRecord -CaseId $idBase -CaseTitle "Delete / read-only ELL (Fault B: orphan ELL false success; detect-and-report; historical/transient recorded run)" -Name $Name -Type $Type -ProgId $progId -Operation "delete" -ReadOnlyCapture $readOnlyCapture
        }
        $cln = Invoke-L7Sanitation -Name $Name -Type $Type -ProgId $progId -EllPath $ellPath -XmlPath $xmlPath -Evidence $evidence
        if ($insertSrcFile -and (Test-Path -Path $insertSrcFile -PathType Leaf)) { Remove-Item -Path $insertSrcFile -Force -ErrorAction SilentlyContinue }
        # L7.3.CLEANUP predicate (L.12 detect-and-report alignment): the
        # production Fault B orphan is expected in the pre-sanitation snapshot
        # and at the production verdict, so cleanup passes only on the separate
        # post-sanitation hygiene state. The production desired-state result is
        # recorded before this sanitation and is not rewritten by this cleanup.
        $l73CleanupPassed = $cln.PostSanitationHygieneOk
        # TVS-L.7 hardening: remove this run's recovery marker ONLY after
        # verified sanitation (see L7.1 finally for rationale).
        $markerSanitized = $cln.PostSanitationHygieneOk
        $markerRemovalOk = $true
        $markerRemovalDetail = ""
        if ($markerPath -and $markerSanitized) {
            $markerRemoval = Remove-L7RecoveryMarker -MarkerPath $markerPath
            $markerRemovalOk = $markerRemoval.Succeeded
            $markerRemovalDetail = $markerRemoval.Detail
        }
        if ($markerPath -and $markerSanitized -and -not $markerRemovalOk) { $l73CleanupPassed = $false }
        $cleanupDetail = $cln.Detail
        if ($markerPath -and $markerSanitized -and -not $markerRemovalOk) { $cleanupDetail += " marker_cleanup=$markerRemovalDetail" }
        Record-Result -StepId "$($idBase).CLEANUP" -Description "$idBase cleanup restored read-only attribute, removed warehouse entry and post-sanitation artifacts; L7.3.g/h/i production verdict (detect-and-report) was recorded before sanitation and is not rewritten by hygiene cleanup" `
            -Passed $l73CleanupPassed -Detail $cleanupDetail | Out-Null
        if ($evidence) {
            # Production-before-sanitation fields were populated by the helper
            # before its first sanitation action; only marker results are added
            # after sanitation here.
            $evidence.RecoveryMarkerPath = $markerPath
            $evidence.RecoveryMarkerSanitized = $markerSanitized
            $evidence.RecoveryMarkerRemovalSucceeded = $markerRemovalOk
            $evidence.RecoveryMarkerRemovalDetail = $markerRemovalDetail
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
    $records = @()
    foreach ($item in $script:L7TrackedItems) {
        $itemProgId = [int]$item.ProgId
        $record = @{
            CaseId = "L7.4.$($item.Name)"
            ItemIdentifier = $item.Name
            ComponentType = $item.Type
            ProgId = $itemProgId
            PreSanitationSnapshotComplete = $false
            PreSanitationWarehouseQuerySucceeded = $false
            PreSanitationWarehouseEntryExists = $null
            PreSanitationEllExists = $null
            PreSanitationEllReadOnly = $null
            PreSanitationEllHash = $null
            PreSanitationEllHashCaptured = $false
            PreSanitationXmlExists = $null
            PreSanitationXmlReadOnly = $null
            PreSanitationXmlHash = $null
            PreSanitationXmlHashCaptured = $false
            SanitationReadOnlyRestored = $false
            SanitationWarehouseCleanupSucceeded = $false
            SanitationArtifactRemovalAttempted = $false
            SanitationArtifactEllGoneBeforeRemoval = $null
            SanitationArtifactXmlGoneBeforeRemoval = $null
            PostSanitationSnapshotComplete = $false
            PostSanitationWarehouseQuerySucceeded = $false
            PostSanitationWarehouseEntryExists = $null
            PostSanitationEllExists = $null
            PostSanitationEllReadOnly = $null
            PostSanitationEllHash = $null
            PostSanitationEllHashCaptured = $false
            PostSanitationXmlExists = $null
            PostSanitationXmlReadOnly = $null
            PostSanitationXmlHash = $null
            PostSanitationXmlHashCaptured = $false
            PostSanitationHygienePassed = $false
            Detail = ""
        }

        # This guarded snapshot is deliberately the first operation for the
        # item. It must precede read-only restoration, the sanitation CLI delete,
        # and exact artifact removal. An incomplete snapshot withholds all
        # artifact mutation and is not treated as an absent/clean state.
        try {
            $preSnapshot = Get-L7SanitationSnapshot -Name $item.Name -Type $item.Type -ProgId $itemProgId -EllPath $item.EllPath -XmlPath $item.XmlPath
        } catch {
            $allClean = $false
            $record.Detail = "pre-sanitation snapshot failed: $($_.Exception.Message)"
            $details += "$($item.Name): $($record.Detail)"
            $records += $record
            continue
        }
        $record.PreSanitationSnapshotComplete = $preSnapshot.StateComplete
        $record.PreSanitationWarehouseQuerySucceeded = $preSnapshot.WarehouseStateKnown
        $record.PreSanitationWarehouseEntryExists = $preSnapshot.WarehouseEntryExists
        $record.PreSanitationEllExists = $preSnapshot.EllExists
        $record.PreSanitationEllReadOnly = $preSnapshot.EllReadOnly
        $record.PreSanitationEllHash = $preSnapshot.EllHash
        $record.PreSanitationXmlExists = $preSnapshot.XmlExists
        $record.PreSanitationXmlReadOnly = $preSnapshot.XmlReadOnly
        $record.PreSanitationXmlHash = $preSnapshot.XmlHash

        $ellState = $preSnapshot.EllState
        $xmlState = $preSnapshot.XmlState
        $prePathsKnown = ($preSnapshot.EllStateKnown -and $preSnapshot.XmlStateKnown)
        $itemClean = [bool]$preSnapshot.StateComplete
        if (-not $preSnapshot.StateComplete) {
            $details += "incomplete pre-sanitation snapshot for $($item.Name); mutation remains fail-closed"
        }

        # Sanitation action: clear read-only only after the production snapshot
        # is complete and the exact ELL state is known. This preserves the
        # recovery behavior without converting an unknown state into success.
        $readOnlyRestored = $false
        $roRestore = $null
        if ($preSnapshot.StateComplete -and $preSnapshot.EllStateKnown) {
            if (-not $ellState.Exists) {
                $readOnlyRestored = $true
            } elseif ($ellState.IsReadOnly) {
                try {
                    $roRestore = Clear-ReadOnlyAttributeExact -Path $item.EllPath
                    $readOnlyRestored = ($roRestore.ExitCode -eq 0 -and $roRestore.IsReadOnlyAfter -eq $false)
                } catch {
                    $details += "read-only restoration failed for $($item.Name): $($_.Exception.Message)"
                }
            } else {
                $readOnlyRestored = $true
            }
        }
        $record.SanitationReadOnlyRestored = $readOnlyRestored
        if (-not $readOnlyRestored) {
            $itemClean = $false
            $details += "read-only state unverified for $($item.Name)"
        }

        # Sanitation action: warehouse cleanup follows the complete
        # pre-sanitation snapshot and attribute action.
        $warehouse = $null
        try {
            $warehouse = Invoke-L7WarehouseCleanup -Type $item.Type -Name $item.Name
        } catch {
            $warehouse = @{ CleanupSucceeded = $false; ExistsGone = $false; Detail = "warehouse cleanup exception=$($_.Exception.Message)"; DeleteAttempted = $false; DeleteExitCode = -1 }
        }
        $record.SanitationWarehouseCleanupSucceeded = [bool]$warehouse.CleanupSucceeded
        $stillExists = -not [bool]$warehouse.ExistsGone
        if (-not $warehouse.CleanupSucceeded) {
            $itemClean = $false
            $details += "warehouse cleanup unverified for $($item.Name): $($warehouse.Detail)"
        }

        # Separate sanitation-stage observation, after the warehouse action but
        # before exact artifact removal. It is never a pre-sanitation value.
        $artifactBeforeRemovalEllState = Get-L7ArtifactState -Path $item.EllPath -Type $item.Type -ProgId $itemProgId -Extension 'ell'
        $artifactBeforeRemovalXmlState = Get-L7ArtifactState -Path $item.XmlPath -Type $item.Type -ProgId $itemProgId -Extension 'xml'
        $artifactBeforeRemovalEllKnown = ($artifactBeforeRemovalEllState.QuerySucceeded -and $artifactBeforeRemovalEllState.PathValidated -and (-not $artifactBeforeRemovalEllState.Exists -or $artifactBeforeRemovalEllState.IsRegularFile))
        $artifactBeforeRemovalXmlKnown = ($artifactBeforeRemovalXmlState.QuerySucceeded -and $artifactBeforeRemovalXmlState.PathValidated -and (-not $artifactBeforeRemovalXmlState.Exists -or $artifactBeforeRemovalXmlState.IsRegularFile))
        $record.SanitationArtifactEllGoneBeforeRemoval = ($artifactBeforeRemovalEllKnown -and -not $artifactBeforeRemovalEllState.Exists)
        $record.SanitationArtifactXmlGoneBeforeRemoval = ($artifactBeforeRemovalXmlKnown -and -not $artifactBeforeRemovalXmlState.Exists)

        $ellStill = (-not $artifactBeforeRemovalEllKnown -or [bool]$artifactBeforeRemovalEllState.Exists)
        $xmlStill = (-not $artifactBeforeRemovalXmlKnown -or [bool]$artifactBeforeRemovalXmlState.Exists)
        $ellRemoval = $null
        $xmlRemoval = $null
        $artifactMutationAllowed = ($preSnapshot.StateComplete -and $warehouse.CleanupSucceeded -and $readOnlyRestored -and $prePathsKnown -and $artifactBeforeRemovalEllKnown -and $artifactBeforeRemovalXmlKnown)
        if ($artifactMutationAllowed) {
            $record.SanitationArtifactRemovalAttempted = $true
            $ellRemoval = Remove-L7ExactFile -Path $item.EllPath -Type $item.Type -ProgId $itemProgId -Extension 'ell'
            $xmlRemoval = Remove-L7ExactFile -Path $item.XmlPath -Type $item.Type -ProgId $itemProgId -Extension 'xml'
            $ellStill = -not ($ellRemoval.Succeeded -and $ellRemoval.ExistsAfter -eq $false)
            $xmlStill = -not ($xmlRemoval.Succeeded -and $xmlRemoval.ExistsAfter -eq $false)
            if ($ellStill) { $details += "ELL cleanup unverified for $($item.Name): $($ellRemoval.Detail)" }
            if ($xmlStill) { $details += "XML cleanup unverified for $($item.Name): $($xmlRemoval.Detail)" }
        } else {
            $details += "artifact cleanup withheld for $($item.Name) until complete warehouse/attribute/path state is verified"
        }
        if ($stillExists -or $ellStill -or $xmlStill) {
            $itemClean = $false
            $details += "RESIDUAL $($item.Name): entry=$stillExists ell=$ellStill xml=$xmlStill"
        }

        # Post-sanitation hygiene state is a separate guarded observation. It
        # cannot rewrite the pre-sanitation snapshot or the action outcome.
        try {
            $postSnapshot = Get-L7SanitationSnapshot -Name $item.Name -Type $item.Type -ProgId $itemProgId -EllPath $item.EllPath -XmlPath $item.XmlPath
            $record.PostSanitationSnapshotComplete = $postSnapshot.StateComplete
            $record.PostSanitationWarehouseQuerySucceeded = $postSnapshot.WarehouseStateKnown
            $record.PostSanitationWarehouseEntryExists = $postSnapshot.WarehouseEntryExists
            $record.PostSanitationEllExists = $postSnapshot.EllExists
            $record.PostSanitationEllReadOnly = $postSnapshot.EllReadOnly
            $record.PostSanitationEllHash = $postSnapshot.EllHash
            $record.PostSanitationEllHashCaptured = $postSnapshot.EllHashCaptured
            $record.PostSanitationXmlExists = $postSnapshot.XmlExists
            $record.PostSanitationXmlReadOnly = $postSnapshot.XmlReadOnly
            $record.PostSanitationXmlHash = $postSnapshot.XmlHash
            $record.PostSanitationXmlHashCaptured = $postSnapshot.XmlHashCaptured
            $record.PostSanitationHygienePassed = ($postSnapshot.StateComplete -and $warehouse.CleanupSucceeded -and $readOnlyRestored -and -not $postSnapshot.WarehouseEntryExists -and -not $postSnapshot.EllExists -and -not $postSnapshot.XmlExists)
        } catch {
            $record.PostSanitationHygienePassed = $false
            $details += "post-sanitation snapshot failed for $($item.Name): $($_.Exception.Message)"
        }
        if (-not $record.PostSanitationHygienePassed) { $itemClean = $false }
        $record.Detail = "pre_complete=$($record.PreSanitationSnapshotComplete) pre_warehouse=$($record.PreSanitationWarehouseEntryExists) pre_ell=$($record.PreSanitationEllExists) pre_xml=$($record.PreSanitationXmlExists) ro_restored=$readOnlyRestored warehouse_cleanup=$($warehouse.CleanupSucceeded) artifact_removal_attempted=$($record.SanitationArtifactRemovalAttempted) post_warehouse=$($record.PostSanitationWarehouseEntryExists) post_ell=$($record.PostSanitationEllExists) post_xml=$($record.PostSanitationXmlExists) post_hygiene=$($record.PostSanitationHygienePassed)"
        if (-not $itemClean) { $allClean = $false }
        $records += $record
    }
    return @{ AllClean = $allClean; Details = ($details -join '; '); Records = $records }
}

# ---------------------------------------------------------------------------
# L7 OPT-IN EXECUTION (handoff Sec.6). Gated by -IncludeRedFaultInjection; the
# registered default regression suite never sets this switch.
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
    # type-specific <prefix><ProgId>.ell/.xml / under-MyWork\Bin contracts.
    Write-Host "--- L7.0: stale recovery-marker sweep (prior crashed runs) ---" -ForegroundColor Cyan
    $staleRecovery = Invoke-L7StaleMarkerRecovery
    $staleRecovered = $staleRecovery.Recovered
    $staleSkipped = $staleRecovery.Skipped
    $staleRecords = @($staleRecovery.Records)
    $staleOk = ($staleSkipped.Count -eq 0)
    $staleDetail = "recovered=$($staleRecovered.Count) skipped=$($staleSkipped.Count) lifecycle_records=$($staleRecords.Count)"
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
        $finalLifecycleRecords = @($finalSan.Records)

        # TVS-L.7 hardening: any markers left by cases whose per-case finally
        # could not verify sanitation are reported (NOT silently deleted) so
        # the next run's L7.0 sweep can attempt recovery again. This is a
        # visibility-only check; it does not delete markers itself.
        $leftoverMarkers = @()
        $markerEnumerationOk = $true
        $recoveryState = Get-L7PathState -Path $script:L7RecoveryDir
        if (-not $recoveryState.QuerySucceeded) {
            $markerEnumerationOk = $false
            $markerEnumerationDetail = "recovery directory inspection failed: $($recoveryState.Error)"
        } elseif ($recoveryState.Exists -and -not $recoveryState.IsDirectory) {
            $markerEnumerationOk = $false
            $markerEnumerationDetail = "recovery path is not a directory: $script:L7RecoveryDir"
        } elseif ($recoveryState.Exists) {
            try {
                Test-L7EvidencePathHasReparsePoint -Path $script:L7RecoveryDir | Out-Null
                $markerEntries = @(Get-ChildItem -Path $script:L7RecoveryDir -Filter '*.json' -File -ErrorAction Stop)
                foreach ($markerEntry in $markerEntries) {
                    $markerEntryState = Get-L7PathState -Path $markerEntry.FullName
                    if (-not $markerEntryState.QuerySucceeded -or -not $markerEntryState.Exists -or -not $markerEntryState.IsRegularFile) {
                        throw "marker inspection failed for '$($markerEntry.FullName)'"
                    }
                    $leftoverMarkers += $markerEntry.Name
                }
                $markerEnumerationDetail = "recovery marker enumeration verified"
            } catch {
                $markerEnumerationOk = $false
                $markerEnumerationDetail = "recovery marker enumeration failed: $($_.Exception.Message)"
            }
        } else {
            $markerEnumerationDetail = "recovery directory absent; no markers to enumerate"
        }
        if ($leftoverMarkers.Count -gt 0) {
            Write-Host "  NOTE: $($leftoverMarkers.Count) recovery marker(s) left for next-run sweep: $($leftoverMarkers -join ', ')" -ForegroundColor Yellow
        }
        if (-not $markerEnumerationOk) {
            $finalSan.AllClean = $false
            $finalSan.Details = ($finalSan.Details + "; " + $markerEnumerationDetail).Trim('; ')
        }
        $finalSan.Details = ($finalSan.Details + "; lifecycle_records=$($finalLifecycleRecords.Count)").Trim('; ')
        Record-Result -StepId "L7.4" -Description "all L7 disposable items, attributes, and artifacts sanitized even when assertions fail" `
            -Passed $finalSan.AllClean -Detail $finalSan.Details | Out-Null

        # The durable evidence write occurs after this sanitation finally
        # block, below transient-directory cleanup. A caller-supplied tmp
        # destination remains selected and is not removed as scratch data.
    }
}

# ---------------------------------------------------------------------------
# Final temp-directory cleanup (after all L1 + L4 + L5 + L7 sections). The L1
# finally block intentionally leaves $tmpDir in place so the L4 / L5 / L7
# sections can reuse it.
# ---------------------------------------------------------------------------
# An evidence destination is normally outside $tmpDir. If a caller explicitly
# selected a path inside that scratch directory and it already exists, leave
# the scratch directory intact so the no-overwrite rule is not bypassed by
# deleting the destination before the exclusive evidence write.
$preserveScratchForEvidence = $false
if ($IncludeRedFaultInjection) {
    try {
        $tmpDirFull = [System.IO.Path]::GetFullPath($tmpDir).TrimEnd([char[]]@('\', '/'))
        $tmpDirPrefix = $tmpDirFull + [System.IO.Path]::DirectorySeparatorChar
        $evidenceInsideScratch = ($RedFaultInjectionDurablePath.StartsWith($tmpDirPrefix, [System.StringComparison]::OrdinalIgnoreCase) -or [System.String]::Equals($RedFaultInjectionDurablePath, $tmpDirFull, [System.StringComparison]::OrdinalIgnoreCase))
        if ($evidenceInsideScratch) {
            $evidenceStateBeforeScratchCleanup = Get-L7PathState -Path $RedFaultInjectionDurablePath
            $preserveScratchForEvidence = (-not $evidenceStateBeforeScratchCleanup.QuerySucceeded -or $evidenceStateBeforeScratchCleanup.Exists)
            # Also preserve scratch when an existing path component is a file;
            # otherwise recursive scratch cleanup could delete that component
            # and turn an invalid destination into a different directory tree.
            $parentToCheck = Split-Path -Parent $RedFaultInjectionDurablePath
            while ($parentToCheck -and -not $preserveScratchForEvidence) {
                $parentState = Get-L7PathState -Path $parentToCheck
                if (-not $parentState.QuerySucceeded) {
                    $preserveScratchForEvidence = $true
                    break
                }
                if ($parentState.Exists) {
                    if (-not $parentState.IsDirectory) { $preserveScratchForEvidence = $true }
                    break
                }
                $nextParent = Split-Path -Parent $parentToCheck
                if ([string]::Equals($nextParent, $parentToCheck, [System.StringComparison]::OrdinalIgnoreCase)) { break }
                $parentToCheck = $nextParent
            }
        }
    } catch {
        # An unsafe/unknown destination must not cause scratch cleanup to
        # delete a path that the final fail-closed evidence write will inspect.
        $preserveScratchForEvidence = $true
    }
}
if ((Test-Path -Path $tmpDir -PathType Container) -and -not $preserveScratchForEvidence) {
    Remove-Item -Path $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
}

if ($IncludeRedFaultInjection) {
    # WRITE-GUARD: the destination was selected and path-validated before any
    # L7 work. Write only now, after transient scratch cleanup, so a caller-
    # supplied tmp destination is not itself removed by that cleanup. The
    # writer uses CreateNew for both tmp and explicitly authorized tracked paths
    # and therefore fails closed if any destination already exists.
    try {
        Write-L7DurableEvidence -Path $RedFaultInjectionDurablePath -Evidence $script:L7Evidence -AllowTrackedPath:$UpdateTrackedEvidence
        Record-Result -StepId "L7.EVIDENCE" -Description "durable RED evidence summary written" `
            -Passed $true -Detail "path=$RedFaultInjectionDurablePath cases=$($script:L7Evidence.Count)" | Out-Null
    } catch {
        Record-Result -StepId "L7.EVIDENCE" -Description "durable RED evidence summary write FAILED" `
            -Passed $false -Detail "path=$RedFaultInjectionDurablePath error=$($_.Exception.Message)" | Out-Null
    }
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
