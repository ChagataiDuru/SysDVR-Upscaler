#include "decode/SysDvrPipeSource.h"

#include "decode/SysDvrBridgeProtocol.h"
#include "utility/Log.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace ns60 {
namespace {
#ifdef _WIN32
std::wstring widen(std::string_view text) {
    if (text.empty()) return {};
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0) throw std::runtime_error("Failed to convert pipe name to UTF-16");
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), required);
    return result;
}

std::string win32Error(DWORD code) {
    return std::format("Win32 error {}", static_cast<unsigned long>(code));
}

std::wstring pipePathFor(std::string_view name) {
    constexpr std::string_view prefix = R"(\\.\pipe\)";
    if (name.starts_with(prefix)) return widen(name);
    return std::wstring(LR"(\\.\pipe\)") + widen(name);
}
#endif
} // namespace

struct SysDvrPipeSource::Impl {
    explicit Impl(std::string requestedName) : name(std::move(requestedName)) {
        if (name.empty()) throw std::invalid_argument("SysDVR pipe name cannot be empty");
#ifdef _WIN32
        const auto path = pipePathFor(name);
        pipe = CreateNamedPipeW(path.c_str(), PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 1 << 20, 1 << 20, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) throw std::runtime_error("Failed to create SysDVR pipe '" + name + "': " + win32Error(GetLastError()));
        Log::info("Waiting for SysDVR-UpscalerBridge on pipe " + name);
#else
        throw std::runtime_error("SysDVR named-pipe input is only available on Windows");
#endif
    }

    ~Impl() {
#ifdef _WIN32
        if (pipe != INVALID_HANDLE_VALUE) {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
        }
#endif
    }

    const std::string name;
    std::vector<std::uint8_t> payload;
    std::size_t payloadOffset{};
    std::uint64_t expectedSequence{};
    std::uint64_t lastSourceTimestamp{};
    std::uint64_t lastBridgeTimestamp{};
    sysdvr_bridge::HelloMessage hello{};
    bool connected{};
    bool helloRead{};
    bool eof{};
#ifdef _WIN32
    HANDLE pipe{INVALID_HANDLE_VALUE};
#endif

    int read(std::uint8_t* destination, int destinationSize) {
        if (destinationSize <= 0) return 0;
        if (eof) return 0;
        connectIfNeeded();

        while (payloadOffset >= payload.size()) {
            payload.clear();
            payloadOffset = 0;
            if (!readNextVideoPayload()) return 0;
        }

        const auto available = payload.size() - payloadOffset;
        const auto toCopy = std::min<std::size_t>(available, static_cast<std::size_t>(destinationSize));
        std::memcpy(destination, payload.data() + payloadOffset, toCopy);
        payloadOffset += toCopy;
        return static_cast<int>(toCopy);
    }

    void connectIfNeeded() {
        if (connected) return;
#ifdef _WIN32
        const BOOL ok = ConnectNamedPipe(pipe, nullptr);
        if (!ok) {
            const DWORD error = GetLastError();
            if (error != ERROR_PIPE_CONNECTED) throw std::runtime_error("SysDVR pipe connection failed: " + win32Error(error));
        }
        connected = true;
        Log::info("SysDVR-UpscalerBridge connected to pipe " + name);
        readHello();
#endif
    }

    void readHello() {
        if (helloRead) return;
        std::array<std::uint8_t, sysdvr_bridge::HelloHeaderSize> header{};
        if (!readExact(header.data(), header.size())) {
            eof = true;
            throw std::runtime_error("SysDVR bridge disconnected before sending hello");
        }
        hello = sysdvr_bridge::parseHello(header);
        helloRead = true;
        Log::info(std::format("SysDVR-UpscalerBridge hello: protocol {}, pid {}, capabilities 0x{:08x}, qpc {} Hz",
            hello.version, hello.bridgeProcessId, hello.capabilities, hello.bridgeTimestampFrequency));
    }

    bool readNextVideoPayload() {
        while (true) {
            std::array<std::uint8_t, sysdvr_bridge::StreamHeaderSize> headerBytes{};
            if (!readExact(headerBytes.data(), headerBytes.size())) {
                eof = true;
                return false;
            }

            const auto header = sysdvr_bridge::parseStreamHeader(headerBytes);
            if (header.sequence != expectedSequence) {
                Log::warning(std::format("SysDVR bridge sequence gap: expected {}, got {}", expectedSequence, header.sequence));
                expectedSequence = header.sequence;
            }
            ++expectedSequence;
            lastSourceTimestamp = header.sourceTimestamp;
            lastBridgeTimestamp = header.bridgeTimestamp;

            switch (header.type) {
            case sysdvr_bridge::MessageType::VideoPayload:
                if (header.payloadSize == 0) continue;
                payload.resize(header.payloadSize);
                if (!readExact(payload.data(), payload.size())) {
                    eof = true;
                    return false;
                }
                return true;
            case sysdvr_bridge::MessageType::Discontinuity:
                discardPayload(header.payloadSize);
                Log::warning(std::format("SysDVR bridge discontinuity received, flags 0x{:08x}", header.flags));
                continue;
            case sysdvr_bridge::MessageType::Status:
                discardPayload(header.payloadSize);
                Log::debug("SysDVR bridge status message ignored by raw H.264 input");
                continue;
            case sysdvr_bridge::MessageType::EndOfStream:
                discardPayload(header.payloadSize);
                eof = true;
                return false;
            }
            throw std::runtime_error("Unhandled SysDVR bridge message type after validation");
        }
    }

    void discardPayload(std::uint32_t byteCount) {
        std::array<std::uint8_t, 16 * 1024> scratch{};
        std::uint32_t remaining = byteCount;
        while (remaining > 0) {
            const auto chunk = std::min<std::uint32_t>(remaining, static_cast<std::uint32_t>(scratch.size()));
            if (!readExact(scratch.data(), chunk)) {
                eof = true;
                throw std::runtime_error("SysDVR bridge disconnected while discarding payload");
            }
            remaining -= chunk;
        }
    }

    bool readExact(std::uint8_t* destination, std::size_t byteCount) {
#ifdef _WIN32
        std::size_t total{};
        while (total < byteCount) {
            DWORD bytesRead{};
            const auto remaining = std::min<std::size_t>(byteCount - total, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()));
            const BOOL ok = ReadFile(pipe, destination + total, static_cast<DWORD>(remaining), &bytesRead, nullptr);
            if (!ok) {
                const DWORD error = GetLastError();
                if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA) return false;
                throw std::runtime_error("SysDVR pipe read failed: " + win32Error(error));
            }
            if (bytesRead == 0) return false;
            total += bytesRead;
        }
        return true;
#else
        (void)destination;
        (void)byteCount;
        return false;
#endif
    }
};

SysDvrPipeSource::SysDvrPipeSource(std::string pipeName) : impl_(std::make_unique<Impl>(std::move(pipeName))) {}
SysDvrPipeSource::~SysDvrPipeSource() = default;
const std::string& SysDvrPipeSource::pipeName() const noexcept { return impl_->name; }
int SysDvrPipeSource::read(std::uint8_t* destination, int destinationSize) { return impl_->read(destination, destinationSize); }

} // namespace ns60