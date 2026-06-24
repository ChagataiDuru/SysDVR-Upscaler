#pragma once
#include <array>
#include <cstdint>
namespace ns60 {
struct FsrEasuConstants { std::array<std::uint32_t,4> con0{},con1{},con2{},con3{}; };
struct SharpenConstants { std::array<std::uint32_t,4> con0{},con1{}; };
[[nodiscard]] FsrEasuConstants makeFsrEasuConstants(float inputWidth,float inputHeight,float outputWidth,float outputHeight) noexcept;
[[nodiscard]] std::array<std::uint32_t,4> makeFsrRcasConstants(float userStrength) noexcept;
[[nodiscard]] SharpenConstants makeCasConstants(float userStrength,float width,float height) noexcept;
} // namespace ns60
