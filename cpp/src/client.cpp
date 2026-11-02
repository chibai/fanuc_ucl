#include "fanuc_snpx/client.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <format>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using NativeSocket = SOCKET;
static constexpr NativeSocket invalid_socket = INVALID_SOCKET;
static int socket_error() { return WSAGetLastError(); }
static bool would_block(int error) {
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
}
static void close_socket(NativeSocket socket) { closesocket(socket); }
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using NativeSocket = int;
static constexpr NativeSocket invalid_socket = -1;
static int socket_error() { return errno; }
static bool would_block(int error) {
    return error == EAGAIN || error == EWOULDBLOCK || error == EINPROGRESS;
}
static void close_socket(NativeSocket socket) { ::close(socket); }
#endif

namespace fanuc::snpx {
namespace {

Error make_error(Error::Code code, std::string message) {
    return Error{code, std::move(message)};
}

template <class T>
std::future<Result<T>> failed_future(Error error) {
    std::promise<Result<T>> promise;
    auto future = promise.get_future();
    promise.set_value(std::unexpected(std::move(error)));
    return future;
}

void append_u8(std::vector<std::uint8_t>& out, std::uint8_t value) {
    out.push_back(value);
}

void append_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
    }
}

struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t offset{};

    bool take_u8(std::uint8_t& value) {
        if (offset >= bytes.size()) return false;
        value = bytes[offset++];
        return true;
    }
    bool take_u16(std::uint16_t& value) {
        if (bytes.size() - offset < 2) return false;
        value = static_cast<std::uint16_t>(bytes[offset]) |
                static_cast<std::uint16_t>(bytes[offset + 1] << 8);
        offset += 2;
        return true;
    }
    bool take_u32(std::uint32_t& value) {
        if (bytes.size() - offset < 4) return false;
        value = static_cast<std::uint32_t>(bytes[offset]) |
                (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
                (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
                (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
        offset += 4;
        return true;
    }
    bool take(std::span<std::uint8_t> destination) {
        if (bytes.size() - offset < destination.size()) return false;
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                    destination.size(), destination.begin());
        offset += destination.size();
        return true;
    }
};

Header client_header(MessageType type, std::uint8_t sequence, Operation operation) {
    Header header;
    header.packet_type = type == MessageType::init ? 0 : 2;
    header.sequence1 = sequence;
    header.sequence2 = sequence;
    header.operation1 = static_cast<std::uint32_t>(operation);
    header.operation2 = static_cast<std::uint32_t>(operation);
    header.message_type = type;
    header.mailbox_destination = 0x00000e10;
    header.packet_number = type == MessageType::init ? 0 : 1;
    header.total_packets = type == MessageType::init ? 0 : 1;
    return header;
}

bool valid_message_type(std::uint8_t type) {
    return type == static_cast<std::uint8_t>(MessageType::init) ||
           type == static_cast<std::uint8_t>(MessageType::request) ||
           type == static_cast<std::uint8_t>(MessageType::response) ||
           type == static_cast<std::uint8_t>(MessageType::extended_request) ||
           type == static_cast<std::uint8_t>(MessageType::extended_response);
}

} // namespace

Message make_init(bool acknowledgement) {
    Message message;
    message.header.packet_type = acknowledgement ? 1 : 0;
    message.header.operation1 = acknowledgement
        ? static_cast<std::uint32_t>(Operation::init_finish)
        : static_cast<std::uint32_t>(Operation::init);
    message.header.operation2 = static_cast<std::uint32_t>(Operation::init);
    message.header.message_type = MessageType::init;
    message.service = Service::plc_status;
    message.segment = Segment::init;
    return message;
}

Message make_magic(std::uint8_t sequence) {
    Message message;
    message.header = client_header(MessageType::request, sequence, Operation::writing);
    message.header.packet_type = 8;
    message.header.sequence1 = 1;
    message.header.sequence2 = 1;
    message.service = Service::magic;
    message.segment = Segment::magic;
    return message;
}

Message make_read(
    std::uint8_t sequence, Service service, Segment segment,
    std::uint16_t index, std::uint16_t count) {
    Message message;
    message.header = client_header(MessageType::request, sequence, Operation::reading);
    message.service = service;
    message.segment = segment;
    message.target_index = index;
    message.target_size = count;
    return message;
}

Result<Message> make_write(
    std::uint8_t sequence, Segment segment, std::uint16_t index,
    std::uint16_t item_count, std::span<const std::uint8_t> data) {
    if (data.size() > std::numeric_limits<std::uint16_t>::max()) {
        return std::unexpected(make_error(Error::Code::invalid_argument, "SNP-X payload exceeds 65535 bytes"));
    }
    Message message;
    message.service = Service::write_system_memory;
    message.segment = segment;
    message.target_index = index;
    message.target_size = item_count;
    if (data.size() <= message.inline_payload.size()) {
        message.header = client_header(MessageType::request, sequence, Operation::writing);
        std::copy(data.begin(), data.end(), message.inline_payload.begin());
    } else {
        message.header = client_header(MessageType::extended_request, sequence, Operation::writing);
        message.header.payload_size = static_cast<std::uint16_t>(data.size());
        message.payload.assign(data.begin(), data.end());
    }
    return message;
}

Message make_response(std::uint8_t sequence) {
    Message message;
    message.header = client_header(MessageType::response, sequence, Operation::reading);
    message.header.packet_type = 3;
    message.header.mailbox_source = 0x00000e10;
    return message;
}

std::vector<std::uint8_t> encode(const Message& message) {
    std::vector<std::uint8_t> out;
    out.reserve(fixed_frame_size + message.payload.size());
    const auto& h = message.header;
    append_u16(out, h.packet_type);
    append_u16(out, h.sequence1);
    append_u16(out, h.payload_size);
    append_u16(out, h.unknown1);
    append_u32(out, h.operation1);
    append_u32(out, h.unknown2);
    append_u32(out, h.operation2);
    append_u32(out, h.unknown3);
    append_u16(out, h.unknown4);
    out.insert(out.end(), h.time.begin(), h.time.end());
    append_u8(out, h.unknown5);
    append_u8(out, h.sequence2);
    append_u8(out, static_cast<std::uint8_t>(h.message_type));
    append_u32(out, h.mailbox_source);
    append_u32(out, h.mailbox_destination);
    append_u8(out, h.packet_number);
    append_u8(out, h.total_packets);

    switch (h.message_type) {
    case MessageType::init:
    case MessageType::request:
        append_u8(out, static_cast<std::uint8_t>(message.service));
        append_u8(out, static_cast<std::uint8_t>(message.segment));
        append_u16(out, message.target_index);
        append_u16(out, message.target_size);
        out.insert(out.end(), message.inline_payload.begin(), message.inline_payload.end());
        append_u16(out, 0);
        break;
    case MessageType::response:
        append_u16(out, message.response_unknown);
        out.insert(out.end(), message.inline_payload.begin(), message.inline_payload.end());
        out.insert(out.end(), message.plc_status.begin(), message.plc_status.end());
        break;
    case MessageType::extended_request:
        out.insert(out.end(), 6, 0);
        append_u8(out, 1);
        append_u8(out, 1);
        append_u8(out, static_cast<std::uint8_t>(message.service));
        append_u8(out, static_cast<std::uint8_t>(message.segment));
        append_u16(out, message.target_index);
        append_u16(out, message.target_size);
        out.insert(out.end(), message.payload.begin(), message.payload.end());
        break;
    case MessageType::extended_response:
        out.insert(out.end(), 6, 0);
        append_u8(out, 1);
        append_u8(out, 1);
        append_u8(out, static_cast<std::uint8_t>(message.service));
        append_u8(out, static_cast<std::uint8_t>(message.segment));
        append_u16(out, message.response_unknown);
        out.insert(out.end(), message.plc_status.begin(), message.plc_status.begin() + 2);
        out.insert(out.end(), message.payload.begin(), message.payload.end());
        break;
    case MessageType::error:
        break;
    }
    return out;
}

Result<std::pair<Message, std::size_t>> decode(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < header_size) {
        return std::unexpected(make_error(Error::Code::protocol, "incomplete SNP-X header"));
    }
    Reader reader{bytes};
    Message message;
    auto& h = message.header;
    std::uint8_t type{};
    if (!reader.take_u16(h.packet_type) || !reader.take_u16(h.sequence1) ||
        !reader.take_u16(h.payload_size) || !reader.take_u16(h.unknown1) ||
        !reader.take_u32(h.operation1) || !reader.take_u32(h.unknown2) ||
        !reader.take_u32(h.operation2) || !reader.take_u32(h.unknown3) ||
        !reader.take_u16(h.unknown4) || !reader.take(h.time) ||
        !reader.take_u8(h.unknown5) || !reader.take_u8(h.sequence2) ||
        !reader.take_u8(type) || !reader.take_u32(h.mailbox_source) ||
        !reader.take_u32(h.mailbox_destination) || !reader.take_u8(h.packet_number) ||
        !reader.take_u8(h.total_packets)) {
        return std::unexpected(make_error(Error::Code::protocol, "malformed SNP-X header"));
    }
    if (!valid_message_type(type)) {
        return std::unexpected(make_error(Error::Code::protocol, "unsupported SNP-X message type"));
    }
    h.message_type = static_cast<MessageType>(type);
    const auto frame_size = fixed_frame_size + h.payload_size;
    if (bytes.size() < frame_size) {
        return std::unexpected(make_error(Error::Code::protocol, "incomplete SNP-X frame"));
    }
    reader.bytes = bytes.first(frame_size);
    reader.offset = header_size;
    switch (h.message_type) {
    case MessageType::init:
    case MessageType::request: {
        std::uint8_t service{}, segment{};
        if (!reader.take_u8(service) || !reader.take_u8(segment) ||
            !reader.take_u16(message.target_index) || !reader.take_u16(message.target_size) ||
            !reader.take(message.inline_payload) || !reader.take_u16(message.response_unknown)) {
            return std::unexpected(make_error(Error::Code::protocol, "malformed SNP-X request body"));
        }
        message.service = static_cast<Service>(service);
        message.segment = static_cast<Segment>(segment);
        break;
    }
    case MessageType::response:
        if (!reader.take_u16(message.response_unknown) ||
            !reader.take(message.inline_payload) || !reader.take(message.plc_status)) {
            return std::unexpected(make_error(Error::Code::protocol, "malformed SNP-X response body"));
        }
        break;
    case MessageType::extended_request: {
        std::array<std::uint8_t, 6> unknown{};
        std::uint8_t packet{}, total{}, service{}, segment{};
        if (!reader.take(unknown) || !reader.take_u8(packet) || !reader.take_u8(total) ||
            !reader.take_u8(service) || !reader.take_u8(segment) ||
            !reader.take_u16(message.target_index) || !reader.take_u16(message.target_size)) {
            return std::unexpected(make_error(Error::Code::protocol, "malformed SNP-X extended request"));
        }
        message.service = static_cast<Service>(service);
        message.segment = static_cast<Segment>(segment);
        message.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(reader.offset),
                              bytes.begin() + static_cast<std::ptrdiff_t>(frame_size));
        break;
    }
    case MessageType::extended_response: {
        std::array<std::uint8_t, 6> unknown{};
        std::uint8_t packet{}, total{}, service{}, segment{};
        std::array<std::uint8_t, 2> flags{};
        if (!reader.take(unknown) || !reader.take_u8(packet) || !reader.take_u8(total) ||
            !reader.take_u8(service) || !reader.take_u8(segment) ||
            !reader.take_u16(message.response_unknown) || !reader.take(flags)) {
            return std::unexpected(make_error(Error::Code::protocol, "malformed SNP-X extended response"));
        }
        message.service = static_cast<Service>(service);
        message.segment = static_cast<Segment>(segment);
        message.plc_status[0] = flags[0];
        message.plc_status[1] = flags[1];
        message.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(reader.offset),
                              bytes.begin() + static_cast<std::ptrdiff_t>(frame_size));
        break;
    }
    case MessageType::error:
        break;
    }
    return std::pair{std::move(message), frame_size};
}

std::vector<std::uint8_t> payload_bytes(const Message& message) {
    if (message.header.message_type == MessageType::extended_response) {
        return message.payload;
    }
    return {message.inline_payload.begin(), message.inline_payload.end()};
}

Result<std::vector<bool>> decode_bits(
    std::span<const std::uint8_t> bytes, std::size_t start_bit, std::size_t count) {
    if (count > std::numeric_limits<std::uint16_t>::max() ||
        start_bit > std::numeric_limits<std::uint16_t>::max()) {
        return std::unexpected(make_error(Error::Code::invalid_argument, "bit range exceeds SNP-X address space"));
    }
    if (start_bit > std::numeric_limits<std::size_t>::max() - count ||
        (start_bit + count + 7) / 8 > bytes.size()) {
        return std::unexpected(make_error(Error::Code::protocol, "bit payload is shorter than requested range"));
    }
    std::vector<bool> values;
    values.reserve(count);
    for (std::size_t bit = start_bit; bit < start_bit + count; ++bit) {
        values.push_back((bytes[bit / 8] & (1u << (bit % 8))) != 0);
    }
    return values;
}

Result<std::vector<std::int16_t>> decode_words(
    std::span<const std::uint8_t> bytes, std::size_t count) {
    if (count > bytes.size() / 2) {
        return std::unexpected(make_error(Error::Code::protocol, "word payload is shorter than requested count"));
    }
    std::vector<std::int16_t> values;
    values.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto raw = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes[2 * i]) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[2 * i + 1]) << 8));
        values.push_back(std::bit_cast<std::int16_t>(raw));
    }
    return values;
}

std::vector<std::uint8_t> encode_bits(std::size_t start_bit, std::span<const bool> values) {
    std::vector<std::uint8_t> bytes((start_bit % 8 + values.size() + 7) / 8);
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (values[i]) {
            const auto bit = start_bit % 8 + i;
            bytes[bit / 8] |= static_cast<std::uint8_t>(1u << (bit % 8));
        }
    }
    return bytes;
}

std::vector<std::uint8_t> encode_words(std::span<const std::int16_t> values) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(values.size() * 2);
    for (const auto value : values) {
        const auto raw = std::bit_cast<std::uint16_t>(value);
        append_u16(bytes, raw);
    }
    return bytes;
}

struct Client::Impl {
    struct PendingWrite {
        std::vector<std::uint8_t> bytes;
        std::size_t offset{};
        std::uint8_t sequence{};
    };

    NativeSocket socket{invalid_socket};
    std::atomic<bool> live{false};
    std::jthread worker;
    std::mutex mutex;
    std::deque<PendingWrite> outgoing;
    std::unordered_map<std::uint8_t, std::shared_ptr<std::promise<Response>>> pending;
    std::uint8_t next_sequence{};
    std::mutex asg_mutex;
    std::unordered_map<std::string, std::pair<std::uint16_t, std::uint16_t>> asg;
    std::uint32_t next_asg_address{1};
#ifdef _WIN32
    bool winsock_initialized{};
#endif

    std::uint8_t allocate_sequence() {
        for (unsigned i = 0; i < 256; ++i) {
            const auto candidate = next_sequence++;
            if (!pending.contains(candidate)) return candidate;
        }
        throw std::runtime_error("all SNP-X sequence numbers are in use");
    }

    ResponseFuture enqueue(Message message) {
        std::lock_guard lock(mutex);
        if (!live.load()) {
            return failed_future<Message>(
                make_error(Error::Code::disconnected, "SNP-X client is not connected"));
        }
        std::uint8_t sequence{};
        try {
            sequence = allocate_sequence();
        } catch (const std::exception& e) {
            return failed_future<Message>(
                make_error(Error::Code::invalid_argument, e.what()));
        }
        message.header.sequence1 = sequence;
        message.header.sequence2 = sequence;
        auto promise = std::make_shared<std::promise<Response>>();
        auto future = promise->get_future();
        pending.emplace(sequence, promise);
        outgoing.push_back(PendingWrite{encode(message), 0, sequence});
        return future;
    }

    void fail_all(Error error) {
        std::unordered_map<std::uint8_t, std::shared_ptr<std::promise<Response>>> waiting;
        {
            std::lock_guard lock(mutex);
            waiting.swap(pending);
            outgoing.clear();
        }
        for (auto& [sequence, promise] : waiting) {
            (void)sequence;
            try { promise->set_value(std::unexpected(error)); } catch (...) {}
        }
    }

    void finish_response(Message message) {
        std::shared_ptr<std::promise<Response>> promise;
        const auto sequence = static_cast<std::uint8_t>(message.header.sequence1);
        {
            std::lock_guard lock(mutex);
            const auto it = pending.find(sequence);
            if (it == pending.end()) return;
            promise = std::move(it->second);
            pending.erase(it);
        }
        try { promise->set_value(std::move(message)); } catch (...) {}
    }

    void run(std::stop_token stop) {
        std::vector<std::uint8_t> input;
        std::array<std::uint8_t, 4096> scratch{};
        Error terminal = make_error(Error::Code::disconnected, "SNP-X connection closed");
        while (!stop.stop_requested() && live.load()) {
            fd_set reads, writes;
            FD_ZERO(&reads);
            FD_ZERO(&writes);
            FD_SET(socket, &reads);
            {
                std::lock_guard lock(mutex);
                if (!outgoing.empty()) FD_SET(socket, &writes);
            }
            timeval interval{0, 1000};
#ifdef _WIN32
            const int selected = select(0, &reads, &writes, nullptr, &interval);
#else
            const int selected = select(socket + 1, &reads, &writes, nullptr, &interval);
#endif
            if (selected < 0) {
                terminal = make_error(Error::Code::io, std::format("select failed ({})", socket_error()));
                break;
            }
            if (selected == 0) continue;

            if (FD_ISSET(socket, &writes)) {
                std::lock_guard lock(mutex);
                while (!outgoing.empty()) {
                    auto& front = outgoing.front();
                    const auto remaining = front.bytes.size() - front.offset;
#ifdef _WIN32
                    const int sent = ::send(socket,
                        reinterpret_cast<const char*>(front.bytes.data() + front.offset),
                        static_cast<int>(remaining), 0);
#else
#ifdef MSG_NOSIGNAL
                    const auto sent = ::send(socket, front.bytes.data() + front.offset, remaining, MSG_NOSIGNAL);
#else
                    const auto sent = ::send(socket, front.bytes.data() + front.offset, remaining, 0);
#endif
#endif
                    if (sent < 0) {
                        const auto error = socket_error();
                        if (would_block(error)) break;
                        terminal = make_error(Error::Code::io, std::format("send failed ({})", error));
                        live.store(false);
                        break;
                    }
                    if (sent == 0) {
                        terminal = make_error(Error::Code::disconnected, "TCP send returned zero");
                        live.store(false);
                        break;
                    }
                    front.offset += static_cast<std::size_t>(sent);
                    if (front.offset == front.bytes.size()) outgoing.pop_front();
                }
                if (!live.load()) break;
            }

            if (FD_ISSET(socket, &reads)) {
#ifdef _WIN32
                const int received = ::recv(socket, reinterpret_cast<char*>(scratch.data()),
                                            static_cast<int>(scratch.size()), 0);
#else
                const auto received = ::recv(socket, scratch.data(), scratch.size(), 0);
#endif
                if (received == 0) {
                    terminal = make_error(Error::Code::disconnected, "controller closed the TCP connection");
                    break;
                }
                if (received < 0) {
                    const auto error = socket_error();
                    if (would_block(error)) continue;
                    terminal = make_error(Error::Code::io, std::format("recv failed ({})", error));
                    break;
                }
                input.insert(input.end(), scratch.begin(),
                             scratch.begin() + static_cast<std::ptrdiff_t>(received));
                while (input.size() >= header_size) {
                    const auto payload_size = static_cast<std::size_t>(input[4]) |
                                              (static_cast<std::size_t>(input[5]) << 8);
                    if (input.size() < fixed_frame_size + payload_size) break;
                    auto decoded = decode(input);
                    if (!decoded) {
                        terminal = decoded.error();
                        live.store(false);
                        break;
                    }
                    auto [message, consumed] = std::move(*decoded);
                    input.erase(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(consumed));
                    finish_response(std::move(message));
                }
                if (!live.load()) break;
            }
        }
        live.store(false);
        fail_all(std::move(terminal));
    }
};

Client::Client() : impl_(std::make_unique<Impl>()) {}
Client::~Client() { disconnect(); }

Result<void> Client::connect(
    std::string_view host, std::uint16_t port, std::chrono::milliseconds timeout) {
    if (timeout <= std::chrono::milliseconds::zero()) {
        return std::unexpected(make_error(Error::Code::invalid_argument, "connect timeout must be positive"));
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    disconnect();
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return std::unexpected(make_error(Error::Code::io, "WSAStartup failed"));
    }
    impl_->winsock_initialized = true;
#endif
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses{};
    const std::string host_string(host);
    const auto port_string = std::to_string(port);
    const int lookup = getaddrinfo(host_string.c_str(), port_string.c_str(), &hints, &addresses);
    if (lookup != 0) {
#ifdef _WIN32
        WSACleanup();
        impl_->winsock_initialized = false;
        return std::unexpected(make_error(Error::Code::io, std::format("getaddrinfo failed ({})", lookup)));
#else
        return std::unexpected(make_error(Error::Code::io, gai_strerror(lookup)));
#endif
    }

    NativeSocket connected_socket = invalid_socket;
    std::string last_error = "no address resolved";
    for (auto* address = addresses; address; address = address->ai_next) {
        auto candidate = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate == invalid_socket) {
            last_error = std::format("socket failed ({})", socket_error());
            continue;
        }
#ifdef _WIN32
        u_long nonblocking = 1;
        ioctlsocket(candidate, FIONBIO, &nonblocking);
        const int result = ::connect(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen));
        if (result == SOCKET_ERROR && socket_error() != WSAEWOULDBLOCK && socket_error() != WSAEINPROGRESS) {
#else
        const int flags = fcntl(candidate, F_GETFL, 0);
        fcntl(candidate, F_SETFL, flags | O_NONBLOCK);
        const int result = ::connect(candidate, address->ai_addr, address->ai_addrlen);
        if (result < 0 && errno != EINPROGRESS) {
#endif
            last_error = std::format("connect failed ({})", socket_error());
            close_socket(candidate);
            continue;
        }
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(candidate, &writable);
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            last_error = "TCP connect timed out";
            close_socket(candidate);
            break;
        }
        const auto timeout_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        timeval wait{
            static_cast<long>(timeout_ms / 1000),
            static_cast<long>((timeout_ms % 1000) * 1000)};
#ifdef _WIN32
        const int selected = select(0, nullptr, &writable, nullptr, &wait);
#else
        const int selected = select(candidate + 1, nullptr, &writable, nullptr, &wait);
#endif
        if (selected <= 0) {
            last_error = selected == 0 ? "TCP connect timed out" :
                std::format("connect select failed ({})", socket_error());
            close_socket(candidate);
            continue;
        }
        int so_error{};
#ifdef _WIN32
        int option_len = sizeof(so_error);
#else
        socklen_t option_len = sizeof(so_error);
#endif
        if (getsockopt(candidate, SOL_SOCKET, SO_ERROR,
#ifdef _WIN32
                       reinterpret_cast<char*>(&so_error),
#else
                       &so_error,
#endif
                       &option_len) != 0 || so_error != 0) {
            last_error = std::format("TCP connect failed ({})", so_error == 0 ? socket_error() : so_error);
            close_socket(candidate);
            continue;
        }
        connected_socket = candidate;
        break;
    }
    freeaddrinfo(addresses);
    if (connected_socket == invalid_socket) {
#ifdef _WIN32
        WSACleanup();
        impl_->winsock_initialized = false;
#endif
        return std::unexpected(make_error(Error::Code::io, std::move(last_error)));
    }
#ifdef _WIN32
    BOOL no_delay = TRUE;
    setsockopt(connected_socket, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));
#else
    const int no_delay = 1;
    setsockopt(connected_socket, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
#ifdef SO_NOSIGPIPE
    const int no_sigpipe = 1;
    setsockopt(connected_socket, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
#endif
    impl_->socket = connected_socket;
    impl_->next_sequence = 0;
    {
        std::lock_guard lock(impl_->asg_mutex);
        impl_->asg.clear();
        impl_->next_asg_address = 1;
    }
    impl_->live.store(true);
    impl_->worker = std::jthread([state = impl_.get()](std::stop_token stop) { state->run(stop); });

    const auto await = [&](ResponseFuture future) -> Result<Message> {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline || future.wait_until(deadline) != std::future_status::ready) {
            return std::unexpected(make_error(Error::Code::timeout, "SNP-X handshake timed out"));
        }
        return future.get();
    };
    auto init_result = await(request(make_init()));
    if (!init_result || init_result->header != make_init(true).header) {
        auto error = init_result ? make_error(Error::Code::protocol, "controller returned an invalid INIT_ACK")
                                 : init_result.error();
        disconnect();
        return std::unexpected(std::move(error));
    }
    auto magic_result = await(request(make_magic(1)));
    if (!magic_result) {
        auto error = magic_result.error();
        disconnect();
        return std::unexpected(std::move(error));
    }
    auto clear_result = await(command("CLRASG"));
    if (!clear_result) {
        auto error = clear_result.error();
        disconnect();
        return std::unexpected(std::move(error));
    }
    return {};
}

void Client::disconnect() noexcept {
    if (!impl_) return;
    impl_->live.store(false);
    if (impl_->socket != invalid_socket) {
#ifdef _WIN32
        shutdown(impl_->socket, SD_BOTH);
#else
        shutdown(impl_->socket, SHUT_RDWR);
#endif
    }
    if (impl_->worker.joinable()) {
        impl_->worker.request_stop();
        impl_->worker.join();
    }
    if (impl_->socket != invalid_socket) {
        close_socket(impl_->socket);
        impl_->socket = invalid_socket;
    }
#ifdef _WIN32
    if (impl_->winsock_initialized) {
        WSACleanup();
        impl_->winsock_initialized = false;
    }
#endif
}

bool Client::connected() const noexcept {
    return impl_ && impl_->live.load();
}

Client::ResponseFuture Client::request(Message message) {
    return impl_->enqueue(std::move(message));
}

Client::ResponseFuture Client::read(Port port, std::size_t index, std::size_t count) {
    if ((!port.zero_indexed && index == 0) ||
        index > std::numeric_limits<std::uint16_t>::max() ||
        count > std::numeric_limits<std::uint16_t>::max()) {
        return failed_future<Message>(
            make_error(Error::Code::invalid_argument, "port index/count is outside its valid range"));
    }
    const auto address = index - (port.zero_indexed ? 0 : 1);
    if (address + port.base_offset > std::numeric_limits<std::uint16_t>::max() ||
        (count != 0 && address + port.base_offset + count > 65536)) {
        return failed_future<Message>(
            make_error(Error::Code::invalid_argument, "port address exceeds SNP-X range"));
    }
    auto target = address + port.base_offset;
    auto adjusted_count = count;
    if (port.bit_packed && count != 0) {
        const auto end = target + count;
        target -= target % 8;
        adjusted_count = ((end + 7) / 8) * 8 - target;
    }
    if (adjusted_count > std::numeric_limits<std::uint16_t>::max() ||
        target > std::numeric_limits<std::uint16_t>::max()) {
        return failed_future<Message>(
            make_error(Error::Code::invalid_argument, "aligned port range exceeds SNP-X range"));
    }
    return request(make_read(0, Service::read_system_memory, port.segment,
                             static_cast<std::uint16_t>(target),
                             static_cast<std::uint16_t>(adjusted_count)));
}

Client::ResponseFuture Client::write_bits(
    Port port, std::size_t index, std::span<const bool> values) {
    if (!port.writable) {
        return failed_future<Message>(
            make_error(Error::Code::invalid_argument, "refusing to write a read-only SNP-X port"));
    }
    if (!port.bit_packed || (!port.zero_indexed && index == 0)) {
        return failed_future<Message>(
            make_error(Error::Code::invalid_argument, "invalid bit port or index"));
    }
    const auto address = index - (port.zero_indexed ? 0 : 1);
    const auto target = address + port.base_offset;
    if (target + values.size() > 65536) {
        return failed_future<Message>(
            make_error(Error::Code::invalid_argument, "bit write range exceeds SNP-X address space"));
    }
    if (target > std::numeric_limits<std::uint16_t>::max() ||
        values.size() > std::numeric_limits<std::uint16_t>::max()) {
        return failed_future<Message>(
            make_error(Error::Code::invalid_argument, "bit write exceeds SNP-X address range"));
    }
    const auto data = encode_bits(target, values);
    auto message = make_write(0, port.segment, static_cast<std::uint16_t>(target),
                              static_cast<std::uint16_t>(values.size()), data);
    if (!message) return failed_future<Message>(message.error());
    return request(std::move(*message));
}

Client::ResponseFuture Client::write_words(
    Port port, std::size_t index, std::span<const std::int16_t> values) {
    if (!port.writable) {
        return failed_future<Message>(
            make_error(Error::Code::invalid_argument, "refusing to write a read-only SNP-X port"));
    }
    if (port.bit_packed || (!port.zero_indexed && index == 0)) {
        return failed_future<Message>(
            make_error(Error::Code::invalid_argument, "invalid word port or index"));
    }
    const auto address = index - (port.zero_indexed ? 0 : 1);
    const auto target = address + port.base_offset;
    if (target + values.size() > 65536) {
        return failed_future<Message>(
            make_error(Error::Code::invalid_argument, "word write range exceeds SNP-X address space"));
    }
    if (target > std::numeric_limits<std::uint16_t>::max() ||
        values.size() > std::numeric_limits<std::uint16_t>::max()) {
        return failed_future<Message>(
            make_error(Error::Code::invalid_argument, "word write exceeds SNP-X address range"));
    }
    const auto data = encode_words(values);
    auto message = make_write(0, port.segment, static_cast<std::uint16_t>(target),
                              static_cast<std::uint16_t>(values.size()), data);
    if (!message) return failed_future<Message>(message.error());
    return request(std::move(*message));
}

Client::ResponseFuture Client::command(std::string_view text) {
    if (text.size() > std::numeric_limits<std::uint16_t>::max()) {
        return failed_future<Message>(
            make_error(Error::Code::invalid_argument, "command is too long"));
    }
    const auto data = std::span(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    auto message = make_write(0, Segment::global_byte, 0,
                              static_cast<std::uint16_t>(text.size()), data);
    if (!message) return failed_future<Message>(message.error());
    return request(std::move(*message));
}

Result<std::uint16_t> Client::register_asg(
    std::string_view variable, std::uint16_t registers, float multiplier,
    std::chrono::milliseconds timeout) {
    if (variable.empty() || registers == 0) {
        return std::unexpected(make_error(Error::Code::invalid_argument, "ASG name and register count must be non-empty"));
    }
    if (std::ranges::any_of(variable, [](unsigned char ch) {
            return ch <= 0x20 || ch == 0x7f;
        })) {
        return std::unexpected(make_error(Error::Code::invalid_argument, "ASG name cannot contain whitespace or control characters"));
    }
    std::lock_guard lock(impl_->asg_mutex);
    const std::string name(variable);
    if (const auto it = impl_->asg.find(name); it != impl_->asg.end()) {
        return it->second.first;
    }
    if (impl_->next_asg_address + registers > 65536) {
        return std::unexpected(make_error(Error::Code::invalid_argument, "ASG register space exhausted"));
    }
    const auto address = static_cast<std::uint16_t>(impl_->next_asg_address);
    const auto statement = std::format("SETASG {} {} {} {}", address, registers, name, multiplier);
    auto future = command(statement);
    if (future.wait_for(timeout) != std::future_status::ready) {
        return std::unexpected(make_error(Error::Code::timeout, "SETASG acknowledgement timed out"));
    }
    auto response = future.get();
    if (!response) return std::unexpected(response.error());
    impl_->asg.emplace(name, std::pair{address, registers});
    impl_->next_asg_address += registers;
    return address;
}

Client::ResponseFuture Client::read_asg(std::uint16_t address, std::uint16_t registers) {
    return read(Port{Segment::registers, 0, false, false, true}, address, registers);
}

Client::ResponseFuture Client::write_asg(
    std::uint16_t address, std::span<const std::int16_t> words) {
    return write_words(Port{Segment::registers, 0, false, false, true}, address, words);
}

} // namespace fanuc::snpx
