#pragma once

#include <string_view>

namespace ns60 {

enum class DecoderBackend { Software, D3D11VA, Auto };

[[nodiscard]] inline std::string_view toString(DecoderBackend backend) noexcept {
    switch (backend) {
    case DecoderBackend::Software: return "software";
    case DecoderBackend::D3D11VA: return "d3d11va";
    case DecoderBackend::Auto: return "auto";
    }
    return "unknown";
}

} // namespace ns60
