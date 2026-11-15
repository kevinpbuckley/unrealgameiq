# Build the Game IQ index for a project — fully in Unreal, no Node.
#
# Runs the GameIQBuild commandlet, which in one editor boot runs every extractor
# (registry, assets, blueprints, config, code, docs) and ingests them into
# <Project>/.gameiq/index.db. Incremental updates thereafter are automatic via the
# in-editor save hook.
#
#   ./scripts/build-index.ps1 -Project E:\Repo\MyGame            # folder: its *.uproject is found
#   ./scripts/build-index.ps1 -Project E:\Repo\MyGame\MyGame.uproject -Engine "D:\UE_5.9"
#
# The engine comes from the .uproject's EngineAssociation unless -Engine is given. An editor with
# THIS project open must be closed first (a commandlet needs exclusive project access); editors on
# other projects are fine.

param(
    [Parameter(Mandatory = $true)][string]$Project,
    [string]$Engine,
    [switch]$Full
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "GameIQCommon.ps1")

$uproject = Resolve-GameIQProject $Project
$engineRoot = Resolve-GameIQEngine $uproject $Engine
$cmd = Join-Path $engineRoot "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
if (-not (Test-Path $cmd)) { throw "UnrealEditor-Cmd not found: $cmd" }

Write-Host "=== Game IQ build ===" -ForegroundColor Cyan
Write-Host "Project: $uproject"
Write-Host "Engine : $engineRoot"

if (Test-GameIQProjectOpen $uproject) { throw "Close the Unreal Editor that has $uproject open first (a commandlet needs exclusive project access)." }

$extra = @()
if ($Full) { $extra += "-full" }
& $cmd $uproject -run=GameIQBuild -unattended -nopause -nosplash -stdout @extra |
    Select-String -Pattern "LogGameIQ" | ForEach-Object { $_.Line }

Write-Host "=== Done ===" -ForegroundColor Cyan
