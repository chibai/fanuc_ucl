#include "fanuc_snpx/client.hpp"
#include "fanuc_snpx/fanuc_driver.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <thread>

#include <boost/asio.hpp>

#undef assert
#define assert(expression) do { if (!(expression)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << ": " << #expression << '\n'; \
    std::abort(); \
} } while (false)

namespace asio = boost::asio;
using Tcp = asio::ip::tcp;
using TestSocket = Tcp::socket&;

using namespace fanuc::snpx;
using namespace std::chrono_literals;

static bool read_exact(TestSocket socket, std::span<std::uint8_t> buffer) {
    boost::system::error_code error;
    asio::read(socket, asio::buffer(buffer.data(), buffer.size()), error);
    if (error == asio::error::eof || error == asio::error::connection_reset) return false;
    assert(!error);
    return !error;
}

static void send_all(TestSocket socket, std::span<const std::uint8_t> buffer) {
    boost::system::error_code error;
    asio::write(socket, asio::buffer(buffer.data(), buffer.size()), error);
    assert(!error);
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
    explicit Controller(std::function<void(TestSocket)> serve)
        : listener_(context_, Tcp::endpoint(asio::ip::address_v4::loopback(), 0)) {
        port = listener_.local_endpoint().port();
        worker_ = std::jthread([this, serve = std::move(serve)] {
            auto peer = listener_.accept();
            peer.set_option(Tcp::no_delay(true));
            serve(peer);
        });
    }

    ~Controller() {
        worker_.join();
    }

    std::uint16_t port{};

private:
    asio::io_context context_;
    Tcp::acceptor listener_;
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
    assert(client.connect("localhost", controller.port, 2s));
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

static void driver_session(robot::RobotDriver& driver) {
    Controller controller([](TestSocket peer) {
        handshake(peer);
        for (auto segment : {Segment::output_bit, Segment::input_bit}) {
            for (std::size_t offset = 0; offset < 8; ++offset) {
                const auto request = receive(peer);
                assert(request.segment == segment);
                assert(request.target_index == 0);
                assert(request.target_size == (offset == 7 ? 16 : 8));
                auto response = make_response(static_cast<std::uint8_t>(request.header.sequence1));
                response.inline_payload[0] = 0x80;
                response.inline_payload[1] = 1;
                send_all(peer, encode(response));
            }
        }
        for (auto segment : {Segment::analog_output, Segment::analog_input, Segment::registers}) {
            const auto request = receive(peer);
            assert(request.segment == segment);
            assert(request.target_index == (segment == Segment::registers ? 0 : 1000));
            assert(request.target_size == 2);
            auto response = make_response(static_cast<std::uint8_t>(request.header.sequence1));
            response.inline_payload = {0xff, 0xff, 0x34, 0x12, 0, 0};
            send_all(peer, encode(response));
        }
        const auto bit_write = receive(peer);
        assert(bit_write.segment == Segment::input_bit);
        assert(bit_write.target_index == 7 && bit_write.target_size == 2);
        assert(bit_write.inline_payload[0] == 0x80 && bit_write.inline_payload[1] == 1);
        reply(peer, bit_write);
        for (auto segment : {Segment::analog_input, Segment::registers}) {
            const auto request = receive(peer);
            assert(request.segment == segment);
            assert(request.target_index == (segment == Segment::registers ? 0 : 1000));
            assert(request.target_size == 2);
            assert(request.inline_payload[0] == 0xff && request.inline_payload[1] == 0xff);
            assert(request.inline_payload[2] == 0x34 && request.inline_payload[3] == 0x12);
            reply(peer, request);
        }
        const auto last_register = receive(peer);
        assert(last_register.segment == Segment::registers);
        assert(last_register.target_index == 65535 && last_register.target_size == 1);
        reply(peer, last_register);

        const auto short_read = receive(peer);
        assert(short_read.target_size == 4);
        auto response = make_response(static_cast<std::uint8_t>(short_read.header.sequence1));
        response.header.message_type = MessageType::extended_response;
        response.header.payload_size = 2;
        response.payload = {1, 0};
        send_all(peer, encode(response));
        std::array<std::uint8_t, 1> end{};
        assert(!read_exact(peer, end));
    });
    assert(driver.connect("127.0.0.1", controller.port, 2s));
    assert(driver.connected());
    for (auto port : {robot::Port::digital_input, robot::Port::digital_output}) {
        for (std::size_t offset = 0; offset < 8; ++offset) {
            const auto bits = driver.read_bits(port, offset, 2);
            assert(bits && bits->size() == 2);
            assert((*bits)[0] == (offset == 7));
            assert((*bits)[1] == (offset >= 6));
        }
    }
    for (auto port : {robot::Port::analog_input, robot::Port::analog_output, robot::Port::registers}) {
        const auto words = driver.read_words(port, 0, 2);
        assert((words && *words == std::vector<std::int16_t>{-1, 0x1234}));
    }
    const std::array<bool, 2> bits{true, true};
    assert(driver.write_bits(robot::Port::digital_output, 7, bits));
    const std::array<std::int16_t, 2> words{-1, 0x1234};
    assert(driver.write_words(robot::Port::analog_output, 0, words));
    assert(driver.write_words(robot::Port::registers, 0, words));
    const auto last_register = driver.read_words(robot::Port::registers, 65535);
    assert(last_register && last_register->size() == 1);
    const auto short_read = driver.read_words(robot::Port::registers, 0, 4);
    assert(!short_read && short_read.error().code == robot::Error::Code::protocol);
    driver.disconnect();
    assert(!driver.connected());
}

int main() {
    {
        auto driver = std::make_unique<fanuc::FanucDriver>();
        std::unique_ptr<robot::RobotDriver> base = std::move(driver);
        driver_session(*base);
        driver_session(*base);
        Controller controller([](TestSocket peer) {
            handshake(peer);
            receive(peer);
            std::array<std::uint8_t, 1> end{};
            assert(!read_exact(peer, end));
        });
        assert(base->connect("127.0.0.1", controller.port, 2s, 40ms));
        const auto timeout = base->read_bits(robot::Port::digital_output, 0);
        assert(!timeout && timeout.error().code == robot::Error::Code::timeout);
        assert(!base->connected());
    }
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
    {
        asio::io_context context;
        Tcp::acceptor refused(context);
        refused.open(Tcp::v4());
        refused.bind(Tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        auto result = client.connect("127.0.0.1", refused.local_endpoint().port(), 500ms);
        assert(!result && result.error().code == Error::Code::io);
        assert(!client.connected());
    }
    {
        Controller controller([](TestSocket peer) {
            handshake(peer);
            auto request = receive(peer);
            reply(peer, request);
            request = receive(peer);
            reply(peer, request);
            std::array<std::uint8_t, 1> end{};
            assert(!read_exact(peer, end));
        });
        assert(client.connect("127.0.0.1", controller.port, 2s, 40ms));
        assert(await(client.read(digital_output, 1)));
        std::this_thread::sleep_for(100ms);
        assert(client.connected());
        assert(await(client.read(digital_output, 1)));
        client.disconnect();
    }
    {
        Controller controller([](TestSocket peer) {
            handshake(peer);
            const auto request = receive(peer);
            auto response = make_response(static_cast<std::uint8_t>(request.header.sequence1));
            response.header.message_type = MessageType::extended_response;
            response.payload.resize(65534, 0xa5);
            response.header.payload_size = static_cast<std::uint16_t>(response.payload.size());
            const auto frame = encode(response);
            for (std::size_t offset = 0; offset < frame.size(); offset += 997) {
                send_all(peer, std::span(frame).subspan(
                    offset, std::min<std::size_t>(997, frame.size() - offset)));
            }
            std::array<std::uint8_t, 1> end{};
            assert(!read_exact(peer, end));
        });
        assert(client.connect("127.0.0.1", controller.port, 2s));
        auto result = await(client.read(register_port, 1, 32767));
        assert(result && result->payload.size() == 65534);
        assert(std::ranges::all_of(result->payload, [](auto byte) { return byte == 0xa5; }));
        client.disconnect();
    }
    {
        Controller controller([](TestSocket peer) {
            handshake(peer);
            const auto request = receive(peer);
            const auto frame = encode(make_response(
                static_cast<std::uint8_t>(request.header.sequence1)));
            send_all(peer, std::span(frame).first(header_size + 3));
            std::array<std::uint8_t, 1> end{};
            assert(!read_exact(peer, end));
        });
        assert(client.connect("127.0.0.1", controller.port, 2s, 40ms));
        auto result = await(client.read(digital_output, 1));
        assert(!result && result.error().code == Error::Code::timeout);
        client.disconnect();
    }
    {
        Controller controller([](TestSocket peer) {
            handshake(peer);
            std::this_thread::sleep_for(20ms);
            std::array<std::uint8_t, 4096> bytes{};
            while (read_exact(peer, bytes)) {}
        });
        std::vector<Client::ResponseFuture> futures;
        {
            Client scoped;
            assert(scoped.connect("127.0.0.1", controller.port, 2s, 5s));
            const std::string command(65535, 'X');
            for (int i = 0; i < 128; ++i) futures.push_back(scoped.command(command));
        }
        for (auto& future : futures) {
            auto result = await(std::move(future));
            assert(!result && result.error().code == Error::Code::disconnected);
        }
    }
    normal_session(client);
}
