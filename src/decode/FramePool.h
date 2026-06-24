#pragma once

#include "decode/DecodedFrame.h"

#include <cstddef>
#include <span>
#include <vector>

namespace ns60 {

class FramePool final {
public:
    static constexpr std::size_t defaultSlotCount = 4;

    FramePool(std::size_t slotCount, int width, int height);
    [[nodiscard]] std::size_t size() const noexcept { return slots_.size(); }
    [[nodiscard]] Yuv420FrameSlot& at(std::size_t index) { return slots_.at(index); }
    [[nodiscard]] const Yuv420FrameSlot& at(std::size_t index) const { return slots_.at(index); }
    [[nodiscard]] std::span<Yuv420FrameSlot> slots() noexcept { return slots_; }

private:
    std::vector<Yuv420FrameSlot> slots_;
};

} // namespace ns60

