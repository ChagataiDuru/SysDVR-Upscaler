param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$projectRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$bridgeProject = Join-Path $projectRoot "external\SysDVR-UpscalerBridge\Client\Client.csproj"
$outputDir = Join-Path $projectRoot "artifacts\sysdvr-upscaler-bridge\win-x64"
$nativeResourceDir = Join-Path $projectRoot "external\SysDVR-UpscalerBridge\Client\Platform\Resources\win-x64\native"
$libusbDll = Join-Path $nativeResourceDir "libusb-1.0.dll"

if (-not (Test-Path $bridgeProject)) {
    throw "SysDVR-UpscalerBridge submodule is missing at external\SysDVR-UpscalerBridge."
}

$sdks = & dotnet --list-sdks
$hasNet9 = $sdks | Where-Object { $_ -match '^9\.0' } | Select-Object -First 1
if ($LASTEXITCODE -ne 0 -or -not $hasNet9) {
    throw ".NET 9 SDK is required to build SysDVR-UpscalerBridge. Installed SDKs: $($sdks -join ', ')"
}

New-Item -ItemType Directory -Force $outputDir | Out-Null
New-Item -ItemType Directory -Force $nativeResourceDir | Out-Null

if (-not (Test-Path $libusbDll)) {
    $sevenZip = Get-Command 7z.exe -ErrorAction SilentlyContinue
    if (-not $sevenZip) {
        $sevenZip = Get-Command "C:\Program Files\7-Zip\7z.exe" -ErrorAction SilentlyContinue
    }
    if (-not $sevenZip) {
        throw "libusb-1.0.dll is missing and 7-Zip was not found. Install 7-Zip or run external\SysDVR-UpscalerBridge\Client\Platform\BuildWindows.bat once."
    }

    $tempDir = Join-Path $env:TEMP ("sysdvr-libusb-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force $tempDir | Out-Null
    try {
        $archive = Join-Path $tempDir "libusb.7z"
        Invoke-WebRequest "https://github.com/libusb/libusb/releases/download/v1.0.26/libusb-1.0.26-binaries.7z" -OutFile $archive
        & $sevenZip.Source x $archive "libusb-1.0.26-binaries\VS2015-x64\dll\libusb-1.0.dll" "-o$tempDir" -y | Out-Host
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        Copy-Item -Force (Join-Path $tempDir "libusb-1.0.26-binaries\VS2015-x64\dll\libusb-1.0.dll") $libusbDll
    }
    finally {
        Remove-Item -Recurse -Force $tempDir -ErrorAction SilentlyContinue
    }
}

dotnet restore $bridgeProject
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

dotnet publish $bridgeProject `
    -c $Configuration `
    -r win-x64 `
    --self-contained true `
    -p:SysDvrTarget=windows `
    -o $outputDir
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Copy-Item -Force $libusbDll (Join-Path $outputDir "libusb-1.0.dll")

exit 0