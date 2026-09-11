#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "decode/FramePool.h"
#include "decode/FrameQueue.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

using namespace std::chrono_literals;

namespace {
class TestLease final : public ns60::D3D11FrameLease {
public:
    explicit TestLease(std::shared_ptr<int> lifetime) : lifetime_(std::move(lifetime)) {}
    [[nodiscard]] const ns60::D3D11TextureDescription& description() const noexcept override { return description_; }
    [[nodiscard]] ns60::AdapterLuid adapterLuid() const noexcept override { return {}; }
    [[nodiscard]] std::uintptr_t fenceIdentity() const noexcept override { return 1; }
    [[nodiscard]] ns60::PlatformHandle createTextureHandle() const override { return {}; }
    [[nodiscard]] ns60::PlatformHandle createFenceHandle() const override { return {}; }
private:
    ns60::D3D11TextureDescription description_{};
    std::shared_ptr<int> lifetime_;
};
}

static_assert(!std::is_copy_constructible_v<ns60::DecodedFrame>);
static_assert(std::is_nothrow_move_constructible_v<ns60::DecodedFrame>);

TEST_CASE("FrameQueue preserves order, wraps, and reuses slots") {
    ns60::FramePool pool(4, 16, 16);
    ns60::FrameQueue queue(pool);
    for (std::uint64_t cycle = 0; cycle < 12; ++cycle) {
        const auto write = queue.acquireWrite();
        REQUIRE(write);
        pool.at(*write).metadata.frameNumber = cycle;
        queue.commitWrite(*write);
        const auto read = queue.acquireRead();
        REQUIRE(read);
        CHECK(pool.at(*read).metadata.frameNumber == cycle);
        queue.releaseRead(*read);
    }
    CHECK(queue.occupancy() == 0);
    CHECK(queue.highWaterMark() == 1);
}

TEST_CASE("FrameQueue full queue blocks producer and never overwrites unread slots") {
    ns60::FramePool pool(4, 16, 16);
    ns60::FrameQueue queue(pool);
    std::vector<std::size_t> written;
    for (std::uint64_t i = 0; i < 4; ++i) {
        const auto slot = queue.acquireWrite();
        REQUIRE(slot);
        pool.at(*slot).metadata.frameNumber = i;
        written.push_back(*slot);
        queue.commitWrite(*slot);
    }
    CHECK(queue.occupancy() == 4);
    auto blocked = std::async(std::launch::async, [&queue] { return queue.acquireWrite(); });
    CHECK(blocked.wait_for(30ms) == std::future_status::timeout);

    const auto read = queue.acquireRead();
    REQUIRE(read);
    CHECK(pool.at(*read).metadata.frameNumber == 0);
    queue.releaseRead(*read);
    CHECK(blocked.wait_for(500ms) == std::future_status::ready);
    const auto reused = blocked.get();
    REQUIRE(reused);
    CHECK(*reused == written.front());
    queue.cancelWrite(*reused);
}

TEST_CASE("FrameQueue empty consumer wakes on push") {
    ns60::FramePool pool(4, 16, 16);
    ns60::FrameQueue queue(pool);
    auto waiting = std::async(std::launch::async, [&queue] { return queue.acquireRead(); });
    CHECK(waiting.wait_for(30ms) == std::future_status::timeout);
    const auto write = queue.acquireWrite();
    REQUIRE(write);
    queue.commitWrite(*write);
    CHECK(waiting.wait_for(500ms) == std::future_status::ready);
    const auto read = waiting.get();
    REQUIRE(read);
    queue.releaseRead(*read);
}

TEST_CASE("FrameQueue stop wakes producer and consumer") {
    ns60::FramePool producerPool(1, 16, 16);
    ns60::FrameQueue producerQueue(producerPool);
    const auto write = producerQueue.acquireWrite();
    REQUIRE(write);
    producerQueue.commitWrite(*write);
    auto producer = std::async(std::launch::async, [&producerQueue] { return producerQueue.acquireWrite(); });
    producerQueue.stop();
    CHECK_FALSE(producer.get().has_value());

    ns60::FramePool consumerPool(1, 16, 16);
    ns60::FrameQueue consumerQueue(consumerPool);
    auto consumer = std::async(std::launch::async, [&consumerQueue] { return consumerQueue.acquireRead(); });
    consumerQueue.stop();
    CHECK_FALSE(consumer.get().has_value());
}


TEST_CASE("FrameQueue latest policy drops oldest unpublished frame when full") {
    ns60::FramePool pool(2, 16, 16);
    ns60::FrameQueue queue(pool);

    auto first = queue.acquireWriteLatest();
    REQUIRE(first);
    pool.at(*first).metadata.frameNumber = 1;
    queue.commitWrite(*first);

    auto second = queue.acquireWriteLatest();
    REQUIRE(second);
    pool.at(*second).metadata.frameNumber = 2;
    queue.commitWrite(*second);

    auto replacement = queue.acquireWriteLatest();
    REQUIRE(replacement);
    pool.at(*replacement).metadata.frameNumber = 3;
    queue.commitWrite(*replacement);

    CHECK(queue.staleDropCount() == 1);
    auto read = queue.acquireRead();
    REQUIRE(read);
    CHECK(pool.at(*read).metadata.frameNumber == 2);
    queue.releaseRead(*read);

    read = queue.acquireRead();
    REQUIRE(read);
    CHECK(pool.at(*read).metadata.frameNumber == 3);
    queue.releaseRead(*read);
}


TEST_CASE("FrameQueue live consumer acquires newest and releases older ready slots") {
    ns60::FramePool pool(3, 16, 16);
    ns60::FrameQueue queue(pool);

    for (std::uint64_t frame = 1; frame <= 3; ++frame) {
        auto write = queue.acquireWrite();
        REQUIRE(write);
        pool.at(*write).metadata.frameNumber = frame;
        queue.commitWrite(*write);
    }

    auto newest = queue.tryAcquireNewest();
    REQUIRE(newest);
    CHECK(pool.at(*newest).metadata.frameNumber == 3);
    CHECK(queue.occupancy() == 0);
    CHECK(queue.staleDropCount() == 2);
    queue.releaseRead(*newest);

    for (std::uint64_t frame = 4; frame <= 6; ++frame) {
        auto write = queue.acquireWrite();
        REQUIRE(write);
        pool.at(*write).metadata.frameNumber = frame;
        queue.commitWrite(*write);
    }

    newest = queue.tryAcquireNewest();
    REQUIRE(newest);
    CHECK(pool.at(*newest).metadata.frameNumber == 6);
    CHECK(queue.occupancy() == 0);
    CHECK(queue.staleDropCount() == 4);
    queue.releaseRead(*newest);
}

TEST_CASE("FrameQueue live newest read is non-blocking when empty") {
    ns60::FramePool pool(2, 16, 16);
    ns60::FrameQueue queue(pool);
    CHECK_FALSE(queue.tryAcquireNewest().has_value());

    auto write = queue.acquireWriteLatest();
    REQUIRE(write);
    pool.at(*write).metadata.frameNumber = 10;
    queue.commitWrite(*write);

    auto newest = queue.tryAcquireNewest();
    REQUIRE(newest);
    CHECK(pool.at(*newest).metadata.frameNumber == 10);
    queue.releaseRead(*newest);
    CHECK_FALSE(queue.tryAcquireNewest().has_value());
}


TEST_CASE("FrameQueue latest policy supports depth one replacement") {
    ns60::FramePool pool(1, 16, 16);
    ns60::FrameQueue queue(pool);

    auto first = queue.acquireWriteLatest();
    REQUIRE(first);
    pool.at(*first).metadata.frameNumber = 1;
    queue.commitWrite(*first);

    auto second = queue.acquireWriteLatest();
    REQUIRE(second);
    CHECK(*second == *first);
    pool.at(*second).metadata.frameNumber = 2;
    queue.commitWrite(*second);

    CHECK(queue.staleDropCount() == 1);
    auto read = queue.tryAcquireNewest();
    REQUIRE(read);
    CHECK(pool.at(*read).metadata.frameNumber == 2);
    queue.releaseRead(*read);
}
TEST_CASE("FramePool allocates NV12-capable chroma storage") {
    ns60::FramePool pool(1, 16, 16);
    auto& slot = pool.at(0);
    CHECK(slot.yPlane.size() == 16 * 16);
    CHECK(slot.uPlane.size() >= 16 * 16 / 2);
    CHECK(slot.vPlane.size() == 8 * 8);
    CHECK(static_cast<int>(slot.storage) == static_cast<int>(ns60::DecodedFrameStorage::CpuYuv420P));
    CHECK(static_cast<int>(slot.metadata.storage) == static_cast<int>(ns60::DecodedFrameStorage::CpuYuv420P));
}

TEST_CASE("Interop frame pools can omit all CPU plane allocations") {
    ns60::FramePool pool(2, 16, 16, false);
    CHECK(pool.at(0).yPlane.empty());
    CHECK(pool.at(0).uPlane.empty());
    CHECK(pool.at(0).vPlane.empty());
}

TEST_CASE("D3D11 interop pool includes every externally retained surface") {
    CHECK(ns60::d3d11InteropPoolSize(12, 4 + 3 + 1 + 1) == 21);
    CHECK_THROWS_AS((void)ns60::d3d11InteropPoolSize(2040, 9), std::runtime_error);
}

TEST_CASE("Adapter LUID matching requires two valid identical identifiers") {
    ns60::AdapterLuid first{{1, 2, 3, 4, 5, 6, 7, 8}, true};
    ns60::AdapterLuid same = first;
    ns60::AdapterLuid different{{1, 2, 3, 4, 5, 6, 7, 9}, true};
    ns60::AdapterLuid invalid = first;
    invalid.valid = false;
    CHECK(ns60::adapterLuidsMatch(first, same));
    CHECK_FALSE(ns60::adapterLuidsMatch(first, different));
    CHECK_FALSE(ns60::adapterLuidsMatch(first, invalid));
}

TEST_CASE("Interop cache keys distinguish pool generations and array slices") {
    const ns60::ExternalTextureKey first{2, 0x1234};
    CHECK(first == ns60::ExternalTextureKey{2, 0x1234});
    CHECK_FALSE(first == ns60::ExternalTextureKey{3, 0x1234});
    CHECK_FALSE(ns60::ExternalViewKey{first, 1} == ns60::ExternalViewKey{first, 2});
}

TEST_CASE("Cancelled and stopped interop leases release immediately") {
    ns60::FramePool pool(1, 16, 16, false);
    ns60::FrameQueue queue(pool);
    auto lifetime = std::make_shared<int>(7);
    std::weak_ptr<int> observed = lifetime;

    auto write = queue.acquireWriteLatest();
    REQUIRE(write);
    pool.at(*write).storage = ns60::DecodedFrameStorage::D3D11Nv12;
    pool.at(*write).d3d11Lease = std::make_shared<TestLease>(lifetime);
    lifetime.reset();
    queue.cancelWrite(*write);
    CHECK(observed.expired());

    lifetime = std::make_shared<int>(8);
    observed = lifetime;
    write = queue.acquireWriteLatest();
    REQUIRE(write);
    pool.at(*write).d3d11Lease = std::make_shared<TestLease>(lifetime);
    lifetime.reset();
    queue.commitWrite(*write);
    queue.stop();
    CHECK(observed.expired());
}

TEST_CASE("Displayed and flight owners retain an interop lease until the final release") {
    ns60::FramePool pool(1, 16, 16, false);
    ns60::FrameQueue queue(pool);
    auto lifetime = std::make_shared<int>(9);
    std::weak_ptr<int> observed = lifetime;

    const auto write = queue.acquireWrite();
    REQUIRE(write);
    pool.at(*write).storage = ns60::DecodedFrameStorage::D3D11Nv12;
    pool.at(*write).d3d11Lease = std::make_shared<TestLease>(lifetime);
    lifetime.reset();
    queue.commitWrite(*write);

    const auto read = queue.acquireRead();
    REQUIRE(read);
    auto displayed = pool.at(*read).d3d11Lease;
    auto flight = displayed;
    queue.releaseRead(*read);
    CHECK_FALSE(observed.expired());
    displayed.reset();
    CHECK_FALSE(observed.expired());
    flight.reset();
    CHECK(observed.expired());
}

TEST_CASE("Discarding queued interop frames releases their leases") {
    ns60::FramePool pool(1, 16, 16, false);
    ns60::FrameQueue queue(pool);
    auto lifetime = std::make_shared<int>(10);
    std::weak_ptr<int> observed = lifetime;

    const auto write = queue.acquireWrite();
    REQUIRE(write);
    pool.at(*write).d3d11Lease = std::make_shared<TestLease>(lifetime);
    lifetime.reset();
    queue.commitWrite(*write);
    queue.discardReady();
    CHECK(observed.expired());
}
