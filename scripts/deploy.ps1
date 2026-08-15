<#
.SYNOPSIS
  Deploy the root-level Game IQ plugin into an Unreal project.

.DESCRIPTION
  Copies the plugin payload into <Project>/Plugins/GameIQ, enables it in the .uproject,
  and can optionally build the editor target and create the initial Game IQ index.
  Engine location is resolved from the project's EngineAssociation.

.PARAMETER Project
  Path to the target project root (the folder containing its .uproject).

.PARAMETER Build
  Build the <ProjectName>Editor target after deployment.

.PARAMETER Index
  Run the all-in-one GameIQBuild commandlet. The legacy -Extract name remains an alias.

.PARAMETER All
  Shorthand for -Build -Index.

.EXAMPLE
  ./scripts/deploy.ps1 -Project E:\Repo\ThirdPerson58 -All
#>
param(
    [Parameter(Mandatory = $true)][string]$Project,
    [switch]$Build,
    [Alias("Extract")][switch]$Index,
    [switch]$All
)

$ErrorActionPreference = "Stop"
if ($All) { $Build = $true; $Index = $true }

$repoRoot = Split-Path $PSScriptRoot -Parent
$projectRoot = (Resolve-Path $Project).Path
$uproject = Get-ChildItem -Path $projectRoot -Filter "*.uproject" -File | Select-Object -First 1
if (-not $uproject) { throw "No .uproject found in $projectRoot" }

$projectName = $uproject.BaseName
$projectPath = $uproject.FullName
$pluginDst = Join-Path $projectRoot "Plugins\GameIQ"

Write-Host "=== Game IQ deploy ===" -ForegroundColor Cyan
Write-Host "Source : $repoRoot" -ForegroundColor Gray
Write-Host "Project: $projectPath" -ForegroundColor Yellow

# Copy only the deployable plugin payload. Keep target build artifacts for incremental builds.
New-Item -ItemType Directory -Force -Path $pluginDst | Out-Null
Copy-Item -Force (Join-Path $repoRoot "GameIQ.uplugin") $pluginDst
foreach ($directory in @("Source", "Content", "Resources", "Config")) {
    $source = Join-Path $repoRoot $directory
    if (-not (Test-Path $source)) { continue }

    $destination = Join-Path $pluginDst $directory
    robocopy $source $destination /MIR /XD Binaries Intermediate Saved DerivedDataCache /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed for $directory ($LASTEXITCODE)" }
    $global:LASTEXITCODE = 0
}

# Enable GameIQ without duplicating an existing project entry.
$projectJson = Get-Content $projectPath -Raw | ConvertFrom-Json
$plugins = @($projectJson.Plugins)
$entry = $plugins | Where-Object { $_.Name -eq "GameIQ" } | Select-Object -First 1
if ($entry) {
    $entry.Enabled = $true
    Write-Host "GameIQ already present; ensured it is enabled." -ForegroundColor Gray
} else {
    $projectJson.Plugins = $plugins + [pscustomobject]@{
        Name = "GameIQ"
        Enabled = $true
        TargetAllowList = @("Editor")
    }
    Write-Host "Enabled GameIQ in $($uproject.Name)." -ForegroundColor Green
}
($projectJson | ConvertTo-Json -Depth 20) | Set-Content $projectPath -Encoding utf8

function Resolve-Engine([string]$Association) {
    try {
        $builds = Get-ItemProperty "HKCU:\SOFTWARE\Epic Games\Unreal Engine\Builds" -ErrorAction SilentlyContinue
        if ($builds.$Association) { return $builds.$Association }
    } catch {}

    try {
        $release = Get-ItemProperty "HKLM:\SOFTWARE\EpicGames\Unreal Engine\$Association" -ErrorAction SilentlyContinue
        if ($release.InstalledDirectory) { return $release.InstalledDirectory }
    } catch {}

    foreach ($candidate in @(
        "E:\Program Files\Epic Games\UE_$Association",
        "C:\Program Files\Epic Games\UE_$Association",
        "D:\Program Files\Epic Games\UE_$Association"
    )) {
        if (Test-Path $candidate) { return $candidate }
    }
    return $null
}

$enginePath = $null
if ($Build -or $Index) {
    $enginePath = Resolve-Engine $projectJson.EngineAssociation
    if (-not $enginePath) { throw "Could not locate Unreal Engine '$($projectJson.EngineAssociation)'" }
    Write-Host "Engine : $enginePath" -ForegroundColor Yellow
}

if ($Build) {
    $buildBat = Join-Path $enginePath "Engine\Build\BatchFiles\Build.bat"
    Write-Host "Building ${projectName}Editor..." -ForegroundColor Yellow
    & $buildBat "${projectName}Editor" Win64 Development "-project=$projectPath" -waitmutex
    if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }
    Write-Host "Build OK" -ForegroundColor Green
}

if ($Index) {
    if (Get-Process UnrealEditor -ErrorAction SilentlyContinue) {
        throw "Close Unreal Editor before running GameIQBuild."
    }

    $commandlet = Join-Path $enginePath "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
    $marker = Join-Path $projectRoot ".gameiq\last-build.json"
    $started = Get-Date
    Write-Host "Building Game IQ index..." -ForegroundColor Yellow
    & $commandlet $projectPath -run=GameIQBuild -unattended -nopause -nosplash -stdout |
        Select-String -Pattern "LogGameIQ" | ForEach-Object { $_.Line }

    if (-not (Test-Path $marker) -or (Get-Item $marker).LastWriteTime -lt $started) {
        throw "GameIQBuild did not write a fresh result marker: $marker"
    }
    $result = Get-Content $marker -Raw | ConvertFrom-Json
    if (-not $result.success) { throw "GameIQBuild reported failure; inspect its stage logs." }
    Write-Host "Index OK" -ForegroundColor Green
}

Write-Host "=== Done ===" -ForegroundColor Cyan
