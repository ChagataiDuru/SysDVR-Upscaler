#pragma once

#include <array>
#include <cstdint>

namespace ns60 {

enum class ColorRange { Limited, Full };
enum class ColorMatrix { Bt601, Bt709, Bt2020Ncl };

struct ColorDescription {
    ColorRange range{ColorRange::Limited};
    ColorMatrix matrix{ColorMatrix::Bt709};
};

[[nodiscard]] std::array<float, 3> yuvToRgb(std::uint8_t y, std::uint8_t cb, std::uint8_t cr,
                                            ColorDescription color) noexcept;
[[nodiscard]] const char* toString(ColorRange value) noexcept;
[[nodiscard]] const char* toString(ColorMatrix value) noexcept;

} // namespace ns60

