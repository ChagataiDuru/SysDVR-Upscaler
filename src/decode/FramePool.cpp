#include "decode/FramePool.h"

#include <algorithm>
#include <stdexcept>

namespace ns60 {

FramePool::FramePool(std::size_t slotCount, int width, int height) : slots_(slotCount) {
    if (slotCount == 0 || width <= 0 || height <= 0 || (width & 1) != 0 || (height & 1) != 0) {
        throw std::invalid_argument("FramePool requires non-zero slots and positive even dimensions");
    }
    const auto ySize = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    const auto chromaSize = static_cast<std::size_t>(width / 2) * static_cast<std::size_t>(height / 2);
    const auto nv12ChromaSize = ySize / 2;
    for (auto& slot : slots_) {
        slot.yPlane.resize(ySize);
        slot.uPlane.resize(std::max(chromaSize, nv12ChromaSize));
        slot.vPlane.resize(chromaSize);
        slot.yStride = width;
        slot.uStride = width / 2;
        slot.vStride = width / 2;
        slot.storage = DecodedFrameStorage::CpuYuv420P;
        slot.metadata.width = width;
        slot.metadata.height = height;
        slot.metadata.storage = slot.storage;
    }
}

} // namespace ns60
