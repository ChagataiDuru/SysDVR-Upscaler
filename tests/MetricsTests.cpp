#include <doctest/doctest.h>

#include "telemetry/Metrics.h"

TEST_CASE("fixed history handles empty and single-sample percentiles") {
    ns60::RollingMetric<4> history;
    CHECK(history.average() == doctest::Approx(0.0));
    CHECK(history.percentile(0.95) == doctest::Approx(0.0));
    history.add(16.666);
    CHECK(history.latest() == doctest::Approx(16.666));
    CHECK(history.percentile(0.50) == doctest::Approx(16.666));
    CHECK(history.minimum() == doctest::Approx(16.666));
    CHECK(history.maximum() == doctest::Approx(16.666));
}

TEST_CASE("fixed history evicts oldest samples") {
    ns60::RollingMetric<3> history;
    history.add(10.0);
    history.add(20.0);
    history.add(30.0);
    history.add(40.0);
    CHECK(history.count() == 3);
    CHECK(history.minimum() == doctest::Approx(20.0));
    CHECK(history.maximum() == doctest::Approx(40.0));
    CHECK(history.newest(0) == doctest::Approx(40.0));
    CHECK(history.newest(2) == doctest::Approx(20.0));
}

TEST_CASE("stable 16.666 ms intervals report approximately 60 fps") {
    ns60::RollingMetric<> intervals;
    for (int i = 0; i < 120; ++i) intervals.add(1000.0 / 60.0);
    CHECK(ns60::fpsFromLatestInterval(intervals) == doctest::Approx(60.0).epsilon(0.001));
    CHECK(ns60::fpsFromAverageInterval(intervals) == doctest::Approx(60.0).epsilon(0.001));
    CHECK(ns60::fpsFromNewestIntervalWindow(intervals, 1000.0) == doctest::Approx(60.0).epsilon(0.02));
    CHECK(intervals.percentile(0.95) == doctest::Approx(1000.0 / 60.0));
}

TEST_CASE("mixed frame intervals produce ordered percentiles") {
    ns60::RollingMetric<8> intervals;
    for (double v : {10.0, 11.0, 12.0, 13.0, 20.0, 25.0, 30.0, 40.0}) intervals.add(v);
    CHECK(intervals.percentile(0.50) <= intervals.percentile(0.95));
    CHECK(intervals.percentile(0.95) <= intervals.percentile(0.99));
}