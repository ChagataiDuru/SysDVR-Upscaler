#include "decode/FFmpegRuntime.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
}

#include <array>

namespace ns60 {
namespace {
std::string versionString(unsigned version) {
    return std::to_string(AV_VERSION_MAJOR(version)) + "." +
           std::to_string(AV_VERSION_MINOR(version)) + "." +
           std::to_string(AV_VERSION_MICRO(version));
}
} // namespace

FFmpegVersions ffmpegVersions() {
    return {versionString(avformat_version()), versionString(avcodec_version()), versionString(avutil_version())};
}

std::string ffmpegError(int errorCode) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    if (av_strerror(errorCode, buffer.data(), buffer.size()) < 0) return "FFmpeg error " + std::to_string(errorCode);
    return buffer.data();
}
} // namespace ns60

