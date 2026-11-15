# Shared helpers for the Game IQ scripts (dot-source: . "$PSScriptRoot\GameIQCommon.ps1").
#
# Resolve-GameIQProject  — a project folder or .uproject path -> the .uproject file. A folder is
#                          searched for *.uproject, so a project file named differently from its
#                          folder (DeadReel.uproject in MegaJam2026/) is found.
# Resolve-GameIQEngine   — the engine root for a project: an explicit -Engine wins; otherwise the
#                          .uproject EngineAssociation is looked up in the launcher registry
#                          (installed versions) and the source-build registry (GUID associations),
#                          then in the usual install folders.
# Test-GameIQProjectOpen — true when an Unreal Editor process has THIS project open.

function Resolve-GameIQProject([Parameter(Mandatory = $true)][string]$Project) {
    if ($Project.EndsWith(".uproject", [StringComparison]::OrdinalIgnoreCase)) {
        if (-not (Test-Path $Project)) { throw "uproject not found: $Project" }
        return (Resolve-Path $Project).Path
    }
    if (-not (Test-Path $Project -PathType Container)) { throw "project folder not found: $Project" }
    $found = @(Get-ChildItem -Path $Project -Filter "*.uproject" -File)
    if ($found.Count -eq 0) { throw "no .uproject in $Project" }
    if ($found.Count -gt 1) {
        $leaf = Split-Path (Resolve-Path $Project).Path -Leaf
        $match = $found | Where-Object { $_.BaseName -eq $leaf } | Select-Object -First 1
        if ($match) { return $match.FullName }
        throw "several .uproject files in ${Project}: pass one with -Project <file>.uproject"
    }
    return $found[0].FullName
}

function Resolve-GameIQEngine([Parameter(Mandatory = $true)][string]$UProject, [string]$Engine) {
    if ($Engine) {
        if (-not (Test-Path (Join-Path $Engine "Engine\Binaries\Win64"))) { throw "not an engine root: $Engine" }
        return $Engine
    }
    $association = (Get-Content $UProject -Raw | ConvertFrom-Json).EngineAssociation
    if (-not $association) { throw "$UProject has no EngineAssociation; pass -Engine <engine root>" }

    try {
        $release = Get-ItemProperty "HKLM:\SOFTWARE\EpicGames\Unreal Engine\$association" -ErrorAction Stop
        if ($release.InstalledDirectory -and (Test-Path $release.InstalledDirectory)) { return $release.InstalledDirectory }
    } catch {}
    try {
        $builds = Get-ItemProperty "HKCU:\SOFTWARE\Epic Games\Unreal Engine\Builds" -ErrorAction Stop
        $path = $builds.$association
        if ($path -and (Test-Path $path)) { return $path }
    } catch {}
    foreach ($drive in @("C", "D", "E", "F")) {
        $candidate = "${drive}:\Program Files\Epic Games\UE_$association"
        if (Test-Path $candidate) { return $candidate }
    }
    throw "could not find Unreal Engine '$association' (registry or Program Files); pass -Engine <engine root>"
}

function Test-GameIQProjectOpen([Parameter(Mandatory = $true)][string]$UProject) {
    $full = (Resolve-Path $UProject).Path
    $procs = Get-CimInstance Win32_Process -Filter "Name LIKE 'UnrealEditor%'" -ErrorAction SilentlyContinue
    foreach ($p in $procs) {
        if ($p.CommandLine -and $p.CommandLine.IndexOf($full, [StringComparison]::OrdinalIgnoreCase) -ge 0) { return $true }
    }
    return $false
}
