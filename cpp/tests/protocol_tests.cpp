#include "fanuc_snpx/client.hpp"
#include "fanuc_snpx/fanuc_driver.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <iostream>
#include <ranges>
#include <type_traits>
#include <vector>

#undef assert
#define assert(expression) do { if (!(expression)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << ": " << #expression << '\n'; \
    std::abort(); \
} } while (false)

using namespace fanuc::snpx;

int main() {
    static_assert(std::is_abstract_v<robot::RobotDriver>);
    static_assert(std::has_virtual_destructor_v<robot::RobotDriver>);
    static_assert(std::is_base_of_v<robot::RobotDriver, fanuc::FanucDriver>);
    static_assert(std::is_same_v<Error, robot::Error>);
    static_assert(std::is_same_v<Result<void>, robot::Result<void>>);
    {
        auto driver = std::make_unique<fanuc::FanucDriver>();
        std::unique_ptr<robot::RobotDriver> base = std::move(driver);
        assert(!base->connected());
        const auto invalid = [&](const auto& result) {
            assert(!result && result.error().code == robot::Error::Code::invalid_argument);
        };
        auto offline = base->read_bits(robot::Port::digital_output, 0);
        assert(!offline && offline.error().code == robot::Error::Code::disconnected);
        auto offline_words = base->read_words(robot::Port::registers, 0);
        assert(!offline_words && offline_words.error().code == robot::Error::Code::disconnected);
        const std::array<bool, 1> bits{true};
        const std::array<std::int16_t, 1> words{-1};
        auto offline_write = base->write_bits(robot::Port::digital_output, 0, bits);
        assert(!offline_write && offline_write.error().code == robot::Error::Code::disconnected);
        auto offline_word_write = base->write_words(robot::Port::registers, 0, words);
        assert(!offline_word_write &&
               offline_word_write.error().code == robot::Error::Code::disconnected);
        invalid(base->read_bits(robot::Port::registers, 0));
        invalid(base->read_words(robot::Port::digital_input, 0));
        invalid(base->read_bits(static_cast<robot::Port>(999), 0));
        invalid(base->write_bits(robot::Port::digital_input, 0, bits));
        invalid(base->write_words(robot::Port::analog_input, 0, words));
        invalid(base->write_bits(robot::Port::registers, 0, bits));
        invalid(base->write_words(robot::Port::digital_output, 0, words));
        invalid(base->read_bits(robot::Port::digital_output, 0, 0));
        invalid(base->read_words(robot::Port::registers, 0, 0));
        invalid(base->read_bits(robot::Port::digital_output,
                                std::numeric_limits<std::size_t>::max()));
        invalid(base->read_words(robot::Port::analog_output, 64536));
        invalid(base->write_bits(robot::Port::digital_output, 0, {}));
        invalid(base->write_words(robot::Port::registers, 0, {}));
        invalid(base->write_words(robot::Port::registers, 65535,
                                 std::array<std::int16_t, 2>{1, 2}));
        invalid(base->connect("", 60008));
        base->disconnect();
        base->disconnect();
    }

    const auto init = encode(make_init());
    assert(init.size() == fixed_frame_size);
    assert(init[0] == 0 && init[1] == 0);
    assert(init[31] == static_cast<std::uint8_t>(MessageType::init));
    auto decoded_init = decode(init);
    assert(decoded_init);
    assert(decoded_init->second == fixed_frame_size);
    assert(decoded_init->first.header.packet_type == 0);
    assert(decoded_init->first.segment == Segment::init);

    auto words = std::array<std::int16_t, 4>{-1, 0, 1, 0x1234};
    const auto encoded_words = encode_words(words);
    assert((encoded_words == std::vector<std::uint8_t>{
        0xff, 0xff, 0, 0, 1, 0, 0x34, 0x12}));
    auto decoded_words = decode_words(encoded_words, words.size());
    assert(decoded_words && std::ranges::equal(*decoded_words, words));

    const std::array<bool, 5> bits{true, false, true, true, false};
    const auto encoded_bits = encode_bits(3, bits);
    assert(encoded_bits.size() == 1);
    assert(encoded_bits[0] == 0b01101000);
    auto decoded_bits = decode_bits(encoded_bits, 3, bits.size());
    assert(decoded_bits && std::ranges::equal(*decoded_bits, bits));

    const std::array<std::uint8_t, 8> body{1, 2, 3, 4, 5, 6, 7, 8};
    auto write = make_write(42, Segment::registers, 9, 4, body);
    assert(write);
    assert(write->header.message_type == MessageType::extended_request);
    const auto extended = encode(*write);
    assert(extended.size() == fixed_frame_size + body.size());
    auto decoded_extended = decode(extended);
    assert(decoded_extended);
    assert(decoded_extended->first.header.sequence1 == 42);
    assert(decoded_extended->first.target_index == 9);
    assert(decoded_extended->first.target_size == 4);
    assert(decoded_extended->first.payload == std::vector<std::uint8_t>(body.begin(), body.end()));
    assert(decoded_extended->second == extended.size());

    const auto truncated = decode(std::span<const std::uint8_t>(extended).first(50));
    assert(!truncated);
    assert(truncated.error().code == Error::Code::protocol);

    const auto magic = encode(make_magic(37));
    assert(magic[0] == 8 && magic[2] == 37 && magic[30] == 37);
    assert(magic[42] == 0x4f && magic[43] == 1);

    const auto read = encode(make_read(9, Service::read_system_memory, Segment::input_bit, 8, 16));
    assert(read.size() == 56 && read[0] == 2 && read[2] == 9);
    assert(read[8] == 0 && read[9] == 2 && read[31] == 0xc0);
    assert(read[36] == 0x10 && read[37] == 0x0e);
    assert(read[40] == 1 && read[41] == 1);
    assert(read[42] == 4 && read[43] == 0x46 && read[44] == 8 && read[46] == 16);

    for (std::size_t length = 0; length < extended.size(); ++length) {
        assert(!decode(std::span(extended).first(length)));
    }
    auto invalid = read;
    invalid[4] = 1;
    assert(!decode(invalid));
    invalid = read;
    invalid[31] = 0xd1;
    assert(!decode(invalid));
    invalid = read;
    invalid[41] = 2;
    assert(!decode(invalid));
    invalid = extended;
    invalid[49] = 2;
    assert(!decode(invalid));

    auto joined = extended;
    joined.insert(joined.end(), read.begin(), read.end());
    auto first = decode(joined);
    assert(first && first->second == extended.size());
    auto second = decode(std::span(joined).subspan(first->second));
    assert(second && second->second == read.size());

    Message response = make_response(42);
    response.inline_payload = {0x34, 0x12, 0xff, 0xff, 0, 0};
    auto fixed_response = decode(encode(response));
    assert(fixed_response);
    auto response_words = decode_words(payload_bytes(fixed_response->first), 2);
    assert(response_words && (*response_words)[0] == 0x1234 && (*response_words)[1] == -1);
    response.header.message_type = MessageType::extended_response;
    response.header.payload_size = static_cast<std::uint16_t>(body.size());
    response.payload.assign(body.begin(), body.end());
    auto extended_response = decode(encode(response));
    assert(extended_response && payload_bytes(extended_response->first) == response.payload);

    const std::array<bool, 3> crossing{true, false, true};
    auto crossing_bytes = encode_bits(7, crossing);
    assert((crossing_bytes == std::vector<std::uint8_t>{0x80, 2}));
    auto crossing_values = decode_bits(crossing_bytes, 7, crossing.size());
    assert(crossing_values && std::ranges::equal(*crossing_values, crossing));
    assert(!decode_bits(crossing_bytes, 7, 10));
    assert(!decode_bits({}, std::numeric_limits<std::size_t>::max(), 1));
    assert(!decode_words(body, 5));
    const std::vector<std::uint8_t> oversized(65536);
    assert(!make_write(0, Segment::registers, 0, 1, oversized));

    Client client;
    assert(!client.connect("", default_port));
    assert(!client.connect("127.0.0.1", 0));
    assert(!client.connect("127.0.0.1", default_port, std::chrono::milliseconds(0)));
    assert(!client.request(make_response(0)).get());
    auto inconsistent = *write;
    inconsistent.header.payload_size = 1;
    assert(!client.request(inconsistent).get());
    assert(!client.read(digital_output, 0).get());
    assert(!client.read(digital_output, 1, 0).get());
    assert(!client.read(register_port, 65535, 3).get());
    auto excessive_read = client.read(register_port, 1, 32768).get();
    assert(!excessive_read && excessive_read.error().code == Error::Code::invalid_argument);
    assert(!client.write_bits(digital_output, std::numeric_limits<std::size_t>::max(), bits).get());
    assert(!client.write_words(analog_output, std::numeric_limits<std::size_t>::max(), words).get());
    assert(!client.write_words(group_input, 0, words).get());
    assert(!client.register_asg("A B", 2));
    assert(!client.register_asg("A", 2, std::numeric_limits<float>::quiet_NaN()));
    assert(!client.command("").get());
    auto disconnected = client.read(digital_output, 1).get();
    assert(!disconnected && disconnected.error().code == Error::Code::disconnected);
}
