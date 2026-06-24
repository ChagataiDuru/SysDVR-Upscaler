#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace ns60 {

enum class UpscaleMode { Nearest, Bilinear, BicubicCatmullRom, Lanczos2, BilinearCas, Lanczos2Cas, Fsr1Easu, Fsr1EasuRcas };
enum class PresentationMode { Exact, Fit, Fill, Integer };
enum class FinalFilter { Nearest, Bilinear };
enum class ChromaUpscaleMode { Bilinear, BicubicCatmullRom, Lanczos2, EdgeAware };
struct SharpenSettings { float casSharpness{0.35F}; float rcasSharpness{0.25F}; };
struct UpscaleParameters {
    std::uint32_t sourceWidth{}, sourceHeight{}, outputWidth{}, outputHeight{};
    SharpenSettings sharpen{};
    bool antiRinging{true};
    ChromaUpscaleMode chromaMode{ChromaUpscaleMode::BicubicCatmullRom};
    bool chromaAntiRinging{true};
};
struct PresentationGeometry {
    std::uint32_t reconstructionWidth{};
    std::uint32_t reconstructionHeight{};
    std::uint32_t framebufferWidth{};
    std::uint32_t framebufferHeight{};
    std::int32_t viewportX{};
    std::int32_t viewportY{};
    std::uint32_t viewportWidth{};
    std::uint32_t viewportHeight{};
    PresentationMode requestedMode{PresentationMode::Fit};
    PresentationMode effectiveMode{PresentationMode::Fit};
    bool exactOneToOne{};
    bool finalResampleActive{};
};

[[nodiscard]] std::string_view toString(UpscaleMode mode) noexcept;
[[nodiscard]] std::string_view displayName(UpscaleMode mode) noexcept;
[[nodiscard]] std::optional<UpscaleMode> parseUpscaleMode(std::string_view value) noexcept;
[[nodiscard]] std::string_view toString(PresentationMode mode) noexcept;
[[nodiscard]] std::optional<PresentationMode> parsePresentationMode(std::string_view value) noexcept;
[[nodiscard]] std::string_view toString(FinalFilter filter) noexcept;
[[nodiscard]] std::optional<FinalFilter> parseFinalFilter(std::string_view value) noexcept;
[[nodiscard]] std::string_view toString(ChromaUpscaleMode mode) noexcept;
[[nodiscard]] std::string_view displayName(ChromaUpscaleMode mode) noexcept;
[[nodiscard]] std::optional<ChromaUpscaleMode> parseChromaUpscaleMode(std::string_view value) noexcept;
[[nodiscard]] bool validSharpness(float value) noexcept;
[[nodiscard]] double mapOutputPixelCenter(std::uint32_t pixel, std::uint32_t sourceExtent, std::uint32_t outputExtent) noexcept;
[[nodiscard]] double leftSitedChromaCoordinate(std::uint32_t lumaPixel, bool horizontal) noexcept;
[[nodiscard]] double catmullRomWeight(double distance) noexcept;
[[nodiscard]] double lanczos2Weight(double distance) noexcept;
[[nodiscard]] PresentationGeometry calculatePresentationGeometry(std::uint32_t reconstructionWidth, std::uint32_t reconstructionHeight,
    std::uint32_t framebufferWidth, std::uint32_t framebufferHeight, PresentationMode mode) noexcept;

} // namespace ns60








