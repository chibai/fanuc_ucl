#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fanuc::snpx {

inline constexpr std::uint16_t default_port = 60008;
inline constexpr std::size_t header_size = 42;
inline constexpr std::size_t body_size = 14;
inline constexpr std::size_t fixed_frame_size = header_size + body_size;

enum class MessageType : std::uint8_t {
    init = 0x00,
    error = 0xd1,
    request = 0xc0,
    response = 0xd4,
    extended_request = 0x80,
    extended_response = 0x94,
};

enum class Service : std::uint8_t {
    plc_status = 0x00,
    read_system_memory = 0x04,
    write_system_memory = 0x07,
    magic = 0x4f,
};

enum class Segment : std::uint8_t {
    init = 0x00,
    magic = 0x01,
    registers = 0x08,
    analog_input = 0x0a,
    analog_output = 0x0c,
    global_byte = 0x38,
    input_bit = 0x46,
    output_bit = 0x48,
};

enum class Operation : std::uint32_t {
    init = 0,
    init_finish = 1,
    writing = 0x100,
    reading = 0x200,
};

struct Error {
    enum class Code {
        invalid_argument,
        io,
        timeout,
        disconnected,
        protocol,
        resource_exhausted,
    };

    Code code;
    std::string message;
};

template <class T>
using Result = std::expected<T, Error>;

struct Header {
    std::uint16_t packet_type{};
    std::uint16_t sequence1{};
    std::uint16_t payload_size{};
    std::uint16_t unknown1{};
    std::uint32_t operation1{};
    std::uint32_t unknown2{};
    std::uint32_t operation2{};
    std::uint32_t unknown3{};
    std::uint16_t unknown4{};
    std::array<std::uint8_t, 3> time{};
    std::uint8_t unknown5{};
    std::uint8_t sequence2{};
    MessageType message_type{MessageType::request};
    std::uint32_t mailbox_source{};
    std::uint32_t mailbox_destination{};
    std::uint8_t packet_number{};
    std::uint8_t total_packets{};

    bool operator==(const Header&) const = default;
};

struct Message {
    Header header;
    Service service{Service::plc_status};
    Segment segment{Segment::init};
    std::uint16_t target_index{};
    std::uint16_t target_size{};
    std::array<std::uint8_t, 6> inline_payload{};
    std::uint16_t response_unknown{};
    std::array<std::uint8_t, 6> plc_status{};
    std::vector<std::uint8_t> payload;
};

[[nodiscard]] Message make_init(bool acknowledgement = false);
[[nodiscard]] Message make_magic(std::uint8_t sequence);
[[nodiscard]] Message make_read(
    std::uint8_t sequence, Service service, Segment segment,
    std::uint16_t index, std::uint16_t count);
[[nodiscard]] Result<Message> make_write(
    std::uint8_t sequence, Segment segment, std::uint16_t index,
    std::uint16_t item_count, std::span<const std::uint8_t> data);
[[nodiscard]] Message make_response(std::uint8_t sequence);

[[nodiscard]] std::vector<std::uint8_t> encode(const Message& message);
[[nodiscard]] Result<std::pair<Message, std::size_t>> decode(
    std::span<const std::uint8_t> bytes);
[[nodiscard]] std::vector<std::uint8_t> payload_bytes(const Message& message);

struct Port {
    Segment segment;
    std::uint16_t base_offset;
    bool zero_indexed;
    bool bit_packed;
    bool writable;
};

inline constexpr Port digital_input{Segment::output_bit, 0, true, true, false};
inline constexpr Port digital_output{Segment::input_bit, 0, false, true, true};
inline constexpr Port robot_input{Segment::output_bit, 5000, true, true, false};
inline constexpr Port robot_output{Segment::input_bit, 5000, true, true, true};
inline constexpr Port uop_input{Segment::output_bit, 6000, true, true, false};
inline constexpr Port uop_output{Segment::input_bit, 6000, true, true, true};
inline constexpr Port sop_input{Segment::output_bit, 7000, false, true, false};
inline constexpr Port sop_output{Segment::input_bit, 7000, false, true, true};
inline constexpr Port weld_input{Segment::output_bit, 8000, true, true, false};
inline constexpr Port weld_output{Segment::input_bit, 8000, true, true, true};
inline constexpr Port wire_stick_input{Segment::output_bit, 8400, true, true, false};
inline constexpr Port wire_stick_output{Segment::input_bit, 8400, true, true, true};
inline constexpr Port group_input{Segment::analog_output, 0, true, false, false};
inline constexpr Port group_output{Segment::analog_input, 0, false, false, true};
inline constexpr Port analog_input{Segment::analog_output, 1000, true, false, false};
inline constexpr Port analog_output{Segment::analog_input, 1000, false, false, true};
inline constexpr Port register_port{Segment::registers, 0, false, false, true};

class Client {
public:
    using Response = Result<Message>;
    using ResponseFuture = std::future<Response>;

    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) = delete;
    Client& operator=(Client&&) = delete;

    [[nodiscard]] Result<void> connect(
        std::string_view host,
        std::uint16_t port = default_port,
        std::chrono::milliseconds timeout = std::chrono::seconds(1),
        std::chrono::milliseconds request_timeout = std::chrono::seconds(1));
    void disconnect() noexcept;
    [[nodiscard]] bool connected() const noexcept;

    [[nodiscard]] ResponseFuture request(Message message);
    [[nodiscard]] ResponseFuture read(Port port, std::size_t index, std::size_t count = 1);
    [[nodiscard]] ResponseFuture write_bits(
        Port port, std::size_t index, std::span<const bool> values);
    [[nodiscard]] ResponseFuture write_words(
        Port port, std::size_t index, std::span<const std::int16_t> values);
    [[nodiscard]] ResponseFuture command(std::string_view text);

    [[nodiscard]] Result<std::uint16_t> register_asg(
        std::string_view variable, std::uint16_t registers, float multiplier = 1.0F,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(100));
    [[nodiscard]] ResponseFuture read_asg(std::uint16_t address, std::uint16_t registers);
    [[nodiscard]] ResponseFuture write_asg(
        std::uint16_t address, std::span<const std::int16_t> words);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] Result<std::vector<bool>> decode_bits(
    std::span<const std::uint8_t> bytes, std::size_t start_bit, std::size_t count);
[[nodiscard]] Result<std::vector<std::int16_t>> decode_words(
    std::span<const std::uint8_t> bytes, std::size_t count);
[[nodiscard]] std::vector<std::uint8_t> encode_bits(
    std::size_t start_bit, std::span<const bool> values);
[[nodiscard]] std::vector<std::uint8_t> encode_words(std::span<const std::int16_t> values);

} // namespace fanuc::snpx
