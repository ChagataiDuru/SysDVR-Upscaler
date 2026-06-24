#pragma once

#include <string>

namespace ns60 {

struct FFmpegVersions {
    std::string avformat;
    std::string avcodec;
    std::string avutil;
};

[[nodiscard]] FFmpegVersions ffmpegVersions();
[[nodiscard]] std::string ffmpegError(int errorCode);

} // namespace ns60

