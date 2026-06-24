[CmdletBinding()]
param(
    [ValidateSet('win-debug', 'win-release', 'core-tests')]
    [string]$Preset = 'win-release'
)
$ErrorActionPreference = 'Stop'
cmake --build --preset $Preset
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($Preset -eq 'core-tests') {
    ctest --preset core-tests
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

