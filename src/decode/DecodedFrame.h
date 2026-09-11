#pragma once

#include "utility/ColorMath.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace ns60 {

enum class FrameTimingKind { ContainerPts, LiveArrival };
enum class DecodedFrameStorage { CpuYuv420P, CpuNv12 };

[[nodiscard]] inline std::string_view toString(DecodedFrameStorage storage) noexcept {
    switch (storage) {
    case DecodedFrameStorage::CpuYuv420P: return "CPU YUV420P";
    case DecodedFrameStorage::CpuNv12: return "CPU NV12";
    }
    return "unknown";
}

struct VideoFrameMetadata {
    int width{};
    int height{};
    std::int64_t sourcePts{};
    double ptsSeconds{};
    double durationSeconds{};
    std::uint64_t frameNumber{};
    bool keyFrame{};
    FrameTimingKind timingKind{FrameTimingKind::ContainerPts};
    ColorDescription color{};
    DecodedFrameStorage storage{DecodedFrameStorage::CpuYuv420P};
};

struct Yuv420FrameSlot {
    VideoFrameMetadata metadata{};
    DecodedFrameStorage storage{DecodedFrameStorage::CpuYuv420P};
    std::vector<std::byte> yPlane;
    // Planar YUV420P uses uPlane as U. NV12 uses uPlane as interleaved UV.
    std::vector<std::byte> uPlane;
    std::vector<std::byte> vPlane;
    int yStride{};
    int uStride{};
    int vStride{};
};

} // namespace ns60
