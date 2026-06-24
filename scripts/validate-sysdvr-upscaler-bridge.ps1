param(
    [string]$ParentExe = ".\build\win-release\NexusStream60.exe",
    [string]$BridgeExe = ".\artifacts\sysdvr-upscaler-bridge\win-x64\SysDVR-Client.exe"
)

$ErrorActionPreference = "Stop"
$projectRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $projectRoot

$checks = @(
    @{ Name = "Parent executable"; Ok = Test-Path $ParentExe },
    @{ Name = "Bridge executable"; Ok = Test-Path $BridgeExe },
    @{ Name = "Protocol documentation"; Ok = Test-Path ".\docs\sysdvr-upscaler-bridge-protocol.md" },
    @{ Name = "Bridge submodule"; Ok = Test-Path ".\external\SysDVR-UpscalerBridge\Client\Client.csproj" },
    @{ Name = ".NET SDK"; Ok = $null -ne (Get-Command dotnet -ErrorAction SilentlyContinue) }
)

$failed = $false
foreach ($check in $checks) {
    if ($check.Ok) { Write-Host "[OK] $($check.Name)" }
    else { Write-Host "[FAIL] $($check.Name)"; $failed = $true }
}

if (Test-Path $BridgeExe) { & $BridgeExe --version }
if (Test-Path $ParentExe) { & $ParentExe --version }

if ($failed) { exit 1 }