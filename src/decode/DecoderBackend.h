#pragma once

#include <string_view>

namespace ns60 {

enum class DecoderBackend { Software, D3D11VA, Auto };
enum class DecoderPath { Readback, D3D11VulkanInterop };

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
    }
    return "unknown";
}

} // namespace ns60
