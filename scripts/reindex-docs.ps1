# Reindex ONLY documentation in the Game IQ index — fast, and safe to run with the editor open.
#
# Runs the GameIQDocsBuild commandlet: it re-runs just the doc extractors (docs, images, external
# connectors), ingests only their producers (producer-scoped, so code/asset/Blueprint entities are
# left untouched), and refreshes the intent->implementation links. Use this after editing design
# docs / adding images — it skips the expensive asset + Blueprint rescan a full build does.
#
#   ./scripts/reindex-docs.ps1 -Project E:\Repo\ThirdPerson58
#
# Unlike a full build, this can run WHILE the editor is open — it's a separate headless process and
# the index is a rollback-journal SQLite file that tolerates concurrent readers.

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

Write-Host "=== Game IQ docs reindex ===" -ForegroundColor Cyan
Write-Host "Project: $uproject"

& $cmd $uproject -run=GameIQDocsBuild -unattended -nopause -nosplash -stdout |
    Select-String -Pattern "LogGameIQ" | ForEach-Object { $_.Line }

Write-Host "=== Done ===" -ForegroundColor Cyan
