# Windows dependencies

The supported path is Visual Studio 2022 x64 + Ninja + the Vulkan SDK + vcpkg manifest mode. Set `VCPKG_ROOT` and `VULKAN_SDK`; do not add machine-specific directories to project files.

```powershell
$env:VCPKG_ROOT = 'C:\src\vcpkg'
$env:VULKAN_SDK = 'C:\VulkanSDK\<version>'
cmake --preset win-debug
cmake --build --preset win-debug
```

The manifest resolves `ffmpeg[avcodec,avformat]`, `glfw3`, `imgui[glfw-binding,vulkan-binding]`, `stb`, and `doctest`. Vulkan comes from the SDK so headers, loader, layers, and shader compiler remain a coherent set.

For a non-vcpkg FFmpeg build, `FFMPEG_ROOT/include` must contain `libavformat/avformat.h` and `FFMPEG_ROOT/lib` must contain linkable avformat, avcodec, and avutil libraries. Runtime DLLs, when using a shared build, must be beside the executable or on `PATH`. The Gyan “essentials” command-line archive is useful for validation scripts but normally lacks the development files required by the application.

