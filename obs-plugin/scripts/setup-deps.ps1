# Downloads the OBS sources (headers) and obs-deps (FFmpeg) needed to build the plugin into ..\.deps
# Versions MUST match the installed OBS. After updating OBS, change these values and run again:
#   - ObsVersion: installed OBS version
#   - DepsVersion/DepsHash: "prebuilt" entry in CMakePresets.json of that obs-studio version
# (This file is ASCII only because Windows PowerShell 5.1 misreads UTF-8 files without a BOM.)
param(
    [string]$ObsVersion = "33.0.1",
    [string]$DepsVersion = "2026-10-23",
    [string]$DepsHash = "7508972a867a0ef68c26cd8bdba61fd999edd0a92fa9eb9585774de9bf51bfbe"
)

$ErrorActionPreference = "Stop"
$deps = Join-Path $PSScriptRoot "..\.deps"
New-Item -ItemType Directory -Force $deps | Out-Null
$deps = (Resolve-Path $deps).Path

$obsSource = Join-Path $deps "obs-studio"
if (-not (Test-Path (Join-Path $obsSource "libobs\obs-module.h"))) {
    Write-Host "Cloning OBS $ObsVersion sources..."
    git -c advice.detachedHead=false clone --depth 1 --branch $ObsVersion --quiet https://github.com/obsproject/obs-studio.git $obsSource
    if ($LASTEXITCODE -ne 0) { throw "git clone failed" }
}

$obsDeps = Join-Path $deps "obs-deps"
if (-not (Test-Path (Join-Path $obsDeps "include\libavcodec\avcodec.h"))) {
    $zip = Join-Path $deps "windows-deps-$DepsVersion-x64.zip"
    if (-not (Test-Path $zip)) {
        Write-Host "Downloading obs-deps $DepsVersion..."
        curl.exe -sSL --fail -o $zip "https://github.com/obsproject/obs-deps/releases/download/$DepsVersion/windows-deps-$DepsVersion-x64.zip"
        if ($LASTEXITCODE -ne 0) { throw "obs-deps download failed" }
    }
    $hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
    if ($hash -ne $DepsHash) {
        Remove-Item $zip
        throw "obs-deps checksum mismatch: $hash"
    }
    Expand-Archive -Force $zip $obsDeps
}

Write-Host "Dependencies ready: $deps"
