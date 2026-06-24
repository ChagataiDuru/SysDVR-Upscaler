#pragma once

#include "decode/DecodedFrame.h"
#include "render/Upscaling.h"
#include "render/VulkanContext.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vulkan/vulkan.h>

namespace ns60 {

struct ScreenshotRequest {
    std::filesystem::path path;
    std::filesystem::path metadataPath;
    std::string sourceFilename;
    VideoFrameMetadata frame;
    int outputWidth{};
    int outputHeight{};
    int framebufferWidth{};
    int framebufferHeight{};
    UpscaleMode mode{UpscaleMode::Bilinear};
    SharpenSettings sharpen{};
    bool antiRinging{};
    ChromaUpscaleMode chromaMode{ChromaUpscaleMode::BicubicCatmullRom};
    FinalFilter finalFilter{FinalFilter::Bilinear};
    bool comparison{};
    UpscaleMode comparisonA{UpscaleMode::Bilinear};
    UpscaleMode comparisonB{UpscaleMode::Fsr1EasuRcas};
    float divider{0.5F};
    std::optional<GpuTimings> timings;
};

class VideoPipeline final {
public:
    VideoPipeline(VulkanContext& context, int sourceWidth, int sourceHeight, int outputWidth, int outputHeight,
                  UpscaleMode mode, SharpenSettings sharpen, bool antiRinging, PresentationMode presentation,
                  bool presentationExplicit, FinalFilter finalFilter, ChromaUpscaleMode chromaMode);
    ~VideoPipeline();
    VideoPipeline(const VideoPipeline&) = delete;
    VideoPipeline& operator=(const VideoPipeline&) = delete;

    void setMode(UpscaleMode mode);
    void setSharpenSettings(SharpenSettings settings) noexcept { parameters_.sharpen = settings; }
    void setAntiRinging(bool enabled) noexcept { parameters_.antiRinging = enabled; }
    void setComparison(bool enabled, UpscaleMode a, UpscaleMode b);
    void setComparisonDivider(float value) noexcept;
    void setPresentationMode(PresentationMode mode) noexcept { presentationMode_ = mode; presentationExplicit_ = true; }
    void setFinalFilter(FinalFilter filter) noexcept { finalFilter_ = filter; }
    void setChromaUpscaleMode(ChromaUpscaleMode mode) noexcept { parameters_.chromaMode = mode; }
    void setZoomInspector(bool enabled) noexcept { zoomEnabled_ = enabled; }
    void setZoom(float value) noexcept;
    void moveZoom(float x, float y) noexcept;
    [[nodiscard]] float zoom() const noexcept { return zoom_; }
    [[nodiscard]] float zoomCenterX() const noexcept { return zoomCenterX_; }
    [[nodiscard]] float zoomCenterY() const noexcept { return zoomCenterY_; }
    [[nodiscard]] UpscaleMode mode() const noexcept { return mode_; }
    [[nodiscard]] const UpscaleParameters& parameters() const noexcept { return parameters_; }
    [[nodiscard]] bool comparisonEnabled() const noexcept { return comparisonEnabled_; }
    [[nodiscard]] UpscaleMode comparisonA() const noexcept { return comparisonA_; }
    [[nodiscard]] UpscaleMode comparisonB() const noexcept { return comparisonB_; }
    [[nodiscard]] float comparisonDivider() const noexcept { return comparisonDivider_; }
    [[nodiscard]] PresentationGeometry presentationGeometry() const noexcept;
    [[nodiscard]] PresentationMode effectivePresentationMode() const noexcept;
    [[nodiscard]] FinalFilter finalFilter() const noexcept { return finalFilter_; }

    void prepareFrame(std::uint32_t flight, const Yuv420FrameSlot& frame);
    void recordProcessing(VkCommandBuffer command, std::uint32_t flight);
    void beginPresent(VkCommandBuffer command, std::uint32_t flight, std::uint32_t swapchainImage);
    void endPresent(VkCommandBuffer command, std::uint32_t flight);
    void requestScreenshot(ScreenshotRequest request);
    void completePendingScreenshot(std::uint32_t flight);

private:
    struct BufferResource { VkBuffer buffer{}; VkDeviceMemory memory{}; void* mapped{}; VkDeviceSize size{}; bool coherent{}; };
    struct ImageResource { VkImage image{}; VkDeviceMemory memory{}; VkImageView view{}; VkFormat format{}; VkExtent2D extent{}; };
    struct FlightResources {
        BufferResource staging, readback;
        ImageResource y, u, v, working, intermediate, output, comparisonOutput;
        VkDescriptorSet colorSet{};
        std::array<VkDescriptorSet, 5> processSets{};
        VkDescriptorSet presentSet{};
        bool initialized{}, screenshotPending{};
        std::optional<ScreenshotRequest> screenshot;
        ColorDescription color{};
    };
    enum ProcessSet : std::size_t { WorkingToOutput, WorkingToIntermediate, IntermediateToOutput, WorkingToComparison, IntermediateToComparison };
    enum Pipeline : std::size_t { Nearest, Bilinear, Bicubic, Lanczos2, Cas, Easu, Rcas, PipelineCount };

    [[nodiscard]] std::uint32_t memoryType(std::uint32_t bits, VkMemoryPropertyFlags required) const;
    [[nodiscard]] BufferResource createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memory);
    [[nodiscard]] ImageResource createImage(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage, const char* name);
    void destroy(BufferResource& resource) noexcept;
    void destroy(ImageResource& resource) noexcept;
    [[nodiscard]] VkShaderModule loadShader(const char* filename) const;
    void createDescriptors();
    void createPipelines();
    void writeDescriptors(std::uint32_t flight);
    void recordMode(VkCommandBuffer command, std::uint32_t flight, UpscaleMode mode, bool comparisonDestination,
                    std::array<GpuPass, 4>& stages, std::uint32_t& stageCount);
    void dispatchPass(VkCommandBuffer command, VkPipeline pipeline, VkDescriptorSet set, const void* push,
                      std::uint32_t pushSize);
    void saveScreenshot(FlightResources& flight);

    VulkanContext& context_;
    VkDevice device_{};
    VkExtent2D sourceExtent_{}, outputExtent_{};
    VkDeviceSize ySize_{}, chromaSize_{}, uOffset_{}, vOffset_{};
    VkSampler sampler_{};
    VkDescriptorPool descriptorPool_{};
    VkDescriptorSetLayout colorLayout_{}, processLayout_{}, presentLayout_{};
    VkPipelineLayout colorPipelineLayout_{}, processPipelineLayout_{}, presentPipelineLayout_{};
    VkPipeline colorPipeline_{}, presentPipeline_{};
    std::array<VkPipeline, PipelineCount> pipelines_{};
    std::array<FlightResources, VulkanContext::framesInFlight> flights_{};
    std::optional<ScreenshotRequest> screenshotRequest_;
    UpscaleParameters parameters_{};
    UpscaleMode mode_{UpscaleMode::Bilinear}, comparisonA_{UpscaleMode::Bilinear}, comparisonB_{UpscaleMode::Fsr1EasuRcas};
    PresentationMode presentationMode_{PresentationMode::Fit};
    bool presentationExplicit_{};
    FinalFilter finalFilter_{FinalFilter::Bilinear};
    bool comparisonEnabled_{}, zoomEnabled_{};
    float comparisonDivider_{0.5F}, zoom_{4.0F}, zoomCenterX_{0.5F}, zoomCenterY_{0.5F};
};

} // namespace ns60
