#include <doctest/doctest.h>

#include "decode/SysDvrBridgeProtocol.h"

#include <array>
#include <cstdint>

namespace {
void writeLe16(std::array<std::uint8_t, ns60::sysdvr_bridge::HelloHeaderSize>& data, std::size_t offset, std::uint16_t value) {
    data[offset] = static_cast<std::uint8_t>(value & 0xff);
    data[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void writeLe16Stream(std::array<std::uint8_t, ns60::sysdvr_bridge::StreamHeaderSize>& data, std::size_t offset, std::uint16_t value) {
    data[offset] = static_cast<std::uint8_t>(value & 0xff);
    data[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void writeLe32(std::span<std::uint8_t> data, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) data[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

void writeLe64(std::span<std::uint8_t> data, std::size_t offset, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) data[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

std::array<std::uint8_t, ns60::sysdvr_bridge::HelloHeaderSize> validHello() {
    std::array<std::uint8_t, ns60::sysdvr_bridge::HelloHeaderSize> data{};
    data[0] = 'S'; data[1] = 'U'; data[2] = 'B'; data[3] = 'H';
    writeLe16(data, 4, ns60::sysdvr_bridge::ProtocolVersion);
    writeLe16(data, 6, ns60::sysdvr_bridge::HelloHeaderSize);
    writeLe32(data, 8, ns60::sysdvr_bridge::CapabilityVideo);
    writeLe32(data, 12, 1234);
    writeLe64(data, 16, 10'000'000);
    return data;
}

std::array<std::uint8_t, ns60::sysdvr_bridge::StreamHeaderSize> validStream() {
    std::array<std::uint8_t, ns60::sysdvr_bridge::StreamHeaderSize> data{};
    data[0] = 'S'; data[1] = 'U'; data[2] = 'B'; data[3] = 'P';
    writeLe16Stream(data, 4, ns60::sysdvr_bridge::ProtocolVersion);
    writeLe16Stream(data, 6, ns60::sysdvr_bridge::StreamHeaderSize);
    writeLe32(data, 8, static_cast<std::uint32_t>(ns60::sysdvr_bridge::MessageType::VideoPayload));
    writeLe32(data, 12, ns60::sysdvr_bridge::FlagPipeQueueRecovered);
    writeLe32(data, 16, 42);
    writeLe64(data, 24, 7);
    writeLe64(data, 32, 100);
    writeLe64(data, 40, 200);
    return data;
}
} // namespace

TEST_CASE("SysDVR bridge hello parser validates little-endian fields") {
    const auto data = validHello();
    const auto hello = ns60::sysdvr_bridge::parseHello(data);
    CHECK(hello.version == 1);
    CHECK(hello.headerSize == 32);
    CHECK(hello.bridgeProcessId == 1234);
    CHECK(hello.bridgeTimestampFrequency == 10'000'000);
}

TEST_CASE("SysDVR bridge hello rejects bad magic and version") {
    auto data = validHello();
    data[0] = 'X';
    CHECK_THROWS((void)ns60::sysdvr_bridge::parseHello(data));
    data = validHello();
    writeLe16(data, 4, 99);
    CHECK_THROWS((void)ns60::sysdvr_bridge::parseHello(data));
}

TEST_CASE("SysDVR bridge stream parser validates header fields") {
    const auto data = validStream();
    const auto header = ns60::sysdvr_bridge::parseStreamHeader(data);
    CHECK(header.type == ns60::sysdvr_bridge::MessageType::VideoPayload);
    CHECK(header.flags == ns60::sysdvr_bridge::FlagPipeQueueRecovered);
    CHECK(header.payloadSize == 42);
    CHECK(header.sequence == 7);
    CHECK(header.sourceTimestamp == 100);
    CHECK(header.bridgeTimestamp == 200);
}

TEST_CASE("SysDVR bridge stream parser rejects oversized payloads") {
    auto data = validStream();
    writeLe32(data, 16, ns60::sysdvr_bridge::MaxPayloadSize + 1);
    CHECK_THROWS((void)ns60::sysdvr_bridge::parseStreamHeader(data));
}