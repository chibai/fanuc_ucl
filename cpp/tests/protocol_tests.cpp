#include "fanuc_snpx/client.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <ranges>
#include <vector>

#undef assert
#define assert(expression) do { if (!(expression)) std::abort(); } while (false)

using namespace fanuc::snpx;

int main() {
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
}
