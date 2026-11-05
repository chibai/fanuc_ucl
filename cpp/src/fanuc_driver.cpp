#include "fanuc_snpx/fanuc_driver.hpp"

namespace fanuc {
namespace {

robot::Result<snpx::Port> select_port(robot::Port port, bool bits) {
    snpx::Port mapped;
    switch (port) {
    case robot::Port::digital_input: mapped = snpx::digital_input; break;
    case robot::Port::digital_output: mapped = snpx::digital_output; break;
    case robot::Port::analog_input: mapped = snpx::analog_input; break;
    case robot::Port::analog_output: mapped = snpx::analog_output; break;
    case robot::Port::registers: mapped = snpx::register_port; break;
    default:
        return std::unexpected(robot::Error{
            robot::Error::Code::invalid_argument, "unknown robot port"});
    }
    if (mapped.bit_packed != bits) {
        return std::unexpected(robot::Error{
            robot::Error::Code::invalid_argument, "port data type does not match the operation"});
    }
    // Normalize only this descriptor; the low-level Client keeps its native indices.
    mapped.zero_indexed = true;
    return mapped;
}

} // namespace

robot::Result<void> FanucDriver::connect(
    std::string_view host, std::uint16_t port,
    std::chrono::milliseconds timeout, std::chrono::milliseconds request_timeout) {
    return client_.connect(host, port, timeout, request_timeout);
}

void FanucDriver::disconnect() noexcept {
    client_.disconnect();
}

bool FanucDriver::connected() const noexcept {
    return client_.connected();
}

robot::Result<std::vector<bool>> FanucDriver::read_bits(
    robot::Port port, std::size_t index, std::size_t count) {
    auto mapped = select_port(port, true);
    if (!mapped) return std::unexpected(mapped.error());
    auto response = client_.read(*mapped, index, count).get();
    if (!response) return std::unexpected(response.error());
    return snpx::decode_bits(snpx::payload_bytes(*response),
                             (index + mapped->base_offset) % 8, count);
}

robot::Result<void> FanucDriver::write_bits(
    robot::Port port, std::size_t index, std::span<const bool> values) {
    auto mapped = select_port(port, true);
    if (!mapped) return std::unexpected(mapped.error());
    auto response = client_.write_bits(*mapped, index, values).get();
    if (!response) return std::unexpected(response.error());
    return {};
}

robot::Result<std::vector<std::int16_t>> FanucDriver::read_words(
    robot::Port port, std::size_t index, std::size_t count) {
    auto mapped = select_port(port, false);
    if (!mapped) return std::unexpected(mapped.error());
    auto response = client_.read(*mapped, index, count).get();
    if (!response) return std::unexpected(response.error());
    return snpx::decode_words(snpx::payload_bytes(*response), count);
}

robot::Result<void> FanucDriver::write_words(
    robot::Port port, std::size_t index, std::span<const std::int16_t> values) {
    auto mapped = select_port(port, false);
    if (!mapped) return std::unexpected(mapped.error());
    auto response = client_.write_words(*mapped, index, values).get();
    if (!response) return std::unexpected(response.error());
    return {};
}

} // namespace fanuc
