# FANUC SNP-X C++ API 参考

本文按公共头文件 `fanuc_snpx/client.hpp` 说明当前实现的功能、接口契约和使用方式。它描述的是本项目实现的 SNP-X HMI 数据访问子集，不是 FANUC 全量协议规范。帧格式、传输细节、数据段偏移及兼容性限制见 [SNP-X 二进制协议说明](../SNPX.md)；线程、超时和构建集成细节见 [架构说明](ARCHITECTURE.md)。

## 1. 实现功能概览

当前库提供：

- TCP 客户端连接、INIT/MAGIC 握手和异步请求/响应调度。
- SNP-X 固定帧和单分片扩展帧的编码、解码。
- 预定义 DI/DO、机器人 I/O、UOP、SOP、焊接 I/O、粘丝 I/O、组 I/O、模拟 I/O 和寄存器端口。
- 位和有符号 16 位字的读写，以及显式字节序编解码。
- `GlobalByte` 命令发送，以及 ASG 变量映射注册、读取和写入。
- 以 `std::expected` 表示的同步错误，以及以 `std::future` 返回的异步操作结果。

不实现 TLS/认证、自动重连、分片重组、完整 PLC 状态码解释或复杂 ASG 类型自动转换。`connect()` 会发送 `CLRASG` 并清空控制器 ASG 表；写端口和命令可能改变真实设备状态。部署安全注意事项见 README。

## 2. 引入与编译

```cpp
#include <fanuc_snpx/client.hpp>
```

使用 `fanuc_snpx::fanuc_snpx` CMake target，目标会传播 C++23、线程依赖和 Windows 所需的网络库。网络实现使用 Boost.Asio，构建和安装包消费者需要 Boost ≥ 1.74 头文件，但公共 API 不暴露 Boost 类型。独立工程可通过 `add_subdirectory` 或安装后 `find_package(fanuc_snpx CONFIG REQUIRED)` 集成，完整命令见 [README](../README.md#集成)。

命名空间为 `fanuc::snpx`。公共头文件不暴露操作系统 socket 类型。

## 3. 类型与通用返回约定

### `Error`、`Result<T>`

```cpp
struct Error {
    enum class Code {
        invalid_argument, io, timeout, disconnected, protocol, resource_exhausted
    };
    Code code;
    std::string message;
};

template<class T>
using Result = std::expected<T, Error>;
```

同步调用以 `if (!result)` 检查失败，错误详情在 `result.error().code` 和 `.message` 中。异步接口返回 `Client::ResponseFuture`，其结果类型为 `std::expected<Message, Error>`；future 就绪后必须检查 expected：

```cpp
auto result = client.read(digital_output, 1).get();
if (!result) {
    std::cerr << result.error().message << '\n';
    return 1;
}
```

future 只能消费一次。丢弃 future 或 `wait_for()` 超时不会取消已提交的控制器请求。任何在途请求超时都会使整个会话失效，其他待处理请求也会失败。

| 错误码 | 常见原因 |
|---|---|
| `invalid_argument` | 空/越界范围、只读端口写入、消息长度或 ASG 参数无效 |
| `io` | 地址解析、socket 建连、发送或接收失败 |
| `timeout` | 握手、请求或 ASG 注册等待超时；会话将被关闭 |
| `disconnected` | 尚未连接、主动断开或控制器关闭连接 |
| `protocol` | 非法/不支持的帧、响应序号未知或响应类型不匹配 |
| `resource_exhausted` | 256 个 8 位请求序号都仍在使用 |

### 消息类型和常量

| 符号 | 值/说明 |
|---|---|
| `default_port` | `60008` |
| `header_size` | 42 字节 |
| `body_size` | 14 字节 |
| `fixed_frame_size` | 56 字节，固定头和固定体之和 |
| `MessageType::init` / `error` | `0x00` / `0xD1` |
| `MessageType::request` / `response` | `0xC0` / `0xD4` |
| `MessageType::extended_request` / `extended_response` | `0x80` / `0x94` |
| `Service::plc_status` / `read_system_memory` | `0x00` / `0x04` |
| `Service::write_system_memory` / `magic` | `0x07` / `0x4F` |
| `Segment::init` / `magic` | `0x00` / `0x01` |
| `Segment::registers` | `0x08` |
| `Segment::analog_input` / `analog_output` | `0x0A` / `0x0C` |
| `Segment::global_byte` | `0x38` |
| `Segment::input_bit` / `output_bit` | `0x46` / `0x48` |
| `Operation::init` / `init_finish` | `0` / `1` |
| `Operation::writing` / `reading` | `0x100` / `0x200` |

这些枚举用于构造底层协议消息。一般应用应优先使用 `Client::read`、`write_bits`、`write_words` 或 ASG 接口。

### `Header` 与 `Message`

`Header` 是字段级帧头表示，不是可直接发送的内存布局；多字节数值由库显式按小端编码。字段包括 packet 类型、两个序号字段、载荷大小、操作码、保留字段、时间、消息类型、mailbox 和分片号。

`Message` 同时表示请求和响应，包含 `header`、`service`、`segment`、`target_index`、`target_size`、6 字节 `inline_payload`、`response_unknown`、6 字节 `plc_status` 和扩展 `payload`。固定响应没有独立的有效载荷长度，读取者必须按请求的数据类型与数量解码；状态原始字节不会由客户端自动判断为成功或失败。

## 4. 端口描述与寻址

`Port` 是可复制的描述结构：

```cpp
struct Port {
    Segment segment;
    std::uint16_t base_offset;
    bool zero_indexed;
    bool bit_packed;
    bool writable;
};
```

对 `Client` 端口读写，API 索引转换为 `wire_target = index - (zero_indexed ? 0 : 1) + base_offset`。预定义端口及属性如下；输入/输出名称对应控制器数据方向，读写能力以表中 `writable` 为准。

| 常量 | Segment | 基址偏移 | API 索引 | 数据形式 | 可写 |
|---|---:|---:|---|---|---|
| `digital_input` | `output_bit` (`0x48`) | 0 | 0 起始 | 位 | 否 |
| `digital_output` | `input_bit` (`0x46`) | 0 | 1 起始 | 位 | 是 |
| `robot_input` / `robot_output` | `0x48` / `0x46` | 5000 | 0 / 0 起始 | 位 | 否 / 是 |
| `uop_input` / `uop_output` | `0x48` / `0x46` | 6000 | 0 / 0 起始 | 位 | 否 / 是 |
| `sop_input` / `sop_output` | `0x48` / `0x46` | 7000 | 1 / 1 起始 | 位 | 否 / 是 |
| `weld_input` / `weld_output` | `0x48` / `0x46` | 8000 | 0 / 0 起始 | 位 | 否 / 是 |
| `wire_stick_input` / `wire_stick_output` | `0x48` / `0x46` | 8400 | 0 / 0 起始 | 位 | 否 / 是 |
| `group_input` / `group_output` | `0x0C` / `0x0A` | 0 | 0 / 1 起始 | int16 字 | 否 / 是 |
| `analog_input` / `analog_output` | `0x0C` / `0x0A` | 1000 | 0 / 1 起始 | int16 字 | 否 / 是 |
| `register_port` | `registers` (`0x08`) | 0 | 1 起始 | int16 字 | 是 |

位按字节内 bit 0 到 bit 7 排列。`read()` 对位范围自动向下/向上对齐到字节边界；解码时要把原目标相对返回首字节的位偏移传给 `decode_bits()`。`Port` 可自定义，但其 `writable` 只是一层 API 校验，不是控制器权限或安全边界。

## 5. `Client` 生命周期与连接

```cpp
Client();
~Client();
Client(const Client&) = delete;
Client& operator=(const Client&) = delete;
Client(Client&&) = delete;
Client& operator=(Client&&) = delete;
```

`Client` 不能复制或移动。析构时自动断开连接。应用必须串行管理 `connect()`、`disconnect()` 和析构；不要与其他 Client 操作并发调用生命周期方法。连接成功后，可以从多个线程提交普通请求。

### `connect`

```cpp
Result<void> connect(
    std::string_view host,
    std::uint16_t port = default_port,
    std::chrono::milliseconds timeout = std::chrono::seconds(1),
    std::chrono::milliseconds request_timeout = std::chrono::seconds(1));
```

- `host` 必须非空且不含 NUL；`port` 和两个超时必须大于 0。
- `timeout` 用于 TCP 连接和整个握手预算；`request_timeout` 是握手成功后每个请求的期限。
- 成功前依次进行 INIT、MAGIC、`CLRASG`。成功时 ASG 表已清空；重新连接需重新注册 ASG。
- 建连或握手失败返回 `Error` 并清理连接。Asio 异步解析、TCP 连接和握手共享总预算；超时取消解析并关闭 socket。底层系统 DNS 解析的取消清理仍可能延迟返回，不保证硬实时期限。

### `disconnect` / `connected`

```cpp
void disconnect() noexcept;
bool connected() const noexcept;
```

`disconnect()` 可重复调用，会停止后台 I/O 线程、关闭 socket，并使未完成请求以 `disconnected` 完成。`connected()` 仅表示检查瞬间的状态；调用后仍必须处理请求错误。

## 6. 读写和命令

下列异步方法均立即返回 future。参数错误通常也通过一个已失败的 future 报告，因此需要调用 `.get()` 并检查结果。

### `request`

```cpp
ResponseFuture request(Message message);
```

发送专家级原始请求。仅接受 `init`、`request`、`extended_request` 类型；校验 payload 与 `header.payload_size` 一致、扩展载荷不超过 65535 字节、固定请求没有扩展 payload，且非 INIT 分片字段为 `1/1`。请求序号由 Client 分配并覆盖消息中的序号。INIT 请求只接受 INIT 响应；其他请求接受固定或扩展响应。未知序号或其他响应类型会使会话失败。

使用前应熟悉 [协议说明](../SNPX.md)。低层请求可绕过预定义端口的只读保护，不应暴露给不可信输入。

### `read`

```cpp
ResponseFuture read(Port port, std::size_t index, std::size_t count = 1);
```

按端口读取位或字。`index` 遵循端口的 0/1 起始规则；`count` 必须大于 0 且不超过 65535。访问范围必须落在 16 位 SNP-X 地址空间内。字读取最多 32767 项；位读取会对齐传输范围，因此对齐后的数量也必须能由 16 位 `target_size` 表示。该方法返回原始 `Message`，不自动解释数据。

位读取示例：读取 DO8、DO9。DO8 的 wire 位置是 7，读取会对齐到首字节，解码偏移为 7：

```cpp
auto response = client.read(digital_output, 8, 2).get();
if (!response) {
    std::cerr << response.error().message << '\n';
    return 1;
}
auto values = decode_bits(payload_bytes(*response), 7, 2);
if (!values) {
    std::cerr << values.error().message << '\n';
    return 1;
}
// values->at(0) 是 DO8，values->at(1) 是 DO9
```

### `write_bits`

```cpp
ResponseFuture write_bits(
    Port port, std::size_t index, std::span<const bool> values);
```

只接受 `bit_packed == true`、`writable == true` 且非空的 `values`；地址和数量不得越界。写操作按传入的第一个位索引发起，位数据按小端位顺序打包。

```cpp
const std::array<bool, 2> outputs{true, false};
auto result = client.write_bits(digital_output, 8, outputs).get();
if (!result) { /* 处理 result.error() */ }
```

`std::vector<bool>` 是特殊位容器，不能直接转换为 `span<const bool>`；使用 `std::array<bool, N>` 或连续 bool 数组。

### `write_words`

```cpp
ResponseFuture write_words(
    Port port, std::size_t index, std::span<const std::int16_t> values);
```

只接受字端口（`bit_packed == false`）、可写端口和非空数据；字以 int16 小端编码。传输载荷最多 65535 字节（32767 个字），且地址范围不能越界。

```cpp
const std::array<std::int16_t, 2> values{123, -1};
auto result = client.write_words(register_port, 1, values).get();
if (!result) { /* 处理 result.error() */ }
```

成功仅表示收到可接受类型的响应，不保证物理动作完成或 PLC 状态码代表成功。写入真实输出前必须确认控制器配置并使用独立回读和设备安全措施。

### `command`

```cpp
ResponseFuture command(std::string_view text);
```

将 1 到 65535 字节原样写到 `GlobalByte` 段。接口不会解析或限制命令语法；调用者必须确保命令与控制器兼容。连接时自动发送 `CLRASG`，直接发送 `SETASG` 等命令会绕过客户端的 ASG 缓存管理。

## 7. ASG 接口

ASG API 对控制器的全局映射表进行操作。所有注册由客户端从 `R[1]` 起顺序分配；`connect()` 清表后会重置本地缓存和分配地址。

### `register_asg`

```cpp
Result<std::uint16_t> register_asg(
    std::string_view variable,
    std::uint16_t registers,
    float multiplier = 1.0F,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(100));
```

注册过程发送 `SETASG <address> <registers> <variable> <multiplier>` 并同步等待响应，返回起始寄存器地址。

- 变量名和寄存器数必须非空/非零；倍率必须有限；超时必须为正。
- 名称不能包含空白或控制字符。
- 同一名称以相同寄存器数和倍率再次注册时返回缓存地址；参数不同则返回 `invalid_argument`。
- 寄存器空间从地址 1 开始顺序分配；空间耗尽返回 `invalid_argument`。
- 等待超时后无法确定控制器是否执行了注册，客户端会主动断开连接；不要复用该连接状态。

变量名必须替换为目标控制器中存在且允许访问的变量。寄存器数与倍率由调用者按控制器定义确定；库不推断变量类型。

### `read_asg` / `write_asg`

```cpp
ResponseFuture read_asg(std::uint16_t address, std::uint16_t registers);
ResponseFuture write_asg(
    std::uint16_t address, std::span<const std::int16_t> words);
```

这两个方法对寄存器段进行原始 int16 读写；读取数量必须非零，写入数据不得为空，地址使用 1 起始索引并受通用读写范围限制。接口不验证传入地址此前由本 Client 注册，也不做 float32、int32、字符串或结构体转换。按需自行将寄存器字节转换为目标变量表示，并遵循控制器定义。

## 8. 帧构造、编解码辅助函数

这些函数适用于协议测试、工具和需要直接处理帧的应用。常规设备访问优先使用 `Client` 高层接口。

### 消息工厂

```cpp
Message make_init(bool acknowledgement = false);
Message make_magic(std::uint8_t sequence);
Message make_read(
    std::uint8_t sequence, Service service, Segment segment,
    std::uint16_t index, std::uint16_t count);
Result<Message> make_write(
    std::uint8_t sequence, Segment segment, std::uint16_t index,
    std::uint16_t item_count, std::span<const std::uint8_t> data);
Message make_response(std::uint8_t sequence);
```

`make_init()` 构造 INIT，`make_init(true)` 构造 INIT_ACK 模板；`make_magic()` 构造 MAGIC 写请求；`make_read()` 构造读请求。`make_write()` 在数据不超过 6 字节时构造固定请求，更多数据时构造扩展请求，拒绝超过 65535 字节的载荷。`make_response()` 构造固定响应模板，通常用于模拟器和测试。

除 `make_write()` 的载荷上限外，工厂不会验证索引、数量或控制器语义。高层 `Client` API 会执行自己的参数校验。

### `encode`、`decode`、`payload_bytes`

```cpp
std::vector<std::uint8_t> encode(const Message& message);
Result<std::pair<Message, std::size_t>> decode(
    std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> payload_bytes(const Message& message);
```

- `encode()` 按 SNP-X 格式序列化字段和载荷；它是低层序列化器，不负责验证手工构造消息的所有一致性约束。
- `decode()` 解码首个完整帧，返回 `(Message, consumed_bytes)`。输入可包含后续帧；调用者按 `consumed_bytes` 移除首帧再继续解码。数据不完整或协议字段无效时返回 `protocol` 错误。当前只接受单分片帧。
- `payload_bytes()` 对扩展响应返回扩展 `payload`；对其他消息返回 6 字节 `inline_payload`。固定响应中的尾随补零不能视作有效数据，须根据期望数量解码。

### 位与字编解码

```cpp
Result<std::vector<bool>> decode_bits(
    std::span<const std::uint8_t> bytes, std::size_t start_bit, std::size_t count);
Result<std::vector<std::int16_t>> decode_words(
    std::span<const std::uint8_t> bytes, std::size_t count);
std::vector<std::uint8_t> encode_bits(
    std::size_t start_bit, std::span<const bool> values);
std::vector<std::uint8_t> encode_words(std::span<const std::int16_t> values);
```

位以字节内 bit 0 到 bit 7 的顺序编码。`decode_bits` 的 `start_bit` 是从传入字节缓冲区起算的位偏移；若数据不足返回 `protocol`，若起始位或数量超出 16 位 SNP-X 范围返回 `invalid_argument`。`encode_bits` 使用 `start_bit % 8` 作为结果首字节内的起始位置，通常由 `write_bits()` 内部调用。

字按有符号 int16 小端编码。`decode_words` 在缓冲区不足时返回 `protocol`；多余字节忽略。实现使用显式字节操作，不依赖主机字节序或对齐。

## 9. 完整使用示例

读取一个数字输出的完整流程：

```cpp
#include <fanuc_snpx/client.hpp>

#include <iostream>

int main() {
    using namespace fanuc::snpx;

    Client client;
    if (auto result = client.connect("10.0.0.1"); !result) {
        std::cerr << "connect failed: " << result.error().message << '\n';
        return 1;
    }

    auto response = client.read(digital_output, 1).get();
    if (!response) {
        std::cerr << "read failed: " << response.error().message << '\n';
        return 1;
    }

    auto values = decode_bits(payload_bytes(*response), 0, 1);
    if (!values) {
        std::cerr << "decode failed: " << values.error().message << '\n';
        return 1;
    }
    std::cout << "DO1 = " << ((*values)[0] ? "ON" : "OFF") << '\n';
}
```

连接会发送 `CLRASG`，即使示例只读取 DO1 也会清空控制器 ASG 表。连接对象离开作用域时自动断开；需要提前释放资源时显式调用 `disconnect()`。
