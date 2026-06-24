#include <doctest/doctest.h>

#include "playback/PlaybackClock.h"

#include <chrono>

using namespace std::chrono_literals;

TEST_CASE("PlaybackClock handles a non-zero first PTS and irregular spacing") {
    ns60::PlaybackClock clock;
    const ns60::PlaybackClock::TimePoint start{10s};
    clock.start(42.5, start);
    CHECK(clock.targetTime(42.5) == start);
    CHECK(clock.targetTime(42.517) == start + 17ms);
    CHECK(clock.targetTime(42.551) == start + 51ms);
    CHECK(clock.latenessSeconds(42.517, start + 19ms) == doctest::Approx(0.002).epsilon(1e-6));
}

TEST_CASE("PlaybackClock pause and resume shifts the wall timeline") {
    ns60::PlaybackClock clock;
    const ns60::PlaybackClock::TimePoint start{10s};
    clock.start(3.0, start);
    clock.pause(start + 100ms);
    CHECK(clock.paused());
    CHECK(clock.latenessSeconds(3.1, start + 5s) == doctest::Approx(0.0).epsilon(1e-6));
    clock.resume(start + 600ms);
    CHECK_FALSE(clock.paused());
    CHECK(clock.targetTime(3.2) == start + 700ms);
}

TEST_CASE("PlaybackClock reset supports seek and loop restart") {
    ns60::PlaybackClock clock;
    const ns60::PlaybackClock::TimePoint start{10s};
    clock.start(5.0, start);
    clock.reset();
    CHECK_FALSE(clock.started());
    clock.start(1.25, start + 2s);
    CHECK(clock.targetTime(1.25) == start + 2s);
}

