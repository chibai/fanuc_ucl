#include "fanuc_snpx/client.hpp"

#include <chrono>
#include <iostream>

using namespace std::chrono_literals;

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: snpx_example <controller-ip-or-host>\n"
                     "Reads DO1 only. Connecting clears the controller ASG table.\n";
        return 2;
    }

    fanuc::snpx::Client client;
    auto connected = client.connect(argv[1], fanuc::snpx::default_port, 1500ms, 250ms);
    if (!connected) {
        std::cerr << "connect/handshake failed: " << connected.error().message << '\n';
        return 1;
    }

    auto response = client.read(fanuc::snpx::digital_output, 1).get();
    if (!response) {
        std::cerr << response.error().message << '\n';
        return 1;
    }
    const auto bytes = fanuc::snpx::payload_bytes(*response);
    auto values = fanuc::snpx::decode_bits(bytes, 0, 1);
    if (!values) {
        std::cerr << values.error().message << '\n';
        return 1;
    }
    std::cout << "DO1 = " << ((*values)[0] ? "ON" : "OFF") << '\n';
}
