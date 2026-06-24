param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

& (Join-Path $PSScriptRoot "build-sysdvr-upscaler-bridge.ps1") -Configuration $Configuration
exit $LASTEXITCODE