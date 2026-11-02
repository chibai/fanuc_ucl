#include "fanuc_snpx/client.hpp"

#include <array>
#include <chrono>
#include <iostream>

using namespace std::chrono_literals;

int main() {
    fanuc::snpx::Client client;
    auto connected = client.connect("10.0.0.1", fanuc::snpx::default_port, 1500ms);
    if (!connected) {
        std::cerr << "connect/handshake failed: " << connected.error().message << '\n';
        return 1;
    }

    auto read_do = client.read(fanuc::snpx::digital_output, 1);
    if (read_do.wait_for(250ms) != std::future_status::ready) {
        std::cerr << "DO read timed out\n";
        return 1;
    }
    auto do_response = read_do.get();
    if (!do_response) {
        std::cerr << do_response.error().message << '\n';
        return 1;
    }
    const auto do_bytes = fanuc::snpx::payload_bytes(*do_response);
    auto do_value = fanuc::snpx::decode_bits(do_bytes, 0, 1);
    if (!do_value) {
        std::cerr << do_value.error().message << '\n';
        return 1;
    }
    std::cout << "DO1 = " << ((*do_value)[0] ? "ON" : "OFF") << '\n';

    // This write changes a physical controller output. Use only after
    // validating the signal mapping and machine safety conditions.
    const std::array<bool, 1> requested{true};
    auto write_do = client.write_bits(fanuc::snpx::digital_output, 1, requested);
    if (write_do.wait_for(250ms) != std::future_status::ready) {
        std::cerr << "DO write acknowledgement timed out\n";
        return 1;
    }
    if (auto result = write_do.get(); !result) {
        std::cerr << result.error().message << '\n';
        return 1;
    }

    auto asg_address = client.register_asg("$MCGR_CFG.$ENABLE", 2, 1.0F, 250ms);
    if (!asg_address) {
        std::cerr << "SETASG failed: " << asg_address.error().message << '\n';
        return 1;
    }
    auto read_enable = client.read_asg(*asg_address, 2);
    if (read_enable.wait_for(250ms) != std::future_status::ready) {
        std::cerr << "ASG read timed out\n";
        return 1;
    }
    auto enable_response = read_enable.get();
    if (!enable_response) {
        std::cerr << enable_response.error().message << '\n';
        return 1;
    }
    const auto words = fanuc::snpx::payload_bytes(*enable_response);
    auto decoded_words = fanuc::snpx::decode_words(words, 2);
    if (!decoded_words) {
        std::cerr << decoded_words.error().message << '\n';
        return 1;
    }
    std::cout << "ASG mapped at R[" << *asg_address << "]\n";
    client.disconnect();
}
