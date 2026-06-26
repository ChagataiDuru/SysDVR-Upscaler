#include "decode/DecoderCapabilities.h"

#include "decode/FFmpegRuntime.h"

#ifdef _WIN32
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#endif
#include <vulkan/vulkan.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#ifdef _WIN32
#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>
#include <wrl/client.h>
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <iomanip>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ns60 {
namespace {
constexpr std::size_t LuidSize = 8;

struct LuidSummary {
    std::array<std::uint8_t, LuidSize> bytes{};
    bool valid{};
};

struct D3D11AdapterSummary {
    bool available{};
    std::string name;
    std::string featureLevel;
    LuidSummary luid;
    std::string error;
};

struct FFmpegDeviceStatus {
    bool available{};
    std::string error;
    std::vector<std::string> validHwFormats;
    std::vector<std::string> validSwFormats;
};

struct VulkanDeviceSummary {
    std::string name;
    std::string type;
    std::string apiVersion;
    std::uint32_t apiVersionValue{};
    std::uint32_t driverVersion{};
    bool supportsTimestamp{};
    bool hasGraphicsComputeQueue{};
    bool supportsRgba16StorageSampled{};
    bool externalMemory{};
    bool externalMemoryWin32{};
    bool externalSemaphore{};
    bool externalSemaphoreWin32{};
    bool externalFence{};
    bool externalFenceWin32{};
    bool d3d11TextureImportRgba8{};
    bool d3d11TextureImportNv12{};
    bool externalSemaphoreImportExport{};
    bool externalFenceImportExport{};
    LuidSummary luid;
};

struct VulkanAudit {
    std::string loaderApiVersion;
    std::vector<VulkanDeviceSummary> devices;
    std::optional<std::size_t> selectedIndex;
    std::string error;
};

std::string yesNo(bool value) { return value ? "yes" : "no"; }

std::string join(const std::vector<std::string>& values, std::string_view separator) {
    if (values.empty()) return "none";
    std::ostringstream output;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) output << separator;
        output << values[index];
    }
    return output.str();
}

std::string formatApiVersion(std::uint32_t version) {
    return std::format("{}.{}.{}", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version), VK_VERSION_PATCH(version));
}

std::string formatVkResult(VkResult result) { return std::format("VkResult {}", static_cast<int>(result)); }

std::string formatLuid(const LuidSummary& luid) {
    if (!luid.valid) return "unavailable";
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < luid.bytes.size(); ++index) {
        if (index != 0) output << '-';
        output << std::setw(2) << static_cast<unsigned>(luid.bytes[index]);
    }
    return output.str();
}

std::string pixelFormatName(AVPixelFormat format) {
    if (const char* name = av_get_pix_fmt_name(format)) return name;
    return std::format("unknown({})", static_cast<int>(format));
}

std::string hwDeviceTypeName(AVHWDeviceType type) {
    if (const char* name = av_hwdevice_get_type_name(type)) return name;
    return std::format("unknown({})", static_cast<int>(type));
}

std::vector<std::string> compiledHardwareDeviceTypes() {
    std::vector<std::string> names;
    AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
    while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE) names.push_back(hwDeviceTypeName(type));
    return names;
}

std::string hwConfigMethods(int methods) {
    std::vector<std::string> names;
    if ((methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0) names.emplace_back("hw_device_ctx");
    if ((methods & AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX) != 0) names.emplace_back("hw_frames_ctx");
    if ((methods & AV_CODEC_HW_CONFIG_METHOD_INTERNAL) != 0) names.emplace_back("internal");
    if ((methods & AV_CODEC_HW_CONFIG_METHOD_AD_HOC) != 0) names.emplace_back("ad_hoc");
    return join(names, "|");
}

std::vector<std::string> h264Profiles(const AVCodec* decoder) {
    std::vector<std::string> profiles;
    if (!decoder || !decoder->profiles) return profiles;
    for (const AVProfile* profile = decoder->profiles; profile->profile != FF_PROFILE_UNKNOWN; ++profile) {
        if (profile->name) profiles.emplace_back(profile->name);
    }
    return profiles;
}

std::vector<std::string> h264HardwareConfigs(const AVCodec* decoder) {
    std::vector<std::string> configs;
    if (!decoder) return configs;
    for (int index = 0;; ++index) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(decoder, index);
        if (!config) break;
        configs.push_back(std::format("{}: hw_format={}, methods={}", hwDeviceTypeName(config->device_type),
            pixelFormatName(config->pix_fmt), hwConfigMethods(config->methods)));
    }
    return configs;
}

void appendPixelFormats(const AVPixelFormat* formats, std::vector<std::string>& output) {
    if (!formats) return;
    for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format) output.push_back(pixelFormatName(*format));
}

FFmpegDeviceStatus queryFFmpegDevice(AVHWDeviceType type) {
    FFmpegDeviceStatus status;
    AVBufferRef* device = nullptr;
    const int result = av_hwdevice_ctx_create(&device, type, nullptr, nullptr, 0);
    if (result < 0) {
        status.error = ffmpegError(result);
        return status;
    }
    status.available = true;
    AVHWFramesConstraints* constraints = av_hwdevice_get_hwframe_constraints(device, nullptr);
    if (constraints) {
        appendPixelFormats(constraints->valid_hw_formats, status.validHwFormats);
        appendPixelFormats(constraints->valid_sw_formats, status.validSwFormats);
        av_hwframe_constraints_free(&constraints);
    }
    av_buffer_unref(&device);
    return status;
}

bool hasExtension(const std::vector<std::string>& extensions, std::string_view name) {
    return std::find(extensions.begin(), extensions.end(), name) != extensions.end();
}

std::vector<std::string> enumerateDeviceExtensions(VkPhysicalDevice device) {
    std::uint32_t count{};
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS) return {};
    std::vector<VkExtensionProperties> properties(count);
    if (count != 0 && vkEnumerateDeviceExtensionProperties(device, nullptr, &count, properties.data()) != VK_SUCCESS) return {};
    std::vector<std::string> names;
    names.reserve(properties.size());
    for (const auto& property : properties) names.emplace_back(property.extensionName);
    return names;
}

bool hasGraphicsComputeQueue(VkPhysicalDevice device) {
    std::uint32_t count{};
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    if (count != 0) vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    return std::any_of(families.begin(), families.end(), [](const VkQueueFamilyProperties& family) {
        constexpr VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        return (family.queueFlags & required) == required;
    });
}

bool supportsRgba16StorageSampled(VkPhysicalDevice device) {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(device, VK_FORMAT_R16G16B16A16_SFLOAT, &properties);
    constexpr VkFormatFeatureFlags required = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    return (properties.optimalTilingFeatures & required) == required;
}

bool queryExternalImageImport(PFN_vkGetPhysicalDeviceImageFormatProperties2 getImageProperties2, VkPhysicalDevice device, VkFormat format) {
#ifdef _WIN32
    if (!getImageProperties2) return false;
    VkPhysicalDeviceExternalImageFormatInfo externalInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
    externalInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    VkPhysicalDeviceImageFormatInfo2 imageInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
    imageInfo.pNext = &externalInfo;
    imageInfo.format = format;
    imageInfo.type = VK_IMAGE_TYPE_2D;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    VkExternalImageFormatProperties externalProperties{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 imageProperties{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    imageProperties.pNext = &externalProperties;
    const VkResult result = getImageProperties2(device, &imageInfo, &imageProperties);
    return result == VK_SUCCESS && (externalProperties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0;
#else
    (void)getImageProperties2;
    (void)device;
    (void)format;
    return false;
#endif
}

bool queryExternalSemaphoreImportExport(PFN_vkGetPhysicalDeviceExternalSemaphoreProperties getSemaphoreProperties, VkPhysicalDevice device) {
    if (!getSemaphoreProperties) return false;
    VkPhysicalDeviceExternalSemaphoreInfo info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
#ifdef _WIN32
    info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
    info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
    VkExternalSemaphoreProperties properties{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
    getSemaphoreProperties(device, &info, &properties);
    constexpr VkExternalSemaphoreFeatureFlags required = VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT | VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT;
    return (properties.externalSemaphoreFeatures & required) == required;
}

bool queryExternalFenceImportExport(PFN_vkGetPhysicalDeviceExternalFenceProperties getFenceProperties, VkPhysicalDevice device) {
    if (!getFenceProperties) return false;
    VkPhysicalDeviceExternalFenceInfo info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO};
#ifdef _WIN32
    info.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
    info.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
    VkExternalFenceProperties properties{VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES};
    getFenceProperties(device, &info, &properties);
    constexpr VkExternalFenceFeatureFlags required = VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT | VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT;
    return (properties.externalFenceFeatures & required) == required;
}

std::string physicalDeviceTypeName(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
    default: return "other";
    }
}

int physicalDeviceScore(const VulkanDeviceSummary& device) {
    if (device.apiVersionValue < VK_API_VERSION_1_2 || !device.supportsTimestamp || !device.hasGraphicsComputeQueue || !device.supportsRgba16StorageSampled) return -1;
    if (device.type == "discrete") return 100;
    if (device.type == "integrated") return 50;
    return 10;
}

VulkanAudit queryVulkanAudit() {
    VulkanAudit audit;
    std::uint32_t loaderVersion = VK_API_VERSION_1_0;
    if (const auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"))) {
        const VkResult result = enumerateVersion(&loaderVersion);
        if (result != VK_SUCCESS) {
            audit.error = "vkEnumerateInstanceVersion failed with " + formatVkResult(result);
            return audit;
        }
    }
    audit.loaderApiVersion = formatApiVersion(loaderVersion);

    const std::uint32_t requestedApi = loaderVersion < VK_API_VERSION_1_2 ? loaderVersion : VK_API_VERSION_1_2;
    const VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "NexusStream60", VK_MAKE_VERSION(0, 1, 0),
        "NexusStream60", VK_MAKE_VERSION(0, 1, 0), requestedApi};
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create.pApplicationInfo = &application;
    VkInstance instance{};
    VkResult result = vkCreateInstance(&create, nullptr, &instance);
    if (result != VK_SUCCESS) {
        audit.error = "vkCreateInstance failed with " + formatVkResult(result);
        return audit;
    }

    const auto getProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2"));
    const auto getImageProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceImageFormatProperties2"));
    const auto getSemaphoreProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceExternalSemaphoreProperties>(vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceExternalSemaphoreProperties"));
    const auto getFenceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceExternalFenceProperties>(vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceExternalFenceProperties"));

    std::uint32_t count{};
    result = vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (result != VK_SUCCESS) {
        audit.error = "vkEnumeratePhysicalDevices failed with " + formatVkResult(result);
        vkDestroyInstance(instance, nullptr);
        return audit;
    }
    std::vector<VkPhysicalDevice> physicalDevices(count);
    if (count != 0) {
        result = vkEnumeratePhysicalDevices(instance, &count, physicalDevices.data());
        if (result != VK_SUCCESS) {
            audit.error = "vkEnumeratePhysicalDevices failed with " + formatVkResult(result);
            vkDestroyInstance(instance, nullptr);
            return audit;
        }
    }

    int bestScore = -1;
    for (const VkPhysicalDevice physicalDevice : physicalDevices) {
        VulkanDeviceSummary device;
        VkPhysicalDeviceProperties properties{};
        if (getProperties2) {
            VkPhysicalDeviceIDProperties idProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            properties2.pNext = &idProperties;
            getProperties2(physicalDevice, &properties2);
            properties = properties2.properties;
            if (idProperties.deviceLUIDValid == VK_TRUE) {
                device.luid.valid = true;
                std::memcpy(device.luid.bytes.data(), idProperties.deviceLUID, device.luid.bytes.size());
            }
        } else {
            vkGetPhysicalDeviceProperties(physicalDevice, &properties);
        }

        device.name = properties.deviceName;
        device.type = physicalDeviceTypeName(properties.deviceType);
        device.apiVersionValue = properties.apiVersion;
        device.apiVersion = formatApiVersion(properties.apiVersion);
        device.driverVersion = properties.driverVersion;
        device.supportsTimestamp = properties.limits.timestampComputeAndGraphics == VK_TRUE;
        device.hasGraphicsComputeQueue = hasGraphicsComputeQueue(physicalDevice);
        device.supportsRgba16StorageSampled = supportsRgba16StorageSampled(physicalDevice);

        const auto extensions = enumerateDeviceExtensions(physicalDevice);
        device.externalMemory = hasExtension(extensions, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
        device.externalSemaphore = hasExtension(extensions, VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME);
        device.externalFence = hasExtension(extensions, VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME);
#ifdef _WIN32
        device.externalMemoryWin32 = hasExtension(extensions, "VK_KHR_external_memory_win32");
        device.externalSemaphoreWin32 = hasExtension(extensions, "VK_KHR_external_semaphore_win32");
        device.externalFenceWin32 = hasExtension(extensions, "VK_KHR_external_fence_win32");
#endif
        device.d3d11TextureImportRgba8 = queryExternalImageImport(getImageProperties2, physicalDevice, VK_FORMAT_R8G8B8A8_UNORM);
        device.d3d11TextureImportNv12 = queryExternalImageImport(getImageProperties2, physicalDevice, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);
        device.externalSemaphoreImportExport = queryExternalSemaphoreImportExport(getSemaphoreProperties, physicalDevice);
        device.externalFenceImportExport = queryExternalFenceImportExport(getFenceProperties, physicalDevice);

        const int score = physicalDeviceScore(device);
        const std::size_t index = audit.devices.size();
        if (score > bestScore) {
            bestScore = score;
            audit.selectedIndex = index;
        }
        audit.devices.push_back(std::move(device));
    }

    vkDestroyInstance(instance, nullptr);
    return audit;
}

#ifdef _WIN32
std::string utf8FromWide(const wchar_t* text) {
    if (!text) return {};
    const int wideLength = static_cast<int>(std::char_traits<wchar_t>::length(text));
    if (wideLength == 0) return {};
    const int required = WideCharToMultiByte(CP_UTF8, 0, text, wideLength, nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, wideLength, result.data(), required, nullptr, nullptr);
    return result;
}

std::string featureLevelName(D3D_FEATURE_LEVEL level) {
    switch (level) {
    case D3D_FEATURE_LEVEL_12_1: return "12.1";
    case D3D_FEATURE_LEVEL_12_0: return "12.0";
    case D3D_FEATURE_LEVEL_11_1: return "11.1";
    case D3D_FEATURE_LEVEL_11_0: return "11.0";
    case D3D_FEATURE_LEVEL_10_1: return "10.1";
    case D3D_FEATURE_LEVEL_10_0: return "10.0";
    default: return "unknown";
    }
}
#endif

D3D11AdapterSummary queryD3D11DefaultAdapter() {
    D3D11AdapterSummary summary;
#ifdef _WIN32
    constexpr std::array requestedLevels{D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL selectedLevel{};
    const HRESULT createResult = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, requestedLevels.data(),
        static_cast<UINT>(requestedLevels.size()), D3D11_SDK_VERSION, &device, &selectedLevel, &context);
    if (FAILED(createResult)) {
        summary.error = std::format("D3D11CreateDevice failed with HRESULT 0x{:08x}", static_cast<std::uint32_t>(createResult));
        return summary;
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT result = device.As(&dxgiDevice);
    if (FAILED(result)) {
        summary.error = std::format("ID3D11Device::QueryInterface(IDXGIDevice) failed with HRESULT 0x{:08x}", static_cast<std::uint32_t>(result));
        return summary;
    }
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    result = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(result)) {
        summary.error = std::format("IDXGIDevice::GetAdapter failed with HRESULT 0x{:08x}", static_cast<std::uint32_t>(result));
        return summary;
    }

    DXGI_ADAPTER_DESC description{};
    result = adapter->GetDesc(&description);
    if (FAILED(result)) {
        summary.error = std::format("IDXGIAdapter::GetDesc failed with HRESULT 0x{:08x}", static_cast<std::uint32_t>(result));
        return summary;
    }

    summary.available = true;
    summary.name = utf8FromWide(description.Description);
    summary.featureLevel = featureLevelName(selectedLevel);
    summary.luid.valid = true;
    std::memcpy(summary.luid.bytes.data(), &description.AdapterLuid, summary.luid.bytes.size());
#else
    summary.error = "D3D11 is only available on Windows";
#endif
    return summary;
}

std::string sameGpuText(const D3D11AdapterSummary& d3d11, const VulkanAudit& vulkan) {
    if (!d3d11.available || !d3d11.luid.valid) return "unknown (D3D11 adapter unavailable)";
    if (!vulkan.selectedIndex || *vulkan.selectedIndex >= vulkan.devices.size()) return "unknown (Vulkan selection unavailable)";
    const auto& selected = vulkan.devices[*vulkan.selectedIndex];
    if (!selected.luid.valid) return "unknown (Vulkan LUID unavailable)";
    return d3d11.luid.bytes == selected.luid.bytes ? "yes" : "no";
}

void appendCodecInventory(std::ostringstream& output, const AVCodec* h264Decoder) {
    if (!h264Decoder) {
        output << "H.264 software decoder: unavailable\n";
        return;
    }
    const AVCodecDescriptor* descriptor = avcodec_descriptor_get(AV_CODEC_ID_H264);
    output << "H.264 software decoder: " << h264Decoder->name;
    if (descriptor && descriptor->long_name) output << " (" << descriptor->long_name << ")";
    output << "\n";
    output << "H.264 decoder profiles: " << join(h264Profiles(h264Decoder), ", ") << "\n";
    const auto configs = h264HardwareConfigs(h264Decoder);
    output << "H.264 FFmpeg hardware configs:\n";
    if (configs.empty()) {
        output << "  none\n";
    } else {
        for (const auto& config : configs) output << "  " << config << "\n";
    }
}

void appendVulkanAudit(std::ostringstream& output, const VulkanAudit& vulkan) {
    output << "Vulkan loader API: " << (vulkan.loaderApiVersion.empty() ? "unavailable" : vulkan.loaderApiVersion) << "\n";
    if (!vulkan.error.empty()) {
        output << "Vulkan audit: unavailable (" << vulkan.error << ")\n";
        return;
    }
    output << "Vulkan physical devices: " << vulkan.devices.size() << "\n";
    if (!vulkan.selectedIndex || *vulkan.selectedIndex >= vulkan.devices.size()) {
        output << "Selected Vulkan device: unavailable\n";
        return;
    }
    const auto& device = vulkan.devices[*vulkan.selectedIndex];
    output << "Selected Vulkan device: " << device.name << " (" << device.type << ")\n";
    output << "Vulkan device API/driver: " << device.apiVersion << " / " << device.driverVersion << "\n";
    output << "Vulkan device LUID: " << formatLuid(device.luid) << "\n";
    output << "Renderer baseline features: timestamp=" << yesNo(device.supportsTimestamp)
           << ", graphics+compute=" << yesNo(device.hasGraphicsComputeQueue)
           << ", RGBA16F sampled/storage=" << yesNo(device.supportsRgba16StorageSampled) << "\n";
    output << "External memory extensions: core=" << yesNo(device.externalMemory)
           << ", win32=" << yesNo(device.externalMemoryWin32) << "\n";
    output << "D3D11 texture import: rgba8=" << yesNo(device.d3d11TextureImportRgba8)
           << ", nv12=" << yesNo(device.d3d11TextureImportNv12) << "\n";
    output << "External sync extensions: semaphore=" << yesNo(device.externalSemaphore)
           << ", semaphore_win32=" << yesNo(device.externalSemaphoreWin32)
           << ", fence=" << yesNo(device.externalFence)
           << ", fence_win32=" << yesNo(device.externalFenceWin32) << "\n";
    output << "External sync import/export query: semaphore=" << yesNo(device.externalSemaphoreImportExport)
           << ", fence=" << yesNo(device.externalFenceImportExport) << "\n";
}
} // namespace

std::string decoderListReport() {
    const auto versions = ffmpegVersions();
    const AVCodec* h264Decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
    std::ostringstream output;
    output << "NexusStream60 decoder inventory\n";
    output << "FFmpeg libraries: avformat " << versions.avformat << ", avcodec " << versions.avcodec
           << ", avutil " << versions.avutil << "\n";
    appendCodecInventory(output, h264Decoder);
    output << "FFmpeg hardware device types: " << join(compiledHardwareDeviceTypes(), ", ") << "\n";
    return output.str();
}

std::string decoderCapabilitiesReport() {
    const auto versions = ffmpegVersions();
    const AVCodec* h264Decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
    const auto d3d11va = queryFFmpegDevice(AV_HWDEVICE_TYPE_D3D11VA);
    const auto d3d11 = queryD3D11DefaultAdapter();
    const auto vulkan = queryVulkanAudit();

    std::ostringstream output;
    output << "NexusStream60 Phase 3 decoder capability report\n";
    output << "FFmpeg libraries: avformat " << versions.avformat << ", avcodec " << versions.avcodec
           << ", avutil " << versions.avutil << "\n";
    output << "FFmpeg hardware device types: " << join(compiledHardwareDeviceTypes(), ", ") << "\n";
    appendCodecInventory(output, h264Decoder);
    output << "D3D11VA FFmpeg device: " << (d3d11va.available ? "available" : "unavailable") << "\n";
    if (!d3d11va.available) output << "D3D11VA error: " << d3d11va.error << "\n";
    output << "D3D11VA hardware output formats: " << join(d3d11va.validHwFormats, ", ") << "\n";
    output << "D3D11VA software transfer formats: " << join(d3d11va.validSwFormats, ", ") << "\n";

    output << "D3D11 default adapter: ";
    if (d3d11.available) {
        output << d3d11.name << " (feature level " << d3d11.featureLevel << ")\n";
        output << "D3D11 adapter LUID: " << formatLuid(d3d11.luid) << "\n";
    } else {
        output << "unavailable (" << d3d11.error << ")\n";
    }

    appendVulkanAudit(output, vulkan);
    output << "D3D11/Vulkan same GPU: " << sameGpuText(d3d11, vulkan) << "\n";
    output << "Runtime decoder backends: software, d3d11va, auto (select with --decoder)\n";
    return output.str();
}

} // namespace ns60
