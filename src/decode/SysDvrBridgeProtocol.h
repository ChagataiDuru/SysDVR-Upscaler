#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace ns60::sysdvr_bridge {

inline constexpr std::uint16_t ProtocolVersion = 1;
inline constexpr std::size_t HelloHeaderSize = 32;
inline constexpr std::size_t StreamHeaderSize = 48;
inline constexpr std::size_t MaxHeaderSize = 256;
inline constexpr std::uint32_t MaxPayloadSize = 16u * 1024u * 1024u;

inline constexpr std::uint32_t CapabilityVideo = 1u << 0;
inline constexpr std::uint32_t CapabilitySourceTimestamp = 1u << 1;
inline constexpr std::uint32_t CapabilityBridgeMonotonicTimestamp = 1u << 2;
inline constexpr std::uint32_t CapabilityDiscontinuityMessages = 1u << 3;
inline constexpr std::uint32_t CapabilityStatusMessages = 1u << 4;

inline constexpr std::uint32_t FlagPipeQueueRecovered = 1u << 0;
inline constexpr std::uint32_t FlagSysDvrSourceReconnect = 1u << 1;
inline constexpr std::uint32_t FlagBridgeShuttingDown = 1u << 2;

enum class MessageType : std::uint32_t {
    VideoPayload = 1,
    Discontinuity = 2,
    EndOfStream = 3,
    Status = 4,
};

struct HelloMessage {
    std::uint16_t version{};
    std::uint16_t headerSize{};
    std::uint32_t capabilities{};
    std::uint32_t bridgeProcessId{};
    std::uint64_t bridgeTimestampFrequency{};
};

struct StreamMessageHeader {
    std::uint16_t version{};
    std::uint16_t headerSize{};
    MessageType type{};
    std::uint32_t flags{};
    std::uint32_t payloadSize{};
    std::uint64_t sequence{};
    std::uint64_t sourceTimestamp{};
    std::uint64_t bridgeTimestamp{};
};

[[nodiscard]] HelloMessage parseHello(std::span<const std::uint8_t> header);
[[nodiscard]] StreamMessageHeader parseStreamHeader(std::span<const std::uint8_t> header);
[[nodiscard]] const char* messageTypeName(MessageType type) noexcept;

} // namespace ns60::sysdvr_bridge