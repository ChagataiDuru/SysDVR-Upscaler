#pragma once

#include "app/AppConfig.h"
#include "decode/FFmpegVideoReader.h"
#include "platform/Window.h"
#include "render/VideoPipeline.h"
#include "render/VulkanContext.h"
#include "telemetry/Metrics.h"

#include <filesystem>
#include <optional>

namespace ns60 {

class ImGuiOverlay final {
public:
    ImGuiOverlay(Window& window, VulkanContext& context, const std::filesystem::path& input,
                 const VideoStreamInfo& stream, int outputWidth, int outputHeight,
                 PlaybackPolicy playbackPolicy, LatencyProfile latencyProfile, int liveFrameQueueDepth);
    ~ImGuiOverlay();
    ImGuiOverlay(const ImGuiOverlay&) = delete;
    ImGuiOverlay& operator=(const ImGuiOverlay&) = delete;

    void setVisible(bool visible) noexcept { visible_ = visible; }
    [[nodiscard]] bool visible() const noexcept { return visible_; }
    void build(const Metrics& metrics, const std::optional<VideoFrameMetadata>& current, bool paused, VideoPipeline& pipeline);
    void record(VkCommandBuffer command);

private:
    Window& window_;
    VulkanContext& context_;
    std::filesystem::path input_;
    VideoStreamInfo stream_;
    int outputWidth_{};
    int outputHeight_{};
    PlaybackPolicy playbackPolicy_{PlaybackPolicy::TimedFile};
    LatencyProfile latencyProfile_{LatencyProfile::Balanced};
    int liveFrameQueueDepth_{};
    bool visible_{true};
};

} // namespace ns60

