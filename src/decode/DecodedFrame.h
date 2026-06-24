#pragma once

#include "utility/ColorMath.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ns60 {

struct VideoFrameMetadata {
    int width{};
    int height{};
    std::int64_t sourcePts{};
    double ptsSeconds{};
    double durationSeconds{};
    std::uint64_t frameNumber{};
    bool keyFrame{};
    ColorDescription color{};
};

struct Yuv420FrameSlot {
    VideoFrameMetadata metadata{};
    std::vector<std::byte> yPlane;
    std::vector<std::byte> uPlane;
    std::vector<std::byte> vPlane;
    int yStride{};
    int uStride{};
    int vStride{};
};

} // namespace ns60

