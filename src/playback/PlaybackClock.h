#pragma once

#include <chrono>
#include <optional>

namespace ns60 {

class PlaybackClock final {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    void start(double firstPtsSeconds, TimePoint now = Clock::now()) noexcept;
    void reset() noexcept;
    void pause(TimePoint now = Clock::now()) noexcept;
    void resume(TimePoint now = Clock::now()) noexcept;
    [[nodiscard]] bool started() const noexcept { return firstPts_.has_value(); }
    [[nodiscard]] bool paused() const noexcept { return pauseStarted_.has_value(); }
    [[nodiscard]] TimePoint targetTime(double ptsSeconds) const;
    [[nodiscard]] double latenessSeconds(double ptsSeconds, TimePoint now = Clock::now()) const;
    [[nodiscard]] double driftSeconds(double ptsSeconds, TimePoint now = Clock::now()) const;

private:
    std::optional<double> firstPts_;
    TimePoint wallStart_{};
    std::optional<TimePoint> pauseStarted_;
};

} // namespace ns60

