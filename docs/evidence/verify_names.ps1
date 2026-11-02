# verify_names.ps1 -- cross-check every enumerated name against tsdev_cli exists.
# Input: enumeration transcript (wh_enum probe output, UTF-16).
# Output: per-row results + summary (ASCII).
param(
    [string]$EnumFile = "C:\Users\freek\projects\tsdev_automation\tmp\enum_run_base.txt",
    [string]$OutFile  = "C:\Users\freek\projects\tsdev_automation\tmp\verify_names_results.txt"
)
$ErrorActionPreference = 'Stop'
$cli = 'C:\Users\freek\projects\tsdev_automation\dist\tsdev_cli.exe'
$tvToType = @{ 0 = 'strategy'; 1 = 'function'; 2 = 'indicator'; 1282 = 'showme'; 5122 = 'tradingapp' }

$rows = @()
$cur = $null
foreach ($line in [System.IO.File]::ReadAllLines($EnumFile, [System.Text.Encoding]::Unicode)) {
    if ($line -match '^=== typeVal=(-?\d+)') { $cur = [int]$Matches[1]; continue }
    if ($null -ne $cur -and $line -match "^ROW`t(\d+)`t(.*)$") {
        $rows += ,@($cur, [int]$Matches[1], $Matches[2])
    }
}

$out = New-Object System.Text.StringBuilder
[void]$out.AppendLine("verify_names: rows=$($rows.Count) from $EnumFile")
$ok = 0; $fail = 0
foreach ($r in $rows) {
    $tv = $r[0]; $id = $r[1]; $name = $r[2]
    $type = $tvToType[$tv]
    $raw = & $cli exists $type $name --json 2>$null | Select-Object -Last 1
    $status = "FAIL"
    $detail = ""
    try {
        $j = $raw | ConvertFrom-Json
        if ($j.exists -eq $true -and $j.stored_name -ceq $name -and $j.prog_id -eq $id) {
            $status = "OK"; $ok++
        } else {
            $fail++
            $detail = "exists=$($j.exists) stored=<$($j.stored_name)> prog_id=$($j.prog_id) expected_id=$id"
        }
    } catch {
        $fail++
        $detail = "parse-error raw=<$raw>"
    }
    if ($status -ne "OK") {
        [void]$out.AppendLine("MISMATCH type=$type id=$id name=<$name> $detail")
    }
}
[void]$out.AppendLine("SUMMARY total=$($rows.Count) ok=$ok fail=$fail")
[System.IO.File]::WriteAllText($OutFile, $out.ToString(), [System.Text.Encoding]::ASCII)
Write-Output "SUMMARY total=$($rows.Count) ok=$ok fail=$fail -> $OutFile"