#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "decode/FramePool.h"
#include "decode/FrameQueue.h"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

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
