#pragma once

#include <string_view>

namespace ns60 {

enum class DecoderBackend { Software, D3D11VA, Auto };
// D3D11VulkanInterop imports the decoder's texture array directly (zero-copy).
// D3D11VulkanInteropCopy copies each decoded slice on the GPU into a standalone
// shared NV12 texture first, for drivers whose decoder-array layout does not
// match a layered Vulkan import.
enum class DecoderPath { Readback, D3D11VulkanInterop, D3D11VulkanInteropCopy };

[[nodiscard]] constexpr bool usesD3D11VulkanInterop(DecoderPath path) noexcept {
    return path == DecoderPath::D3D11VulkanInterop || path == DecoderPath::D3D11VulkanInteropCopy;
}

[[nodiscard]] inline std::string_view toString(DecoderBackend backend) noexcept {
    switch (backend) {
    case DecoderBackend::Software: return "software";
    case DecoderBackend::D3D11VA: return "d3d11va";
    case DecoderBackend::Auto: return "auto";
    }
    return "unknown";
}

[[nodiscard]] inline std::string_view toString(DecoderPath path) noexcept {
    switch (path) {
    case DecoderPath::Readback: return "readback";
    case DecoderPath::D3D11VulkanInterop: return "d3d11-vulkan-interop";
    case DecoderPath::D3D11VulkanInteropCopy: return "d3d11-vulkan-interop-copy";
    }
    return "unknown";
}

} // namespace ns60
