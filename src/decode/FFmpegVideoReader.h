#pragma once

#include "decode/DecodedFrame.h"
#include "decode/DecoderBackend.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace ns60 {

struct SysDvrPipeInput {
    std::string pipeName;
};

struct VideoStreamInfo {
    std::string codecName;
    std::string pixelFormatName;
    DecoderBackend requestedDecoderBackend{DecoderBackend::Software};
    DecoderBackend activeDecoderBackend{DecoderBackend::Software};
    DecoderPath activeDecoderPath{DecoderPath::Readback};
    int width{};
    int height{};
    double declaredFrameRate{};
    double averageFrameRate{};
    double durationSeconds{};
    std::int64_t bitRate{};
    bool live{};
    ColorDescription color{};
    std::string transfer;
    std::string chromaLocation;
};

struct DecodeTiming {
    double decodeMs{};
    double copyMs{};
    std::uint64_t copyBytes{};
};

enum class ReadFrameResult { Frame, EndOfFile };

class FFmpegVideoReader final {
public:
    explicit FFmpegVideoReader(const std::filesystem::path& path, DecoderBackend backend = DecoderBackend::Software,
                               DecoderPath pathMode = DecoderPath::Readback,
                               std::size_t externallyRetainedFrames = 0);
    explicit FFmpegVideoReader(SysDvrPipeInput input, DecoderBackend backend = DecoderBackend::Software,
                               DecoderPath pathMode = DecoderPath::Readback,
                               std::size_t externallyRetainedFrames = 0);
    ~FFmpegVideoReader();
    FFmpegVideoReader(const FFmpegVideoReader&) = delete;
    FFmpegVideoReader& operator=(const FFmpegVideoReader&) = delete;
    FFmpegVideoReader(FFmpegVideoReader&&) noexcept;
    FFmpegVideoReader& operator=(FFmpegVideoReader&&) noexcept;

    [[nodiscard]] const VideoStreamInfo& info() const noexcept;
    [[nodiscard]] std::optional<AdapterLuid> interopAdapterLuid() const noexcept;
    void releaseInteropResources() noexcept;
    [[nodiscard]] ReadFrameResult readFrame(Yuv420FrameSlot& destination, DecodeTiming& timing);
    void seekToBeginning();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ns60
