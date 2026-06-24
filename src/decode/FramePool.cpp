#include "decode/FramePool.h"

#include <stdexcept>

namespace ns60 {

FramePool::FramePool(std::size_t slotCount, int width, int height) : slots_(slotCount) {
    if (slotCount == 0 || width <= 0 || height <= 0 || (width & 1) != 0 || (height & 1) != 0) {
        throw std::invalid_argument("FramePool requires non-zero slots and positive even dimensions");
    }
    const auto ySize = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    const auto chromaSize = static_cast<std::size_t>(width / 2) * static_cast<std::size_t>(height / 2);
    for (auto& slot : slots_) {
        slot.yPlane.resize(ySize);
        slot.uPlane.resize(chromaSize);
        slot.vPlane.resize(chromaSize);
        slot.yStride = width;
        slot.uStride = width / 2;
        slot.vStride = width / 2;
        slot.metadata.width = width;
        slot.metadata.height = height;
    }
}

} // namespace ns60

