#pragma once

#include "decode/DecodedFrame.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace ns60 {

struct SysDvrPipeInput {
    std::string pipeName;
};

struct VideoStreamInfo {
    std::string codecName;
    std::string pixelFormatName;
    int width{};
    int height{};
    double declaredFrameRate{};
    double averageFrameRate{};
    double durationSeconds{};
    std::int64_t bitRate{};
    ColorDescription color{};
    std::string transfer;
    std::string chromaLocation;
};

struct DecodeTiming {
    double decodeMs{};
    double copyMs{};
};

enum class ReadFrameResult { Frame, EndOfFile };

class FFmpegVideoReader final {
public:
    explicit FFmpegVideoReader(const std::filesystem::path& path);
    explicit FFmpegVideoReader(SysDvrPipeInput input);
    ~FFmpegVideoReader();
    FFmpegVideoReader(const FFmpegVideoReader&) = delete;
    FFmpegVideoReader& operator=(const FFmpegVideoReader&) = delete;
    FFmpegVideoReader(FFmpegVideoReader&&) noexcept;
    FFmpegVideoReader& operator=(FFmpegVideoReader&&) noexcept;

    [[nodiscard]] const VideoStreamInfo& info() const noexcept;
    [[nodiscard]] ReadFrameResult readFrame(Yuv420FrameSlot& destination, DecodeTiming& timing);
    void seekToBeginning();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ns60
