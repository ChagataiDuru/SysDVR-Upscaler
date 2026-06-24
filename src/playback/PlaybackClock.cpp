#include "playback/PlaybackClock.h"

#include <stdexcept>

namespace ns60 {

void PlaybackClock::start(double firstPtsSeconds, TimePoint now) noexcept {
    firstPts_ = firstPtsSeconds;
    wallStart_ = now;
    pauseStarted_.reset();
}

void PlaybackClock::reset() noexcept {
    firstPts_.reset();
    pauseStarted_.reset();
}

void PlaybackClock::pause(TimePoint now) noexcept {
    if (firstPts_ && !pauseStarted_) pauseStarted_ = now;
}

void PlaybackClock::resume(TimePoint now) noexcept {
    if (!pauseStarted_) return;
    wallStart_ += now - *pauseStarted_;
    pauseStarted_.reset();
}

PlaybackClock::TimePoint PlaybackClock::targetTime(double ptsSeconds) const {
    if (!firstPts_) throw std::logic_error("PlaybackClock target requested before start");
    return wallStart_ + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(ptsSeconds - *firstPts_));
}

double PlaybackClock::latenessSeconds(double ptsSeconds, TimePoint now) const {
    const auto effectiveNow = pauseStarted_.value_or(now);
    return std::chrono::duration<double>(effectiveNow - targetTime(ptsSeconds)).count();
}

double PlaybackClock::driftSeconds(double ptsSeconds, TimePoint now) const {
    return latenessSeconds(ptsSeconds, now);
}

} // namespace ns60

