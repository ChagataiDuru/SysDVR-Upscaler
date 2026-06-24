[CmdletBinding()]
param(
    [string]$Executable = '.\build\win-release\NexusStream60.exe',
    [string]$SamplesPath = '.\samples',
    [string]$Sample,
    [string]$Upscale = 'fsr1-easu-rcas',
    [double]$CasSharpness = 0.35,
    [double]$RcasSharpness = 0.25,
    [switch]$Compare,
    [switch]$Fullscreen,
    [switch]$Borderless,
    [switch]$Loop
)
$ErrorActionPreference = 'Stop'
$validModes = @('nearest','bilinear','bicubic','lanczos2','bilinear-cas','lanczos2-cas','fsr1-easu','fsr1-easu-rcas')
if ($Upscale -notin $validModes) { throw "Invalid -Upscale '$Upscale'. Valid modes: $($validModes -join ', ')" }
if ($CasSharpness -lt 0 -or $CasSharpness -gt 1) { throw '-CasSharpness must be in [0,1]' }
if ($RcasSharpness -lt 0 -or $RcasSharpness -gt 1) { throw '-RcasSharpness must be in [0,1]' }
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) { throw "Executable not found: $Executable" }
$names = if ($Sample) { @($Sample) } else { @('ui_text_720p60.mp4', 'fast_motion_720p60.mp4', 'flat_color_720p60.mp4') }
foreach ($name in $names) {
    $path = Join-Path $SamplesPath $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { Write-Warning "Skipping missing sample: $path"; continue }
    $arguments = @('--input',$path,'--width','1920','--height','1080','--cas-sharpness',$CasSharpness.ToString([Globalization.CultureInfo]::InvariantCulture),'--rcas-sharpness',$RcasSharpness.ToString([Globalization.CultureInfo]::InvariantCulture))
    if ($Compare) { $arguments += @('--compare',"bilinear,$Upscale") } else { $arguments += @('--upscale',$Upscale) }
    if ($Fullscreen) { $arguments += '--fullscreen' }
    if ($Borderless) { $arguments += '--borderless' }
    if ($Loop) { $arguments += '--loop' }
    & $Executable @arguments
    if ($LASTEXITCODE -ne 0) { throw "NexusStream60 failed for $path with exit code $LASTEXITCODE" }
}
