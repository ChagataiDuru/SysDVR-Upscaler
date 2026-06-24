#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <limits>
#include <optional>

namespace ns60 {

template <typename T, std::size_t Capacity>
class FixedHistory final {
public:
    void add(T value) noexcept { values_[cursor_] = value; cursor_ = (cursor_ + 1) % Capacity; count_ = count_ < Capacity ? count_ + 1 : Capacity; }
    void clear() noexcept { values_ = {}; cursor_ = 0; count_ = 0; }
    [[nodiscard]] T latest() const noexcept { return count_ ? values_[(cursor_ + Capacity - 1) % Capacity] : T{}; }
    [[nodiscard]] T newest(std::size_t offset) const noexcept { return offset < count_ ? values_[(cursor_ + Capacity - 1 - offset) % Capacity] : T{}; }
    [[nodiscard]] T average() const noexcept;
    [[nodiscard]] T minimum() const noexcept;
    [[nodiscard]] T maximum() const noexcept;
    [[nodiscard]] T percentile(double p) const noexcept;
    [[nodiscard]] const std::array<T, Capacity>& data() const noexcept { return values_; }
    [[nodiscard]] std::size_t count() const noexcept { return count_; }
private:
    std::array<T, Capacity> values_{};
    std::size_t cursor_{};
    std::size_t count_{};
};

template <std::size_t Capacity = 240>
using RollingMetric = FixedHistory<double, Capacity>;

enum class GpuPass : std::size_t { Nearest, Bilinear, Bicubic, Lanczos2, Cas, Easu, Rcas, Count };
struct GpuTimings { double uploadMs{},colorMs{};std::array<std::optional<double>,static_cast<std::size_t>(GpuPass::Count)> passes{};double reconstructionMs{},postProcessingMs{},presentMs{},totalMs{}; };
struct Metrics {
 RollingMetric<> decodeMs,planeCopyMs,decoderWaitMs,cpuFrameMs,driftMs,latenessMs,ptsDeltaMs,decodedFps,presentedFps;
 RollingMetric<> activeFrameTimeMs,presentSubmissionIntervalMs;
 RollingMetric<> gpuUploadMs,gpuColorMs;std::array<RollingMetric<>,static_cast<std::size_t>(GpuPass::Count)> gpuPassMs;
 RollingMetric<> gpuReconstructionMs,gpuPostProcessingMs,gpuPresentMs,gpuTotalMs;
 std::uint64_t decodedFrames{},presentedFrames{},presentSubmissions{},droppedFrames{},repeatedFrames{},lateFrames{};std::size_t queueOccupancy{},queueHighWater{};
 double activePlaybackSeconds{};
 void resetActivePlayback() noexcept { activeFrameTimeMs.clear(); presentSubmissionIntervalMs.clear(); presentedFps.clear(); activePlaybackSeconds = 0.0; presentedFrames = 0; }
};

template <typename T, std::size_t C>
T FixedHistory<T, C>::average() const noexcept {
    if (!count_) return T{};
    T sum{};
    for (std::size_t i = 0; i < count_; ++i) sum += values_[i];
    return sum / static_cast<T>(count_);
}

template <typename T, std::size_t C>
T FixedHistory<T, C>::minimum() const noexcept {
    if (!count_) return T{};
    T result = std::numeric_limits<T>::max();
    for (std::size_t i = 0; i < count_; ++i) result = std::min(result, values_[i]);
    return result;
}

template <typename T, std::size_t C>
T FixedHistory<T, C>::maximum() const noexcept {
    if (!count_) return T{};
    T result = std::numeric_limits<T>::lowest();
    for (std::size_t i = 0; i < count_; ++i) result = std::max(result, values_[i]);
    return result;
}

template <typename T, std::size_t C>
T FixedHistory<T, C>::percentile(double p) const noexcept {
    if (!count_) return T{};
    std::array<T, C> sorted{};
    for (std::size_t i = 0; i < count_; ++i) sorted[i] = values_[i];
    std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(count_));
    const double clamped = std::clamp(p, 0.0, 1.0);
    const auto index = static_cast<std::size_t>(std::round(clamped * static_cast<double>(count_ - 1)));
    return sorted[index];
}

[[nodiscard]] double fpsFromAverageInterval(const RollingMetric<>& intervals) noexcept;
[[nodiscard]] double fpsFromLatestInterval(const RollingMetric<>& intervals) noexcept;
[[nodiscard]] double fpsFromNewestIntervalWindow(const RollingMetric<>& intervals, double windowMs) noexcept;

} // namespace ns60