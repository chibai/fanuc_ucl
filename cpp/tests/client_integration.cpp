#include "fanuc_snpx/client.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <span>
#include <thread>
#include <vector>

#undef assert
#define assert(expression) do { if (!(expression)) std::abort(); } while (false)

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using TestSocket = SOCKET;
static constexpr TestSocket invalid_socket = INVALID_SOCKET;
static void close_test_socket(TestSocket socket) { closesocket(socket); }
static int receive_bytes(TestSocket socket, char* data, int size) {
    return recv(socket, data, size, 0);
}
static int send_bytes(TestSocket socket, const char* data, int size) {
    return send(socket, data, size, 0);
}
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using TestSocket = int;
static constexpr TestSocket invalid_socket = -1;
static void close_test_socket(TestSocket socket) { close(socket); }
static ssize_t receive_bytes(TestSocket socket, char* data, std::size_t size) {
    return recv(socket, data, size, 0);
}
static ssize_t send_bytes(TestSocket socket, const char* data, std::size_t size) {
    return send(socket, data, size, 0);
}
#endif

using namespace fanuc::snpx;

static bool read_exact(TestSocket socket, std::span<std::uint8_t> buffer) {
    std::size_t offset = 0;
    while (offset < buffer.size()) {
        const auto count = receive_bytes(
            socket, reinterpret_cast<char*>(buffer.data() + offset),
            static_cast<int>(buffer.size() - offset));
        if (count <= 0) return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

static bool send_all(TestSocket socket, std::span<const std::uint8_t> buffer) {
    std::size_t offset = 0;
    while (offset < buffer.size()) {
        const auto count = send_bytes(
            socket, reinterpret_cast<const char*>(buffer.data() + offset),
            static_cast<int>(buffer.size() - offset));
        if (count <= 0) return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

int main() {
#ifdef _WIN32
    WSADATA winsock{};
    assert(WSAStartup(MAKEWORD(2, 2), &winsock) == 0);
#endif
    const auto listener = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(listener != invalid_socket);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = 0;
    assert(bind(listener, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0);
    assert(listen(listener, 1) == 0);
#ifdef _WIN32
    int local_size = sizeof(local);
#else
    socklen_t local_size = sizeof(local);
#endif
    assert(getsockname(listener, reinterpret_cast<sockaddr*>(&local), &local_size) == 0);
    const auto port = ntohs(local.sin_port);

    std::promise<bool> server_result;
    auto server_done = server_result.get_future();
    std::jthread server([&] {
        const auto peer = accept(listener, nullptr, nullptr);
        if (peer == invalid_socket) {
            server_result.set_value(false);
            return;
        }
        bool success = true;
        for (int request_number = 0; request_number < 7; ++request_number) {
            std::array<std::uint8_t, header_size> header{};
            if (!read_exact(peer, header)) {
                success = false;
                break;
            }
            const auto payload_size = static_cast<std::size_t>(header[4]) |
                                      (static_cast<std::size_t>(header[5]) << 8);
            std::vector<std::uint8_t> frame(fixed_frame_size + payload_size);
            std::copy(header.begin(), header.end(), frame.begin());
            if (!read_exact(peer, std::span(frame).subspan(header_size))) {
                success = false;
                break;
            }
            auto request = decode(frame);
            if (!request) {
                success = false;
                break;
            }
            const auto& incoming = request->first;
            Message response;
            if (incoming.header.message_type == MessageType::init) {
                response = make_init(true);
            } else if (incoming.service == Service::read_system_memory) {
                response = make_response(static_cast<std::uint8_t>(incoming.header.sequence1));
                response.header.message_type = MessageType::extended_response;
                response.header.payload_size = 4;
                response.header.operation1 = static_cast<std::uint32_t>(Operation::reading);
                response.header.operation2 = static_cast<std::uint32_t>(Operation::reading);
                response.payload = {0, 0, 0, 0};
            } else {
                response = make_response(static_cast<std::uint8_t>(incoming.header.sequence1));
            }
            const auto bytes = encode(response);
            if (!send_all(peer, bytes)) {
                success = false;
                break;
            }
        }
        close_test_socket(peer);
        server_result.set_value(success);
    });

    Client client;
    auto connected = client.connect("127.0.0.1", port, std::chrono::seconds(2));
    assert(connected);

    auto read_do = client.read(digital_output, 1);
    assert(read_do.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    auto do_result = read_do.get();
    assert(do_result);

    const std::array<bool, 1> set_bit{true};
    auto refused_input_write = client.write_bits(digital_input, 0, set_bit);
    assert(refused_input_write.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    assert(!refused_input_write.get());

    auto write_do = client.write_bits(digital_output, 1, set_bit);
    assert(write_do.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    assert(write_do.get());

    auto mapped = client.register_asg("$TEST.$VALUE", 2, 1.0F, std::chrono::seconds(1));
    assert(mapped && *mapped == 1);
    auto asg_read = client.read_asg(*mapped, 2);
    assert(asg_read.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    auto asg_result = asg_read.get();
    assert(asg_result);
    assert(decode_words(payload_bytes(*asg_result), 2));
    client.disconnect();

    assert(server_done.get());
    server.join();
    close_test_socket(listener);
#ifdef _WIN32
    WSACleanup();
#endif
}
