#include "fanuc_snpx/client.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <thread>

#undef assert
#define assert(expression) do { if (!(expression)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << ": " << #expression << '\n'; \
    std::abort(); \
} } while (false)

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using TestSocket = SOCKET;
static constexpr TestSocket invalid_socket = INVALID_SOCKET;
static void close_socket(TestSocket socket) { closesocket(socket); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using TestSocket = int;
static constexpr TestSocket invalid_socket = -1;
static void close_socket(TestSocket socket) { close(socket); }
#endif

using namespace fanuc::snpx;
using namespace std::chrono_literals;

static bool read_exact(TestSocket socket, std::span<std::uint8_t> buffer) {
    while (!buffer.empty()) {
        const auto count = recv(socket, reinterpret_cast<char*>(buffer.data()),
                                static_cast<int>(buffer.size()), 0);
        if (count <= 0) return false;
        buffer = buffer.subspan(static_cast<std::size_t>(count));
    }
    return true;
}

static void send_all(TestSocket socket, std::span<const std::uint8_t> buffer) {
    while (!buffer.empty()) {
        const auto count = send(socket, reinterpret_cast<const char*>(buffer.data()),
                                static_cast<int>(buffer.size()), 0);
        assert(count > 0);
        buffer = buffer.subspan(static_cast<std::size_t>(count));
    }
}

static Message receive(TestSocket socket) {
    std::array<std::uint8_t, header_size> header{};
    assert(read_exact(socket, header));
    const auto length = static_cast<std::size_t>(header[4]) |
                        (static_cast<std::size_t>(header[5]) << 8);
    std::vector<std::uint8_t> frame(fixed_frame_size + length);
    std::copy(header.begin(), header.end(), frame.begin());
    assert(read_exact(socket, std::span(frame).subspan(header_size)));
    auto decoded = decode(frame);
    assert(decoded);
    return std::move(decoded->first);
}

static void reply(TestSocket socket, const Message& request) {
    send_all(socket, encode(make_response(static_cast<std::uint8_t>(request.header.sequence1))));
}

static void handshake(TestSocket socket) {
    assert(encode(receive(socket)) == encode(make_init()));
    const auto acknowledgement = encode(make_init(true));
    send_all(socket, std::span(acknowledgement).first(13));
    std::this_thread::sleep_for(2ms);
    send_all(socket, std::span(acknowledgement).subspan(13));
    const auto magic = receive(socket);
    assert(magic.service == Service::magic && magic.header.sequence1 == 1);
    reply(socket, magic);
    const auto clear = receive(socket);
    assert(clear.segment == Segment::global_byte && clear.target_size == 6);
    assert((clear.inline_payload == std::array<std::uint8_t, 6>{'C', 'L', 'R', 'A', 'S', 'G'}));
    reply(socket, clear);
}

class Controller {
public:
    explicit Controller(std::function<void(TestSocket)> serve) {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        assert(listener_ != invalid_socket);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        assert(listen(listener_, 1) == 0);
#ifdef _WIN32
        int size = sizeof(address);
#else
        socklen_t size = sizeof(address);
#endif
        assert(getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size) == 0);
        port = ntohs(address.sin_port);
        worker_ = std::jthread([this, serve = std::move(serve)] {
            const auto peer = accept(listener_, nullptr, nullptr);
            assert(peer != invalid_socket);
            const int enabled = 1;
            assert(setsockopt(peer, IPPROTO_TCP, TCP_NODELAY,
                             reinterpret_cast<const char*>(&enabled), sizeof(enabled)) == 0);
            serve(peer);
            close_socket(peer);
        });
    }

    ~Controller() {
        worker_.join();
        close_socket(listener_);
    }

    std::uint16_t port{};

private:
    TestSocket listener_{invalid_socket};
    std::jthread worker_;
};

static Client::Response await(Client::ResponseFuture future) {
    assert(future.wait_for(2s) == std::future_status::ready);
    return future.get();
}

static void normal_session(Client& client) {
    Controller controller([](TestSocket peer) {
        handshake(peer);
        const auto first = receive(peer);
        const auto second = receive(peer);
        assert(first.target_index == 0 && first.target_size == 16);
        auto second_reply = make_response(static_cast<std::uint8_t>(second.header.sequence1));
        second_reply.inline_payload[0] = 0x34;
        auto first_reply = make_response(static_cast<std::uint8_t>(first.header.sequence1));
        first_reply.inline_payload[0] = 0x80;
        first_reply.inline_payload[1] = 1;
        auto joined = encode(second_reply);
        const auto other = encode(first_reply);
        joined.insert(joined.end(), other.begin(), other.end());
        send_all(peer, joined);

        const auto write = receive(peer);
        assert(write.target_index == 7 && write.target_size == 2);
        assert(write.inline_payload[0] == 0x80 && write.inline_payload[1] == 1);
        reply(peer, write);
        const auto asg = receive(peer);
        const std::string statement(asg.payload.begin(), asg.payload.end());
        assert(statement == "SETASG 1 2 $TEST.$VALUE 1");
        reply(peer, asg);
        const auto asg_read = receive(peer);
        assert(asg_read.target_index == 0 && asg_read.target_size == 2);
        auto response = make_response(static_cast<std::uint8_t>(asg_read.header.sequence1));
        response.header.message_type = MessageType::extended_response;
        response.header.payload_size = 4;
        response.payload = {1, 0, 0xff, 0xff};
        send_all(peer, encode(response));
        std::array<std::uint8_t, 1> end{};
        assert(!read_exact(peer, end));
    });
    assert(client.connect("127.0.0.1", controller.port, 2s));
    auto first = client.read(digital_output, 8, 2);
    auto second = client.read(register_port, 1);
    auto second_result = await(std::move(second));
    auto first_result = await(std::move(first));
    assert(first_result && second_result);
    auto bits = decode_bits(payload_bytes(*first_result), 7, 2);
    assert(bits && (*bits)[0] && (*bits)[1]);
    assert(payload_bytes(*second_result)[0] == 0x34);
    const std::array<bool, 2> values{true, true};
    assert(await(client.write_bits(digital_output, 8, values)));
    auto address = client.register_asg("$TEST.$VALUE", 2);
    assert(address && *address == 1);
    auto cached = client.register_asg("$TEST.$VALUE", 2);
    assert(cached && *cached == *address);
    assert(!client.register_asg("$TEST.$VALUE", 3));
    assert(!client.register_asg("$TEST.$VALUE", 2, 2.0F));
    auto response = await(client.read_asg(*address, 2));
    assert(response);
    auto words = decode_words(payload_bytes(*response), 2);
    assert(words && (*words)[0] == 1 && (*words)[1] == -1);
    client.disconnect();
    assert(!client.connected());
    assert(!client.register_asg("$TEST.$VALUE", 2));
}

int main() {
#ifdef _WIN32
    WSADATA winsock{};
    assert(WSAStartup(MAKEWORD(2, 2), &winsock) == 0);
#endif
    Client client;
    normal_session(client);
    normal_session(client);

    {
        Controller controller([](TestSocket peer) {
            handshake(peer);
            for (int i = 0; i < 80; ++i) reply(peer, receive(peer));
        });
        assert(client.connect("127.0.0.1", controller.port, 2s));
        std::vector<std::jthread> callers;
        for (int i = 0; i < 4; ++i) {
            callers.emplace_back([&client] {
                for (int j = 0; j < 20; ++j) assert(await(client.read(digital_output, 1)));
            });
        }
        for (auto& caller : callers) caller.join();
        client.disconnect();
    }
    {
        Controller controller([](TestSocket peer) {
            handshake(peer);
            for (int i = 0; i < 300; ++i) reply(peer, receive(peer));
        });
        assert(client.connect("127.0.0.1", controller.port, 2s));
        for (int i = 0; i < 300; ++i) assert(await(client.read(digital_output, 1)));
        client.disconnect();
    }
    {
        Controller controller([](TestSocket peer) {
            handshake(peer);
            std::array<std::uint8_t, 1> byte{};
            while (read_exact(peer, byte)) {}
        });
        assert(client.connect("127.0.0.1", controller.port, 2s, 40ms));
        auto first = client.read(digital_output, 1);
        auto second = client.read(digital_output, 2);
        auto a = await(std::move(first));
        auto b = await(std::move(second));
        assert(!a && !b);
        assert(a.error().code == Error::Code::timeout && b.error().code == Error::Code::timeout);
        assert(!client.connected());
        client.disconnect();
    }
    {
        Controller controller([](TestSocket peer) {
            handshake(peer);
            std::array<std::uint8_t, 1> byte{};
            while (read_exact(peer, byte)) {}
        });
        assert(client.connect("127.0.0.1", controller.port, 2s, 5s));
        std::vector<Client::ResponseFuture> futures;
        for (int i = 0; i < 256; ++i) futures.push_back(client.read(digital_output, 1));
        auto rejected = await(client.read(digital_output, 1));
        assert(!rejected && rejected.error().code == Error::Code::resource_exhausted);
        client.disconnect();
        for (auto& future : futures) {
            auto result = await(std::move(future));
            assert(!result && result.error().code == Error::Code::disconnected);
        }
    }
    {
        Controller controller([](TestSocket peer) {
            auto init = receive(peer);
            assert(init.header.message_type == MessageType::init);
            auto invalid = make_init(true);
            invalid.inline_payload[0] = 1;
            send_all(peer, encode(invalid));
        });
        auto result = client.connect("127.0.0.1", controller.port, 2s);
        assert(!result && result.error().code == Error::Code::protocol);
        assert(!client.connected());
    }
    {
        Controller controller([](TestSocket peer) {
            receive(peer);
            std::array<std::uint8_t, 1> byte{};
            assert(!read_exact(peer, byte));
        });
        auto result = client.connect("127.0.0.1", controller.port, 40ms);
        assert(!result && result.error().code == Error::Code::timeout);
    }
    {
        Controller controller([](TestSocket peer) {
            handshake(peer);
            const auto request = receive(peer);
            send_all(peer, encode(make_read(static_cast<std::uint8_t>(request.header.sequence1),
                                           Service::read_system_memory, Segment::registers, 0, 1)));
        });
        assert(client.connect("127.0.0.1", controller.port, 2s));
        auto result = await(client.read(digital_output, 1));
        assert(!result && result.error().code == Error::Code::protocol);
        client.disconnect();
    }
    {
        Controller controller([](TestSocket peer) {
            handshake(peer);
            const auto registration = receive(peer);
            assert(registration.segment == Segment::global_byte);
            std::array<std::uint8_t, 1> byte{};
            assert(!read_exact(peer, byte));
        });
        assert(client.connect("127.0.0.1", controller.port, 2s));
        auto result = client.register_asg("$TEST.$VALUE", 2, 1.0F, 40ms);
        assert(!result && result.error().code == Error::Code::timeout);
        assert(!client.connected());
    }
    for (const bool error_frame : {false, true}) {
        Controller controller([error_frame](TestSocket peer) {
            handshake(peer);
            const auto request = receive(peer);
            auto response = encode(make_response(
                static_cast<std::uint8_t>(request.header.sequence1 + (error_frame ? 0 : 1))));
            if (error_frame) response[31] = 0xd1;
            send_all(peer, response);
        });
        assert(client.connect("127.0.0.1", controller.port, 2s));
        auto result = await(client.read(digital_output, 1));
        assert(!result && result.error().code == Error::Code::protocol);
        client.disconnect();
    }
    {
        Controller controller([](TestSocket peer) {
            handshake(peer);
            receive(peer);
        });
        assert(client.connect("127.0.0.1", controller.port, 2s));
        auto result = await(client.read(digital_output, 1));
        assert(!result && result.error().code == Error::Code::disconnected);
        client.disconnect();
    }
#ifdef _WIN32
    WSACleanup();
#endif
}
