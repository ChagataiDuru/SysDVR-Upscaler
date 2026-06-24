[CmdletBinding()]
param(
    [ValidateSet('win-debug', 'win-release', 'core-tests')]
    [string]$Preset = 'win-release'
)
$ErrorActionPreference = 'Stop'
if (-not $env:VCPKG_ROOT) { throw 'VCPKG_ROOT is not set. Point it to a vcpkg checkout.' }
if (-not $env:VULKAN_SDK -and $Preset -ne 'core-tests') { throw 'VULKAN_SDK is not set. Install the LunarG Vulkan SDK.' }
cmake --preset $Preset
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

