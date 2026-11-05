#pragma once

#include "robot/types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace robot {

enum class Port {
    digital_input,
    digital_output,
    analog_input,
    analog_output,
    registers,
};

class RobotDriver {
public:
    virtual ~RobotDriver() = default;
    RobotDriver(const RobotDriver&) = delete;
    RobotDriver& operator=(const RobotDriver&) = delete;
    RobotDriver(RobotDriver&&) = delete;
    RobotDriver& operator=(RobotDriver&&) = delete;

    [[nodiscard]] virtual Result<void> connect(
        std::string_view host, std::uint16_t port,
        std::chrono::milliseconds timeout = std::chrono::seconds(1),
        std::chrono::milliseconds request_timeout = std::chrono::seconds(1)) = 0;
    virtual void disconnect() noexcept = 0;
    [[nodiscard]] virtual bool connected() const noexcept = 0;

    // All ports use zero-based indices; operations wait for the device response.
    [[nodiscard]] virtual Result<std::vector<bool>> read_bits(
        Port port, std::size_t index, std::size_t count = 1) = 0;
    [[nodiscard]] virtual Result<void> write_bits(
        Port port, std::size_t index, std::span<const bool> values) = 0;
    [[nodiscard]] virtual Result<std::vector<std::int16_t>> read_words(
        Port port, std::size_t index, std::size_t count = 1) = 0;
    [[nodiscard]] virtual Result<void> write_words(
        Port port, std::size_t index, std::span<const std::int16_t> values) = 0;

protected:
    RobotDriver() = default;
};

} // namespace robot
