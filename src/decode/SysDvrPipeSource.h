#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace ns60 {

class SysDvrPipeSource final {
public:
    explicit SysDvrPipeSource(std::string pipeName);
    ~SysDvrPipeSource();
    SysDvrPipeSource(const SysDvrPipeSource&) = delete;
    SysDvrPipeSource& operator=(const SysDvrPipeSource&) = delete;

    [[nodiscard]] const std::string& pipeName() const noexcept;
    [[nodiscard]] int read(std::uint8_t* destination, int destinationSize);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ns60
