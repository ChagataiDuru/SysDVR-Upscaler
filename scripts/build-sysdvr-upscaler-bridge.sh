#!/usr/bin/env bash
# Publishes SysDVR-UpscalerBridge for macOS arm64 into artifacts/sysdvr-upscaler-bridge/osx-arm64.
set -euo pipefail

configuration="${1:-Release}"
case "$configuration" in
  Debug|Release) ;;
  *) echo "Usage: $0 [Debug|Release]" >&2; exit 2 ;;
esac

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bridge_project="$project_root/external/SysDVR-UpscalerBridge/Client/Client.csproj"
runtime="osx-arm64"
output_dir="$project_root/artifacts/sysdvr-upscaler-bridge/$runtime"
native_dir="$output_dir/runtimes/$runtime/native"

if [[ ! -f "$bridge_project" ]]; then
  echo "SysDVR-UpscalerBridge submodule is missing at external/SysDVR-UpscalerBridge. Run 'git submodule update --init'." >&2
  exit 1
fi

if ! command -v dotnet >/dev/null 2>&1; then
  echo ".NET 9 SDK is required to build SysDVR-UpscalerBridge, but dotnet was not found." >&2
  exit 1
fi
if ! dotnet --list-sdks | grep -Eq '^9\.0'; then
  echo ".NET 9 SDK is required to build SysDVR-UpscalerBridge. Installed SDKs:" >&2
  dotnet --list-sdks >&2
  exit 1
fi

libusb_prefix="${LIBUSB_PREFIX:-}"
if [[ -z "$libusb_prefix" ]] && command -v brew >/dev/null 2>&1; then
  libusb_prefix="$(brew --prefix libusb 2>/dev/null || true)"
fi
libusb_dylib="$libusb_prefix/lib/libusb-1.0.dylib"
if [[ -z "$libusb_prefix" || ! -f "$libusb_dylib" ]]; then
  echo "libusb-1.0.dylib was not found. Install it with 'brew install libusb' or set LIBUSB_PREFIX." >&2
  exit 1
fi

mkdir -p "$output_dir"
dotnet restore "$bridge_project" -r "$runtime"
# SysDvrTarget=macos publishes a NativeAOT executable, as upstream's Client/Platform/BuildMacos.sh does.
dotnet publish "$bridge_project" -c "$configuration" -r "$runtime" -p:SysDvrTarget=macos -o "$output_dir"

# The client searches runtimes/<rid>/native next to its executable before the system paths.
mkdir -p "$native_dir"
cp -fL "$libusb_dylib" "$native_dir/libusb-1.0.dylib"
chmod +x "$output_dir/SysDVR-Client"
echo "SysDVR-UpscalerBridge published to $output_dir"
