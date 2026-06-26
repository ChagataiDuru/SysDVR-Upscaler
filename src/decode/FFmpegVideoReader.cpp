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

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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
    copyPlane(destination.yPlane, destination.yStride, source.data[0], source.linesize[0], source.width, source.height);
    copyPlane(destination.uPlane, destination.uStride, source.data[1], source.linesize[1], source.width / 2, source.height / 2);
    copyPlane(destination.vPlane, destination.vStride, source.data[2], source.linesize[2], source.width / 2, source.height / 2);
}

void copyNv12Frame(Yuv420FrameSlot& destination, const AVFrame& source) {
    copyPlane(destination.yPlane, destination.yStride, source.data[0], source.linesize[0], source.width, source.height);
    if (!source.data[1]) throw std::runtime_error("Decoder returned a null NV12 chroma plane");
    const int chromaWidth = source.width / 2;
    const int chromaHeight = source.height / 2;
    for (int row = 0; row < chromaHeight; ++row) {
        const auto* input = source.data[1] + static_cast<std::ptrdiff_t>(row) * source.linesize[1];
        auto* uOutput = destination.uPlane.data() + static_cast<std::size_t>(row * destination.uStride);
        auto* vOutput = destination.vPlane.data() + static_cast<std::size_t>(row * destination.vStride);
        for (int column = 0; column < chromaWidth; ++column) {
            uOutput[column] = static_cast<std::byte>(input[column * 2]);
            vOutput[column] = static_cast<std::byte>(input[column * 2 + 1]);
        }
    }
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

    if (supported.empty()) return {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12};

    std::vector<AVPixelFormat> ordered;
    for (const AVPixelFormat preferred : {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12, AV_PIX_FMT_YUVJ420P}) {
        if (std::find(supported.begin(), supported.end(), preferred) != supported.end()) ordered.push_back(preferred);
    }
    return ordered;
}
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

    explicit Impl(const std::filesystem::path& path, DecoderBackend backend) : requestedBackend(backend) {
        const auto utf8 = path.u8string();
        const std::string filename(utf8.begin(), utf8.end());
        openFile(filename);
    }

    explicit Impl(SysDvrPipeInput input, DecoderBackend backend) : requestedBackend(backend) {
        if (input.pipeName.empty()) throw std::invalid_argument("SysDVR pipe input requires a pipe name");
        seekable = false;
        liveInput = true;
        allowMetadataDefaults = true;
        openPipe(std::move(input.pipeName));
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
        const auto* self = static_cast<const Impl*>(context->opaque);
        if (self) {
            for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
                if (*format == self->hardwarePixelFormat) return *format;
            }
        }
        return formats && *formats != AV_PIX_FMT_NONE ? *formats : AV_PIX_FMT_NONE;
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
        codec->thread_count = 0;
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
        activeBackend = DecoderBackend::Software;
    }

    void configureD3D11VA(const AVCodec* decoder) {
        hardwarePixelFormat = AV_PIX_FMT_NONE;
        for (int index = 0;; ++index) {
            const AVCodecHWConfig* config = avcodec_get_hw_config(decoder, index);
            if (!config) break;
            if (config->device_type == AV_HWDEVICE_TYPE_D3D11VA &&
                (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0) {
                hardwarePixelFormat = config->pix_fmt;
                break;
            }
        }
        if (hardwarePixelFormat == AV_PIX_FMT_NONE) throw std::runtime_error("FFmpeg H.264 decoder does not expose a D3D11VA hw_device_ctx config");

        AVBufferRef* rawDevice{};
        const int result = av_hwdevice_ctx_create(&rawDevice, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
        if (result < 0) throw std::runtime_error("Failed to create FFmpeg D3D11VA device: " + ffmpegError(result));
        hardwareDevice.reset(rawDevice);
        transferFormats = preferredTransferFormats(hardwareDevice.get());
        if (transferFormats.empty()) throw std::runtime_error("D3D11VA device exposes no CPU transfer format usable by the owned YUV420 frame path");

        codec->hw_device_ctx = av_buffer_ref(hardwareDevice.get());
        if (!codec->hw_device_ctx) throw std::bad_alloc();
        codec->opaque = this;
        codec->get_format = getHardwareFormat;
        activeBackend = DecoderBackend::D3D11VA;
    }

    void openSoftwareDecoder(const AVCodec* decoder) {
        resetHardwareState();
        allocateCodecContext(decoder);
        const int result = avcodec_open2(codec.get(), decoder, nullptr);
        if (result < 0) throw std::runtime_error("Failed to open H.264 software decoder: " + ffmpegError(result));
        activeBackend = DecoderBackend::Software;
    }

    void openD3D11Decoder(const AVCodec* decoder) {
        resetHardwareState();
        allocateCodecContext(decoder);
        configureD3D11VA(decoder);
        const int result = avcodec_open2(codec.get(), decoder, nullptr);
        if (result < 0) throw std::runtime_error("Failed to open H.264 D3D11VA decoder: " + ffmpegError(result));
        activeBackend = DecoderBackend::D3D11VA;
    }

    void openSelectedDecoder(const AVCodec* decoder) {
        if (requestedBackend == DecoderBackend::Software) {
            openSoftwareDecoder(decoder);
            return;
        }

        try {
            openD3D11Decoder(decoder);
            return;
        } catch (const std::exception& error) {
            if (requestedBackend == DecoderBackend::D3D11VA) throw;
            Log::warning(std::string("D3D11VA decoder unavailable; falling back to software: ") + error.what());
            openSoftwareDecoder(decoder);
        }
    }

    std::string advertisedPixelFormatName(AVPixelFormat containerPixelFormat) const {
        if (activeBackend == DecoderBackend::D3D11VA) {
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

        if (successfulTransferFormat) {
            const int result = tryTransfer(*successfulTransferFormat);
            if (result < 0) throw std::runtime_error("D3D11VA frame transfer failed: " + ffmpegError(result));
            return transferFrame.get();
        }

        int lastError = AVERROR(EINVAL);
        for (const AVPixelFormat format : transferFormats) {
            lastError = tryTransfer(format);
            if (lastError >= 0) return transferFrame.get();
        }
        throw std::runtime_error("D3D11VA frame transfer failed for all CPU formats: " + ffmpegError(lastError));
    }

    AVFrame* materializeFrame() {
        const auto decodedFormat = static_cast<AVPixelFormat>(frame->format);
        if (activeBackend == DecoderBackend::D3D11VA && decodedFormat == hardwarePixelFormat) return transferHardwareFrame();
        if (activeBackend == DecoderBackend::D3D11VA && requestedBackend == DecoderBackend::D3D11VA) {
            throw std::runtime_error("D3D11VA was requested but FFmpeg returned a non-hardware frame format '" + pixelFormatName(decodedFormat) + "'");
        }
        if (activeBackend == DecoderBackend::D3D11VA && requestedBackend == DecoderBackend::Auto) {
            Log::warning("D3D11VA auto path returned software frames; continuing with software frame copies");
            activeBackend = DecoderBackend::Software;
            streamInfo.activeDecoderBackend = activeBackend;
        }
        return frame.get();
    }

    ReadFrameResult read(Yuv420FrameSlot& destination, DecodeTiming& timing) {
        const auto operationStart = Clock::now();
        while (true) {
            int result = avcodec_receive_frame(codec.get(), frame.get());
            if (result == 0) {
                AVFrame* outputFrame = materializeFrame();
                const auto outputFormat = static_cast<AVPixelFormat>(outputFrame->format);
                if (!isOwnedSlotCompatible(outputFormat)) {
                    throw std::runtime_error("Decoder output changed to unsupported format '" + pixelFormatName(outputFormat) + "'");
                }
                if (outputFrame->width != streamInfo.width || outputFrame->height != streamInfo.height) {
                    throw std::runtime_error(std::format("Midstream resolution change {}x{} to {}x{} is unsupported in Phase 1",
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

                const auto copyStart = Clock::now();
                if (isPlanar420(outputFormat)) copyPlanar420Frame(destination, *outputFrame);
                else copyNv12Frame(destination, *outputFrame);
                const auto copyEnd = Clock::now();
                destination.metadata = {outputFrame->width, outputFrame->height, pts, ptsSeconds, duration, frameNumber++,
                    (frame->flags & AV_FRAME_FLAG_KEY) != 0,
                    timingKind,
                    {mapRange(bestRange(outputFrame->color_range, codec->color_range), allowMetadataDefaults),
                     mapMatrix(bestMatrix(outputFrame->colorspace, codec->colorspace), allowMetadataDefaults)}};
                timing.decodeMs = std::chrono::duration<double, std::milli>(decodedAt - operationStart).count();
                timing.copyMs = std::chrono::duration<double, std::milli>(copyEnd - copyStart).count();
                av_frame_unref(transferFrame.get());
                av_frame_unref(frame.get());
                return ReadFrameResult::Frame;
            }
            if (result == AVERROR_EOF) return ReadFrameResult::EndOfFile;
            if (result != AVERROR(EAGAIN)) throw std::runtime_error("H.264 decoder receive failed: " + ffmpegError(result));
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
                if (result < 0) throw std::runtime_error("H.264 decoder rejected a packet: " + ffmpegError(result));
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

FFmpegVideoReader::FFmpegVideoReader(const std::filesystem::path& path, DecoderBackend backend) : impl_(std::make_unique<Impl>(path, backend)) {}
FFmpegVideoReader::FFmpegVideoReader(SysDvrPipeInput input, DecoderBackend backend) : impl_(std::make_unique<Impl>(std::move(input), backend)) {}
FFmpegVideoReader::~FFmpegVideoReader() = default;
FFmpegVideoReader::FFmpegVideoReader(FFmpegVideoReader&&) noexcept = default;
FFmpegVideoReader& FFmpegVideoReader::operator=(FFmpegVideoReader&&) noexcept = default;
const VideoStreamInfo& FFmpegVideoReader::info() const noexcept { return impl_->streamInfo; }
ReadFrameResult FFmpegVideoReader::readFrame(Yuv420FrameSlot& destination, DecodeTiming& timing) { return impl_->read(destination, timing); }
void FFmpegVideoReader::seekToBeginning() { impl_->seek(); }

} // namespace ns60