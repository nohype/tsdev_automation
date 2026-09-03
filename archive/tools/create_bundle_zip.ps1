# tools/create_bundle_zip.ps1
#
# Auto-creates a timestamped zip of the dist/ bundle for distribution.
# Invoked by the CMake POST_BUILD custom command on the tsdev_cli target
# (Workstream G, TVS-G.5). The zip is written to artifacts/ at the repo root
# with a build-time timestamp (yyMMddHHmm) so each build produces a fresh,
# uniquely-named archive. -Force overwrites any same-minute zip so the latest
# build wins.
#
# Usage (normally invoked by CMake, not manually):
#   powershell -NoProfile -ExecutionPolicy Bypass -File create_bundle_zip.ps1 -SourceDir <repo-root>
param([Parameter(Mandatory)][string]$SourceDir)
$ErrorActionPreference = 'Stop'
$ts = Get-Date -Format 'yyMMddHHmm'
$zipPath = Join-Path $SourceDir "artifacts\tsdev_automation_$ts.zip"
$artifactsDir = Join-Path $SourceDir "artifacts"
if (-not (Test-Path $artifactsDir)) { New-Item -ItemType Directory -Path $artifactsDir | Out-Null }
Compress-Archive -Path "$SourceDir\dist\tsdev_cli.exe","$SourceDir\dist\tsdev_automation","$SourceDir\dist\README.md" -DestinationPath $zipPath -Force
