#pragma once

#include "platform/Window.h"
#include "telemetry/Metrics.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace ns60 {

class VulkanContext final {
public:
    static constexpr std::uint32_t framesInFlight = 3;
    static constexpr std::uint32_t timestampsPerFrame = 9;

    struct AcquiredFrame {
        std::uint32_t flightIndex{};
        std::uint32_t imageIndex{};
        VkCommandBuffer commandBuffer{};
        std::optional<GpuTimings> completedTimings;
    };

    VulkanContext(Window& window, bool validation, bool vsync);
    ~VulkanContext();
    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    [[nodiscard]] std::optional<AcquiredFrame> acquireFrame();
    void submitAndPresent(const AcquiredFrame& frame);
    void setTimingPlan(std::uint32_t flight, const std::array<GpuPass, 4>& stages, std::uint32_t count) noexcept;
    void waitIdle() const noexcept;
    void recreateSwapchain();

    [[nodiscard]] VkInstance instance() const noexcept { return instance_; }
    [[nodiscard]] VkPhysicalDevice physicalDevice() const noexcept { return physicalDevice_; }
    [[nodiscard]] VkDevice device() const noexcept { return device_; }
    [[nodiscard]] VkQueue graphicsQueue() const noexcept { return graphicsQueue_; }
    [[nodiscard]] std::uint32_t graphicsQueueFamily() const noexcept { return graphicsFamily_; }
    [[nodiscard]] VkRenderPass renderPass() const noexcept { return renderPass_; }
    [[nodiscard]] VkFramebuffer framebuffer(std::uint32_t image) const noexcept { return framebuffers_[image]; }
    [[nodiscard]] VkExtent2D extent() const noexcept { return swapchainExtent_; }
    [[nodiscard]] VkFormat swapchainFormat() const noexcept { return surfaceFormat_.format; }
    [[nodiscard]] bool swapchainIsSrgb() const noexcept;
    [[nodiscard]] std::uint32_t swapchainImageCount() const noexcept { return static_cast<std::uint32_t>(swapchainImages_.size()); }
    [[nodiscard]] VkQueryPool queryPool() const noexcept { return queryPool_; }
    [[nodiscard]] const VkPhysicalDeviceProperties& properties() const noexcept { return properties_; }
    [[nodiscard]] const VkPhysicalDeviceMemoryProperties& memoryProperties() const noexcept { return memoryProperties_; }
    [[nodiscard]] const std::string& gpuName() const noexcept { return gpuName_; }
    [[nodiscard]] const std::string& presentModeName() const noexcept { return presentModeName_; }
    [[nodiscard]] bool validationEnabled() const noexcept { return validationEnabled_; }
    void nameObject(VkObjectType type, std::uint64_t handle, const char* name) const noexcept;

private:
    struct Flight {
        VkCommandBuffer commandBuffer{};
        VkSemaphore imageAvailable{};
        VkSemaphore renderComplete{};
        VkFence fence{};
        bool submitted{};
        std::array<GpuPass, 4> timingStages{};
        std::uint32_t timingStageCount{};
    };

    void createInstance();
    void selectPhysicalDevice();
    void createDevice();
    void createCommandResources();
    void createSwapchain();
    void destroySwapchain() noexcept;
    void createRenderPass();
    [[nodiscard]] std::optional<GpuTimings> collectTimings(std::uint32_t flight) const;

    Window& window_;
    bool validationEnabled_{};
    bool vsync_{};
    VkInstance instance_{};
    VkDebugUtilsMessengerEXT debugMessenger_{};
    VkSurfaceKHR surface_{};
    VkPhysicalDevice physicalDevice_{};
    VkPhysicalDeviceProperties properties_{};
    VkPhysicalDeviceMemoryProperties memoryProperties_{};
    VkDevice device_{};
    std::uint32_t graphicsFamily_{};
    VkQueue graphicsQueue_{};
    VkSwapchainKHR swapchain_{};
    VkSurfaceFormatKHR surfaceFormat_{};
    VkPresentModeKHR presentMode_{VK_PRESENT_MODE_FIFO_KHR};
    VkExtent2D swapchainExtent_{};
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainViews_;
    std::vector<VkFramebuffer> framebuffers_;
    std::vector<VkFence> imagesInFlight_;
    VkRenderPass renderPass_{};
    VkCommandPool commandPool_{};
    VkQueryPool queryPool_{};
    std::array<Flight, framesInFlight> flights_{};
    std::uint32_t currentFlight_{};
    std::string gpuName_;
    std::string presentModeName_;
};

} // namespace ns60

