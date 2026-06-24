#include "telemetry/Metrics.h"

namespace ns60 {

double fpsFromAverageInterval(const RollingMetric<>& intervals) noexcept {
    const double averageMs = intervals.average();
    return averageMs > 0.0 ? 1000.0 / averageMs : 0.0;
}

double fpsFromLatestInterval(const RollingMetric<>& intervals) noexcept {
    const double latestMs = intervals.latest();
    return latestMs > 0.0 ? 1000.0 / latestMs : 0.0;
}

double fpsFromNewestIntervalWindow(const RollingMetric<>& intervals, double windowMs) noexcept {
    if (intervals.count() == 0 || windowMs <= 0.0) return 0.0;
    double accumulatedMs{};
    std::size_t samples{};
    for (; samples < intervals.count(); ++samples) {
        accumulatedMs += intervals.newest(samples);
        if (accumulatedMs >= windowMs) break;
    }
    return accumulatedMs > 0.0 ? static_cast<double>(samples + 1) * 1000.0 / accumulatedMs : 0.0;
}

} // namespace ns60