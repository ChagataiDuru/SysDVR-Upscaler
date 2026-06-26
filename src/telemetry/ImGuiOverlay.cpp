#include "telemetry/ImGuiOverlay.h"

#include "utility/ColorMath.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <array>
#include <stdexcept>

namespace ns60 {
namespace {
void checkVulkan(VkResult result) {
    if (result != VK_SUCCESS) throw std::runtime_error("Dear ImGui Vulkan backend failed with VkResult " + std::to_string(result));
}

void metricRow(const char* label, const RollingMetric<>& metric, const char* unit = "ms") {
    ImGui::Text("%-22s %8.3f %s  avg %8.3f  min %8.3f  max %8.3f", label, metric.latest(), unit,
                metric.average(), metric.minimum(), metric.maximum());
}
} // namespace

ImGuiOverlay::ImGuiOverlay(Window& window, VulkanContext& context, const std::filesystem::path& input,
                           const VideoStreamInfo& stream, int outputWidth, int outputHeight,
                           PlaybackPolicy playbackPolicy, LatencyProfile latencyProfile, int liveFrameQueueDepth)
    : window_(window), context_(context), input_(input), stream_(stream), outputWidth_(outputWidth), outputHeight_(outputHeight),
      playbackPolicy_(playbackPolicy), latencyProfile_(latencyProfile), liveFrameQueueDepth_(liveFrameQueueDepth) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    if (!ImGui_ImplGlfw_InitForVulkan(window.native(), true)) throw std::runtime_error("Dear ImGui GLFW backend initialization failed");

    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_2;
    info.Instance = context.instance();
    info.PhysicalDevice = context.physicalDevice();
    info.Device = context.device();
    info.QueueFamily = context.graphicsQueueFamily();
    info.Queue = context.graphicsQueue();
    info.DescriptorPoolSize = 64;
    info.MinImageCount = 2;
    info.ImageCount = context.swapchainImageCount();
    info.RenderPass = context.renderPass();
    info.Subpass = 0;
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.CheckVkResultFn = checkVulkan;
    info.MinAllocationSize = 1024 * 1024;
    if (!ImGui_ImplVulkan_Init(&info)) throw std::runtime_error("Dear ImGui Vulkan backend initialization failed");
}

ImGuiOverlay::~ImGuiOverlay() {
    context_.waitIdle();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void ImGuiOverlay::build(const Metrics& metrics, const std::optional<VideoFrameMetadata>& current, bool paused, VideoPipeline& pipeline) {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    if (visible_) {
        constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
        ImGui::SetNextWindowBgAlpha(0.88F);
        if (ImGui::Begin("NexusStream60 telemetry", nullptr, flags)) {
            const auto filename = input_.filename().string();
            ImGui::Text("Input: %s", filename.c_str());
            ImGui::Text("Codec / format: %s / %s", stream_.codecName.c_str(), stream_.pixelFormatName.c_str());
            ImGui::Text("Decoder backend: %s", toString(stream_.activeDecoderBackend).data());
            ImGui::Text("Input / output: %dx%d / %dx%d", stream_.width, stream_.height, outputWidth_, outputHeight_);
            ImGui::Text("Color: %s, %s, transfer %s", toString(stream_.color.range), toString(stream_.color.matrix), stream_.transfer.c_str());
            if (stream_.live) {
                ImGui::Text("Source mode: Live SysDVR");
                ImGui::Text("Scheduling: %s", toString(playbackPolicy_).data());
                ImGui::Text("Latency profile: %s", toString(latencyProfile_).data());
                ImGui::Text("Live decoded queue depth: %d", liveFrameQueueDepth_);
                ImGui::TextUnformatted("Rate declared / average: N/A / N/A");
            } else {
                ImGui::Text("Rate declared / average: %.3f / %.3f fps", stream_.declaredFrameRate, stream_.averageFrameRate);
            }
            ImGui::Text("State: %s", paused ? "paused" : (stream_.live ? "live" : "playing"));
            ImGui::Text("Upscale: %s", displayName(pipeline.mode()).data());
            int selected = static_cast<int>(pipeline.mode());
            constexpr const char* modeNames[] = {"Nearest","Bilinear","Bicubic Catmull-Rom","Lanczos2","Bilinear + CAS","Lanczos2 + CAS","FSR1 EASU","FSR1 EASU + RCAS"};
            if (ImGui::Combo("Mode", &selected, modeNames, 8)) pipeline.setMode(static_cast<UpscaleMode>(selected));
            auto sharpen = pipeline.parameters().sharpen;
            bool changed = ImGui::SliderFloat("CAS strength", &sharpen.casSharpness, 0.0F, 1.0F, "%.2f");
            changed |= ImGui::SliderFloat("RCAS strength", &sharpen.rcasSharpness, 0.0F, 1.0F, "%.2f");
            if (changed) pipeline.setSharpenSettings(sharpen);
            bool anti = pipeline.parameters().antiRinging;
            if (ImGui::Checkbox("Anti-ringing", &anti)) pipeline.setAntiRinging(anti);
            ImGui::Text("Comparison: %s", pipeline.comparisonEnabled() ? "enabled" : "disabled");
            if (pipeline.comparisonEnabled()) ImGui::Text("A: %s  B: %s  Divider: %.0f%%", displayName(pipeline.comparisonA()).data(), displayName(pipeline.comparisonB()).data(), pipeline.comparisonDivider() * 100.0F);
            const auto extent = context_.extent(); const auto geometry = pipeline.presentationGeometry();
            const auto size = window_.sizeInfo(); const auto monitor = window_.monitorInfo();
            ImGui::Separator();
            ImGui::TextUnformatted("Presentation");
            ImGui::Text("Requested reconstruction: %dx%d", outputWidth_, outputHeight_);
            ImGui::Text("Actual framebuffer: %dx%d", size.framebufferWidth, size.framebufferHeight);
            ImGui::Text("Swapchain extent: %ux%u", extent.width, extent.height);
            ImGui::Text("Presentation viewport: %ux%u at %d,%d", geometry.viewportWidth, geometry.viewportHeight, geometry.viewportX, geometry.viewportY);
            ImGui::Text("Presentation mode: %s", toString(geometry.effectiveMode).data());
            ImGui::Text("1:1 output mapping: %s", geometry.exactOneToOne ? "Yes" : "No");
            ImGui::Text("Final resample: %s%s%s", geometry.finalResampleActive ? "Active (" : "Inactive", geometry.finalResampleActive ? toString(pipeline.finalFilter()).data() : "", geometry.finalResampleActive ? ")" : "");
            ImGui::Text("DPI scale: %.2f, %.2f", size.contentScaleX, size.contentScaleY);
            ImGui::Text("Monitor: %d %s (%dx%d at %d,%d)", monitor.index, monitor.name.c_str(), monitor.width, monitor.height, monitor.x, monitor.y);
            ImGui::Text("Chroma: %s, location %s", displayName(pipeline.parameters().chromaMode).data(), stream_.chromaLocation.c_str());
            const float regionWidth = outputWidth_ / pipeline.zoom(), regionHeight = outputHeight_ / pipeline.zoom();
            ImGui::Text("Zoom: %.1fx  Region: %.0f,%.0f %.0fx%.0f", pipeline.zoom(),
                pipeline.zoomCenterX() * outputWidth_ - regionWidth * 0.5F,
                pipeline.zoomCenterY() * outputHeight_ - regionHeight * 0.5F, regionWidth, regionHeight);
            if (current) {
                if (stream_.live || current->timingKind == FrameTimingKind::LiveArrival) {
                    ImGui::TextUnformatted("Frame timing: live arrival (container PTS N/A)");
                    ImGui::Text("Decoded live frame: %llu", static_cast<unsigned long long>(current->frameNumber));
                } else {
                    ImGui::Text("PTS / delta / duration: %.6f s / %.3f ms / %.3f ms", current->ptsSeconds,
                        metrics.ptsDeltaMs.latest(), current->durationSeconds * 1000.0);
                    ImGui::Text("Decoded frame: %llu", static_cast<unsigned long long>(current->frameNumber));
                }
            }
            ImGui::Separator();
            ImGui::Text("Instant FPS: %.2f", fpsFromLatestInterval(metrics.presentSubmissionIntervalMs));
            ImGui::Text("Rolling FPS (1s / 120 frames): %.2f / %.2f", fpsFromNewestIntervalWindow(metrics.presentSubmissionIntervalMs, 1000.0), fpsFromAverageInterval(metrics.presentSubmissionIntervalMs));
            ImGui::Text("Lifetime active presented FPS: %.2f", metrics.activePlaybackSeconds > 0.0 ? static_cast<double>(metrics.presentedFrames) / metrics.activePlaybackSeconds : 0.0);
            ImGui::Text("Decoder throughput FPS: %.2f", metrics.decodedFps.latest());
            ImGui::Text("Decoded queue: %zu (high-water %zu)", metrics.queueOccupancy, metrics.queueHighWater);
            if (stream_.live) {
                ImGui::Text("Dropped / repeated: %llu / %llu",
                    static_cast<unsigned long long>(metrics.droppedFrames), static_cast<unsigned long long>(metrics.repeatedFrames));
                ImGui::Text("Stale decoded frames dropped: %llu", static_cast<unsigned long long>(metrics.staleDecodedFramesDropped));
            } else {
                ImGui::Text("Dropped / repeated / late: %llu / %llu / %llu",
                    static_cast<unsigned long long>(metrics.droppedFrames), static_cast<unsigned long long>(metrics.repeatedFrames),
                    static_cast<unsigned long long>(metrics.lateFrames));
                ImGui::Text("Stale decoded frames dropped: %llu", static_cast<unsigned long long>(metrics.staleDecodedFramesDropped));
                metricRow("Playback drift", metrics.driftMs);
                metricRow("Frame lateness", metrics.latenessMs);
            }
            metricRow("CPU decode", metrics.decodeMs);
            metricRow("CPU plane copy", metrics.planeCopyMs);
            metricRow("CPU decoder wait", metrics.decoderWaitMs);
            metricRow("CPU frame", metrics.cpuFrameMs);
            metricRow("Active frame time", metrics.activeFrameTimeMs);
            ImGui::Text("Frame-time P50/P95/P99: %.3f / %.3f / %.3f ms", metrics.activeFrameTimeMs.percentile(0.50), metrics.activeFrameTimeMs.percentile(0.95), metrics.activeFrameTimeMs.percentile(0.99));
            ImGui::Text("Present interval P50/P95/P99: %.3f / %.3f / %.3f ms", metrics.presentSubmissionIntervalMs.percentile(0.50), metrics.presentSubmissionIntervalMs.percentile(0.95), metrics.presentSubmissionIntervalMs.percentile(0.99));
            metricRow("GPU upload", metrics.gpuUploadMs);
            metricRow("GPU YUV-to-RGB", metrics.gpuColorMs);
            constexpr std::array<const char*, 7> passLabels{"GPU nearest","GPU bilinear","GPU bicubic","GPU Lanczos2","GPU CAS","GPU EASU","GPU RCAS"};
            for (std::size_t pass = 0; pass < passLabels.size(); ++pass) {
                if (metrics.gpuPassMs[pass].count()) metricRow(passLabels[pass], metrics.gpuPassMs[pass]);
                else ImGui::Text("%-22s N/A", passLabels[pass]);
            }
            metricRow("GPU reconstruction", metrics.gpuReconstructionMs);
            metricRow("GPU post-processing", metrics.gpuPostProcessingMs);
            metricRow("GPU present", metrics.gpuPresentMs);
            metricRow("GPU total", metrics.gpuTotalMs);
            ImGui::Separator();
            ImGui::Text("Present mode: %s", context_.presentModeName().c_str());
            ImGui::Text("GPU: %s", context_.gpuName().c_str());

            std::array<float, 240> graph{};
            const auto& source = metrics.activeFrameTimeMs.data();
            for (std::size_t i = 0; i < graph.size(); ++i) graph[i] = static_cast<float>(source[i]);
            ImGui::PlotLines("CPU frame history", graph.data(), static_cast<int>(metrics.activeFrameTimeMs.count()), 0,
                nullptr, 0.0F, 33.0F, ImVec2(420.0F, 55.0F));
        }
        ImGui::End();
    }
    ImGui::Render();
}

void ImGuiOverlay::record(VkCommandBuffer command) {
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), command);
}

} // namespace ns60
