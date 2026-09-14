# macOS dependencies

The supported path is Apple Silicon + Xcode Command Line Tools + CMake/Ninja + the LunarG Vulkan SDK + vcpkg manifest mode. Set `VCPKG_ROOT` and source the Vulkan SDK environment; do not add machine-specific directories to project files.

```sh
xcode-select --install
brew install cmake ninja pkg-config libusb
git clone https://github.com/microsoft/vcpkg ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh -disableMetrics

export VCPKG_ROOT="$HOME/vcpkg"
source ~/VulkanSDK/<version>/setup-env.sh
cmake --preset mac-debug
cmake --build --preset mac-debug
```

The manifest resolves the same packages as on Windows. On `arm64-osx`, vcpkg builds FFmpeg with `--enable-videotoolbox`; the first configure compiles it from source (expect several minutes), and later presets restore it from vcpkg's binary cache. vcpkg's FFmpeg CMake wrapper uses `pkg-config` to attach the Apple framework link flags.

`imgui[vulkan-binding]` also brings in vcpkg's `vulkan-loader`, which the executable links. That loader discovers the MoltenVK ICD the LunarG SDK installs under `/usr/local/share/vulkan/icd.d`. `setup-env.sh` puts `glslc` on `PATH` and points the loader at the SDK's validation layer, which Debug builds enable by default.

No deployment target is pinned: the project and vcpkg's ports both build for the host SDK, so their objects link without version-mismatch warnings.

## Live bridge

SysDVR-UpscalerBridge targets .NET 9; an installed .NET 8 SDK is not enough. A user-level install avoids a system package:

```sh
curl -sSL https://dot.net/v1/dotnet-install.sh -o dotnet-install.sh
bash dotnet-install.sh --channel 9.0 --install-dir "$HOME/.dotnet"
export DOTNET_ROOT="$HOME/.dotnet" PATH="$HOME/.dotnet:$PATH"
./scripts/build-sysdvr-upscaler-bridge.sh
```

The script publishes a NativeAOT `SysDVR-Client` to `artifacts/sysdvr-upscaler-bridge/osx-arm64/` and copies Homebrew's `libusb-1.0.dylib` into `runtimes/osx-arm64/native/`, the first place the client looks for native libraries. Set `LIBUSB_PREFIX` to use a libusb that is not installed through Homebrew. The headless bridge mode does not load SDL or cimgui.

## Duplicate MoltenVK installs

Installing Homebrew's `molten-vk` as well as the LunarG SDK system-wide leaves two copies of `libMoltenVK.dylib` (`/opt/homebrew/...` and `/usr/local/lib`). `vulkaninfo` then prints an Objective-C "implemented in both" warning. Keep one copy so only one MoltenVK is loaded into the process.
