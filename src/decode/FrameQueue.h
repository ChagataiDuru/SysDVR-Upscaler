#pragma once

#include "decode/FramePool.h"

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <vector>

namespace ns60 {

// A bounded SPSC ownership queue. The mutex protects only index/state changes;
// decoding and rendering happen after the lock is released. Fixed storage means
// push/pop never allocate. This intentionally favors auditable correctness over a
// fragile lock-free implementation.
class FrameQueue final {
public:
    explicit FrameQueue(FramePool& pool);
    FrameQueue(const FrameQueue&) = delete;
    FrameQueue& operator=(const FrameQueue&) = delete;

    [[nodiscard]] std::optional<std::size_t> acquireWrite();
    [[nodiscard]] std::optional<std::size_t> acquireWriteLatest();
    void commitWrite(std::size_t slot);
    void cancelWrite(std::size_t slot);
    [[nodiscard]] std::optional<std::size_t> acquireRead();
    void releaseRead(std::size_t slot);
    void stop() noexcept;
    [[nodiscard]] bool stopped() const noexcept;
    [[nodiscard]] std::size_t occupancy() const noexcept;
    [[nodiscard]] std::size_t highWaterMark() const noexcept;
    [[nodiscard]] std::size_t staleDropCount() const noexcept;
    [[nodiscard]] FramePool& pool() noexcept { return pool_; }

private:
    enum class SlotState : unsigned char { Free, Writing, Ready, Reading };
    FramePool& pool_;
    mutable std::mutex mutex_;
    std::condition_variable readable_;
    std::condition_variable writable_;
    std::vector<std::size_t> readyRing_;
    std::vector<SlotState> states_;
    std::size_t readPos_{};
    std::size_t writePos_{};
    std::size_t readyCount_{};
    std::size_t highWater_{};
    std::size_t staleDrops_{};
    bool stopped_{};
};

} // namespace ns60

