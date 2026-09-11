#pragma once

#include "decode/DecodedFrame.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vulkan/vulkan.h>

namespace ns60 {

class VulkanContext;

struct PreparedInteropFrame {
    // NV12 array import: images[0] only. Separate-plane import: luma, chroma.
    std::array<VkImage, 2> images{};
    VkImageView yView{};
    VkImageView uvView{};
    VkSemaphore readySemaphore{};
    std::uint64_t readyValue{};
    std::uint32_t arraySlice{};
    std::shared_ptr<D3D11FrameLease> lease;
    std::shared_ptr<void> importedTexture;
};

class D3D11VulkanInterop final {
public:
    explicit D3D11VulkanInterop(VulkanContext& context);
    ~D3D11VulkanInterop();
    D3D11VulkanInterop(const D3D11VulkanInterop&) = delete;
    D3D11VulkanInterop& operator=(const D3D11VulkanInterop&) = delete;

    [[nodiscard]] PreparedInteropFrame prepare(std::shared_ptr<D3D11FrameLease> lease,
                                               VkExtent2D visibleExtent);
    void recordAcquire(VkCommandBuffer command, const PreparedInteropFrame& frame) const;
    void recordRelease(VkCommandBuffer command, const PreparedInteropFrame& frame) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ns60
