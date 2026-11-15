# Reindex ONLY documentation in the Game IQ index — fast, and safe to run with the editor open.
#
# Runs the GameIQDocsBuild commandlet: it re-runs just the doc extractors (docs, images, external
# connectors), ingests only their producers (producer-scoped, so code/asset/Blueprint entities are
# left untouched), and refreshes the intent->implementation links. Use this after editing design
# docs / adding images — it skips the expensive asset + Blueprint rescan a full build does. (With
# the editor open this normally happens by itself: Project Settings > Plugins > Game IQ > Auto
# Reindex Docs watches the docs folder.)
#
#   ./scripts/reindex-docs.ps1 -Project E:\Repo\MyGame            # folder: its *.uproject is found
#   ./scripts/reindex-docs.ps1 -Project E:\Repo\MyGame\MyGame.uproject -Engine "D:\UE_5.9"
#
# Unlike a full build, this can run WHILE the editor is open — it's a separate headless process and
# the index is a rollback-journal SQLite file that tolerates concurrent readers.

param(
    [Parameter(Mandatory = $true)][string]$Project,
    [string]$Engine
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "GameIQCommon.ps1")

$uproject = Resolve-GameIQProject $Project
$engineRoot = Resolve-GameIQEngine $uproject $Engine
$cmd = Join-Path $engineRoot "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
if (-not (Test-Path $cmd)) { throw "UnrealEditor-Cmd not found: $cmd" }

Write-Host "=== Game IQ docs reindex ===" -ForegroundColor Cyan
Write-Host "Project: $uproject"
Write-Host "Engine : $engineRoot"

& $cmd $uproject -run=GameIQDocsBuild -unattended -nopause -nosplash -stdout |
    Select-String -Pattern "LogGameIQ" | ForEach-Object { $_.Line }

Write-Host "=== Done ===" -ForegroundColor Cyan
