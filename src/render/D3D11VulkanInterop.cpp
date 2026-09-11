#include "render/D3D11VulkanInterop.h"

#include "render/VulkanContext.h"
#include "utility/Log.h"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace ns60 {
namespace {

void vkCheck(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed with VkResult " + std::to_string(result));
    }
}

} // namespace

struct D3D11VulkanInterop::Impl {
    struct ImportedTexture {
        VkDevice device{};
        std::vector<VkImage> images;
        std::vector<VkDeviceMemory> memories;
        std::vector<VkImageView> yViews;
        std::vector<VkImageView> uvViews;

        ~ImportedTexture() {
            for (const auto view : uvViews) if (view) vkDestroyImageView(device, view, nullptr);
            for (const auto view : yViews) if (view) vkDestroyImageView(device, view, nullptr);
            for (const auto image : images) if (image) vkDestroyImage(device, image, nullptr);
            for (const auto memory : memories) if (memory) vkFreeMemory(device, memory, nullptr);
        }
    };

    explicit Impl(VulkanContext& owner) : context(owner), device(owner.device()) {
#ifndef _WIN32
        throw std::runtime_error("D3D11/Vulkan interop is only available on Windows");
#endif
    }

    ~Impl() {
        cache.clear();
        if (readySemaphore) vkDestroySemaphore(device, readySemaphore, nullptr);
    }

    [[nodiscard]] std::uint32_t memoryType(std::uint32_t bits) const {
        const auto& properties = context.memoryProperties();
        for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
            if ((bits & (1U << index)) &&
                (properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) return index;
        }
        for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
            if (bits & (1U << index)) return index;
        }
        throw std::runtime_error("Imported D3D11 texture exposes no compatible Vulkan memory type");
    }

    void ensureSemaphore(const D3D11FrameLease& lease) {
#ifdef _WIN32
        if (readySemaphore) {
            if (fenceIdentity != lease.fenceIdentity()) {
                throw std::runtime_error("D3D11 decoder fence changed during an interop session");
            }
            return;
        }
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        type.initialValue = 0;
        const VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
        vkCheck(vkCreateSemaphore(device, &create, nullptr, &readySemaphore), "vkCreateSemaphore(D3D11 timeline)");
        auto handle = lease.createFenceHandle();
        if (!handle) throw std::runtime_error("D3D11 decoder returned an invalid shared fence handle");
        const auto importFunction = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
            vkGetDeviceProcAddr(device, "vkImportSemaphoreWin32HandleKHR"));
        if (!importFunction) throw std::runtime_error("vkImportSemaphoreWin32HandleKHR is unavailable");
        VkImportSemaphoreWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
        import.semaphore = readySemaphore;
        import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;
        import.handle = reinterpret_cast<HANDLE>(handle.get());
        vkCheck(importFunction(device, &import), "vkImportSemaphoreWin32HandleKHR(D3D11 fence)");
        fenceIdentity = lease.fenceIdentity();
#else
        (void)lease;
#endif
    }

    void importImage(ImportedTexture& imported, const PlatformHandle& handle, VkFormat format,
                     std::uint32_t width, std::uint32_t height, const char* name) {
#ifdef _WIN32
        if (!handle) throw std::runtime_error(std::format("D3D11 returned an invalid shared handle for the {}", name));
        VkPhysicalDeviceExternalImageFormatInfo externalQuery{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        externalQuery.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        VkPhysicalDeviceImageFormatInfo2 imageQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        imageQuery.pNext = &externalQuery;
        imageQuery.format = format;
        imageQuery.type = VK_IMAGE_TYPE_2D;
        imageQuery.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageQuery.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        VkExternalImageFormatProperties externalProperties{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 imageProperties{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        imageProperties.pNext = &externalProperties;
        vkCheck(vkGetPhysicalDeviceImageFormatProperties2(context.physicalDevice(), &imageQuery, &imageProperties),
                "vkGetPhysicalDeviceImageFormatProperties2(D3D11 import)");
        if ((externalProperties.externalMemoryProperties.externalMemoryFeatures &
             VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0) {
            throw std::runtime_error(std::format("Vulkan driver cannot import the {} as a D3D11 texture", name));
        }

        VkExternalMemoryImageCreateInfo externalCreate{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        externalCreate.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        VkImageCreateInfo imageCreate{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageCreate.pNext = &externalCreate;
        imageCreate.imageType = VK_IMAGE_TYPE_2D;
        imageCreate.format = format;
        imageCreate.extent = {width, height, 1};
        imageCreate.mipLevels = 1;
        imageCreate.arrayLayers = 1;
        imageCreate.samples = VK_SAMPLE_COUNT_1_BIT;
        imageCreate.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageCreate.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        imageCreate.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageCreate.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage image{};
        vkCheck(vkCreateImage(device, &imageCreate, nullptr, &image), "vkCreateImage(import D3D11 texture)");
        imported.images.push_back(image);

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, image, &requirements);
        const auto propertiesFunction = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
            vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandlePropertiesKHR"));
        if (!propertiesFunction) throw std::runtime_error("vkGetMemoryWin32HandlePropertiesKHR is unavailable");
        VkMemoryWin32HandlePropertiesKHR handleProperties{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
        vkCheck(propertiesFunction(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
                                   reinterpret_cast<HANDLE>(handle.get()), &handleProperties),
                "vkGetMemoryWin32HandlePropertiesKHR(D3D11 texture)");
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.image = image;
        VkImportMemoryWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
        import.pNext = &dedicated;
        import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        import.handle = reinterpret_cast<HANDLE>(handle.get());
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.pNext = &import;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = memoryType(requirements.memoryTypeBits & handleProperties.memoryTypeBits);
        VkDeviceMemory memory{};
        vkCheck(vkAllocateMemory(device, &allocate, nullptr, &memory), "vkAllocateMemory(import D3D11 texture)");
        imported.memories.push_back(memory);
        vkCheck(vkBindImageMemory(device, image, memory, 0), "vkBindImageMemory(import D3D11 texture)");
        context.nameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<std::uint64_t>(image), name);
#else
        (void)imported; (void)handle; (void)format; (void)width; (void)height; (void)name;
        throw std::runtime_error("D3D11/Vulkan interop is only available on Windows");
#endif
    }

    [[nodiscard]] VkImageView createView(VkImage image, VkFormat format, const char* operation) const {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView result{};
        vkCheck(vkCreateImageView(device, &view, nullptr, &result), operation);
        return result;
    }

    [[nodiscard]] std::shared_ptr<ImportedTexture> importTexture(const D3D11FrameLease& lease) {
        const auto& description = lease.description();
        if (description.chromaTextureIdentity == 0) {
            throw std::runtime_error(
                "Strict D3D11/Vulkan NV12 zero-copy is unsupported on this driver; use --decoder-path interop-copy");
        }
        auto imported = std::make_shared<ImportedTexture>();
        imported->device = device;
        importImage(*imported, lease.createTextureHandle(), VK_FORMAT_R8_UNORM,
                    description.textureWidth, description.textureHeight, "Imported D3D11 luma plane");
        importImage(*imported, lease.createChromaTextureHandle(), VK_FORMAT_R8G8_UNORM,
                    description.textureWidth / 2, description.textureHeight / 2, "Imported D3D11 chroma plane");
        imported->yViews.push_back(createView(imported->images[0], VK_FORMAT_R8_UNORM,
                                              "vkCreateImageView(D3D11 luma)"));
        imported->uvViews.push_back(createView(imported->images[1], VK_FORMAT_R8G8_UNORM,
                                               "vkCreateImageView(D3D11 chroma)"));
        Log::info(std::format("Actual decoder texture import: success ({}x{} separate R8/R8G8 plane textures)",
                              description.textureWidth, description.textureHeight));
        return imported;
    }

    [[nodiscard]] PreparedInteropFrame prepare(std::shared_ptr<D3D11FrameLease> lease,
                                               VkExtent2D visibleExtent) {
        if (!lease) throw std::invalid_argument("D3D11 interop preparation requires a frame lease");
        const auto& description = lease->description();
        if (description.textureWidth < visibleExtent.width || description.textureHeight < visibleExtent.height) {
            throw std::runtime_error(std::format(
                "D3D11 decoder texture {}x{} is smaller than visible frame {}x{}",
                description.textureWidth, description.textureHeight, visibleExtent.width, visibleExtent.height));
        }
        if (description.arraySize == 0 || description.arraySlice >= description.arraySize) {
            throw std::runtime_error("D3D11 decoder returned an invalid texture-array description");
        }
        ensureSemaphore(*lease);
        if (!counterReported) {
            std::uint64_t counter{};
            vkCheck(vkGetSemaphoreCounterValue(device, readySemaphore, &counter),
                    "vkGetSemaphoreCounterValue(D3D11 fence)");
            Log::debug(std::format("Imported D3D11 fence counter: {}, first frame requires {}",
                                   counter, description.readyFenceValue));
            counterReported = true;
        }
        const ExternalTextureKey key{description.poolGeneration, description.textureIdentity};
        if (key.poolGeneration != cacheGeneration) {
            cache.clear();
            cacheGeneration = key.poolGeneration;
        }
        auto& imported = cache[{key.poolGeneration, key.textureIdentity}];
        if (!imported) imported = importTexture(*lease);

        PreparedInteropFrame frame;
        for (std::size_t index = 0; index < std::min(frame.images.size(), imported->images.size()); ++index) {
            frame.images[index] = imported->images[index];
        }
        frame.yView = imported->yViews.at(description.arraySlice);
        frame.uvView = imported->uvViews.at(description.arraySlice);
        frame.readySemaphore = readySemaphore;
        frame.readyValue = description.readyFenceValue;
        frame.arraySlice = description.arraySlice;
        frame.lease = std::move(lease);
        frame.importedTexture = imported;
        return frame;
    }

    VulkanContext& context;
    VkDevice device{};
    VkSemaphore readySemaphore{};
    std::uintptr_t fenceIdentity{};
    bool counterReported{};
    std::map<std::pair<std::uint64_t, std::uintptr_t>, std::shared_ptr<ImportedTexture>> cache;
    std::uint64_t cacheGeneration{};
};

namespace {

void recordOwnershipTransfer(VkCommandBuffer command, const PreparedInteropFrame& frame, bool acquire,
                             std::uint32_t queueFamily) {
    std::array<VkImageMemoryBarrier, 2> barriers{};
    std::uint32_t count = 0;
    for (const auto image : frame.images) {
        if (!image) continue;
        barriers[count++] = VkImageMemoryBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr,
            acquire ? 0U : static_cast<VkAccessFlags>(VK_ACCESS_SHADER_READ_BIT),
            acquire ? static_cast<VkAccessFlags>(VK_ACCESS_SHADER_READ_BIT) : 0U,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            acquire ? VK_QUEUE_FAMILY_EXTERNAL : queueFamily,
            acquire ? queueFamily : VK_QUEUE_FAMILY_EXTERNAL,
            image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    }
    if (count == 0) return;
    vkCmdPipelineBarrier(command,
                         acquire ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         acquire ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, count, barriers.data());
}

} // namespace

D3D11VulkanInterop::D3D11VulkanInterop(VulkanContext& context) : impl_(std::make_unique<Impl>(context)) {}
D3D11VulkanInterop::~D3D11VulkanInterop() = default;

PreparedInteropFrame D3D11VulkanInterop::prepare(std::shared_ptr<D3D11FrameLease> lease,
                                                 VkExtent2D visibleExtent) {
    return impl_->prepare(std::move(lease), visibleExtent);
}

void D3D11VulkanInterop::recordAcquire(VkCommandBuffer command, const PreparedInteropFrame& frame) const {
    recordOwnershipTransfer(command, frame, true, impl_->context.graphicsQueueFamily());
}

void D3D11VulkanInterop::recordRelease(VkCommandBuffer command, const PreparedInteropFrame& frame) const {
    recordOwnershipTransfer(command, frame, false, impl_->context.graphicsQueueFamily());
}

} // namespace ns60
