#include "decode/FrameQueue.h"

#include <algorithm>
#include <stdexcept>

namespace ns60 {

FrameQueue::FrameQueue(FramePool& pool)
    : pool_(pool), readyRing_(pool.size()), states_(pool.size(), SlotState::Free) {
    if (pool.size() == 0) throw std::invalid_argument("FrameQueue requires a non-empty pool");
}

std::optional<std::size_t> FrameQueue::acquireWrite() {
    std::unique_lock lock(mutex_);
    writable_.wait(lock, [this] {
        return stopped_ || std::find(states_.begin(), states_.end(), SlotState::Free) != states_.end();
    });
    if (stopped_) return std::nullopt;
    const auto iterator = std::find(states_.begin(), states_.end(), SlotState::Free);
    const auto slot = static_cast<std::size_t>(std::distance(states_.begin(), iterator));
    states_[slot] = SlotState::Writing;
    return slot;
}

void FrameQueue::commitWrite(std::size_t slot) {
    {
        std::lock_guard lock(mutex_);
        if (slot >= states_.size() || states_[slot] != SlotState::Writing) {
            throw std::logic_error("commitWrite called for a slot not owned by the producer");
        }
        readyRing_[writePos_] = slot;
        writePos_ = (writePos_ + 1) % readyRing_.size();
        states_[slot] = SlotState::Ready;
        ++readyCount_;
        highWater_ = std::max(highWater_, readyCount_);
    }
    readable_.notify_one();
}

void FrameQueue::cancelWrite(std::size_t slot) {
    {
        std::lock_guard lock(mutex_);
        if (slot >= states_.size() || states_[slot] != SlotState::Writing) {
            throw std::logic_error("cancelWrite called for a slot not owned by the producer");
        }
        states_[slot] = SlotState::Free;
    }
    writable_.notify_one();
}

std::optional<std::size_t> FrameQueue::acquireRead() {
    std::unique_lock lock(mutex_);
    readable_.wait(lock, [this] { return stopped_ || readyCount_ != 0; });
    if (readyCount_ == 0) return std::nullopt;
    const auto slot = readyRing_[readPos_];
    readPos_ = (readPos_ + 1) % readyRing_.size();
    --readyCount_;
    if (states_[slot] != SlotState::Ready) throw std::logic_error("FrameQueue state corruption");
    states_[slot] = SlotState::Reading;
    return slot;
}

void FrameQueue::releaseRead(std::size_t slot) {
    {
        std::lock_guard lock(mutex_);
        if (slot >= states_.size() || states_[slot] != SlotState::Reading) {
            throw std::logic_error("releaseRead called for a slot not owned by the consumer");
        }
        states_[slot] = SlotState::Free;
    }
    writable_.notify_one();
}

void FrameQueue::stop() noexcept {
    {
        std::lock_guard lock(mutex_);
        stopped_ = true;
    }
    readable_.notify_all();
    writable_.notify_all();
}

bool FrameQueue::stopped() const noexcept { std::lock_guard lock(mutex_); return stopped_; }
std::size_t FrameQueue::occupancy() const noexcept { std::lock_guard lock(mutex_); return readyCount_; }
std::size_t FrameQueue::highWaterMark() const noexcept { std::lock_guard lock(mutex_); return highWater_; }

} // namespace ns60
