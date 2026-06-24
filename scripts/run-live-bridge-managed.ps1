param(
    [string]$ParentExe = ".\build\win-release\NexusStream60.exe",
    [string]$BridgeExe = ".\artifacts\sysdvr-upscaler-bridge\win-x64\SysDVR-Client.exe"
)

$ErrorActionPreference = "Stop"
$projectRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $projectRoot

if (-not (Test-Path $ParentExe)) { throw "Parent executable not found: $ParentExe" }
if (-not (Test-Path $BridgeExe)) { throw "Published bridge executable not found: $BridgeExe" }

& $ParentExe --source sysdvr --sysdvr-bridge $BridgeExe --quality-preset balanced --fullscreen --borderless --presentation exact
exit $LASTEXITCODE