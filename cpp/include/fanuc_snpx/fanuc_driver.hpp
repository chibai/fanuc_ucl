#pragma once

#include "fanuc_snpx/client.hpp"
#include "robot/robot_driver.hpp"

namespace fanuc {

class FanucDriver final : public robot::RobotDriver {
public:
    [[nodiscard]] robot::Result<void> connect(
        std::string_view host, std::uint16_t port = snpx::default_port,
        std::chrono::milliseconds timeout = std::chrono::seconds(1),
        std::chrono::milliseconds request_timeout = std::chrono::seconds(1)) override;
    void disconnect() noexcept override;
    [[nodiscard]] bool connected() const noexcept override;

    [[nodiscard]] robot::Result<std::vector<bool>> read_bits(
        robot::Port port, std::size_t index, std::size_t count = 1) override;
    [[nodiscard]] robot::Result<void> write_bits(
        robot::Port port, std::size_t index, std::span<const bool> values) override;
    [[nodiscard]] robot::Result<std::vector<std::int16_t>> read_words(
        robot::Port port, std::size_t index, std::size_t count = 1) override;
    [[nodiscard]] robot::Result<void> write_words(
        robot::Port port, std::size_t index, std::span<const std::int16_t> values) override;

private:
    snpx::Client client_;
};

} // namespace fanuc
