# Builds a CamNeuron PC plugin release into obs-plugin\dist:
#   CamNeuron-PC-Plugin-<version>-Setup.exe    installer (Inno Setup 6)
#   CamNeuron-PC-Plugin-<version>-windows-x64.zip   manual install (copy the folder into C:\ProgramData\obs-studio\plugins)
#   powershell -ExecutionPolicy Bypass -File obs-plugin\scripts\package.ps1
# The version comes from project(VERSION) in CMakeLists.txt, the OBS version from setup-deps.ps1.
# (ASCII only: Windows PowerShell 5.1 misreads UTF-8 files without a BOM.)
param([string]$Iscc = "")

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

$cmakeLists = Get-Content (Join-Path $root "CMakeLists.txt") -Raw
if ($cmakeLists -notmatch 'project\(obs-phonecam VERSION ([0-9.]+)') { throw "version not found in CMakeLists.txt" }
$version = $matches[1]
$depsScript = Get-Content (Join-Path $root "scripts\setup-deps.ps1") -Raw
if ($depsScript -notmatch '\$ObsVersion = "([^"]+)"') { throw "ObsVersion not found in setup-deps.ps1" }
$obsVersion = $matches[1]
# The plugin links to the FFmpeg DLLs that ship with OBS; their names carry the FFmpeg major version.
$depsBin = Join-Path $root ".deps\obs-deps\bin"
$avcodec = (Get-ChildItem $depsBin -Filter "avcodec-*.dll" | Select-Object -First 1).Name
$avutil = (Get-ChildItem $depsBin -Filter "avutil-*.dll" | Select-Object -First 1).Name
if (-not $avcodec -or -not $avutil) { throw "FFmpeg DLLs not found in $depsBin (run setup-deps.ps1)" }
Write-Host "CamNeuron PC plugin $version for OBS $obsVersion ($avcodec, $avutil)"

& cmake --build (Join-Path $root "build") --config Release
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$dist = Join-Path $root "dist"
$stage = Join-Path $dist "stage"
$plugin = Join-Path $stage "obs-phonecam"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $plugin "data") | Out-Null
Copy-Item (Join-Path $root "build\Release\obs-phonecam.dll") $plugin
Copy-Item (Join-Path $root "data\locale") (Join-Path $plugin "data") -Recurse
Copy-Item (Join-Path $root "LICENSE") (Join-Path $plugin "LICENSE.txt")
Copy-Item (Join-Path $root "installer\INSTALL.txt") $stage

$zip = Join-Path $dist "CamNeuron-PC-Plugin-$version-windows-x64.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip

if (-not $Iscc) {
    $candidates = @(
        (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe"),
        (Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe"),
        (Join-Path $env:ProgramFiles "Inno Setup 6\ISCC.exe")
    )
    $Iscc = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $Iscc) { throw "Inno Setup 6 not found (winget install JRSoftware.InnoSetup)" }
& $Iscc /Q "/DAppVersion=$version" "/DStageDir=$stage" "/DObsTarget=$obsVersion" "/DAvcodecDll=$avcodec" "/DAvutilDll=$avutil" "/O$dist" (Join-Path $root "installer\camneuron-plugin.iss")
if ($LASTEXITCODE -ne 0) { throw "installer build failed" }

Get-ChildItem $dist -File | Select-Object Name, Length
