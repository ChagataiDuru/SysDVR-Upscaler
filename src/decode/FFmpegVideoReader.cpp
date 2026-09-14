#include "decode/FFmpegVideoReader.h"

#include "decode/FFmpegRuntime.h"
#include "decode/SysDvrPipeSource.h"
#include "utility/Log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
}

#ifdef _WIN32
#include <libavutil/hwcontext_d3d11va.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>
#endif

namespace ns60 {
namespace {
using Clock = std::chrono::steady_clock;

struct FormatDeleter { void operator()(AVFormatContext* value) const noexcept { avformat_close_input(&value); } };
struct CodecDeleter { void operator()(AVCodecContext* value) const noexcept { avcodec_free_context(&value); } };
struct PacketDeleter { void operator()(AVPacket* value) const noexcept { av_packet_free(&value); } };
struct FrameDeleter { void operator()(AVFrame* value) const noexcept { av_frame_free(&value); } };
struct BufferRefDeleter { void operator()(AVBufferRef* value) const noexcept { av_buffer_unref(&value); } };
using BufferRefPtr = std::unique_ptr<AVBufferRef, BufferRefDeleter>;

double rational(AVRational value) noexcept {
    return value.den != 0 ? av_q2d(value) : 0.0;
}

ColorRange mapRange(AVColorRange range, bool allowDefault) {
    if (range == AVCOL_RANGE_MPEG) return ColorRange::Limited;
    if (range == AVCOL_RANGE_JPEG) return ColorRange::Full;
    if (allowDefault) return ColorRange::Limited;
    throw std::runtime_error("Input color range is unspecified. Phase 1 requires explicit limited/TV or full/JPEG metadata.");
}

ColorMatrix mapMatrix(AVColorSpace matrix, bool allowDefault) {
    switch (matrix) {
    case AVCOL_SPC_BT709: return ColorMatrix::Bt709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: return ColorMatrix::Bt601;
    case AVCOL_SPC_BT2020_NCL: return ColorMatrix::Bt2020Ncl;
    default:
        if (allowDefault) return ColorMatrix::Bt709;
        throw std::runtime_error("Unsupported or unspecified YUV matrix; Phase 1 accepts BT.601, BT.709, or BT.2020 NCL metadata");
    }
}

AVColorRange bestRange(AVColorRange primary, AVColorRange fallback) noexcept {
    return primary != AVCOL_RANGE_UNSPECIFIED ? primary : fallback;
}

AVColorSpace bestMatrix(AVColorSpace primary, AVColorSpace fallback) noexcept {
    return primary != AVCOL_SPC_UNSPECIFIED ? primary : fallback;
}

std::string pixelFormatName(AVPixelFormat format) {
    if (const char* name = av_get_pix_fmt_name(format)) return name;
    return std::format("unknown({})", static_cast<int>(format));
}

bool isPlanar420(AVPixelFormat format) noexcept {
    return format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P;
}

bool isOwnedSlotCompatible(AVPixelFormat format) noexcept {
    return isPlanar420(format) || format == AV_PIX_FMT_NV12;
}

void copyPlane(std::vector<std::byte>& output, int outputStride, const std::uint8_t* input,
               int inputStride, int width, int height) {
    if (!input) throw std::runtime_error("Decoder returned a null YUV plane");
    for (int row = 0; row < height; ++row) {
        std::memcpy(output.data() + static_cast<std::size_t>(row * outputStride),
                    input + static_cast<std::ptrdiff_t>(row) * inputStride,
                    static_cast<std::size_t>(width));
    }
}

void copyPlanar420Frame(Yuv420FrameSlot& destination, const AVFrame& source) {
    destination.storage = DecodedFrameStorage::CpuYuv420P;
    destination.uStride = source.width / 2;
    destination.vStride = source.width / 2;
    copyPlane(destination.yPlane, destination.yStride, source.data[0], source.linesize[0], source.width, source.height);
    copyPlane(destination.uPlane, destination.uStride, source.data[1], source.linesize[1], source.width / 2, source.height / 2);
    copyPlane(destination.vPlane, destination.vStride, source.data[2], source.linesize[2], source.width / 2, source.height / 2);
}

void copyNv12Frame(Yuv420FrameSlot& destination, const AVFrame& source) {
    destination.storage = DecodedFrameStorage::CpuNv12;
    destination.uStride = source.width;
    destination.vStride = source.width / 2;
    copyPlane(destination.yPlane, destination.yStride, source.data[0], source.linesize[0], source.width, source.height);
    copyPlane(destination.uPlane, destination.uStride, source.data[1], source.linesize[1], source.width, source.height / 2);
}

std::vector<AVPixelFormat> preferredTransferFormats(AVBufferRef* device) {
    std::vector<AVPixelFormat> supported;
    AVHWFramesConstraints* constraints = av_hwdevice_get_hwframe_constraints(device, nullptr);
    if (constraints && constraints->valid_sw_formats) {
        for (const AVPixelFormat* format = constraints->valid_sw_formats; *format != AV_PIX_FMT_NONE; ++format) {
            supported.push_back(*format);
        }
    }
    if (constraints) av_hwframe_constraints_free(&constraints);

    if (supported.empty()) return {AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P};

    std::vector<AVPixelFormat> ordered;
    for (const AVPixelFormat preferred : {AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUVJ420P}) {
        if (std::find(supported.begin(), supported.end(), preferred) != supported.end()) ordered.push_back(preferred);
    }
    return ordered;
}

const char* hardwareBackendName(DecoderBackend backend) noexcept {
    switch (backend) {
    case DecoderBackend::D3D11VA: return "D3D11VA";
    case DecoderBackend::VideoToolbox: return "VideoToolbox";
    case DecoderBackend::Software:
    case DecoderBackend::Auto: break;
    }
    return "hardware";
}

AVHWDeviceType hardwareDeviceType(DecoderBackend backend) {
    switch (backend) {
    case DecoderBackend::D3D11VA: return AV_HWDEVICE_TYPE_D3D11VA;
    case DecoderBackend::VideoToolbox: return AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
    case DecoderBackend::Software:
    case DecoderBackend::Auto: break;
    }
    throw std::logic_error(std::format("Decoder backend '{}' has no FFmpeg hardware device", toString(backend)));
}

// --decoder auto tries the platform's native hardware decoder first.
constexpr DecoderBackend platformHardwareBackend() noexcept {
#ifdef __APPLE__
    return DecoderBackend::VideoToolbox;
#else
    return DecoderBackend::D3D11VA;
#endif
}

#ifdef _WIN32
std::string hresultText(HRESULT result) {
    return std::format("HRESULT 0x{:08x}", static_cast<std::uint32_t>(result));
}

void closeNativeHandle(std::uintptr_t value) noexcept {
    if (value) CloseHandle(reinterpret_cast<HANDLE>(value));
}

PlatformHandle duplicateNativeHandle(HANDLE source, std::string_view label) {
    HANDLE duplicate{};
    const BOOL duplicated = DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &duplicate,
                                            0, FALSE, DUPLICATE_SAME_ACCESS);
    if (!duplicated) {
        throw std::runtime_error(std::format("Failed to duplicate the {} shared handle: Win32 error {}",
                                             label, GetLastError()));
    }
    return {reinterpret_cast<std::uintptr_t>(duplicate), closeNativeHandle};
}

// Splits one NV12 texture (read through R8/R8G8 plane views) into standalone
// R8 luma and R8G8 chroma textures. Load/store of UNORM8 values is exact.
constexpr char planeSplitShader[] = R"(
Texture2D<float> lumaIn : register(t0);
Texture2D<float2> chromaIn : register(t1);
RWTexture2D<unorm float> lumaOut : register(u0);
RWTexture2D<unorm float2> chromaOut : register(u1);
cbuffer Parameters : register(b0) { uint width; uint height; uint padding0; uint padding1; };

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    lumaOut[id.xy] = lumaIn.Load(int3(id.xy, 0));
    if (id.x < width / 2 && id.y < height / 2) chromaOut[id.xy] = chromaIn.Load(int3(id.xy, 0));
}
)";

class D3D11InteropRuntime final {
public:
    explicit D3D11InteropRuntime(AVD3D11VADeviceContext& avDevice) : avDevice_(&avDevice) {
        HRESULT result = avDevice.device->QueryInterface(IID_PPV_ARGS(&device_));
        if (FAILED(result)) throw std::runtime_error("D3D11/Vulkan interop requires ID3D11Device5: " + hresultText(result));
        result = avDevice.device_context->QueryInterface(IID_PPV_ARGS(&context_));
        if (FAILED(result)) throw std::runtime_error("D3D11/Vulkan interop requires ID3D11DeviceContext4: " + hresultText(result));
        result = device_->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence_));
        if (FAILED(result)) throw std::runtime_error("Failed to create the shared D3D11 decode fence: " + hresultText(result));
        result = fence_->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fenceHandle_);
        if (FAILED(result)) throw std::runtime_error("Failed to export the D3D11 decode fence: " + hresultText(result));

        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        result = avDevice.device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
        if (FAILED(result)) throw std::runtime_error("Failed to query the D3D11 DXGI device: " + hresultText(result));
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        result = dxgiDevice->GetAdapter(&adapter);
        if (FAILED(result)) throw std::runtime_error("Failed to query the D3D11 adapter: " + hresultText(result));
        DXGI_ADAPTER_DESC adapterDescription{};
        result = adapter->GetDesc(&adapterDescription);
        if (FAILED(result)) throw std::runtime_error("Failed to query the D3D11 adapter LUID: " + hresultText(result));
        luid_.valid = true;
        std::memcpy(luid_.bytes.data(), &adapterDescription.AdapterLuid, luid_.bytes.size());
    }

    ~D3D11InteropRuntime() {
        if (fenceHandle_) CloseHandle(fenceHandle_);
    }

    [[nodiscard]] AdapterLuid adapterLuid() const noexcept { return luid_; }
    [[nodiscard]] std::uintptr_t fenceIdentity() const noexcept { return reinterpret_cast<std::uintptr_t>(fence_.Get()); }

    [[nodiscard]] PlatformHandle createFenceHandle() const {
        return duplicateNativeHandle(fenceHandle_, "D3D11 fence");
    }

    std::uint64_t signalDecodeReady() {
        const std::uint64_t value = ++readyValue_;
        if (avDevice_->lock) avDevice_->lock(avDevice_->lock_ctx);
        const HRESULT result = context_->Signal(fence_.Get(), value);
        if (SUCCEEDED(result)) context_->Flush();
        if (avDevice_->unlock) avDevice_->unlock(avDevice_->lock_ctx);
        if (FAILED(result)) throw std::runtime_error("Failed to signal the D3D11 decode fence: " + hresultText(result));
        return value;
    }

    // Copies one decoder-array slice into a private NV12 texture, splits it into
    // the luma/chroma UAV targets, and signals the fence on the same immediate
    // context, so the Vulkan timeline wait covers the copy and the split.
    std::uint64_t splitAndSignal(ID3D11Texture2D* source, UINT sourceSlice, UINT width, UINT height,
                                 ID3D11UnorderedAccessView* lumaOut, ID3D11UnorderedAccessView* chromaOut) {
        ensurePlaneSplitter();
        ensureStaging(width, height);
        const std::array<std::uint32_t, 4> parameters{width, height, 0, 0};
        const std::uint64_t value = ++readyValue_;
        if (avDevice_->lock) avDevice_->lock(avDevice_->lock_ctx);
        // R8/R8G8 plane views on a single NV12 Texture2D are the canonical D3D11
        // case; creating them on the decoder texture array failed E_INVALIDARG.
        context_->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, source, D3D11CalcSubresource(0, sourceSlice, 1), nullptr);
        context_->UpdateSubresource(splitParameters_.Get(), 0, nullptr, parameters.data(), 0, 0);
        context_->CSSetShader(splitShader_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* shaderResources[2]{stagingLuma_.Get(), stagingChroma_.Get()};
        context_->CSSetShaderResources(0, 2, shaderResources);
        ID3D11UnorderedAccessView* targets[2]{lumaOut, chromaOut};
        context_->CSSetUnorderedAccessViews(0, 2, targets, nullptr);
        ID3D11Buffer* constants = splitParameters_.Get();
        context_->CSSetConstantBuffers(0, 1, &constants);
        context_->Dispatch((width + 15) / 16, (height + 15) / 16, 1);
        // Leave no shared or decoder resource bound to the compute stage.
        ID3D11ShaderResourceView* noShaderResources[2]{};
        ID3D11UnorderedAccessView* noTargets[2]{};
        ID3D11Buffer* noConstants{};
        context_->CSSetShaderResources(0, 2, noShaderResources);
        context_->CSSetUnorderedAccessViews(0, 2, noTargets, nullptr);
        context_->CSSetConstantBuffers(0, 1, &noConstants);
        context_->CSSetShader(nullptr, nullptr, 0);
        const HRESULT result = context_->Signal(fence_.Get(), value);
        if (SUCCEEDED(result)) context_->Flush();
        if (avDevice_->unlock) avDevice_->unlock(avDevice_->lock_ctx);
        if (FAILED(result)) throw std::runtime_error("Failed to signal the D3D11 plane-split fence: " + hresultText(result));
        return value;
    }

    [[nodiscard]] ID3D11Device* device() const noexcept { return device_.Get(); }

private:
    void ensurePlaneSplitter() {
        if (splitShader_) return;
        Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        HRESULT result = D3DCompile(planeSplitShader, sizeof(planeSplitShader) - 1, "ns60_plane_split.hlsl",
                                    nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                    &bytecode, &errors);
        if (FAILED(result)) {
            throw std::runtime_error("Failed to compile the D3D11 NV12 plane-split shader: " +
                (errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
                        : hresultText(result)));
        }
        result = device_->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &splitShader_);
        if (FAILED(result)) throw std::runtime_error("Failed to create the D3D11 plane-split shader: " + hresultText(result));
        D3D11_BUFFER_DESC parameters{};
        parameters.ByteWidth = 16;
        parameters.Usage = D3D11_USAGE_DEFAULT;
        parameters.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        result = device_->CreateBuffer(&parameters, nullptr, &splitParameters_);
        if (FAILED(result)) throw std::runtime_error("Failed to create the D3D11 plane-split constants: " + hresultText(result));
    }

    // Private single NV12 texture each decoder slice is copied into, so it can
    // be read through R8 (luma) and R8G8 (chroma) plane views.
    void ensureStaging(UINT width, UINT height) {
        if (staging_ && stagingWidth_ == width && stagingHeight_ == height) return;
        staging_.Reset();
        stagingLuma_.Reset();
        stagingChroma_.Reset();
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width;
        description.Height = height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_NV12;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HRESULT result = device_->CreateTexture2D(&description, nullptr, &staging_);
        if (FAILED(result)) {
            throw std::runtime_error(std::format("Failed to create the {}x{} NV12 plane-split staging texture: {}",
                                                 width, height, hresultText(result)));
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        view.Format = DXGI_FORMAT_R8_UNORM;
        result = device_->CreateShaderResourceView(staging_.Get(), &view, &stagingLuma_);
        if (FAILED(result)) throw std::runtime_error("Failed to create the NV12 luma shader view: " + hresultText(result));
        view.Format = DXGI_FORMAT_R8G8_UNORM;
        result = device_->CreateShaderResourceView(staging_.Get(), &view, &stagingChroma_);
        if (FAILED(result)) throw std::runtime_error("Failed to create the NV12 chroma shader view: " + hresultText(result));
        stagingWidth_ = width;
        stagingHeight_ = height;
    }

    AVD3D11VADeviceContext* avDevice_{};
    Microsoft::WRL::ComPtr<ID3D11Device5> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context_;
    Microsoft::WRL::ComPtr<ID3D11Fence> fence_;
    HANDLE fenceHandle_{};
    AdapterLuid luid_{};
    std::uint64_t readyValue_{};
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> splitShader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> splitParameters_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> stagingLuma_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> stagingChroma_;
    UINT stagingWidth_{};
    UINT stagingHeight_{};
};

class D3D11SharedTexturePool final {
public:
    D3D11SharedTexturePool(ID3D11Texture2D* texture, std::uint64_t generation)
        : texture_(texture), generation_(generation) {
        if (!texture_) throw std::runtime_error("Cannot export a null D3D11VA decoder texture");
        Microsoft::WRL::ComPtr<IDXGIResource1> resource;
        HRESULT result = texture_->QueryInterface(IID_PPV_ARGS(&resource));
        if (FAILED(result)) throw std::runtime_error("Decoder texture does not expose IDXGIResource1: " + hresultText(result));
        result = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                              nullptr, &handle_);
        if (FAILED(result)) throw std::runtime_error("The real D3D11VA decoder texture cannot be exported: " + hresultText(result));
    }

    ~D3D11SharedTexturePool() {
        if (handle_) CloseHandle(handle_);
    }

    [[nodiscard]] ID3D11Texture2D* texture() const noexcept { return texture_.Get(); }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] PlatformHandle createTextureHandle() const {
        return duplicateNativeHandle(handle_, "D3D11 NV12 texture");
    }

private:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    std::uint64_t generation_{};
    HANDLE handle_{};
};

class FFmpegD3D11FrameLease final : public D3D11FrameLease {
public:
    FFmpegD3D11FrameLease(const AVFrame& source, std::shared_ptr<D3D11InteropRuntime> runtime,
                          std::shared_ptr<D3D11SharedTexturePool> pool, std::uint64_t readyValue)
        : frame_(av_frame_clone(&source)), runtime_(std::move(runtime)), pool_(std::move(pool)) {
        if (!frame_) throw std::bad_alloc();
        auto* texture = reinterpret_cast<ID3D11Texture2D*>(frame_->data[0]);
        if (!texture) throw std::runtime_error("FFmpeg returned a null D3D11 decoder texture");
        if (!pool_ || pool_->texture() != texture) {
            throw std::runtime_error("FFmpeg returned a D3D11 texture outside the configured shared decoder pool");
        }
        D3D11_TEXTURE2D_DESC textureDescription{};
        texture->GetDesc(&textureDescription);
        const auto slice = static_cast<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(frame_->data[1]));
        if (textureDescription.Format != DXGI_FORMAT_NV12) {
            throw std::runtime_error(std::format("D3D11/Vulkan interop requires an NV12 decoder texture; DXGI format is {}",
                                                static_cast<unsigned>(textureDescription.Format)));
        }
        if (slice >= textureDescription.ArraySize) throw std::runtime_error("FFmpeg returned an invalid D3D11 texture-array slice");
        description_ = {reinterpret_cast<std::uintptr_t>(texture), pool_->generation(), textureDescription.Width,
                        textureDescription.Height, textureDescription.ArraySize, static_cast<std::uint32_t>(slice),
                        readyValue};
    }

    ~FFmpegD3D11FrameLease() override = default;
    [[nodiscard]] const D3D11TextureDescription& description() const noexcept override { return description_; }
    [[nodiscard]] AdapterLuid adapterLuid() const noexcept override { return runtime_->adapterLuid(); }
    [[nodiscard]] std::uintptr_t fenceIdentity() const noexcept override { return runtime_->fenceIdentity(); }
    [[nodiscard]] PlatformHandle createFenceHandle() const override { return runtime_->createFenceHandle(); }

    [[nodiscard]] PlatformHandle createTextureHandle() const override {
        return pool_->createTextureHandle();
    }

private:
    std::unique_ptr<AVFrame, FrameDeleter> frame_;
    std::shared_ptr<D3D11InteropRuntime> runtime_;
    std::shared_ptr<D3D11SharedTexturePool> pool_;
    D3D11TextureDescription description_{};
};

// Standalone shared single-plane textures (R8 luma, R8G8 chroma) that each
// decoded NV12 slice is split into for --decoder-path interop-copy. On the
// tested NVIDIA driver, sampling an imported multi-planar NV12 image through
// R8/R8G8 plane views gave corrupt output (decoder array) or GPU faults and
// device loss (standalone NV12 textures); plain single-plane imports avoid it.
// A slot is reused only after every lease referencing it has been released.
class D3D11CopyRing final {
public:
    D3D11CopyRing(ID3D11Device& device, const D3D11_TEXTURE2D_DESC& source, std::size_t count, std::uint64_t generation)
        : width_(source.Width), height_(source.Height), generation_(generation), slots_(count),
          inUse_(std::make_unique<std::atomic<bool>[]>(count)) {
        if (count == 0) throw std::invalid_argument("The D3D11 GPU-copy ring requires at least one texture");
        if ((width_ & 1U) != 0 || (height_ & 1U) != 0) {
            throw std::runtime_error("D3D11 GPU-copy interop requires even decoder texture dimensions");
        }
        for (auto& slot : slots_) {
            createPlane(device, DXGI_FORMAT_R8_UNORM, width_, height_, "luma", slot.luma);
            createPlane(device, DXGI_FORMAT_R8G8_UNORM, width_ / 2, height_ / 2, "chroma", slot.chroma);
        }
    }

    ~D3D11CopyRing() {
        for (const auto& slot : slots_) {
            if (slot.luma.handle) CloseHandle(slot.luma.handle);
            if (slot.chroma.handle) CloseHandle(slot.chroma.handle);
        }
    }
    D3D11CopyRing(const D3D11CopyRing&) = delete;
    D3D11CopyRing& operator=(const D3D11CopyRing&) = delete;

    [[nodiscard]] bool matches(const D3D11_TEXTURE2D_DESC& source) const noexcept {
        return source.Width == width_ && source.Height == height_;
    }

    // Called only from the decoder thread; release() may run on the render thread.
    [[nodiscard]] std::size_t acquire() {
        for (std::size_t attempt = 0; attempt < slots_.size(); ++attempt) {
            const std::size_t index = (next_ + attempt) % slots_.size();
            bool expected = false;
            if (inUse_[index].compare_exchange_strong(expected, true, std::memory_order_acquire)) {
                next_ = (index + 1) % slots_.size();
                return index;
            }
        }
        throw std::runtime_error(std::format("All {} D3D11 GPU-copy textures are still retained", slots_.size()));
    }
    void release(std::size_t index) noexcept { inUse_[index].store(false, std::memory_order_release); }

    [[nodiscard]] ID3D11Texture2D* lumaTexture(std::size_t index) const noexcept { return slots_[index].luma.texture.Get(); }
    [[nodiscard]] ID3D11Texture2D* chromaTexture(std::size_t index) const noexcept { return slots_[index].chroma.texture.Get(); }
    [[nodiscard]] ID3D11UnorderedAccessView* lumaTarget(std::size_t index) const noexcept { return slots_[index].luma.target.Get(); }
    [[nodiscard]] ID3D11UnorderedAccessView* chromaTarget(std::size_t index) const noexcept { return slots_[index].chroma.target.Get(); }
    [[nodiscard]] PlatformHandle createLumaHandle(std::size_t index) const {
        return duplicateNativeHandle(slots_[index].luma.handle, "D3D11 GPU-copy luma texture");
    }
    [[nodiscard]] PlatformHandle createChromaHandle(std::size_t index) const {
        return duplicateNativeHandle(slots_[index].chroma.handle, "D3D11 GPU-copy chroma texture");
    }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] std::size_t size() const noexcept { return slots_.size(); }

private:
    struct Plane {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> target;
        HANDLE handle{};
    };
    struct Slot {
        Plane luma;
        Plane chroma;
    };

    static void createPlane(ID3D11Device& device, DXGI_FORMAT format, UINT width, UINT height,
                            const char* label, Plane& plane) {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width;
        description.Height = height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = format;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        description.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        HRESULT result = device.CreateTexture2D(&description, nullptr, &plane.texture);
        if (FAILED(result)) {
            throw std::runtime_error(std::format("Failed to create the shared {}x{} GPU-copy {} texture: {}",
                                                 width, height, label, hresultText(result)));
        }
        result = device.CreateUnorderedAccessView(plane.texture.Get(), nullptr, &plane.target);
        if (FAILED(result)) {
            throw std::runtime_error(std::format("Failed to create the GPU-copy {} UAV: {}", label, hresultText(result)));
        }
        Microsoft::WRL::ComPtr<IDXGIResource1> resource;
        result = plane.texture.As(&resource);
        if (FAILED(result)) throw std::runtime_error("GPU-copy texture does not expose IDXGIResource1: " + hresultText(result));
        result = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                              nullptr, &plane.handle);
        if (FAILED(result)) {
            throw std::runtime_error(std::format("Failed to export the GPU-copy {} texture: {}", label, hresultText(result)));
        }
    }

    std::uint32_t width_{};
    std::uint32_t height_{};
    std::uint64_t generation_{};
    std::vector<Slot> slots_;
    std::unique_ptr<std::atomic<bool>[]> inUse_;
    std::size_t next_{};
};

class D3D11CopyFrameLease final : public D3D11FrameLease {
public:
    D3D11CopyFrameLease(std::shared_ptr<D3D11InteropRuntime> runtime, std::shared_ptr<D3D11CopyRing> ring,
                        std::size_t slot, std::uint64_t readyValue)
        : runtime_(std::move(runtime)), ring_(std::move(ring)), slot_(slot) {
        description_ = {reinterpret_cast<std::uintptr_t>(ring_->lumaTexture(slot_)), ring_->generation(),
                        ring_->width(), ring_->height(), 1, 0, readyValue,
                        reinterpret_cast<std::uintptr_t>(ring_->chromaTexture(slot_))};
    }
    ~D3D11CopyFrameLease() override { ring_->release(slot_); }
    D3D11CopyFrameLease(const D3D11CopyFrameLease&) = delete;
    D3D11CopyFrameLease& operator=(const D3D11CopyFrameLease&) = delete;

    [[nodiscard]] const D3D11TextureDescription& description() const noexcept override { return description_; }
    [[nodiscard]] AdapterLuid adapterLuid() const noexcept override { return runtime_->adapterLuid(); }
    [[nodiscard]] std::uintptr_t fenceIdentity() const noexcept override { return runtime_->fenceIdentity(); }
    [[nodiscard]] PlatformHandle createFenceHandle() const override { return runtime_->createFenceHandle(); }
    [[nodiscard]] PlatformHandle createTextureHandle() const override { return ring_->createLumaHandle(slot_); }
    [[nodiscard]] PlatformHandle createChromaTextureHandle() const override { return ring_->createChromaHandle(slot_); }

private:
    std::shared_ptr<D3D11InteropRuntime> runtime_;
    std::shared_ptr<D3D11CopyRing> ring_;
    std::size_t slot_{};
    D3D11TextureDescription description_{};
};
#endif
} // namespace

struct FFmpegVideoReader::Impl {
    std::unique_ptr<SysDvrPipeSource> pipeSource;
    std::exception_ptr pipeError;
    AVIOContext* customAvio{};
    std::unique_ptr<AVFormatContext, FormatDeleter> format;
    std::unique_ptr<AVCodecContext, CodecDeleter> codec;
    std::unique_ptr<AVPacket, PacketDeleter> packet{av_packet_alloc()};
    std::unique_ptr<AVFrame, FrameDeleter> frame{av_frame_alloc()};
    std::unique_ptr<AVFrame, FrameDeleter> transferFrame{av_frame_alloc()};
    BufferRefPtr hardwareDevice;
    AVPixelFormat hardwarePixelFormat{AV_PIX_FMT_NONE};
    std::vector<AVPixelFormat> transferFormats;
    std::optional<AVPixelFormat> successfulTransferFormat;
    DecoderBackend requestedBackend{DecoderBackend::Software};
    DecoderBackend activeBackend{DecoderBackend::Software};
    DecoderPath requestedPath{DecoderPath::Readback};
    std::size_t externallyRetainedFrames{};
    std::uint64_t poolGeneration{};
    std::string hardwareFormatError;
#ifdef _WIN32
    std::shared_ptr<D3D11InteropRuntime> interopRuntime;
    std::shared_ptr<D3D11SharedTexturePool> interopPool;
    std::shared_ptr<D3D11CopyRing> copyRing;
#endif
    AVStream* stream{};
    int streamIndex{-1};
    VideoStreamInfo streamInfo;
    bool draining{};
    bool seekable{true};
    bool liveInput{};
    bool allowMetadataDefaults{};
    std::uint64_t frameNumber{};
    std::int64_t lastPts{AV_NOPTS_VALUE};
    double fallbackPts{};
    // A live SysDVR stream joins mid-GOP. VideoToolbox cannot conceal the missing
    // references and reports bad data, so live VideoToolbox decode discards
    // non-key frames until a keyframe decodes, and re-arms that wait on errors.
    static constexpr int maxConsecutiveLiveDecodeErrors = 30;
    bool awaitingLiveKeyframe{};
    int consecutiveLiveDecodeErrors{};

    [[nodiscard]] bool gatesLiveKeyframes() const noexcept {
        return liveInput && activeBackend == DecoderBackend::VideoToolbox;
    }

    void armLiveKeyframeWait() noexcept {
        if (!gatesLiveKeyframes()) return;
        awaitingLiveKeyframe = true;
        codec->skip_frame = AVDISCARD_NONKEY;
    }

    void releaseLiveKeyframeWait() {
        consecutiveLiveDecodeErrors = 0;
        if (!awaitingLiveKeyframe) return;
        awaitingLiveKeyframe = false;
        codec->skip_frame = AVDISCARD_DEFAULT;
        Log::info("VideoToolbox live decode synchronized on a keyframe");
    }

    // Absorbs a live VideoToolbox decode error by waiting for the next keyframe.
    // This never switches backends; persistent failure still ends the stream.
    [[nodiscard]] bool absorbLiveDecodeError(int error) {
        if (!gatesLiveKeyframes() || !hardwareFormatError.empty()) return false;
        if (++consecutiveLiveDecodeErrors > maxConsecutiveLiveDecodeErrors) return false;
        Log::warning(std::format("VideoToolbox rejected live H.264 data ({}); waiting for the next keyframe ({}/{})",
                                 ffmpegError(error), consecutiveLiveDecodeErrors, maxConsecutiveLiveDecodeErrors));
        armLiveKeyframeWait();
        return true;
    }

    explicit Impl(const std::filesystem::path& path, DecoderBackend backend, DecoderPath pathMode,
                  std::size_t retainedFrames)
        : requestedBackend(backend), requestedPath(pathMode), externallyRetainedFrames(retainedFrames) {
        validateRequestedPath();
        const auto utf8 = path.u8string();
        const std::string filename(utf8.begin(), utf8.end());
        openFile(filename);
    }

    explicit Impl(SysDvrPipeInput input, DecoderBackend backend, DecoderPath pathMode,
                  std::size_t retainedFrames)
        : requestedBackend(backend), requestedPath(pathMode), externallyRetainedFrames(retainedFrames) {
        validateRequestedPath();
        if (input.pipeName.empty()) throw std::invalid_argument("SysDVR pipe input requires a pipe name");
        seekable = false;
        liveInput = true;
        allowMetadataDefaults = true;
        openPipe(std::move(input.pipeName));
    }

    void validateRequestedPath() const {
        if (!usesD3D11VulkanInterop(requestedPath)) return;
        if (requestedBackend != DecoderBackend::D3D11VA) {
            throw std::invalid_argument("D3D11/Vulkan interop requires an explicit D3D11VA decoder");
        }
        if (externallyRetainedFrames == 0) {
            throw std::invalid_argument("D3D11/Vulkan interop requires a non-zero retained-frame capacity");
        }
#ifndef _WIN32
        throw std::runtime_error("D3D11/Vulkan interop is only available on Windows");
#endif
    }

    void releaseInterop() noexcept {
        if (!usesD3D11VulkanInterop(requestedPath)) return;
        if (frame) av_frame_unref(frame.get());
        if (transferFrame) av_frame_unref(transferFrame.get());
        if (packet) av_packet_unref(packet.get());
        codec.reset();
#ifdef _WIN32
        copyRing.reset();
        interopPool.reset();
        interopRuntime.reset();
#endif
        hardwareDevice.reset();
    }

    ~Impl() {
        codec.reset();
        hardwareDevice.reset();
        format.reset();
        if (customAvio) {
            av_freep(&customAvio->buffer);
            avio_context_free(&customAvio);
        }
    }

    static int readPipePacket(void* opaque, std::uint8_t* buffer, int bufferSize) noexcept {
        auto* self = static_cast<Impl*>(opaque);
        try {
            const int bytes = self->pipeSource->read(buffer, bufferSize);
            return bytes == 0 ? AVERROR_EOF : bytes;
        } catch (...) {
            if (!self->pipeError) self->pipeError = std::current_exception();
            return AVERROR(EIO);
        }
    }

    static AVPixelFormat getHardwareFormat(AVCodecContext* context, const AVPixelFormat* formats) noexcept {
        auto* self = static_cast<Impl*>(context->opaque);
        if (self) {
            for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
                if (*format == self->hardwarePixelFormat) {
                    if (self->requestedPath != DecoderPath::D3D11VulkanInterop) return *format;
                    try {
                        self->configureInteropFrames(*context);
                        return *format;
                    } catch (const std::exception& error) {
                        self->hardwareFormatError = error.what();
                        return AV_PIX_FMT_NONE;
                    } catch (...) {
                        self->hardwareFormatError = "Unknown failure while configuring the D3D11VA interop surface pool";
                        return AV_PIX_FMT_NONE;
                    }
                }
            }
        }
        return formats && *formats != AV_PIX_FMT_NONE ? *formats : AV_PIX_FMT_NONE;
    }

    void configureInteropFrames(AVCodecContext& context) {
#ifdef _WIN32
        AVBufferRef* rawFrames{};
        const int parametersResult = avcodec_get_hw_frames_parameters(
            &context, hardwareDevice.get(), hardwarePixelFormat, &rawFrames);
        if (parametersResult < 0) {
            throw std::runtime_error("Failed to obtain D3D11VA decoder frame parameters in get_format: " +
                                     ffmpegError(parametersResult));
        }
        BufferRefPtr frames(rawFrames);
        auto* frameContext = reinterpret_cast<AVHWFramesContext*>(frames->data);
        auto* d3dFrames = static_cast<AVD3D11VAFramesContext*>(frameContext->hwctx);
        frameContext->sw_format = AV_PIX_FMT_NV12;
        const auto requestedPoolSize = d3d11InteropPoolSize(
            static_cast<std::size_t>(std::max(frameContext->initial_pool_size, 0)), externallyRetainedFrames,
            D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION);
        frameContext->initial_pool_size = static_cast<int>(requestedPoolSize);
        d3dFrames->BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
        d3dFrames->MiscFlags = D3D11_RESOURCE_MISC_SHARED |
                               D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

        D3D11_TEXTURE2D_DESC requestedDescription{};
        requestedDescription.Width = static_cast<UINT>(frameContext->width);
        requestedDescription.Height = static_cast<UINT>(frameContext->height);
        requestedDescription.MipLevels = 1;
        requestedDescription.ArraySize = static_cast<UINT>(requestedPoolSize);
        requestedDescription.Format = DXGI_FORMAT_NV12;
        requestedDescription.SampleDesc.Count = 1;
        requestedDescription.Usage = D3D11_USAGE_DEFAULT;
        requestedDescription.BindFlags = d3dFrames->BindFlags;
        requestedDescription.MiscFlags = d3dFrames->MiscFlags;
        auto* deviceContext = reinterpret_cast<AVHWDeviceContext*>(hardwareDevice->data);
        auto* d3d11Device = static_cast<AVD3D11VADeviceContext*>(deviceContext->hwctx);
        Microsoft::WRL::ComPtr<ID3D11Texture2D> validationTexture;
        const HRESULT validationResult = d3d11Device->device->CreateTexture2D(
            &requestedDescription, nullptr, &validationTexture);
        if (FAILED(validationResult)) {
            throw std::runtime_error(std::format(
                "D3D11 rejected the required shared NV12 decoder array {}x{}, {} slices "
                "(bind 0x{:x}, misc 0x{:x}): {}; strict interop cannot fall back to a copy",
                requestedDescription.Width, requestedDescription.Height, requestedDescription.ArraySize,
                requestedDescription.BindFlags, requestedDescription.MiscFlags, hresultText(validationResult)));
        }
        validationTexture.Reset();

        const int initResult = av_hwframe_ctx_init(frames.get());
        if (initResult < 0) {
            throw std::runtime_error("Failed to initialize the shared NV12 D3D11VA surface pool: " + ffmpegError(initResult));
        }
        if (!d3dFrames->texture) throw std::runtime_error("FFmpeg did not create the required D3D11VA array texture");
        D3D11_TEXTURE2D_DESC description{};
        d3dFrames->texture->GetDesc(&description);
        if (description.Format != DXGI_FORMAT_NV12 || description.ArraySize != requestedPoolSize ||
            (description.BindFlags & (D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE)) !=
                (D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE) ||
            (description.MiscFlags & D3D11_RESOURCE_MISC_SHARED) == 0 ||
            (description.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) == 0) {
            throw std::runtime_error(std::format(
                "D3D11VA returned an incompatible shared surface pool (format {}, array {}, bind 0x{:x}, misc 0x{:x})",
                static_cast<unsigned>(description.Format), description.ArraySize, description.BindFlags, description.MiscFlags));
        }
        ++poolGeneration;
        interopPool = std::make_shared<D3D11SharedTexturePool>(d3dFrames->texture, poolGeneration);
        context.hw_frames_ctx = frames.release();
        Log::info(std::format(
            "D3D11VA interop pool generation {}: FFmpeg recommendation + retained capacity = {} surfaces "
            "(bind 0x{:x}, misc 0x{:x})",
            poolGeneration, requestedPoolSize, description.BindFlags, description.MiscFlags));
#else
        (void)context;
        throw std::runtime_error("D3D11/Vulkan interop is only available on Windows");
#endif
    }

    void openFile(const std::string& filename) {
        AVFormatContext* rawFormat{};
        int result = avformat_open_input(&rawFormat, filename.c_str(), nullptr, nullptr);
        if (result < 0) throw std::runtime_error("Failed to open input '" + filename + "': " + ffmpegError(result));
        format.reset(rawFormat);
        openDecoder("'" + filename + "'");
    }

    void openPipe(std::string pipeName) {
        pipeSource = std::make_unique<SysDvrPipeSource>(std::move(pipeName));
        auto* rawFormat = avformat_alloc_context();
        if (!rawFormat) throw std::bad_alloc();

        constexpr int avioBufferSize = 64 * 1024;
        auto* avioBuffer = static_cast<std::uint8_t*>(av_malloc(avioBufferSize));
        if (!avioBuffer) {
            avformat_free_context(rawFormat);
            throw std::bad_alloc();
        }
        customAvio = avio_alloc_context(avioBuffer, avioBufferSize, 0, this, readPipePacket, nullptr, nullptr);
        if (!customAvio) {
            av_free(avioBuffer);
            avformat_free_context(rawFormat);
            throw std::bad_alloc();
        }

        rawFormat->pb = customAvio;
        rawFormat->flags |= AVFMT_FLAG_CUSTOM_IO;
        const AVInputFormat* h264 = av_find_input_format("h264");
        if (!h264) {
            avformat_free_context(rawFormat);
            throw std::runtime_error("FFmpeg h264 demuxer is unavailable");
        }

        AVFormatContext* opened = rawFormat;
        const int result = avformat_open_input(&opened, "nexus-sysdvr-pipe.h264", h264, nullptr);
        if (result < 0) {
            if (pipeError) std::rethrow_exception(pipeError);
            if (opened) avformat_free_context(opened);
            throw std::runtime_error("Failed to open SysDVR pipe input: " + ffmpegError(result));
        }
        format.reset(opened);
        openDecoder("SysDVR pipe '" + pipeSource->pipeName() + "'");
    }

    void allocateCodecContext(const AVCodec* decoder) {
        codec.reset(avcodec_alloc_context3(decoder));
        if (!codec || !packet || !frame || !transferFrame) throw std::bad_alloc();
        const int result = avcodec_parameters_to_context(codec.get(), stream->codecpar);
        if (result < 0) throw std::runtime_error("Failed to copy H.264 decoder parameters: " + ffmpegError(result));
        // FFmpeg's frame-thread clones can invoke get_format concurrently and
        // create independent hardware pools. Strict interop needs one shared
        // decoder array whose generation and fence are tracked deterministically.
        codec->thread_count = usesD3D11VulkanInterop(requestedPath) ? 1 : 0;
    }

    void resetHardwareState() {
        if (codec) {
            codec->get_format = nullptr;
            codec->opaque = nullptr;
            av_buffer_unref(&codec->hw_device_ctx);
        }
        hardwareDevice.reset();
        hardwarePixelFormat = AV_PIX_FMT_NONE;
        transferFormats.clear();
        successfulTransferFormat.reset();
        hardwareFormatError.clear();
#ifdef _WIN32
        interopPool.reset();
        copyRing.reset();
        interopRuntime.reset();
#endif
        activeBackend = DecoderBackend::Software;
    }

    void configureHardwareDecoder(const AVCodec* decoder, DecoderBackend backend) {
        const AVHWDeviceType deviceType = hardwareDeviceType(backend);
        const char* backendName = hardwareBackendName(backend);
        hardwarePixelFormat = AV_PIX_FMT_NONE;
        for (int index = 0;; ++index) {
            const AVCodecHWConfig* config = avcodec_get_hw_config(decoder, index);
            if (!config) break;
            if (config->device_type == deviceType &&
                (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0) {
                hardwarePixelFormat = config->pix_fmt;
                break;
            }
        }
        if (hardwarePixelFormat == AV_PIX_FMT_NONE) {
            throw std::runtime_error(std::format("FFmpeg H.264 decoder does not expose a {} hw_device_ctx config", backendName));
        }

        AVBufferRef* rawDevice{};
        const int result = av_hwdevice_ctx_create(&rawDevice, deviceType, nullptr, nullptr, 0);
        if (result < 0) throw std::runtime_error(std::format("Failed to create FFmpeg {} device: {}", backendName, ffmpegError(result)));
        hardwareDevice.reset(rawDevice);
        if (requestedPath == DecoderPath::Readback) {
            transferFormats = preferredTransferFormats(hardwareDevice.get());
            if (transferFormats.empty()) {
                throw std::runtime_error(std::format("{} device exposes no CPU transfer format usable by the owned YUV420 frame path", backendName));
            }
        }
#ifdef _WIN32
        if (backend == DecoderBackend::D3D11VA && usesD3D11VulkanInterop(requestedPath)) {
            auto* deviceContext = reinterpret_cast<AVHWDeviceContext*>(hardwareDevice->data);
            auto* d3d11Context = static_cast<AVD3D11VADeviceContext*>(deviceContext->hwctx);
            interopRuntime = std::make_shared<D3D11InteropRuntime>(*d3d11Context);
        }
#endif

        codec->hw_device_ctx = av_buffer_ref(hardwareDevice.get());
        if (!codec->hw_device_ctx) throw std::bad_alloc();
        codec->opaque = this;
        codec->get_format = getHardwareFormat;
        activeBackend = backend;
    }

    void openSoftwareDecoder(const AVCodec* decoder) {
        resetHardwareState();
        allocateCodecContext(decoder);
        const int result = avcodec_open2(codec.get(), decoder, nullptr);
        if (result < 0) throw std::runtime_error("Failed to open H.264 software decoder: " + ffmpegError(result));
        activeBackend = DecoderBackend::Software;
    }

    void openHardwareDecoder(const AVCodec* decoder, DecoderBackend backend) {
        resetHardwareState();
        allocateCodecContext(decoder);
        // VideoToolbox decodes on the media engine; frame threads only add latency
        // and report a rejected frame on a later packet than the one that caused it.
        if (backend == DecoderBackend::VideoToolbox) codec->thread_count = 1;
        configureHardwareDecoder(decoder, backend);
        const int result = avcodec_open2(codec.get(), decoder, nullptr);
        if (result < 0) {
            throw std::runtime_error(std::format("Failed to open H.264 {} decoder: {}", hardwareBackendName(backend), ffmpegError(result)));
        }
        activeBackend = backend;
    }

    void openSelectedDecoder(const AVCodec* decoder) {
        if (requestedBackend == DecoderBackend::Software) {
            openSoftwareDecoder(decoder);
            return;
        }

        const DecoderBackend hardwareBackend =
            requestedBackend == DecoderBackend::Auto ? platformHardwareBackend() : requestedBackend;
        try {
            openHardwareDecoder(decoder, hardwareBackend);
            return;
        } catch (const std::exception& error) {
            if (requestedBackend != DecoderBackend::Auto) throw;
            Log::warning(std::format("{} decoder unavailable; falling back to software: {}",
                                     hardwareBackendName(hardwareBackend), error.what()));
            openSoftwareDecoder(decoder);
        }
    }

    std::string advertisedPixelFormatName(AVPixelFormat containerPixelFormat) const {
        if (isHardwareBackend(activeBackend)) {
            if (requestedPath == DecoderPath::D3D11VulkanInterop) return "d3d11 / D3D11 NV12";
            if (requestedPath == DecoderPath::D3D11VulkanInteropCopy) return "d3d11 / D3D11 NV12 (GPU copy)";
            const AVPixelFormat transferFormat = successfulTransferFormat.value_or(transferFormats.empty() ? AV_PIX_FMT_NONE : transferFormats.front());
            return pixelFormatName(hardwarePixelFormat) + " -> CPU " + pixelFormatName(transferFormat);
        }
        return containerPixelFormat == AV_PIX_FMT_NONE ? "deferred" : pixelFormatName(containerPixelFormat);
    }

    void openDecoder(const std::string& label) {
        int result = avformat_find_stream_info(format.get(), nullptr);
        if (result < 0) {
            if (pipeError) std::rethrow_exception(pipeError);
            throw std::runtime_error("Failed to read stream metadata for " + label + ": " + ffmpegError(result));
        }

        const AVCodec* decoder{};
        streamIndex = av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
        if (streamIndex < 0) throw std::runtime_error("No decodable video stream in " + label + ": " + ffmpegError(streamIndex));
        if (!decoder) throw std::runtime_error("FFmpeg did not provide an H.264 decoder for " + label);
        stream = format->streams[streamIndex];
        if (stream->codecpar->codec_id != AV_CODEC_ID_H264) {
            throw std::runtime_error("Unsupported codec '" + std::string(avcodec_get_name(stream->codecpar->codec_id)) + "'; Phase 1 requires H.264");
        }

        openSelectedDecoder(decoder);
        armLiveKeyframeWait();

        const auto pixelFormat = static_cast<AVPixelFormat>(stream->codecpar->format);
        if (pixelFormat != AV_PIX_FMT_YUV420P && pixelFormat != AV_PIX_FMT_YUVJ420P && pixelFormat != AV_PIX_FMT_NONE) {
            const char* name = av_get_pix_fmt_name(pixelFormat);
            throw std::runtime_error("Unsupported input pixel format '" + std::string(name ? name : "unknown") + "'; Phase 1 requires yuv420p/yuvj420p");
        }
        if (codec->width <= 0 || codec->height <= 0 || (codec->width & 1) || (codec->height & 1)) {
            throw std::runtime_error("YUV420P input dimensions must be positive and even");
        }

        const auto range = bestRange(codec->color_range, stream->codecpar->color_range);
        const auto matrix = bestMatrix(codec->colorspace, stream->codecpar->color_space);
        auto chromaLocation = stream->codecpar->chroma_location;
        if (chromaLocation == AVCHROMA_LOC_UNSPECIFIED && allowMetadataDefaults) chromaLocation = AVCHROMA_LOC_LEFT;

        streamInfo.codecName = decoder->name;
        streamInfo.pixelFormatName = advertisedPixelFormatName(pixelFormat);
        streamInfo.requestedDecoderBackend = requestedBackend;
        streamInfo.activeDecoderBackend = activeBackend;
        streamInfo.activeDecoderPath = requestedPath;
        streamInfo.width = codec->width;
        streamInfo.height = codec->height;
        streamInfo.declaredFrameRate = rational(stream->r_frame_rate);
        streamInfo.averageFrameRate = rational(stream->avg_frame_rate);
        streamInfo.durationSeconds = stream->duration != AV_NOPTS_VALUE
            ? static_cast<double>(stream->duration) * rational(stream->time_base)
            : (format->duration != AV_NOPTS_VALUE ? static_cast<double>(format->duration) / AV_TIME_BASE : 0.0);
        streamInfo.bitRate = stream->codecpar->bit_rate > 0 ? stream->codecpar->bit_rate : format->bit_rate;
        streamInfo.live = liveInput;
        if (liveInput) {
            streamInfo.declaredFrameRate = 0.0;
            streamInfo.averageFrameRate = 0.0;
            streamInfo.durationSeconds = 0.0;
            streamInfo.bitRate = 0;
        }
        streamInfo.color = {mapRange(range, allowMetadataDefaults), mapMatrix(matrix, allowMetadataDefaults)};
        streamInfo.transfer = av_color_transfer_name(stream->codecpar->color_trc)
            ? av_color_transfer_name(stream->codecpar->color_trc) : "unspecified";
        streamInfo.chromaLocation = av_chroma_location_name(chromaLocation)
            ? av_chroma_location_name(chromaLocation) : "unspecified";
        if (chromaLocation != AVCHROMA_LOC_LEFT) {
            throw std::runtime_error("Unsupported chroma location '" + streamInfo.chromaLocation + "'; Phase 1 requires left-sited 4:2:0 chroma");
        }
        Log::info(std::format("Decoder backend: requested {}, active {}", toString(requestedBackend), toString(activeBackend)));
    }

    AVFrame* transferHardwareFrame() {
        const auto tryTransfer = [&](AVPixelFormat format) -> int {
            av_frame_unref(transferFrame.get());
            transferFrame->format = format;
            transferFrame->width = frame->width;
            transferFrame->height = frame->height;
            const int result = av_hwframe_transfer_data(transferFrame.get(), frame.get(), 0);
            if (result >= 0) {
                const int copyPropsResult = av_frame_copy_props(transferFrame.get(), frame.get());
                if (copyPropsResult < 0) return copyPropsResult;
                successfulTransferFormat = static_cast<AVPixelFormat>(transferFrame->format);
                streamInfo.pixelFormatName = advertisedPixelFormatName(AV_PIX_FMT_NONE);
            }
            return result;
        };

        const char* backendName = hardwareBackendName(activeBackend);
        if (successfulTransferFormat) {
            const int result = tryTransfer(*successfulTransferFormat);
            if (result < 0) throw std::runtime_error(std::format("{} frame transfer failed: {}", backendName, ffmpegError(result)));
            return transferFrame.get();
        }

        int lastError = AVERROR(EINVAL);
        for (const AVPixelFormat transferFormat : transferFormats) {
            lastError = tryTransfer(transferFormat);
            if (lastError >= 0) return transferFrame.get();
        }
        throw std::runtime_error(std::format("{} frame transfer failed for all CPU formats: {}", backendName, ffmpegError(lastError)));
    }

    AVFrame* materializeFrame() {
        const auto decodedFormat = static_cast<AVPixelFormat>(frame->format);
        if (isHardwareBackend(activeBackend) && decodedFormat == hardwarePixelFormat) {
            if (usesD3D11VulkanInterop(requestedPath)) return frame.get();
            return transferHardwareFrame();
        }
        if (isHardwareBackend(activeBackend) && requestedBackend == activeBackend) {
            throw std::runtime_error(std::format("{} was requested but FFmpeg returned a non-hardware frame format '{}'",
                                                 hardwareBackendName(activeBackend), pixelFormatName(decodedFormat)));
        }
        if (isHardwareBackend(activeBackend) && requestedBackend == DecoderBackend::Auto) {
            Log::warning(std::format("{} auto path returned software frames; continuing with software frame copies",
                                     hardwareBackendName(activeBackend)));
            activeBackend = DecoderBackend::Software;
            streamInfo.activeDecoderBackend = activeBackend;
        }
        return frame.get();
    }

#ifdef _WIN32
    std::shared_ptr<D3D11FrameLease> copyDecodedFrame(const AVFrame& source) {
        auto* texture = reinterpret_cast<ID3D11Texture2D*>(source.data[0]);
        if (!texture) throw std::runtime_error("FFmpeg returned a null D3D11 decoder texture");
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        if (description.Format != DXGI_FORMAT_NV12) {
            throw std::runtime_error(std::format("D3D11/Vulkan GPU-copy interop requires an NV12 decoder texture; DXGI format is {}",
                                                 static_cast<unsigned>(description.Format)));
        }
        const auto slice = static_cast<UINT>(reinterpret_cast<std::uintptr_t>(source.data[1]));
        if (slice >= description.ArraySize) throw std::runtime_error("FFmpeg returned an invalid D3D11 texture-array slice");
        if (!copyRing || !copyRing->matches(description)) {
            copyRing = std::make_shared<D3D11CopyRing>(*interopRuntime->device(), description,
                                                       externallyRetainedFrames, ++poolGeneration);
            Log::info(std::format("D3D11 GPU-copy ring generation {}: {} shared {}x{} R8/R8G8 plane texture pairs",
                                  poolGeneration, copyRing->size(), copyRing->width(), copyRing->height()));
        }
        const std::size_t slot = copyRing->acquire();
        try {
            const auto readyValue = interopRuntime->splitAndSignal(texture, slice, copyRing->width(), copyRing->height(),
                                                                   copyRing->lumaTarget(slot), copyRing->chromaTarget(slot));
            return std::make_shared<D3D11CopyFrameLease>(interopRuntime, copyRing, slot, readyValue);
        } catch (...) {
            copyRing->release(slot);
            throw;
        }
    }
#endif

    ReadFrameResult read(Yuv420FrameSlot& destination, DecodeTiming& timing) {
        const auto operationStart = Clock::now();
        while (true) {
            int result = avcodec_receive_frame(codec.get(), frame.get());
            if (result == 0) {
                releaseLiveKeyframeWait();
                AVFrame* outputFrame = materializeFrame();
                const auto outputFormat = static_cast<AVPixelFormat>(outputFrame->format);
                const bool interopFrame = usesD3D11VulkanInterop(requestedPath);
                if (interopFrame && outputFormat != hardwarePixelFormat) {
                    throw std::runtime_error("D3D11/Vulkan interop decoder output changed away from AV_PIX_FMT_D3D11");
                }
                if (!interopFrame && !isOwnedSlotCompatible(outputFormat)) {
                    throw std::runtime_error("Decoder output changed to unsupported format '" + pixelFormatName(outputFormat) + "'");
                }
                if (outputFrame->width != streamInfo.width || outputFrame->height != streamInfo.height) {
                    throw std::runtime_error(std::format("Midstream resolution change {}x{} to {}x{} is unsupported",
                        streamInfo.width, streamInfo.height, outputFrame->width, outputFrame->height));
                }
                const auto decodedAt = Clock::now();
                auto pts = frame->best_effort_timestamp;
                double ptsSeconds{};
                double duration{};
                auto timingKind = FrameTimingKind::ContainerPts;
                if (liveInput) {
                    timingKind = FrameTimingKind::LiveArrival;
                } else {
                    const double fallbackDuration = streamInfo.averageFrameRate > 0.0 ? 1.0 / streamInfo.averageFrameRate : 1.0 / 60.0;
                    if (pts == AV_NOPTS_VALUE) {
                        ptsSeconds = fallbackPts;
                        Log::warning("Decoded frame has no timestamp; synthesizing from the preceding frame duration");
                    } else {
                        ptsSeconds = static_cast<double>(pts) * rational(stream->time_base);
                        lastPts = pts;
                    }
                    duration = frame->duration > 0
                        ? static_cast<double>(frame->duration) * rational(stream->time_base) : fallbackDuration;
                    fallbackPts = ptsSeconds + duration;
                }

                destination.resetPayload();
                const auto copyStart = Clock::now();
                if (interopFrame) {
#ifdef _WIN32
                    if (!interopRuntime) {
                        throw std::runtime_error("D3D11/Vulkan interop frame arrived without an initialized D3D11 runtime");
                    }
                    destination.storage = DecodedFrameStorage::D3D11Nv12;
                    if (requestedPath == DecoderPath::D3D11VulkanInteropCopy) {
                        destination.d3d11Lease = copyDecodedFrame(*outputFrame);
                    } else {
                        if (poolGeneration == 0) {
                            throw std::runtime_error("D3D11/Vulkan interop frame arrived without an initialized shared surface pool");
                        }
                        const auto readyValue = interopRuntime->signalDecodeReady();
                        destination.d3d11Lease = std::make_shared<FFmpegD3D11FrameLease>(
                            *outputFrame, interopRuntime, interopPool, readyValue);
                    }
#else
                    throw std::runtime_error("D3D11/Vulkan interop is only available on Windows");
#endif
                } else if (isPlanar420(outputFormat)) {
                    copyPlanar420Frame(destination, *outputFrame);
                } else {
                    copyNv12Frame(destination, *outputFrame);
                }
                const auto copyEnd = Clock::now();
                destination.metadata = {outputFrame->width, outputFrame->height, pts, ptsSeconds, duration, frameNumber++,
                    (frame->flags & AV_FRAME_FLAG_KEY) != 0,
                    timingKind,
                    {mapRange(bestRange(outputFrame->color_range, codec->color_range), allowMetadataDefaults),
                     mapMatrix(bestMatrix(outputFrame->colorspace, codec->colorspace), allowMetadataDefaults)}};
                destination.metadata.storage = destination.storage;
                timing.decodeMs = std::chrono::duration<double, std::milli>(decodedAt - operationStart).count();
                timing.copyMs = requestedPath == DecoderPath::D3D11VulkanInterop
                    ? 0.0 : std::chrono::duration<double, std::milli>(copyEnd - copyStart).count();
                timing.copyBytes = interopFrame ? 0 : static_cast<std::uint64_t>(outputFrame->width) *
                    static_cast<std::uint64_t>(outputFrame->height) * 3 / 2;
                av_frame_unref(transferFrame.get());
                av_frame_unref(frame.get());
                return ReadFrameResult::Frame;
            }
            if (result == AVERROR_EOF) return ReadFrameResult::EndOfFile;
            if (result != AVERROR(EAGAIN)) {
                if (absorbLiveDecodeError(result)) continue;
                if (!hardwareFormatError.empty()) throw std::runtime_error("D3D11VA format setup failed: " + hardwareFormatError);
                throw std::runtime_error("H.264 decoder receive failed: " + ffmpegError(result));
            }
            if (draining) throw std::runtime_error("H.264 decoder requested input while draining at end of stream");

            result = av_read_frame(format.get(), packet.get());
            if (result == AVERROR_EOF) {
                result = avcodec_send_packet(codec.get(), nullptr);
                if (result < 0 && result != AVERROR_EOF) throw std::runtime_error("Failed to drain H.264 decoder: " + ffmpegError(result));
                draining = true;
                continue;
            }
            if (result < 0) {
                if (pipeError) std::rethrow_exception(pipeError);
                throw std::runtime_error("H.264 demux failed: " + ffmpegError(result));
            }
            if (packet->stream_index == streamIndex) {
                result = avcodec_send_packet(codec.get(), packet.get());
                av_packet_unref(packet.get());
                if (result < 0) {
                    if (absorbLiveDecodeError(result)) continue;
                    if (!hardwareFormatError.empty()) {
                        throw std::runtime_error("D3D11VA format setup failed: " + hardwareFormatError);
                    }
                    throw std::runtime_error("H.264 decoder rejected a packet: " + ffmpegError(result));
                }
            } else {
                av_packet_unref(packet.get());
            }
        }
    }

    void seek() {
        if (!seekable) {
            Log::warning("Ignoring seek request for live SysDVR pipe input");
            return;
        }
        const std::int64_t target = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;
        const int result = av_seek_frame(format.get(), streamIndex, target, AVSEEK_FLAG_BACKWARD);
        if (result < 0) throw std::runtime_error("Failed to seek input to the beginning: " + ffmpegError(result));
        avformat_flush(format.get());
        avcodec_flush_buffers(codec.get());
        av_packet_unref(packet.get());
        av_frame_unref(frame.get());
        av_frame_unref(transferFrame.get());
        draining = false;
        frameNumber = 0;
        lastPts = AV_NOPTS_VALUE;
        fallbackPts = 0.0;
    }
};

FFmpegVideoReader::FFmpegVideoReader(const std::filesystem::path& path, DecoderBackend backend,
                                     DecoderPath pathMode, std::size_t externallyRetainedFrames)
    : impl_(std::make_unique<Impl>(path, backend, pathMode, externallyRetainedFrames)) {}
FFmpegVideoReader::FFmpegVideoReader(SysDvrPipeInput input, DecoderBackend backend,
                                     DecoderPath pathMode, std::size_t externallyRetainedFrames)
    : impl_(std::make_unique<Impl>(std::move(input), backend, pathMode, externallyRetainedFrames)) {}
FFmpegVideoReader::~FFmpegVideoReader() = default;
FFmpegVideoReader::FFmpegVideoReader(FFmpegVideoReader&&) noexcept = default;
FFmpegVideoReader& FFmpegVideoReader::operator=(FFmpegVideoReader&&) noexcept = default;
const VideoStreamInfo& FFmpegVideoReader::info() const noexcept { return impl_->streamInfo; }
std::optional<AdapterLuid> FFmpegVideoReader::interopAdapterLuid() const noexcept {
#ifdef _WIN32
    if (impl_->interopRuntime) return impl_->interopRuntime->adapterLuid();
#endif
    return std::nullopt;
}
void FFmpegVideoReader::releaseInteropResources() noexcept { impl_->releaseInterop(); }
ReadFrameResult FFmpegVideoReader::readFrame(Yuv420FrameSlot& destination, DecodeTiming& timing) { return impl_->read(destination, timing); }
void FFmpegVideoReader::seekToBeginning() { impl_->seek(); }

} // namespace ns60
