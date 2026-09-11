#include "render/VulkanContext.h"

#include "utility/Log.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace ns60 {
namespace {
constexpr std::array validationLayers{"VK_LAYER_KHRONOS_validation"};

void vkCheck(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + " failed with VkResult " + std::to_string(result));
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (!data || !data->pMessage) return VK_FALSE;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) Log::error(data->pMessage);
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) Log::warning(data->pMessage);
    else Log::debug(data->pMessage);
    return VK_FALSE;
}

bool hasLayer(const char* name) {
    std::uint32_t count{};
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    return std::any_of(layers.begin(), layers.end(), [name](const auto& layer) { return std::strcmp(layer.layerName, name) == 0; });
}

bool hasDeviceExtension(VkPhysicalDevice device, const char* name) {
    std::uint32_t count{};
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data());
    return std::any_of(extensions.begin(), extensions.end(), [name](const auto& value) { return std::strcmp(value.extensionName, name) == 0; });
}

AdapterLuid physicalDeviceLuid(VkPhysicalDevice device) {
    VkPhysicalDeviceIDProperties ids{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &ids;
    vkGetPhysicalDeviceProperties2(device, &properties);
    AdapterLuid result{};
    if (ids.deviceLUIDValid == VK_TRUE) {
        result.valid = true;
        std::memcpy(result.bytes.data(), ids.deviceLUID, result.bytes.size());
    }
    return result;
}

bool supportsD3D11FenceImport(VkPhysicalDevice device) {
    VkPhysicalDeviceExternalSemaphoreInfo info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
    info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;
    VkExternalSemaphoreProperties properties{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
    vkGetPhysicalDeviceExternalSemaphoreProperties(device, &info, &properties);
    return (properties.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) != 0;
}

bool supportsTimelineSemaphores(VkPhysicalDevice device) {
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &timeline;
    vkGetPhysicalDeviceFeatures2(device, &features);
    return timeline.timelineSemaphore == VK_TRUE;
}

// The D3D11VA decoder surface pool is one NV12 texture array. A multi-planar
// Vulkan image may only have more than one array layer when ycbcrImageArrays
// is enabled; without it the layered plane layout is undefined.
bool supportsYcbcrImageArrays(VkPhysicalDevice device) {
    if (!hasDeviceExtension(device, VK_EXT_YCBCR_IMAGE_ARRAYS_EXTENSION_NAME)) return false;
    VkPhysicalDeviceYcbcrImageArraysFeaturesEXT arrays{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_YCBCR_IMAGE_ARRAYS_FEATURES_EXT};
    VkPhysicalDeviceSamplerYcbcrConversionFeatures conversion{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES};
    conversion.pNext = &arrays;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &conversion;
    vkGetPhysicalDeviceFeatures2(device, &features);
    return conversion.samplerYcbcrConversion == VK_TRUE && arrays.ycbcrImageArrays == VK_TRUE;
}

std::string apiVersion(std::uint32_t value) {
    return std::format("{}.{}.{}", VK_VERSION_MAJOR(value), VK_VERSION_MINOR(value), VK_VERSION_PATCH(value));
}

const char* presentModeText(VkPresentModeKHR mode) {
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "Immediate";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "Mailbox";
    case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO relaxed";
    default: return "Other";
    }
}
} // namespace

VulkanContext::VulkanContext(Window& window, bool validation, bool vsync,
                             std::optional<AdapterLuid> requiredAdapterLuid, bool requireYcbcrImageArrays)
    : window_(window), validationEnabled_(validation), requireYcbcrImageArrays_(requireYcbcrImageArrays),
      vsync_(vsync), requiredAdapterLuid_(requiredAdapterLuid) {
    createInstance();
    surface_ = window_.createSurface(instance_);
    selectPhysicalDevice();
    createDevice();
    createCommandResources();
    createSwapchain();
}

VulkanContext::~VulkanContext() {
    waitIdle();
    destroySwapchain();
    if (queryPool_) vkDestroyQueryPool(device_, queryPool_, nullptr);
    for (const auto& flight : flights_) {
        if (flight.fence) vkDestroyFence(device_, flight.fence, nullptr);
        if (flight.imageAvailable) vkDestroySemaphore(device_, flight.imageAvailable, nullptr);
    }
    if (commandPool_) vkDestroyCommandPool(device_, commandPool_, nullptr);
    if (device_) vkDestroyDevice(device_, nullptr);
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (debugMessenger_) {
        const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy) destroy(instance_, debugMessenger_, nullptr);
    }
    if (instance_) vkDestroyInstance(instance_, nullptr);
}

void VulkanContext::createInstance() {
    if (validationEnabled_ && !hasLayer(validationLayers[0])) {
        throw std::runtime_error("Vulkan validation was requested but VK_LAYER_KHRONOS_validation is unavailable. Install the Vulkan SDK or run with --validation off.");
    }
    std::uint32_t loaderVersion = VK_API_VERSION_1_0;
    if (const auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"))) {
        enumerate(&loaderVersion);
    }
    if (loaderVersion < VK_API_VERSION_1_2) throw std::runtime_error("Vulkan 1.2+ is required; loader reports " + apiVersion(loaderVersion));

    std::uint32_t glfwCount{};
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwCount);
    if (!glfwExtensions || glfwCount == 0) throw std::runtime_error("GLFW did not report required Vulkan surface extensions");
    std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwCount);
    if (validationEnabled_) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    const VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "NexusStream60", VK_MAKE_VERSION(0, 1, 0),
        "NexusStream60", VK_MAKE_VERSION(0, 1, 0), VK_API_VERSION_1_2};
    VkDebugUtilsMessengerCreateInfoEXT debugInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debugInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debugInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debugInfo.pfnUserCallback = debugCallback;
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create.pApplicationInfo = &application;
    create.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    create.ppEnabledExtensionNames = extensions.data();
    if (validationEnabled_) {
        create.enabledLayerCount = static_cast<std::uint32_t>(validationLayers.size());
        create.ppEnabledLayerNames = validationLayers.data();
        create.pNext = &debugInfo;
    }
    vkCheck(vkCreateInstance(&create, nullptr, &instance_), "vkCreateInstance");
    if (validationEnabled_) {
        const auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (!createMessenger) throw std::runtime_error("VK_EXT_debug_utils was enabled but vkCreateDebugUtilsMessengerEXT is unavailable");
        vkCheck(createMessenger(instance_, &debugInfo, nullptr, &debugMessenger_), "vkCreateDebugUtilsMessengerEXT");
    }
}

void VulkanContext::selectPhysicalDevice() {
    std::uint32_t count{};
    vkCheck(vkEnumeratePhysicalDevices(instance_, &count, nullptr), "vkEnumeratePhysicalDevices");
    if (count == 0) throw std::runtime_error("No Vulkan physical devices were found");
    std::vector<VkPhysicalDevice> devices(count);
    vkCheck(vkEnumeratePhysicalDevices(instance_, &count, devices.data()), "vkEnumeratePhysicalDevices");

    int bestScore = -1;
    bool matchingLuidSeen = false;
    std::string interopRejection;
    for (const auto device : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_2 || !properties.limits.timestampComputeAndGraphics) continue;
        if (!hasDeviceExtension(device, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
        if (requiredAdapterLuid_) {
            const auto luid = physicalDeviceLuid(device);
            if (!adapterLuidsMatch(*requiredAdapterLuid_, luid)) continue;
            matchingLuidSeen = true;
#ifdef _WIN32
            if (!hasDeviceExtension(device, "VK_KHR_external_memory_win32")) {
                interopRejection = "matching Vulkan adapter lacks VK_KHR_external_memory_win32";
                continue;
            }
            if (!hasDeviceExtension(device, "VK_KHR_external_semaphore_win32")) {
                interopRejection = "matching Vulkan adapter lacks VK_KHR_external_semaphore_win32";
                continue;
            }
#else
            interopRejection = "D3D11/Vulkan interop is only available on Windows";
            continue;
#endif
            if (!supportsTimelineSemaphores(device)) {
                interopRejection = "matching Vulkan adapter does not support timeline semaphores";
                continue;
            }
            if (!supportsD3D11FenceImport(device)) {
                interopRejection = "matching Vulkan adapter cannot import timeline D3D11 fence handles";
                continue;
            }
            if (requireYcbcrImageArrays_ && !supportsYcbcrImageArrays(device)) {
                interopRejection = "matching Vulkan adapter lacks VK_EXT_ycbcr_image_arrays/samplerYcbcrConversion "
                                   "(required for the layered D3D11 NV12 decoder array)";
                continue;
            }
            VkFormatProperties yProperties{}, uvProperties{};
            vkGetPhysicalDeviceFormatProperties(device, VK_FORMAT_R8_UNORM, &yProperties);
            vkGetPhysicalDeviceFormatProperties(device, VK_FORMAT_R8G8_UNORM, &uvProperties);
            if ((yProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0 ||
                (uvProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0) {
                interopRejection = "matching Vulkan adapter cannot sample R8/R8G8 NV12 plane views";
                continue;
            }
        }
        VkFormatProperties rgba16{};
        vkGetPhysicalDeviceFormatProperties(device, VK_FORMAT_R16G16B16A16_SFLOAT, &rgba16);
        constexpr VkFormatFeatureFlags required = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((rgba16.optimalTilingFeatures & required) != required) continue;

        std::uint32_t familyCount{};
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());
        for (std::uint32_t family = 0; family < familyCount; ++family) {
            VkBool32 present{};
            vkGetPhysicalDeviceSurfaceSupportKHR(device, family, surface_, &present);
            const auto requiredQueues = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if (present && (families[family].queueFlags & requiredQueues) == requiredQueues) {
                const int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 100 : 10;
                if (score > bestScore) {
                    bestScore = score;
                    physicalDevice_ = device;
                    graphicsFamily_ = family;
                    properties_ = properties;
                }
            }
        }
    }
    if (!physicalDevice_) {
        if (requiredAdapterLuid_) {
            if (!matchingLuidSeen) {
                throw std::runtime_error("No presentation-capable Vulkan device has the D3D11VA decoder adapter LUID");
            }
            if (!interopRejection.empty()) throw std::runtime_error("D3D11/Vulkan interop unavailable: " + interopRejection);
        }
        throw std::runtime_error("No suitable Vulkan 1.2 device: graphics+compute+present queue, timestamp queries, VK_KHR_swapchain, and RGBA16F sampled/storage images are required");
    }
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties_);
    gpuName_ = properties_.deviceName;
}

void VulkanContext::createDevice() {
    constexpr float priority = 1.0F;
    const VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, graphicsFamily_, 1, &priority};
    std::vector<const char*> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (requiredAdapterLuid_) {
#ifdef _WIN32
        extensions.push_back("VK_KHR_external_memory_win32");
        extensions.push_back("VK_KHR_external_semaphore_win32");
#endif
    }
    if (requireYcbcrImageArrays_) extensions.push_back(VK_EXT_YCBCR_IMAGE_ARRAYS_EXTENSION_NAME);
    VkPhysicalDeviceFeatures features{};
    features.shaderStorageImageExtendedFormats = VK_TRUE;
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcrConversion{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES};
    ycbcrConversion.samplerYcbcrConversion = VK_TRUE;
    VkPhysicalDeviceYcbcrImageArraysFeaturesEXT ycbcrArrays{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_YCBCR_IMAGE_ARRAYS_FEATURES_EXT};
    ycbcrArrays.pNext = &ycbcrConversion;
    ycbcrArrays.ycbcrImageArrays = VK_TRUE;
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    if (requireYcbcrImageArrays_) timeline.pNext = &ycbcrArrays;
    if (requiredAdapterLuid_) timeline.timelineSemaphore = VK_TRUE;
    VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    if (requiredAdapterLuid_) create.pNext = &timeline;
    ycbcrImageArraysEnabled_ = requireYcbcrImageArrays_;
    create.queueCreateInfoCount = 1;
    create.pQueueCreateInfos = &queue;
    create.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    create.ppEnabledExtensionNames = extensions.data();
    create.pEnabledFeatures = &features;
    vkCheck(vkCreateDevice(physicalDevice_, &create, nullptr, &device_), "vkCreateDevice");
    vkGetDeviceQueue(device_, graphicsFamily_, 0, &graphicsQueue_);
    nameObject(VK_OBJECT_TYPE_DEVICE, reinterpret_cast<std::uint64_t>(device_), "NexusStream60 logical device");
    Log::info(std::format("Vulkan loader/device API: {} / {}", apiVersion(VK_API_VERSION_1_2), apiVersion(properties_.apiVersion)));
    Log::info(std::format("Selected GPU: {} (driver {}, timestamp period {:.3f} ns)", gpuName_, properties_.driverVersion, properties_.limits.timestampPeriod));
    Log::info(std::format("Graphics/compute/present queue family: {}", graphicsFamily_));
    if (hasDeviceExtension(physicalDevice_, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME))
        Log::debug("VK_KHR_synchronization2 is available; Phase 1 uses explicit legacy barriers for Vulkan 1.2 portability");
}

void VulkanContext::createCommandResources() {
    const VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, graphicsFamily_};
    vkCheck(vkCreateCommandPool(device_, &pool, nullptr, &commandPool_), "vkCreateCommandPool");
    std::array<VkCommandBuffer, framesInFlight> commands{};
    const VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr,
        commandPool_, VK_COMMAND_BUFFER_LEVEL_PRIMARY, framesInFlight};
    vkCheck(vkAllocateCommandBuffers(device_, &allocate, commands.data()), "vkAllocateCommandBuffers");
    for (std::uint32_t i = 0; i < framesInFlight; ++i) {
        flights_[i].commandBuffer = commands[i];
        const VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        const VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
        vkCheck(vkCreateSemaphore(device_, &semaphore, nullptr, &flights_[i].imageAvailable), "vkCreateSemaphore(image available)");
        vkCheck(vkCreateFence(device_, &fence, nullptr, &flights_[i].fence), "vkCreateFence");
        nameObject(VK_OBJECT_TYPE_COMMAND_BUFFER, reinterpret_cast<std::uint64_t>(commands[i]), std::format("Frame {} command buffer", i).c_str());
    }
    const VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0,
        VK_QUERY_TYPE_TIMESTAMP, framesInFlight * timestampsPerFrame, {}};
    vkCheck(vkCreateQueryPool(device_, &query, nullptr, &queryPool_), "vkCreateQueryPool");
    nameObject(VK_OBJECT_TYPE_QUERY_POOL, reinterpret_cast<std::uint64_t>(queryPool_), "GPU frame timestamps");
}

void VulkanContext::createSwapchain() {
    VkSurfaceCapabilitiesKHR capabilities{};
    vkCheck(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &capabilities), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    std::uint32_t formatCount{};
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, nullptr);
    if (!formatCount) throw std::runtime_error("Vulkan surface reports no swapchain formats");
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, formats.data());
    const auto preferred = std::find_if(formats.begin(), formats.end(), [](const auto& format) {
        return (format.format == VK_FORMAT_B8G8R8A8_SRGB || format.format == VK_FORMAT_R8G8B8A8_SRGB) &&
               format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    surfaceFormat_ = preferred != formats.end() ? *preferred : formats.front();

    std::uint32_t modeCount{};
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &modeCount, nullptr);
    std::vector<VkPresentModeKHR> modes(modeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &modeCount, modes.data());
    presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync_) {
        if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end()) presentMode_ = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_IMMEDIATE_KHR) != modes.end()) presentMode_ = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    presentModeName_ = presentModeText(presentMode_);

    int width{}, height{};
    (void)window_.framebufferExtent(width, height);
    if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) swapchainExtent_ = capabilities.currentExtent;
    else {
        swapchainExtent_.width = std::clamp(static_cast<std::uint32_t>(std::max(width, 1)), capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        swapchainExtent_.height = std::clamp(static_cast<std::uint32_t>(std::max(height, 1)), capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }
    std::uint32_t imageCount = std::max(capabilities.minImageCount + 1, framesInFlight);
    if (capabilities.maxImageCount != 0) imageCount = std::min(imageCount, capabilities.maxImageCount);
    VkSwapchainCreateInfoKHR create{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    create.surface = surface_;
    create.minImageCount = imageCount;
    create.imageFormat = surfaceFormat_.format;
    create.imageColorSpace = surfaceFormat_.colorSpace;
    create.imageExtent = swapchainExtent_;
    create.imageArrayLayers = 1;
    create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create.preTransform = capabilities.currentTransform;
    create.compositeAlpha = (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
        ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    create.presentMode = presentMode_;
    create.clipped = VK_TRUE;
    vkCheck(vkCreateSwapchainKHR(device_, &create, nullptr, &swapchain_), "vkCreateSwapchainKHR");
    nameObject(VK_OBJECT_TYPE_SWAPCHAIN_KHR, reinterpret_cast<std::uint64_t>(swapchain_), "NexusStream60 swapchain");

    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
    swapchainImages_.resize(imageCount);
    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchainImages_.data());
    swapchainViews_.resize(imageCount);
    for (std::uint32_t i = 0; i < imageCount; ++i) {
        const VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, swapchainImages_[i],
            VK_IMAGE_VIEW_TYPE_2D, surfaceFormat_.format, {}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCheck(vkCreateImageView(device_, &view, nullptr, &swapchainViews_[i]), "vkCreateImageView(swapchain)");
    }
    createRenderPass();
    framebuffers_.resize(imageCount);
    for (std::uint32_t i = 0; i < imageCount; ++i) {
        const VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0,
            renderPass_, 1, &swapchainViews_[i], swapchainExtent_.width, swapchainExtent_.height, 1};
        vkCheck(vkCreateFramebuffer(device_, &framebuffer, nullptr, &framebuffers_[i]), "vkCreateFramebuffer");
    }
    imagesInFlight_.assign(imageCount, VK_NULL_HANDLE);
    renderComplete_.assign(imageCount, VK_NULL_HANDLE);
    const VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto& renderComplete : renderComplete_) {
        vkCheck(vkCreateSemaphore(device_, &semaphore, nullptr, &renderComplete), "vkCreateSemaphore(render complete)");
    }
    Log::info(std::format("Swapchain: format {} colorspace {}, {}x{}, present mode {}, {} images",
        static_cast<int>(surfaceFormat_.format), static_cast<int>(surfaceFormat_.colorSpace),
        swapchainExtent_.width, swapchainExtent_.height, presentModeName_, imageCount));
}

void VulkanContext::createRenderPass() {
    const VkAttachmentDescription color{0, surfaceFormat_.format, VK_SAMPLE_COUNT_1_BIT,
        VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
    const VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkSubpassDescription subpass{0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, nullptr, 1, &reference, nullptr, nullptr, 0, nullptr};
    const VkSubpassDependency dependency{VK_SUBPASS_EXTERNAL, 0,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0};
    const VkRenderPassCreateInfo create{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0, 1, &color, 1, &subpass, 1, &dependency};
    vkCheck(vkCreateRenderPass(device_, &create, nullptr, &renderPass_), "vkCreateRenderPass");
    nameObject(VK_OBJECT_TYPE_RENDER_PASS, reinterpret_cast<std::uint64_t>(renderPass_), "Swapchain present render pass");
}

void VulkanContext::destroySwapchain() noexcept {
    for (const auto framebuffer : framebuffers_) vkDestroyFramebuffer(device_, framebuffer, nullptr);
    framebuffers_.clear();
    if (renderPass_) vkDestroyRenderPass(device_, renderPass_, nullptr);
    renderPass_ = VK_NULL_HANDLE;
    for (const auto view : swapchainViews_) vkDestroyImageView(device_, view, nullptr);
    swapchainViews_.clear();
    swapchainImages_.clear();
    imagesInFlight_.clear();
    for (const auto renderComplete : renderComplete_) if (renderComplete) vkDestroySemaphore(device_, renderComplete, nullptr);
    renderComplete_.clear();
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

void VulkanContext::recreateSwapchain() {
    int width{}, height{};
    while (!window_.framebufferExtent(width, height) && !window_.shouldClose()) window_.waitEvents(0.1);
    if (window_.shouldClose()) return;
    waitIdle();
    destroySwapchain();
    createSwapchain();
}

std::optional<VulkanContext::AcquiredFrame> VulkanContext::acquireFrame() {
    auto& flight = flights_[currentFlight_];
    vkCheck(vkWaitForFences(device_, 1, &flight.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    const auto timings = flight.submitted ? collectTimings(currentFlight_) : std::nullopt;
    std::uint32_t imageIndex{};
    const VkResult acquire = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, flight.imageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) { recreateSwapchain(); return std::nullopt; }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) vkCheck(acquire, "vkAcquireNextImageKHR");
    if (imagesInFlight_[imageIndex]) vkCheck(vkWaitForFences(device_, 1, &imagesInFlight_[imageIndex], VK_TRUE, UINT64_MAX), "vkWaitForFences(swapchain image)");
    imagesInFlight_[imageIndex] = flight.fence;
    vkCheck(vkResetFences(device_, 1, &flight.fence), "vkResetFences");
    vkCheck(vkResetCommandBuffer(flight.commandBuffer, 0), "vkResetCommandBuffer");
    const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
    vkCheck(vkBeginCommandBuffer(flight.commandBuffer, &begin), "vkBeginCommandBuffer");
    vkCmdResetQueryPool(flight.commandBuffer, queryPool_, currentFlight_ * timestampsPerFrame, timestampsPerFrame);
    return AcquiredFrame{currentFlight_, imageIndex, flight.commandBuffer, timings};
}

void VulkanContext::submitAndPresent(const AcquiredFrame& frame, std::optional<TimelineWait> timelineWait) {
    vkCheck(vkEndCommandBuffer(frame.commandBuffer), "vkEndCommandBuffer");
    auto& flight = flights_[frame.flightIndex];
    constexpr VkPipelineStageFlags imageWaitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    std::array<VkSemaphore, 2> waitSemaphores{flight.imageAvailable, VK_NULL_HANDLE};
    std::array<VkPipelineStageFlags, 2> waitStages{imageWaitStage, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT};
    std::array<std::uint64_t, 2> waitValues{0, 0};
    const std::uint32_t waitCount = timelineWait ? 2U : 1U;
    if (timelineWait) {
        waitSemaphores[1] = timelineWait->semaphore;
        waitStages[1] = timelineWait->stage;
        waitValues[1] = timelineWait->value;
    }
    const std::uint64_t signalValue{};
    VkTimelineSemaphoreSubmitInfo timelineInfo{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timelineInfo.waitSemaphoreValueCount = waitCount;
    timelineInfo.pWaitSemaphoreValues = waitValues.data();
    timelineInfo.signalSemaphoreValueCount = 1;
    timelineInfo.pSignalSemaphoreValues = &signalValue;
    const VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, timelineWait ? &timelineInfo : nullptr,
        waitCount, waitSemaphores.data(), waitStages.data(), 1, &frame.commandBuffer, 1, &renderComplete_[frame.imageIndex]};
    vkCheck(vkQueueSubmit(graphicsQueue_, 1, &submit, flight.fence), "vkQueueSubmit");
    flight.submitted = true;
    const VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &renderComplete_[frame.imageIndex],
        1, &swapchain_, &frame.imageIndex, nullptr};
    const VkResult result = vkQueuePresentKHR(graphicsQueue_, &present);
    currentFlight_ = (currentFlight_ + 1) % framesInFlight;
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) recreateSwapchain();
    else vkCheck(result, "vkQueuePresentKHR");
}

void VulkanContext::setTimingPlan(std::uint32_t flight, const std::array<GpuPass, 4>& stages, std::uint32_t count) noexcept {
    if (flight >= framesInFlight) return;
    flights_[flight].timingStages = stages;
    flights_[flight].timingStageCount = std::min<std::uint32_t>(count, static_cast<std::uint32_t>(stages.size()));
}

std::optional<GpuTimings> VulkanContext::collectTimings(std::uint32_t flight) const {
    std::array<std::uint64_t, timestampsPerFrame> ticks{};
    const VkResult result = vkGetQueryPoolResults(device_, queryPool_, flight * timestampsPerFrame, timestampsPerFrame,
        sizeof(ticks), ticks.data(), sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
    if (result == VK_NOT_READY) return std::nullopt;
    vkCheck(result, "vkGetQueryPoolResults");
    const double toMs = static_cast<double>(properties_.limits.timestampPeriod) / 1'000'000.0;
    GpuTimings timings{};
    timings.uploadMs = (ticks[1] - ticks[0]) * toMs;
    timings.colorMs = (ticks[2] - ticks[1]) * toMs;
    std::uint32_t previous = 2;
    const auto& plan = flights_[flight];
    for (std::uint32_t index = 0; index < plan.timingStageCount; ++index) {
        const double elapsed = (ticks[3 + index] - ticks[previous]) * toMs;
        const auto pass = plan.timingStages[index];
        auto& value = timings.passes[static_cast<std::size_t>(pass)];
        value = value.value_or(0.0) + elapsed;
        if (pass == GpuPass::Cas || pass == GpuPass::Rcas) timings.postProcessingMs += elapsed;
        else timings.reconstructionMs += elapsed;
        previous = 3 + index;
    }
    timings.presentMs = (ticks[7] - ticks[6]) * toMs;
    timings.totalMs = (ticks[8] - ticks[0]) * toMs;
    return timings;
}

bool VulkanContext::swapchainIsSrgb() const noexcept {
    return surfaceFormat_.format == VK_FORMAT_B8G8R8A8_SRGB || surfaceFormat_.format == VK_FORMAT_R8G8B8A8_SRGB;
}

void VulkanContext::waitIdle() const noexcept { if (device_) vkDeviceWaitIdle(device_); }

void VulkanContext::nameObject(VkObjectType type, std::uint64_t handle, const char* name) const noexcept {
    if (!validationEnabled_ || !device_ || !handle || !name) return;
    const auto setName = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(vkGetDeviceProcAddr(device_, "vkSetDebugUtilsObjectNameEXT"));
    if (!setName) return;
    const VkDebugUtilsObjectNameInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, nullptr, type, handle, name};
    (void)setName(device_, &info);
}

} // namespace ns60
