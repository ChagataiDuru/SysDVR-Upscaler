param(
    [string]$ParentExe = ".\build\win-release\NexusStream60.exe",
    [string]$BridgeExe = ".\external\SysDVR-UpscalerBridge\Client\bin\Debug\net9.0\SysDVR-Client.exe",
    [string]$PipeName = "SysDVR-Upscaler.Video",
    [switch]$Launch
)

$ErrorActionPreference = "Stop"
$projectRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $projectRoot

if (-not (Test-Path $ParentExe)) { throw "Parent executable not found: $ParentExe" }
if (-not (Test-Path $BridgeExe)) { throw "Bridge executable not found: $BridgeExe" }

$parentArgs = @("--source", "sysdvr-pipe", "--pipe-name", $PipeName, "--quality-preset", "balanced", "--presentation", "exact")
$bridgeArgs = @("usb", "--upscaler-video-pipe", $PipeName, "--no-audio")

Write-Host "Terminal 1:"
Write-Host "  $ParentExe $($parentArgs -join ' ')"
Write-Host "Terminal 2:"
Write-Host "  $BridgeExe $($bridgeArgs -join ' ')"

if ($Launch) {
    Start-Process -FilePath $ParentExe -ArgumentList $parentArgs
    Start-Sleep -Seconds 1
    Start-Process -FilePath $BridgeExe -ArgumentList $bridgeArgs
}