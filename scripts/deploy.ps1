<#
.SYNOPSIS
  Deploy the Game IQ UE plugin into a project, then optionally build, extract, and index.

.DESCRIPTION
  Automates the loop: copy plugin/GameIQ -> <Project>/Plugins/GameIQ, enable it in the
  .uproject, build the editor target, run the extraction commandlets, and build the index.
  Engine is auto-discovered from the .uproject's EngineAssociation.

.PARAMETER Project
  Path to the target project root (folder containing the .uproject).

.PARAMETER Build    Build the <Name>Editor target after deploying (UBT).
.PARAMETER Extract  Run the GameIQExport + GameIQBlueprints commandlets (needs the editor closed).
.PARAMETER Index    Run `gameiq index` to ingest the extractor output into .gameiq/index.db.
.PARAMETER All      Shorthand for -Build -Extract -Index.

.EXAMPLE
  ./scripts/deploy.ps1 -Project E:\Repo\ThirdPerson58 -All
#>
param(
    [Parameter(Mandatory = $true)][string]$Project,
    [switch]$Build,
    [switch]$Extract,
    [switch]$Index,
    [switch]$All
)

$ErrorActionPreference = "Stop"
if ($All) { $Build = $true; $Extract = $true; $Index = $true }

$repoRoot   = Split-Path $PSScriptRoot -Parent
$pluginSrc  = Join-Path $repoRoot "plugin\GameIQ"
$projectRoot = (Resolve-Path $Project).Path

$uproject = Get-ChildItem -Path $projectRoot -Filter "*.uproject" -File | Select-Object -First 1
if (-not $uproject) { Write-Host "ERROR: no .uproject in $projectRoot" -ForegroundColor Red; exit 1 }
$projectName = $uproject.BaseName
$projectPath = $uproject.FullName

Write-Host "=== Game IQ deploy ===" -ForegroundColor Cyan
Write-Host "Plugin : $pluginSrc"  -ForegroundColor Gray
Write-Host "Project: $projectPath" -ForegroundColor Yellow

# --- 1. copy plugin (keep any existing Binaries/Intermediate for incremental builds) ---
$pluginDst = Join-Path $projectRoot "Plugins\GameIQ"
Write-Host "Copying plugin -> $pluginDst" -ForegroundColor Yellow
robocopy $pluginSrc $pluginDst /E /XD Binaries Intermediate /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { Write-Host "ERROR: robocopy failed ($LASTEXITCODE)" -ForegroundColor Red; exit 1 }
$global:LASTEXITCODE = 0  # robocopy 1-7 are success

# --- 2. enable GameIQ in the .uproject ---
$json = Get-Content $projectPath -Raw | ConvertFrom-Json
$plugins = @($json.Plugins)
if (-not ($plugins | Where-Object { $_.Name -eq "GameIQ" })) {
    $entry = [pscustomobject]@{ Name = "GameIQ"; Enabled = $true; TargetAllowList = @("Editor") }
    $json.Plugins = $plugins + $entry
    ($json | ConvertTo-Json -Depth 10) | Set-Content $projectPath -Encoding utf8
    Write-Host "Enabled GameIQ in $($uproject.Name)" -ForegroundColor Green
} else {
    Write-Host "GameIQ already enabled in $($uproject.Name)" -ForegroundColor Gray
}

# --- engine discovery (from EngineAssociation) ---
function Resolve-Engine([string]$assoc) {
    foreach ($hive in @("HKCU:\SOFTWARE\Epic Games\Unreal Engine\Builds")) {
        try { $b = Get-ItemProperty $hive -ErrorAction SilentlyContinue; if ($b.$assoc) { return $b.$assoc } } catch {}
    }
    try {
        $r = Get-ItemProperty "HKLM:\SOFTWARE\EpicGames\Unreal Engine\$assoc" -ErrorAction SilentlyContinue
        if ($r.InstalledDirectory) { return $r.InstalledDirectory }
    } catch {}
    foreach ($c in @("E:\Program Files\Epic Games\UE_$assoc", "C:\Program Files\Epic Games\UE_$assoc", "D:\Program Files\Epic Games\UE_$assoc")) {
        if (Test-Path $c) { return $c }
    }
    return $null
}

$enginePath = $null
if ($Build -or $Extract) {
    $assoc = ($json.EngineAssociation)
    $enginePath = Resolve-Engine $assoc
    if (-not $enginePath) { Write-Host "ERROR: could not find UE '$assoc'" -ForegroundColor Red; exit 1 }
    Write-Host "Engine : $enginePath" -ForegroundColor Yellow
}

# --- 3. build the editor target ---
if ($Build) {
    $buildBat = Join-Path $enginePath "Engine\Build\BatchFiles\Build.bat"
    Write-Host "Building ${projectName}Editor..." -ForegroundColor Yellow
    & $buildBat "${projectName}Editor" Win64 Development $projectPath -waitmutex
    if ($LASTEXITCODE -ne 0) { Write-Host "Build failed ($LASTEXITCODE)" -ForegroundColor Red; exit 1 }
    Write-Host "Build OK" -ForegroundColor Green
}

# --- 4. run extraction commandlets (editor must be closed) ---
# Commandlets exit non-zero on teardown / per-asset load errors (ShowErrorCount),
# so we don't trust the exit code — we verify the output file was written, and retry once.
if ($Extract) {
    $cmdExe = Join-Path $enginePath "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
    $extractDir = Join-Path $projectRoot ".gameiq\extract"

    # Clear lingering UE processes first — a leftover editor / ShaderCompileWorker holds
    # locks that make the commandlet exit before writing. This was the main flakiness source.
    $stale = Get-Process | Where-Object { $_.ProcessName -like "UnrealEditor*" -or $_.ProcessName -like "ShaderCompileWorker*" }
    if ($stale) {
        Write-Host "Clearing $($stale.Count) lingering UE process(es) before extraction..." -ForegroundColor DarkYellow
        $stale | ForEach-Object { taskkill /F /PID $_.Id /T 2>$null | Out-Null }
        Start-Sleep 2
    }
    $expected = [ordered]@{ "GameIQExport" = "registry.json"; "GameIQBlueprints" = "blueprints.json"; "GameIQAssets" = "assets.json" }
    foreach ($run in $expected.Keys) {
        $outFile = Join-Path $extractDir $expected[$run]
        # Don't delete the last-good output — a transient failure shouldn't destroy it.
        # Success = the file exists AND was (re)written after we started this attempt.
        # The first run right after a rebuild often fails to write (shader/DDC cold-start);
        # warm runs succeed, so retry a few times before giving up.
        foreach ($attempt in 1..3) {
            $startTime = Get-Date
            Write-Host "Running commandlet: $run (attempt $attempt)" -ForegroundColor Yellow
            # -unattended is required: without it the commandlet can block on a modal dialog and hang.
            & $cmdExe $projectPath -run=$run -unattended -nopause -nosplash -stdout | Out-Null
            if ((Test-Path $outFile) -and ((Get-Item $outFile).LastWriteTime -ge $startTime)) { break }
            Write-Host "  $($expected[$run]) not freshly written; retrying..." -ForegroundColor DarkYellow
        }
        if ((Test-Path $outFile) -and ((Get-Item $outFile).LastWriteTime -ge $startTime)) {
            Write-Host "  wrote $($expected[$run])" -ForegroundColor Green
        } else {
            Write-Host "  WARNING: $run did not freshly write $($expected[$run]) (keeping any prior copy)" -ForegroundColor Red
        }
    }
}

# --- 5. build the index ---
if ($Index) {
    Write-Host "Indexing..." -ForegroundColor Yellow
    Push-Location $repoRoot
    try { & node --import tsx "packages\core\src\cli\index.ts" index --project $projectRoot }
    finally { Pop-Location }
}

Write-Host "=== Done ===" -ForegroundColor Green
