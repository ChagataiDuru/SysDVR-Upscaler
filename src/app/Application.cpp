#include "app/ApplicationEntry.h"

#include "decode/FFmpegRuntime.h"
#include "decode/FFmpegVideoReader.h"
#include "decode/FramePool.h"
#include "decode/FrameQueue.h"
#include "platform/Window.h"
#include "playback/PlaybackClock.h"
#include "render/VideoPipeline.h"
#include "render/VulkanContext.h"
#include "telemetry/ImGuiOverlay.h"
#include "telemetry/Metrics.h"
#include "utility/ColorMath.h"
#include "utility/Log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef NS60_VERSION
#define NS60_VERSION "development"
#endif

#ifdef _WIN32
#include <windows.h>
#endif

namespace ns60 {
namespace {
using Clock = std::chrono::steady_clock;

void copyOwnedFrame(Yuv420FrameSlot& destination, const Yuv420FrameSlot& source) {
    destination.metadata = source.metadata;
    std::memcpy(destination.yPlane.data(), source.yPlane.data(), source.yPlane.size());
    std::memcpy(destination.uPlane.data(), source.uPlane.data(), source.uPlane.size());
    std::memcpy(destination.vPlane.data(), source.vPlane.data(), source.vPlane.size());
}

void waitForTarget(PlaybackClock::TimePoint target) {
    while (true) {
        const auto now = Clock::now();
        if (now >= target) return;
        const auto remaining = target - now;
        if (remaining > std::chrono::milliseconds(2)) {
            std::this_thread::sleep_for(remaining - std::chrono::milliseconds(1));
        } else {
            std::this_thread::yield();
        }
    }
}

std::string sourceStem(const AppConfig& config) {
    if (config.source == SourceKind::File) {
        const auto stem = config.input.stem().string();
        return stem.empty() ? "input" : stem;
    }
    return config.source == SourceKind::SysDvr ? "sysdvr" : "sysdvr-pipe";
}

std::string sourceDisplayName(const AppConfig& config) {
    if (config.source == SourceKind::File) return config.input.filename().string();
    if (config.source == SourceKind::SysDvr) return "SysDVR USB";
    return "SysDVR pipe " + config.pipeName;
}

std::filesystem::path overlayInputPath(const AppConfig& config) {
    return config.source == SourceKind::File ? config.input : std::filesystem::path(sourceDisplayName(config));
}
ScreenshotRequest screenshotFor(const AppConfig& config, const VideoFrameMetadata& frame,
                                const VideoPipeline& pipeline, const VulkanContext& context,
                                const std::optional<GpuTimings>& timings) {
    const auto stem = sourceStem(config);
    const auto base = pipeline.comparisonEnabled() ? std::format("{}_frame_{:06}_compare_{}_vs_{}", stem, frame.frameNumber, toString(pipeline.comparisonA()), toString(pipeline.comparisonB())) : std::format("{}_frame_{:06}_{}", stem, frame.frameNumber, toString(pipeline.mode()));
    ScreenshotRequest request;
    request.path = std::filesystem::path("captures") / (base + ".png");
    request.metadataPath = std::filesystem::path("captures") / (base + ".json");
    request.sourceFilename = sourceDisplayName(config);
    request.frame = frame;
    request.outputWidth = config.outputWidth;
    request.outputHeight = config.outputHeight;
    request.framebufferWidth = static_cast<int>(context.extent().width);
    request.framebufferHeight = static_cast<int>(context.extent().height);
    request.mode = pipeline.mode();
    request.sharpen = pipeline.parameters().sharpen;
    request.antiRinging = pipeline.parameters().antiRinging;
    request.chromaMode = pipeline.parameters().chromaMode;
    request.finalFilter = pipeline.finalFilter();
    request.comparison = pipeline.comparisonEnabled();
    request.comparisonA = pipeline.comparisonA();
    request.comparisonB = pipeline.comparisonB();
    request.divider = pipeline.comparisonDivider();
    request.timings = timings;
    return request;
}

#ifdef _WIN32
std::wstring widenUtf8(std::string_view text) {
    if (text.empty()) return {};
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0) throw std::runtime_error("Failed to convert bridge argument to UTF-16");
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), required);
    return result;
}

std::wstring quoteArg(const std::wstring& argument) {
    if (argument.empty()) return L"\"\"";
    const bool needsQuotes = argument.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!needsQuotes) return argument;
    std::wstring quoted{L'\"'};
    for (wchar_t ch : argument) {
        if (ch == L'\"') quoted.push_back(L'\\');
        quoted.push_back(ch);
    }
    quoted.push_back(L'\"');
    return quoted;
}

std::wstring commandLineFor(const std::vector<std::wstring>& arguments) {
    std::wstring command;
    for (const auto& argument : arguments) {
        if (!command.empty()) command.push_back(L' ');
        command += quoteArg(argument);
    }
    return command;
}

std::string makeUniquePipeName() {
    const auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
    return std::format("SysDVR-Upscaler.Video.{}.{}", static_cast<unsigned long>(GetCurrentProcessId()), ticks);
}

class BridgeProcess final {
public:
    BridgeProcess() = default;
    explicit BridgeProcess(PROCESS_INFORMATION info) : process_(info.hProcess) {
        if (info.hThread) CloseHandle(info.hThread);
    }
    ~BridgeProcess() { stop(); }
    BridgeProcess(const BridgeProcess&) = delete;
    BridgeProcess& operator=(const BridgeProcess&) = delete;
    BridgeProcess(BridgeProcess&& other) noexcept : process_(std::exchange(other.process_, nullptr)) {}
    BridgeProcess& operator=(BridgeProcess&& other) noexcept {
        if (this != &other) {
            stop();
            process_ = std::exchange(other.process_, nullptr);
        }
        return *this;
    }

private:
    HANDLE process_{};

    void stop() noexcept {
        if (!process_) return;
        DWORD exitCode{};
        if (GetExitCodeProcess(process_, &exitCode) && exitCode == STILL_ACTIVE) {
            TerminateProcess(process_, 0);
            WaitForSingleObject(process_, 3000);
        }
        CloseHandle(process_);
        process_ = nullptr;
    }
};

BridgeProcess launchSysDvrBridge(const std::filesystem::path& bridgePath, const std::string& pipeName) {
    const auto executable = std::filesystem::absolute(bridgePath);
    if (!std::filesystem::exists(executable)) {
        throw std::runtime_error("SysDVR bridge executable does not exist: " + executable.string());
    }

    std::vector<std::wstring> arguments{
        executable.wstring(),
        L"usb",
        L"--upscaler-video-pipe",
        widenUtf8(pipeName),
        L"--no-audio"
    };
    auto commandLine = commandLineFor(arguments);
    auto workingDirectory = executable.parent_path().wstring();

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const DWORD flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP;
    if (!CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, FALSE, flags, nullptr,
            workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &startup, &process)) {
        throw std::runtime_error(std::format("Failed to launch SysDVR bridge: Win32 error {}", static_cast<unsigned long>(GetLastError())));
    }

    Log::info("Launched SysDVR bridge with pipe " + pipeName);
    return BridgeProcess(process);
}
#else
std::string makeUniquePipeName() { return {}; }
class BridgeProcess final {};
BridgeProcess launchSysDvrBridge(const std::filesystem::path&, const std::string&) {
    throw std::runtime_error("Unified SysDVR launch is only available on Windows");
}
#endif
} // namespace

int runApplication(AppConfig config) {
    std::optional<BridgeProcess> bridgeProcess;
    if (config.source == SourceKind::SysDvr) {
        config.pipeName = makeUniquePipeName();
        bridgeProcess.emplace(launchSysDvrBridge(config.sysdvrBridge, config.pipeName));
    }
    if (config.source == SourceKind::File && !std::filesystem::exists(config.input)) throw std::runtime_error("Input file does not exist: " + config.input.string());
    Log::info("NexusStream60 " NS60_VERSION);
#ifdef NDEBUG
    Log::info("Build configuration: Release");
#else
    Log::info("Build configuration: Debug");
#endif
    Log::info("Source: " + std::string(toString(config.source)));
    if (config.source == SourceKind::File) Log::info("Input path: " + std::filesystem::absolute(config.input).string());
    else Log::info("Input pipe: " + config.pipeName);
    const auto versions = ffmpegVersions();
    Log::info(std::format("FFmpeg libraries: avformat {}, avcodec {}, avutil {}", versions.avformat, versions.avcodec, versions.avutil));

    FFmpegVideoReader reader = config.source == SourceKind::File
        ? FFmpegVideoReader(config.input)
        : FFmpegVideoReader(SysDvrPipeInput{config.pipeName});
    const auto info = reader.info();
    Log::info(std::format("Input: {} {}x{} {}, {:.3f}/{:.3f} fps, {:.3f} s, {} / {}, chroma {}",
        info.codecName, info.width, info.height, info.pixelFormatName, info.declaredFrameRate, info.averageFrameRate,
        info.durationSeconds, toString(info.color.range), toString(info.color.matrix), info.chromaLocation));
    Log::info(std::format("Output: {}x{}, {}", config.outputWidth, config.outputHeight, displayName(config.upscale)));

    const bool liveMode = config.source != SourceKind::File;
    const std::size_t decodeSlots = liveMode ? 3u : FramePool::defaultSlotCount;
    FramePool pool(decodeSlots, info.width, info.height);
    FrameQueue queue(pool);
    FramePool displayPool(1, info.width, info.height);
    auto& displayed = displayPool.at(0);
    Window window(config.outputWidth, config.outputHeight, "NexusStream60", config.fullscreen, config.borderless, config.monitorIndex);
    VulkanContext context(window, config.validation, config.vsync);
    VideoPipeline pipeline(context, info.width, info.height, config.outputWidth, config.outputHeight, config.upscale, config.sharpen, config.antiRinging, config.presentation, config.presentationExplicit, config.finalFilter, config.chromaUpscale);
    if (config.comparisonA && config.comparisonB) pipeline.setComparison(true, *config.comparisonA, *config.comparisonB);
    ImGuiOverlay overlay(window, context, overlayInputPath(config), info, config.outputWidth, config.outputHeight);

    std::atomic<bool> seekRequested{};
    std::atomic<bool> decodeEof{};
    std::atomic<double> lastDecodeMs{};
    std::atomic<double> lastCopyMs{};
    std::atomic<double> lastWaitMs{};
    std::atomic<std::uint64_t> decodedCount{};
    std::mutex decoderErrorMutex;
    std::exception_ptr decoderError;

    std::jthread decoder([&](std::stop_token stop) {
        try {
            while (!stop.stop_requested()) {
                if (seekRequested.exchange(false)) reader.seekToBeginning();
                const auto waitStart = Clock::now();
                const auto slot = liveMode ? queue.acquireWriteLatest() : queue.acquireWrite();
                lastWaitMs.store(std::chrono::duration<double, std::milli>(Clock::now() - waitStart).count(), std::memory_order_relaxed);
                if (!slot || stop.stop_requested()) break;
                if (seekRequested.exchange(false)) {
                    queue.cancelWrite(*slot);
                    reader.seekToBeginning();
                    continue;
                }
                DecodeTiming timing{};
                const auto result = reader.readFrame(pool.at(*slot), timing);
                if (result == ReadFrameResult::EndOfFile) {
                    queue.cancelWrite(*slot);
                    if (config.loop) {
                        reader.seekToBeginning();
                        continue;
                    }
                    decodeEof.store(true, std::memory_order_release);
                    queue.stop();
                    break;
                }
                if (seekRequested.load(std::memory_order_relaxed)) {
                    queue.cancelWrite(*slot);
                    continue;
                }
                lastDecodeMs.store(timing.decodeMs, std::memory_order_relaxed);
                lastCopyMs.store(timing.copyMs, std::memory_order_relaxed);
                decodedCount.fetch_add(1, std::memory_order_relaxed);
                queue.commitWrite(*slot);
            }
        } catch (...) {
            {
                std::lock_guard lock(decoderErrorMutex);
                decoderError = std::current_exception();
            }
            decodeEof.store(true, std::memory_order_release);
            queue.stop();
        }
    });

    Metrics metrics;
    PlaybackClock playbackClock;
    std::optional<VideoFrameMetadata> current;
    bool paused{};
    bool step{};
    bool seeking{};
    bool screenshot{};
    bool zoomInspector{};
    std::optional<GpuTimings> lastGpuTimings;
    auto statsAt = Clock::now();
    auto frameStarted = statsAt;
    std::optional<Clock::time_point> lastActivePresent;
    std::uint64_t decodedAt{};
    double previousPts{};
    bool havePreviousPts{};

    try {
        while (!window.shouldClose()) {
            window.pollEvents();
            const auto events = window.consumeEvents();
            if (events.togglePause) {
                paused = !paused;
                if (paused) playbackClock.pause(); else { playbackClock.resume(); lastActivePresent.reset(); frameStarted = Clock::now(); }
            }
            if (events.stepFrame && paused && !zoomInspector) step = true;
            if (events.seekBeginning) {
                seekRequested.store(true, std::memory_order_release);
                playbackClock.reset();
                current.reset();
                havePreviousPts = false;
                lastActivePresent.reset();
                metrics.resetActivePlayback();
                seeking = true;
                step = paused;
            }
            if (events.toggleFullscreen) window.toggleFullscreen();
            if (events.toggleTelemetry) overlay.setVisible(!overlay.visible());
            if (events.screenshot) screenshot = true;
            if (events.upscaleMode >= 0) {
                constexpr std::array modes{UpscaleMode::Nearest, UpscaleMode::Bilinear, UpscaleMode::BicubicCatmullRom,
                    UpscaleMode::Lanczos2, UpscaleMode::BilinearCas, UpscaleMode::Lanczos2Cas,
                    UpscaleMode::Fsr1Easu, UpscaleMode::Fsr1EasuRcas};
                pipeline.setMode(modes[static_cast<std::size_t>(events.upscaleMode)]);
                glfwSetWindowTitle(window.native(), std::format("NexusStream60 - {}", displayName(pipeline.mode())).c_str());
            }
            if (events.toggleComparison) pipeline.setComparison(!pipeline.comparisonEnabled(), pipeline.comparisonA(), pipeline.comparisonB());
            if (events.setComparisonA) pipeline.setComparison(true, pipeline.mode(), pipeline.comparisonB());
            if (events.setComparisonB) pipeline.setComparison(true, pipeline.comparisonA(), pipeline.mode());
            if (events.toggleZoom) {
                zoomInspector = !zoomInspector;
                pipeline.setZoomInspector(zoomInspector);
                if (zoomInspector && !paused) { paused = true; playbackClock.pause(); }
            }
            if (zoomInspector) {
                if (events.zoomDelta != 0.0F) pipeline.setZoom(pipeline.zoom() + events.zoomDelta);
                if (events.zoomMoveX || events.zoomMoveY) pipeline.moveZoom(events.zoomMoveX * 0.02F / pipeline.zoom(), events.zoomMoveY * 0.02F / pipeline.zoom());
            }
            if (events.sharpnessDelta != 0.0F) {
                auto sharpen = pipeline.parameters().sharpen;
                if (pipeline.mode() == UpscaleMode::Fsr1EasuRcas)
                    sharpen.rcasSharpness = std::clamp(sharpen.rcasSharpness + events.sharpnessDelta, 0.0F, 1.0F);
                else sharpen.casSharpness = std::clamp(sharpen.casSharpness + events.sharpnessDelta, 0.0F, 1.0F);
                pipeline.setSharpenSettings(sharpen);
            }
            if (pipeline.comparisonEnabled() && glfwGetMouseButton(window.native(), GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
                double x{}, y{}; int windowWidth{}, windowHeight{};
                glfwGetCursorPos(window.native(), &x, &y); glfwGetWindowSize(window.native(), &windowWidth, &windowHeight);
                if (windowWidth > 0) pipeline.setComparisonDivider(static_cast<float>(x / windowWidth));
            }
            if (events.escape) {
                if (window.fullscreen()) window.toggleFullscreen(); else window.requestClose();
            }
            if (window.shouldClose()) break;

            bool acquiredNewFrame = false;
            if (!paused || step || seeking) {
                const auto slot = queue.acquireRead();
                if (!slot) {
                    if (decodeEof.load(std::memory_order_acquire)) window.requestClose();
                    continue;
                }
                const auto& candidate = pool.at(*slot);
                if (seeking && candidate.metadata.frameNumber != 0) {
                    queue.releaseRead(*slot);
                    continue;
                }
                if (current && candidate.metadata.frameNumber < current->frameNumber) {
                    playbackClock.reset(); // Loop boundary.
                    havePreviousPts = false;
                    lastActivePresent.reset();
                    metrics.resetActivePlayback();
                }
                copyOwnedFrame(displayed, candidate);
                queue.releaseRead(*slot);
                acquiredNewFrame = true;
                seeking = false;

                if (!playbackClock.started()) playbackClock.start(displayed.metadata.ptsSeconds);
                if (!paused) {
                    const auto target = playbackClock.targetTime(displayed.metadata.ptsSeconds);
                    waitForTarget(target);
                    const double late = playbackClock.latenessSeconds(displayed.metadata.ptsSeconds);
                    metrics.latenessMs.add(late * 1000.0);
                    metrics.driftMs.add(playbackClock.driftSeconds(displayed.metadata.ptsSeconds) * 1000.0);
                    if (late > 0.020) ++metrics.lateFrames;
                    if (config.dropLateFrames && late > 0.100) {
                        ++metrics.droppedFrames;
                        continue;
                    }
                } else {
                    playbackClock.start(displayed.metadata.ptsSeconds);
                    playbackClock.pause();
                }
                if (havePreviousPts) metrics.ptsDeltaMs.add((displayed.metadata.ptsSeconds - previousPts) * 1000.0);
                previousPts = displayed.metadata.ptsSeconds;
                havePreviousPts = true;
                current = displayed.metadata;
                step = false;
            } else if (current) {
                ++metrics.repeatedFrames;
                window.waitEvents(1.0 / 30.0);
            }

            if (!current) continue;
            int framebufferWidth{}, framebufferHeight{};
            if (!window.framebufferExtent(framebufferWidth, framebufferHeight)) {
                window.waitEvents(0.1);
                continue;
            }
            if (screenshot) {
                pipeline.requestScreenshot(screenshotFor(config, *current, pipeline, context, lastGpuTimings));
                screenshot = false;
            }
            const auto acquired = context.acquireFrame();
            if (!acquired) continue;
            pipeline.completePendingScreenshot(acquired->flightIndex);
            pipeline.prepareFrame(acquired->flightIndex, displayed);
            pipeline.recordProcessing(acquired->commandBuffer, acquired->flightIndex);

            metrics.decodeMs.add(lastDecodeMs.load(std::memory_order_relaxed));
            metrics.planeCopyMs.add(lastCopyMs.load(std::memory_order_relaxed));
            metrics.decoderWaitMs.add(lastWaitMs.load(std::memory_order_relaxed));
            metrics.decodedFrames = decodedCount.load(std::memory_order_relaxed);
            metrics.queueOccupancy = queue.occupancy();
            metrics.queueHighWater = queue.highWaterMark();
            metrics.staleDecodedFramesDropped = queue.staleDropCount();
            if (acquired->completedTimings) {
                lastGpuTimings = acquired->completedTimings;
                metrics.gpuUploadMs.add(acquired->completedTimings->uploadMs);
                metrics.gpuColorMs.add(acquired->completedTimings->colorMs);
                for (std::size_t pass = 0; pass < acquired->completedTimings->passes.size(); ++pass)
                    if (acquired->completedTimings->passes[pass]) metrics.gpuPassMs[pass].add(*acquired->completedTimings->passes[pass]);
                metrics.gpuReconstructionMs.add(acquired->completedTimings->reconstructionMs);
                metrics.gpuPostProcessingMs.add(acquired->completedTimings->postProcessingMs);
                metrics.gpuPresentMs.add(acquired->completedTimings->presentMs);
                metrics.gpuTotalMs.add(acquired->completedTimings->totalMs);
            }
            overlay.build(metrics, current, paused, pipeline);
            pipeline.beginPresent(acquired->commandBuffer, acquired->flightIndex, acquired->imageIndex);
            overlay.record(acquired->commandBuffer);
            pipeline.endPresent(acquired->commandBuffer, acquired->flightIndex);
            context.submitAndPresent(*acquired);
            ++metrics.presentSubmissions;

            const auto now = Clock::now();
            metrics.cpuFrameMs.add(std::chrono::duration<double, std::milli>(now - frameStarted).count());
            frameStarted = now;
            if (!paused && acquiredNewFrame) {
                if (lastActivePresent) {
                    const double intervalMs = std::chrono::duration<double, std::milli>(now - *lastActivePresent).count();
                    metrics.presentSubmissionIntervalMs.add(intervalMs);
                    metrics.activeFrameTimeMs.add(intervalMs);
                    metrics.activePlaybackSeconds += intervalMs / 1000.0;
                    ++metrics.presentedFrames;
                }
                lastActivePresent = now;
            }
            if (now - statsAt >= std::chrono::milliseconds(500)) {
                const double seconds = std::chrono::duration<double>(now - statsAt).count();
                const auto decodedNow = decodedCount.load(std::memory_order_relaxed);
                metrics.decodedFps.add(static_cast<double>(decodedNow - decodedAt) / seconds);
                metrics.presentedFps.add(fpsFromNewestIntervalWindow(metrics.presentSubmissionIntervalMs, 1000.0));
                decodedAt = decodedNow;
                statsAt = now;
            }
            (void)acquiredNewFrame;
        }
    } catch (...) {
        queue.stop();
        decoder.request_stop();
        if (decoder.joinable()) decoder.join();
        throw;
    }

    queue.stop();
    decoder.request_stop();
    if (decoder.joinable()) decoder.join();
    context.waitIdle();
    for (std::uint32_t i = 0; i < VulkanContext::framesInFlight; ++i) pipeline.completePendingScreenshot(i);
    {
        std::lock_guard lock(decoderErrorMutex);
        if (decoderError) std::rethrow_exception(decoderError);
    }
    return 0;
}

} // namespace ns60
