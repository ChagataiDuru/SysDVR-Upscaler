#pragma once

#include "render/Upscaling.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ns60 {

enum class LogLevel { Trace, Debug, Info, Warning, Error, Critical };
enum class SourceKind { File, SysDvrPipe, SysDvr };

struct AppConfig {
    SourceKind source{SourceKind::File};
    std::filesystem::path input;
    std::string pipeName{"SysDVR-Upscaler.Video"};
    std::filesystem::path sysdvrBridge;
    int outputWidth{1920};
    int outputHeight{1080};
    UpscaleMode upscale{UpscaleMode::Bilinear};
    SharpenSettings sharpen{};
    bool antiRinging{true};
    std::optional<UpscaleMode> comparisonA;
    std::optional<UpscaleMode> comparisonB;
    PresentationMode presentation{PresentationMode::Fit};
    bool presentationExplicit{};
    FinalFilter finalFilter{FinalFilter::Bilinear};
    ChromaUpscaleMode chromaUpscale{ChromaUpscaleMode::BicubicCatmullRom};
    std::optional<int> monitorIndex;
    bool loop{};
    bool fullscreen{};
    bool borderless{};
    bool vsync{true};
    bool validation{};
    bool dropLateFrames{};
    LogLevel logLevel{LogLevel::Info};
};

enum class ParseAction { Run, Help, Version };
struct ParseResult {
    ParseAction action{ParseAction::Run};
    std::optional<AppConfig> config;
    std::string error;
};

[[nodiscard]] ParseResult parseCommandLine(const std::vector<std::string>& args, bool defaultValidation);
[[nodiscard]] std::string commandLineHelp();
[[nodiscard]] const char* toString(LogLevel level) noexcept;
[[nodiscard]] std::string_view toString(SourceKind source) noexcept;

} // namespace ns60
