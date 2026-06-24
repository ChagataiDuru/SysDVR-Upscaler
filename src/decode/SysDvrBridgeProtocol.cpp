#include "decode/SysDvrBridgeProtocol.h"

#include <algorithm>
#include <array>
#include <format>
#include <stdexcept>

namespace ns60::sysdvr_bridge {
namespace {
constexpr std::array<std::uint8_t, 4> HelloMagic{'S', 'U', 'B', 'H'};
constexpr std::array<std::uint8_t, 4> StreamMagic{'S', 'U', 'B', 'P'};

std::uint16_t readLe16(std::span<const std::uint8_t> data, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(data[offset]) |
        static_cast<std::uint16_t>(data[offset + 1] << 8);
}

std::uint32_t readLe32(std::span<const std::uint8_t> data, std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(data[offset]) |
        (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
        (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
        (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

std::uint64_t readLe64(std::span<const std::uint8_t> data, std::size_t offset) noexcept {
    std::uint64_t value{};
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(data[offset + i]) << (8 * i);
    }
    return value;
}

void requireVersion(std::uint16_t version) {
    if (version != ProtocolVersion) {
        throw std::runtime_error(std::format("Unsupported SysDVR bridge protocol version {}", version));
    }
}

void requireHeaderSize(std::uint16_t actual, std::size_t minimum, std::size_t maximum) {
    if (actual < minimum) {
        throw std::runtime_error(std::format("SysDVR bridge header is too small: {} bytes", actual));
    }
    if (actual > maximum || actual > MaxHeaderSize) {
        throw std::runtime_error(std::format("SysDVR bridge header is too large: {} bytes", actual));
    }
}
} // namespace

HelloMessage parseHello(std::span<const std::uint8_t> header) {
    if (header.size() < HelloHeaderSize) {
        throw std::runtime_error("Truncated SysDVR bridge hello header");
    }
    if (!std::equal(HelloMagic.begin(), HelloMagic.end(), header.begin())) {
        throw std::runtime_error("Invalid SysDVR bridge hello magic");
    }

    HelloMessage hello;
    hello.version = readLe16(header, 4);
    hello.headerSize = readLe16(header, 6);
    requireVersion(hello.version);
    requireHeaderSize(hello.headerSize, HelloHeaderSize, HelloHeaderSize);
    hello.capabilities = readLe32(header, 8);
    hello.bridgeProcessId = readLe32(header, 12);
    hello.bridgeTimestampFrequency = readLe64(header, 16);
    if ((hello.capabilities & CapabilityVideo) == 0) {
        throw std::runtime_error("SysDVR bridge hello does not advertise video capability");
    }
    return hello;
}

StreamMessageHeader parseStreamHeader(std::span<const std::uint8_t> header) {
    if (header.size() < StreamHeaderSize) {
        throw std::runtime_error("Truncated SysDVR bridge stream header");
    }
    if (!std::equal(StreamMagic.begin(), StreamMagic.end(), header.begin())) {
        throw std::runtime_error("Invalid SysDVR bridge stream magic");
    }

    StreamMessageHeader parsed;
    parsed.version = readLe16(header, 4);
    parsed.headerSize = readLe16(header, 6);
    requireVersion(parsed.version);
    requireHeaderSize(parsed.headerSize, StreamHeaderSize, StreamHeaderSize);
    parsed.type = static_cast<MessageType>(readLe32(header, 8));
    parsed.flags = readLe32(header, 12);
    parsed.payloadSize = readLe32(header, 16);
    parsed.sequence = readLe64(header, 24);
    parsed.sourceTimestamp = readLe64(header, 32);
    parsed.bridgeTimestamp = readLe64(header, 40);

    if (parsed.payloadSize > MaxPayloadSize) {
        throw std::runtime_error(std::format("SysDVR bridge payload is too large: {} bytes", parsed.payloadSize));
    }
    switch (parsed.type) {
    case MessageType::VideoPayload:
    case MessageType::Discontinuity:
    case MessageType::EndOfStream:
    case MessageType::Status:
        break;
    default:
        throw std::runtime_error(std::format("Unknown SysDVR bridge message type {}", static_cast<std::uint32_t>(parsed.type)));
    }
    return parsed;
}

const char* messageTypeName(MessageType type) noexcept {
    switch (type) {
    case MessageType::VideoPayload: return "VideoPayload";
    case MessageType::Discontinuity: return "Discontinuity";
    case MessageType::EndOfStream: return "EndOfStream";
    case MessageType::Status: return "Status";
    }
    return "Unknown";
}

} // namespace ns60::sysdvr_bridge