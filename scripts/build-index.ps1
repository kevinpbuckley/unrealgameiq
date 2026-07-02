# Build the Game IQ index for a project — fully in Unreal, no Node.
#
# Runs the GameIQBuild commandlet, which in one editor boot runs every extractor
# (registry, assets, blueprints, config, code) and ingests them into
# <Project>/.gameiq/index.db. Incremental updates thereafter are automatic via the
# in-editor save hook.
#
#   ./scripts/build-index.ps1 -Project E:\Repo\ThirdPerson58
#
# The editor must be closed (a commandlet needs exclusive project access).

param(
    [Parameter(Mandatory = $true)][string]$Project,
    [string]$Engine = "E:\Program Files\Epic Games\UE_5.8"
)

$ErrorActionPreference = "Stop"

$uproject = if ($Project.EndsWith(".uproject")) { $Project } else {
    $name = Split-Path $Project -Leaf
    Join-Path $Project "$name.uproject"
}
if (-not (Test-Path $uproject)) { throw "uproject not found: $uproject" }

$cmd = Join-Path $Engine "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
if (-not (Test-Path $cmd)) { throw "UnrealEditor-Cmd not found: $cmd" }

Write-Host "=== Game IQ build ===" -ForegroundColor Cyan
Write-Host "Project: $uproject"

$running = Get-Process UnrealEditor -ErrorAction SilentlyContinue
if ($running) { throw "Close the Unreal Editor first (a commandlet needs exclusive project access)." }

& $cmd $uproject -run=GameIQBuild -unattended -nopause -nosplash -stdout |
    Select-String -Pattern "LogGameIQ" | ForEach-Object { $_.Line }

Write-Host "=== Done ===" -ForegroundColor Cyan
