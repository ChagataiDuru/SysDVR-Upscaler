#include "utility/ColorMath.h"

#include <algorithm>

namespace ns60 {
namespace {
struct Coefficients {
    float rCr;
    float gCb;
    float gCr;
    float bCb;
};

constexpr Coefficients coefficients(ColorMatrix matrix) noexcept {
    switch (matrix) {
    case ColorMatrix::Bt601: return {1.4020F, -0.344136F, -0.714136F, 1.7720F};
    case ColorMatrix::Bt709: return {1.5748F, -0.187324F, -0.468124F, 1.8556F};
    case ColorMatrix::Bt2020Ncl: return {1.4746F, -0.164553F, -0.571353F, 1.8814F};
    }
    return {};
}
} // namespace

std::array<float, 3> yuvToRgb(std::uint8_t y, std::uint8_t cb, std::uint8_t cr,
                              ColorDescription color) noexcept {
    const float yf = static_cast<float>(y) / 255.0F;
    const float cbf = static_cast<float>(cb) / 255.0F;
    const float crf = static_cast<float>(cr) / 255.0F;

    // Studio range reserves code values outside [16,235]/[16,240]. Do not clamp
    // before the matrix: super-black/white values remain meaningful until output.
    const float luma = color.range == ColorRange::Limited
        ? (yf - 16.0F / 255.0F) * (255.0F / 219.0F)
        : yf;
    const float chromaScale = color.range == ColorRange::Limited ? 255.0F / 224.0F : 1.0F;
    const float u = (cbf - 128.0F / 255.0F) * chromaScale;
    const float v = (crf - 128.0F / 255.0F) * chromaScale;
    const auto c = coefficients(color.matrix);
    return {
        std::clamp(luma + c.rCr * v, 0.0F, 1.0F),
        std::clamp(luma + c.gCb * u + c.gCr * v, 0.0F, 1.0F),
        std::clamp(luma + c.bCb * u, 0.0F, 1.0F),
    };
}

const char* toString(ColorRange value) noexcept {
    return value == ColorRange::Limited ? "Limited" : "Full";
}

const char* toString(ColorMatrix value) noexcept {
    switch (value) {
    case ColorMatrix::Bt601: return "BT.601";
    case ColorMatrix::Bt709: return "BT.709";
    case ColorMatrix::Bt2020Ncl: return "BT.2020 NCL";
    }
    return "Unknown";
}
} // namespace ns60

