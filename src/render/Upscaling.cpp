#include "render/Upscaling.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace ns60 {
namespace {
std::uint32_t roundedExtent(double value) noexcept {
    if (!std::isfinite(value) || value <= 0.0) return 0;
    return static_cast<std::uint32_t>(std::max(1.0, std::round(value)));
}

PresentationGeometry scaledGeometry(std::uint32_t rw, std::uint32_t rh, std::uint32_t fw, std::uint32_t fh,
                                    PresentationMode requested, PresentationMode effective, double scale) noexcept {
    PresentationGeometry out{};
    out.reconstructionWidth = rw;
    out.reconstructionHeight = rh;
    out.framebufferWidth = fw;
    out.framebufferHeight = fh;
    out.requestedMode = requested;
    out.effectiveMode = effective;
    out.viewportWidth = std::min(fw, roundedExtent(static_cast<double>(rw) * scale));
    out.viewportHeight = std::min(fh, roundedExtent(static_cast<double>(rh) * scale));
    if (effective == PresentationMode::Fill) {
        out.viewportWidth = roundedExtent(static_cast<double>(rw) * scale);
        out.viewportHeight = roundedExtent(static_cast<double>(rh) * scale);
    }
    out.viewportX = (static_cast<std::int32_t>(fw) - static_cast<std::int32_t>(out.viewportWidth)) / 2;
    out.viewportY = (static_cast<std::int32_t>(fh) - static_cast<std::int32_t>(out.viewportHeight)) / 2;
    out.finalResampleActive = out.viewportWidth != rw || out.viewportHeight != rh;
    out.exactOneToOne = !out.finalResampleActive;
    return out;
}
} // namespace

std::string_view toString(UpscaleMode mode) noexcept {
    switch (mode) {
    case UpscaleMode::Nearest: return "nearest"; case UpscaleMode::Bilinear: return "bilinear";
    case UpscaleMode::BicubicCatmullRom: return "bicubic"; case UpscaleMode::Lanczos2: return "lanczos2";
    case UpscaleMode::BilinearCas: return "bilinear-cas"; case UpscaleMode::Lanczos2Cas: return "lanczos2-cas";
    case UpscaleMode::Fsr1Easu: return "fsr1-easu"; case UpscaleMode::Fsr1EasuRcas: return "fsr1-easu-rcas";
    }
    return "unknown";
}

std::string_view displayName(UpscaleMode mode) noexcept {
    switch (mode) {
    case UpscaleMode::Nearest: return "Nearest"; case UpscaleMode::Bilinear: return "Bilinear";
    case UpscaleMode::BicubicCatmullRom: return "Bicubic Catmull-Rom"; case UpscaleMode::Lanczos2: return "Lanczos2";
    case UpscaleMode::BilinearCas: return "Bilinear + FidelityFX CAS"; case UpscaleMode::Lanczos2Cas: return "Lanczos2 + FidelityFX CAS";
    case UpscaleMode::Fsr1Easu: return "FidelityFX FSR1 EASU"; case UpscaleMode::Fsr1EasuRcas: return "FidelityFX FSR1 EASU + RCAS";
    }
    return "Unknown";
}

std::optional<UpscaleMode> parseUpscaleMode(std::string_view value) noexcept {
    constexpr std::array modes{UpscaleMode::Nearest, UpscaleMode::Bilinear, UpscaleMode::BicubicCatmullRom,
        UpscaleMode::Lanczos2, UpscaleMode::BilinearCas, UpscaleMode::Lanczos2Cas, UpscaleMode::Fsr1Easu, UpscaleMode::Fsr1EasuRcas};
    for (const auto mode : modes) if (toString(mode) == value) return mode;
    if (value == "catmull-rom") return UpscaleMode::BicubicCatmullRom;
    return std::nullopt;
}

std::string_view toString(PresentationMode mode) noexcept {
    switch (mode) { case PresentationMode::Exact: return "exact"; case PresentationMode::Fit: return "fit";
    case PresentationMode::Fill: return "fill"; case PresentationMode::Integer: return "integer"; }
    return "unknown";
}

std::optional<PresentationMode> parsePresentationMode(std::string_view value) noexcept {
    if (value == "exact") return PresentationMode::Exact; if (value == "fit") return PresentationMode::Fit;
    if (value == "fill") return PresentationMode::Fill; if (value == "integer") return PresentationMode::Integer;
    return std::nullopt;
}

std::string_view toString(FinalFilter filter) noexcept {
    switch (filter) { case FinalFilter::Nearest: return "nearest"; case FinalFilter::Bilinear: return "bilinear"; }
    return "unknown";
}

std::optional<FinalFilter> parseFinalFilter(std::string_view value) noexcept {
    if (value == "nearest") return FinalFilter::Nearest;
    if (value == "bilinear") return FinalFilter::Bilinear;
    return std::nullopt;
}

std::string_view toString(ChromaUpscaleMode mode) noexcept {
    switch (mode) {
    case ChromaUpscaleMode::Bilinear: return "bilinear";
    case ChromaUpscaleMode::BicubicCatmullRom: return "bicubic";
    case ChromaUpscaleMode::Lanczos2: return "lanczos2";
    case ChromaUpscaleMode::EdgeAware: return "edge-aware";
    }
    return "unknown";
}

std::string_view displayName(ChromaUpscaleMode mode) noexcept {
    switch (mode) {
    case ChromaUpscaleMode::Bilinear: return "Bilinear";
    case ChromaUpscaleMode::BicubicCatmullRom: return "Bicubic Catmull-Rom";
    case ChromaUpscaleMode::Lanczos2: return "Lanczos2";
    case ChromaUpscaleMode::EdgeAware: return "Edge-aware";
    }
    return "Unknown";
}

std::optional<ChromaUpscaleMode> parseChromaUpscaleMode(std::string_view value) noexcept {
    if (value == "bilinear") return ChromaUpscaleMode::Bilinear;
    if (value == "bicubic" || value == "catmull-rom") return ChromaUpscaleMode::BicubicCatmullRom;
    if (value == "lanczos2") return ChromaUpscaleMode::Lanczos2;
    if (value == "edge-aware") return ChromaUpscaleMode::EdgeAware;
    return std::nullopt;
}

bool validSharpness(float value) noexcept { return std::isfinite(value) && value >= 0.0F && value <= 1.0F; }

double mapOutputPixelCenter(std::uint32_t pixel, std::uint32_t sourceExtent, std::uint32_t outputExtent) noexcept {
    if (!outputExtent) return 0.0;
    return (static_cast<double>(pixel) + 0.5) * static_cast<double>(sourceExtent) / static_cast<double>(outputExtent) - 0.5;
}

double leftSitedChromaCoordinate(std::uint32_t lumaPixel, bool horizontal) noexcept {
    const double p = static_cast<double>(lumaPixel);
    return horizontal ? p * 0.5 : (p - 0.5) * 0.5;
}

double catmullRomWeight(double distance) noexcept {
    const double x = std::abs(distance); if (x < 1.0) return 1.5*x*x*x-2.5*x*x+1.0;
    if (x < 2.0) return -0.5*x*x*x+2.5*x*x-4.0*x+2.0; return 0.0;
}

double lanczos2Weight(double distance) noexcept {
    const double x = std::abs(distance); if (x < 1.0e-12) return 1.0; if (x >= 2.0) return 0.0;
    const double pix = std::numbers::pi * x; return (std::sin(pix)/pix) * (std::sin(pix*0.5)/(pix*0.5));
}

PresentationGeometry calculatePresentationGeometry(std::uint32_t rw, std::uint32_t rh, std::uint32_t fw, std::uint32_t fh, PresentationMode mode) noexcept {
    PresentationGeometry out{};
    out.reconstructionWidth = rw;
    out.reconstructionHeight = rh;
    out.framebufferWidth = fw;
    out.framebufferHeight = fh;
    out.requestedMode = mode;
    out.effectiveMode = mode;
    if (!rw || !rh || !fw || !fh) return out;

    const double sx = static_cast<double>(fw) / static_cast<double>(rw);
    const double sy = static_cast<double>(fh) / static_cast<double>(rh);
    if (mode == PresentationMode::Exact) {
        if (rw <= fw && rh <= fh) return scaledGeometry(rw, rh, fw, fh, mode, mode, 1.0);
        return scaledGeometry(rw, rh, fw, fh, mode, PresentationMode::Fit, std::min(sx, sy));
    }
    if (mode == PresentationMode::Fit) return scaledGeometry(rw, rh, fw, fh, mode, mode, std::min(sx, sy));
    if (mode == PresentationMode::Fill) return scaledGeometry(rw, rh, fw, fh, mode, mode, std::max(sx, sy));

    const double fit = std::min(sx, sy);
    return scaledGeometry(rw, rh, fw, fh, mode, mode, fit >= 1.0 ? std::floor(fit) : fit);
}

} // namespace ns60