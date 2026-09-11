#pragma once

#include "utility/ColorMath.h"

#include <cstddef>
#include <cstdint>
#include <array>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace ns60 {

enum class FrameTimingKind { ContainerPts, LiveArrival };
enum class DecodedFrameStorage { CpuYuv420P, CpuNv12, D3D11Nv12 };

[[nodiscard]] inline std::string_view toString(DecodedFrameStorage storage) noexcept {
    switch (storage) {
    case DecodedFrameStorage::CpuYuv420P: return "CPU YUV420P";
    case DecodedFrameStorage::CpuNv12: return "CPU NV12";
    case DecodedFrameStorage::D3D11Nv12: return "D3D11 NV12";
    }
    return "unknown";
}

struct VideoFrameMetadata {
    int width{};
    int height{};
    std::int64_t sourcePts{};
    double ptsSeconds{};
    double durationSeconds{};
    std::uint64_t frameNumber{};
    bool keyFrame{};
    FrameTimingKind timingKind{FrameTimingKind::ContainerPts};
    ColorDescription color{};
    DecodedFrameStorage storage{DecodedFrameStorage::CpuYuv420P};
};

struct AdapterLuid {
    std::array<std::uint8_t, 8> bytes{};
    bool valid{};

    [[nodiscard]] friend bool operator==(const AdapterLuid&, const AdapterLuid&) noexcept = default;
};

[[nodiscard]] inline bool adapterLuidsMatch(const AdapterLuid& required, const AdapterLuid& candidate) noexcept {
    return required.valid && candidate.valid && required.bytes == candidate.bytes;
}

struct D3D11TextureDescription {
    std::uintptr_t textureIdentity{};
    std::uint64_t poolGeneration{};
    std::uint32_t textureWidth{};
    std::uint32_t textureHeight{};
    std::uint32_t arraySize{};
    std::uint32_t arraySlice{};
    std::uint64_t readyFenceValue{};
    // Non-zero when the frame is two standalone single-plane textures (R8 luma
    // at textureIdentity, R8G8 chroma here) rather than one NV12 texture.
    std::uintptr_t chromaTextureIdentity{};
};

class PlatformHandle final {
public:
    using Closer = void (*)(std::uintptr_t) noexcept;

    PlatformHandle() = default;
    PlatformHandle(std::uintptr_t value, Closer closer) noexcept : value_(value), closer_(closer) {}
    ~PlatformHandle() { reset(); }
    PlatformHandle(const PlatformHandle&) = delete;
    PlatformHandle& operator=(const PlatformHandle&) = delete;
    PlatformHandle(PlatformHandle&& other) noexcept
        : value_(std::exchange(other.value_, 0)), closer_(std::exchange(other.closer_, nullptr)) {}
    PlatformHandle& operator=(PlatformHandle&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, 0);
            closer_ = std::exchange(other.closer_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] std::uintptr_t get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return value_ != 0; }
    void reset() noexcept {
        if (value_ && closer_) closer_(value_);
        value_ = 0;
        closer_ = nullptr;
    }

private:
    std::uintptr_t value_{};
    Closer closer_{};
};

class D3D11FrameLease {
public:
    virtual ~D3D11FrameLease() = default;
    [[nodiscard]] virtual const D3D11TextureDescription& description() const noexcept = 0;
    [[nodiscard]] virtual AdapterLuid adapterLuid() const noexcept = 0;
    [[nodiscard]] virtual std::uintptr_t fenceIdentity() const noexcept = 0;
    [[nodiscard]] virtual PlatformHandle createTextureHandle() const = 0;
    [[nodiscard]] virtual PlatformHandle createFenceHandle() const = 0;
    // Separate-plane frames only: the shared R8G8 chroma texture.
    [[nodiscard]] virtual PlatformHandle createChromaTextureHandle() const {
        throw std::logic_error("This D3D11 frame has no separate chroma texture");
    }
};

struct ExternalTextureKey {
    std::uint64_t poolGeneration{};
    std::uintptr_t textureIdentity{};
    [[nodiscard]] friend bool operator==(const ExternalTextureKey&, const ExternalTextureKey&) noexcept = default;
};

struct ExternalViewKey {
    ExternalTextureKey texture;
    std::uint32_t arraySlice{};
    [[nodiscard]] friend bool operator==(const ExternalViewKey&, const ExternalViewKey&) noexcept = default;
};

// FFmpeg's recommendation covers decoder-internal reference surfaces. The
// caller supplies every surface that can be retained outside the decoder.
[[nodiscard]] inline std::size_t d3d11InteropPoolSize(std::size_t ffmpegRecommended,
                                                      std::size_t externallyRetained,
                                                      std::size_t maximumArraySize = 2048) {
    if (ffmpegRecommended == 0) ffmpegRecommended = 1;
    if (externallyRetained > maximumArraySize || ffmpegRecommended > maximumArraySize - externallyRetained) {
        throw std::runtime_error("D3D11VA interop surface pool exceeds the supported texture-array size");
    }
    return ffmpegRecommended + externallyRetained;
}

struct DecodedFrame {
    VideoFrameMetadata metadata{};
    DecodedFrameStorage storage{DecodedFrameStorage::CpuYuv420P};
    std::vector<std::byte> yPlane;
    // Planar YUV420P uses uPlane as U. NV12 uses uPlane as interleaved UV.
    std::vector<std::byte> uPlane;
    std::vector<std::byte> vPlane;
    int yStride{};
    int uStride{};
    int vStride{};
    std::shared_ptr<D3D11FrameLease> d3d11Lease;

    DecodedFrame() = default;
    DecodedFrame(const DecodedFrame&) = delete;
    DecodedFrame& operator=(const DecodedFrame&) = delete;
    DecodedFrame(DecodedFrame&&) noexcept = default;
    DecodedFrame& operator=(DecodedFrame&&) noexcept = default;

    void resetPayload() noexcept {
        d3d11Lease.reset();
        storage = DecodedFrameStorage::CpuYuv420P;
        metadata.storage = storage;
    }
};

using Yuv420FrameSlot = DecodedFrame;

} // namespace ns60
