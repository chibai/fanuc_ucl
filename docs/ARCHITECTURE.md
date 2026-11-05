# 多品牌驱动与 FANUC SNPX C++23 架构说明

## 1. 目标与取舍

工程提供品牌无关的机械臂数据访问抽象，目前仅实现 FANUC SNP-X 的 TCP HMI 数据访问。设计目标是：业务代码通过基类替换品牌、协议字节布局可检查、请求生命周期有边界、资源由 RAII 管理、失败显式返回，以及上层工程可通过 CMake 正常集成。不把运动控制或尚未实现的品牌协议加入接口承诺。

使用 Boost.Asio 统一跨平台网络，使用 `co_await` 表达异步解析、连接和按帧接收；使用 C++23 `std::expected::and_then` 串联解码和响应验证，`std::to_underlying` 提取强类型枚举值，配合 `std::span`、`std::format`、ranges 和 `std::jthread` 管理接口与资源。不为单连接客户端引入 JSON、HTTP、消息总线或日志框架。实现仍有明确覆盖范围，不宣称厂商认证或全功能 SNPX 支持。

## 2. 组件与库选型

| 职责 | 实际使用 | 原因与边界 |
|---|---|---|
| TCP 网络 | Boost.Asio `ip::tcp::socket`、`resolver` | 统一 Linux/Windows 实现，头文件模式，无 Boost 二进制链接依赖 |
| I/O 调度 | `io_context`、`async_read` / `async_write`、`awaitable` | 事件驱动，不再做 1ms 轮询；单线程 executor 串行操作 socket |
| JSON | **不使用任何 JSON 库** | SNPX 是小端二进制帧，不是 JSON 协议；当前没有 JSON 配置或 REST 接口 |
| 二进制编解码 | 自有显式字节编码 + `std::span`、`std::bit_cast` | 不依赖结构体填充、宿主字节序或未对齐内存访问 |
| 可预期错误 | `std::expected<T, Error>` / `std::unexpected` | 连接、协议、输入校验等失败需要调用方显式处理 |
| 异步结果 | `std::promise` / `std::future` | 每个请求对应一次完成，调用方可等待或轮询 |
| 后台任务 | `std::jthread`、Asio work guard | 连接对象管理线程；销毁时取消 I/O、释放 guard 并 join，不使用 detached 线程 |
| 共享状态 | `std::mutex`、`std::atomic<bool>` | 队列/序号表互斥；在线状态原子读取 |
| 超时 | `asio::steady_timer` + `std::chrono::steady_clock` | 调度最近的请求期限，不受系统墙上时间校准影响 |
| 命令格式化 | `std::format` | 使用标准库，不依赖 fmt；ASG 倍率不受用户 locale 格式影响 |
| 构建/分发 | CMake、CTest、`Threads::Threads`、`Boost::headers` | 编译要求通过目标传播，支持子目录与安装包集成 |
| 测试 | C++ 测试可执行文件 + 本地 TCP 模拟控制器 | 无测试框架依赖；断言在 Release 下仍生效 |

`std::asio` 不是 C++23 标准库设施，本项目明确使用 `boost::asio`。不使用 nlohmann/json、JsonCpp、fmt 或 spdlog。开发容器和 VS Code 的 `.json` 文件是工具配置，不会被客户端解析，也不构成 JSON 运行时依赖。

若未来确有“JSON 配置文件”需求，应在独立应用层选择解析库并校验字段，不改变 SNPX 线上格式；目前不预埋未被使用的依赖。

## 3. 目录与职责

```text
.
├── CMakeLists.txt                    # 根工程与 CTest 入口
├── README.md                         # 快速开始、范围和安全提示
├── SNPX.md                           # 帧格式、数据段、端口、ASG 协议
├── LICENSE                           # 保留原始许可证
├── docs/ARCHITECTURE.md               # 本文
├── cpp/
│   ├── CMakeLists.txt                # 库、示例、测试、安装导出
│   ├── cmake/fanuc_snpxConfig.cmake.in
│   ├── include/robot/types.hpp       # 品牌无关错误与 expected 别名
│   ├── include/robot/robot_driver.hpp # 抽象基类与逻辑端口
│   ├── include/fanuc_snpx/fanuc_driver.hpp # FANUC 派生驱动
│   ├── include/fanuc_snpx/client.hpp  # 原 SNP-X 底层公共接口
│   ├── src/fanuc_driver.cpp          # 通用端口映射、同步等待与解包
│   ├── src/client.cpp                # 协议、端口、会话和 Asio 网络实现
│   ├── examples/snpx_example.cpp     # 读取 DO1 的命令行程序
│   └── tests/
│       ├── protocol_tests.cpp        # 字节布局、编解码、边界校验
│       ├── client_integration.cpp    # 真实 TCP、本地模拟控制器
│       └── package/                  # 安装包的独立消费者工程
├── .github/workflows/check.yml       # C++23 构建与 sanitizer CI
├── .devcontainer/                    # C++ 工具链开发环境
└── .vscode/settings.json             # CMake 编辑器集成
```

驱动层分为 `robot::RobotDriver` → `fanuc::FanucDriver` → `fanuc::snpx::Client`。第一层使用继承定义品牌无关纯虚接口；第二层通过组合复用现有客户端，不改动 SNP-X 网络和会话逻辑。后续品牌继承同一基类，实现自己的协议适配，不依赖 FANUC 类型。基类有虚析构函数，不可复制、不可移动。

通用接口只抽象连接状态、数字位、模拟字和 16 位寄存器读写，统一 0 起始索引和 `robot::Result<T>`。FANUC 适配层通过端口描述副本归一化索引，复用原有校验、异步请求和编解码函数，等待 future 后返回已解包的值；不为每次调用创建线程。原 Client 保持异步 API 和原生索引，ASG、UOP、原始消息等品牌专有功能不进入基类。错误定义上移至 `robot/types.hpp`，原命名空间保留别名。

`client.hpp` 不包含操作系统头文件，也不暴露 socket 句柄、锁和线程；`Client::Impl` 通过 `std::unique_ptr` 隐藏这些细节。调用方不需要定义 Windows 网络宏。

`client.cpp` 内部按职责分为：字节 Reader/Writer、消息工厂与编解码、位/字编解码、`Client::Impl` Asio I/O 调度、公共 Client API。

## 4. C++23 类型与所有权

### 4.1 强类型值

`MessageType`、`Service`、`Segment`、`Operation` 使用固定底层整数类型的 `enum class`，避免把服务码、数据段和操作码混用。`Header`、`Message` 是协议字段的值表示，不是 wire memory layout。

`Port` 描述 segment、偏移、索引起点、位/字类型与写权限。预定义常量覆盖数字、机器人、UOP、SOP、焊接、粘丝、组、模拟和寄存器端口。完整表及索引差异见 `SNPX.md`。

### 4.2 借用与持有

API 使用 `std::string_view` 和 `std::span` 表示只在调用期间借用的参数。发送前，消息会编码成队列拥有的 `std::vector<std::uint8_t>`；调用返回后无需保留原始写入数组。

`Message::payload` 持有扩展载荷，响应通过 future 转移给调用方；没有指向接收缓冲区的悬空视图。

`Client` 不可复制也不可移动，避免活动线程捕获的 `Impl` 生命周期被意外改变。析构函数调用 `disconnect()`，完成线程停止和 socket 关闭。`disconnect()` 可重复调用。

### 4.3 错误契约

```cpp
template<class T>
using Result = std::expected<T, Error>;

using Response = Result<Message>;
using ResponseFuture = std::future<Response>;
```

底层 Client 同步 API：`connect` 返回 `Result<void>`，`register_asg` 返回 `Result<uint16_t>`。异步 API：`read`、`write_bits`、`write_words`、`command`、`request` 返回 future，其值本身仍为 expected。上层 `RobotDriver` / `FanucDriver` 的所有操作同步返回 `Result<T>`，错误原样传播给调用者。

| 错误码 | 含义 | 调用方处理 |
|---|---|---|
| `invalid_argument` | 非法索引、数量、消息长度、ASG 参数等 | 修正参数，不盲目重试 |
| `io` | 系统网络调用失败 | 记录 message 和系统错误码，排查网络 |
| `timeout` | 握手、请求或 ASG 等待超时 | 会话失效，确认现场状态后显式重连 |
| `disconnected` | 未连接、主动断开或对端关闭 | 需要时重连并重新建立 ASG |
| `protocol` | 非法帧、类型不匹配、未知序号等 | 停止使用当前会话，检查兼容性和抓包 |
| `resource_exhausted` | 256 个序号全部占用 | 等已有请求完成后再提交 |

库不直接输出日志，错误通过 expected 交给应用统一记录；示例将错误写入 `std::cerr`。不会使用空结果或默认值伪装成功。

这不是“不抛异常”的全局保证：内存分配、错误使用 future 等标准库异常仍可能抛出。每个 future 只能调用一次 `get()`；不要对已消费的 future 再次等待。

## 5. 网络与会话生命周期

### 5.1 建连

```cpp
auto result = client.connect(
    "10.0.0.1",
    fanuc::snpx::default_port,
    std::chrono::milliseconds(1500), // TCP + 整个握手的预算
    std::chrono::milliseconds(250)   // 就绪后，每个请求的期限
);
```

默认两种超时均为 1 秒，必须为正。传入 IP 或主机名，内部通过 `resolver.async_resolve` 解析地址列表，再由 `async_connect` 逐一尝试连接。解析和连接通过 `co_await` 串联，由同一个 `steady_timer` 限制总预算，无手工 fd_set 或平台 socket 句柄。

超时会取消 resolver 并关闭 socket，返回 `timeout`，解析完成后也会重新检查期限，避免在过期后开始连接。**DNS 限制：Asio 底层系统解析的取消清理可能延迟完成，因此不保证墙钟硬实时期限。** 请求期限也会受到线程调度、网络与锁竞争影响。

连接成功后启用 `TCP_NODELAY`，启动一个 I/O 线程；INIT → MAGIC → CLRASG 成功才返回成功。在 `connect` 尚未返回时，不得从其他线程使用该 Client。

### 5.2 发送

```text
调用线程
  -> 校验参数
  -> mutex 下分配空闲序号
  -> 创建 promise / future
  -> 编码并放入 outgoing，post 唤醒 executor
  -> 返回 future
后台线程
  -> async_write 发送队首完整帧
  -> 完成后移除 outgoing 项，再发送下一帧
```

一个请求从入队开始计时，排队时间也属于期限。序号表包含所有在途请求，最多 256 项。`async_write` 是组合操作，处理部分写入；同一 socket 同时只允许一个写操作，防止帧交错。

Asio 负责平台非阻塞 I/O、就绪通知及平台错误处理。写完成处理器持有编码缓冲区的共享所有权；即使断开清空队列，缓冲区仍存活到取消完成，避免异步 I/O 悬空引用。

### 5.3 接收

后台协程先 `async_read` 完整 42 字节头，再按 payload_size 读取固定体和扩展载荷，最后解码并匹配请求。组合读操作处理半帧；粘连响应按帧依次读取，解析与 TCP 分段无关。

请求序号在互斥区内分配和释放，响应按序号匹配，允许乱序。匹配时验证初始化消息或普通响应类型；无法归属的响应和非法报文会使会话失败，而不是悄悄丢弃。

### 5.4 结束与超时

每个请求持有 `steady_clock` deadline，`steady_timer` 调度最近期限。任何一个超时都会使连接失效，关闭 socket 取消 I/O，并让所有待处理 future 以 timeout 完成。这是有意采用的保守策略：8 位序号没有会话世代，继续使用同一连接无法可靠区分迟到响应和已复用序号的新请求。定时器完成时重新检查在途期限，避免已经排队的旧完成事件误伤新请求。

`future.wait_for` **只等待，不取消请求**。例如调用方等 50ms 超时，而请求期限为 250ms，请求仍可能在剩余时间内被控制器执行。不能据此自动重试输出写入。

主动 `disconnect` 将关闭任务 post 到 executor：标记离线并完成待处理请求 → 取消 timer 并关闭 socket → 释放 work guard → 执行取消完成处理器 → join。所有 socket 操作在同一 executor 上串行完成；Asio 自动管理平台网络资源。所有未完成请求以 disconnected 完成；调用方丢弃 future 也不会取消控制器操作。

没有后台自动重连或自动重发，避免在不确定控制器是否已执行的情况下重复写入。

## 6. 并发约束

连接成功后，普通请求提交可以来自多个线程：序号表和发送队列受同一个 mutex 保护，promise 只完成一次，`connected()` 使用原子状态。

`register_asg` 通过独立 mutex 串行化，确保地址分配和同名注册一致；它会阻塞等待应答。**所有生命周期操作（connect/disconnect/析构）必须由应用串行管理，不能与其他 API 并发调用。** ASG 注册可能因超时主动断开，所以调用注册时也应在应用层与其他操作串行化。

`connected()` 只是瞬时观测，不保证下一次请求成功。网络可以在检查之后立刻断开，必须处理 future 中的错误。

协程仅用于内部网络实现，公共 API 保留 future，不暴露 Boost 类型或执行器。work guard 保持后台 executor 存活；会话失败后线程空闲等待显式 disconnect，不轮询、不自动重连。

## 7. 编解码和调用示例

### 7.1 位读取与偏移

```cpp
auto response = client.read(fanuc::snpx::digital_output, 8, 2).get();
if (!response) {
    std::cerr << response.error().message << '\n';
    return 1;
}
auto bytes = fanuc::snpx::payload_bytes(*response);
auto values = fanuc::snpx::decode_bits(bytes, 7, 2);
if (!values) {
    std::cerr << values.error().message << '\n';
    return 1;
}
```

DO8 的 wire target 为 7，因此解码从首字节 bit 7 开始。不要把所有端口都套成 1 起始索引。

### 7.2 字读取

```cpp
auto response = client.read(fanuc::snpx::register_port, 1, 2).get();
if (!response) {
    std::cerr << response.error().message << '\n';
    return 1;
}
auto words = fanuc::snpx::decode_words(fanuc::snpx::payload_bytes(*response), 2);
if (!words) {
    std::cerr << words.error().message << '\n';
    return 1;
}
```

返回类型为 `vector<int16_t>`。不能通过对 `bytes.data()` 做 `reinterpret_cast<int16_t*>` 解码，因为这依赖地址对齐和主机字节序。

### 7.3 写入与物理安全

`write_bits` 接收 `span<const bool>`，可使用 `std::array<bool, N>`。`std::vector<bool>` 是特殊的位容器，不提供普通 bool 连续存储，不能直接作为这个 span。

`write_words` 接收 `span<const int16_t>`，拒绝位端口；`write_bits` 拒绝字端口；预定义输入端口不可通过这两种接口写入。

写入 future 成功表示接收到了允许类型的应答，**不是设备动作完成证明**，也不是完整 PLC 状态码校验。需要上层回读、状态机和硬件安全联锁。

### 7.4 ASG

```cpp
auto address = client.register_asg("$TEST.$VALUE", 2);
if (!address) {
    std::cerr << address.error().message << '\n';
    return 1;
}
auto response = client.read_asg(*address, 2).get();
if (!response) {
    std::cerr << response.error().message << '\n';
    return 1;
}
```

`$TEST.$VALUE` 是占位变量名，必须换成控制器中存在且允许访问的变量。映射长度由调用者确认；接口不自动推断 float、字符串、位置、报警和程序状态布局。重新连接后必须丢弃旧地址并重新注册。

同名且参数一致时复用本地缓存；同名长度/倍率冲突会报错。注册等待超时立即断开连接，因为无法确认控制器是否已建立映射。

通过低层 `command` 自行发送 `CLRASG` / `SETASG` 会绕过本地映射管理。不要混用两种管理方式；若已改变控制器映射表，应重新连接并注册，不能继续信任旧缓存。

### 7.5 低层协议接口

`make_init`、`make_magic`、`make_read`、`make_write`、`encode`、`decode` 可独立使用；`request` 是专家级原始消息接口。

使用工厂函数建立合法消息，尤其注意 `payload_size` 与实际载荷长度一致。`encode` 是低层序列化器，不负责修复或校验手工构造的所有字段；`request` 会拒绝不支持的请求类型、长度或分片组合。`decode` 返回消息和消费字节数，可用于带后续帧的缓冲区；数据不足返回 protocol，由流式调用方在凑齐整帧后再解码。

## 8. 构建与安装集成

需要预先安装 Boost ≥ 1.74 头文件，CMake 通过 `find_package(Boost)` 查找，不自动下载依赖。Asio 与 Boost.System 使用头文件模式；Windows 系统网络库仍由目标链接。安装包通过 `find_dependency(Boost)` 重新查找消费者的 Boost。

Linux：

```sh
sudo apt-get install cmake libboost-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix install
```

Windows（Visual Studio 开发终端）：

先安装 Boost 并设置 `BOOST_ROOT`；也可使用 vcpkg `boost-asio` 和相应 CMake toolchain。

```powershell
cmake -S . -B build
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix install
```

需要协程及完整实现 `std::expected`（含 `and_then`）与 `std::format`、`std::to_underlying` 的编译器及标准库。仅指定 `-std=c++23` 不能弥补旧标准库缺少这些设施。推荐 GCC 13+ / libstdc++ 13+ 或 VS 2022 17.8+。其他平台没有在当前 CI 中作兼容性承诺。

支持以下选项：

| 选项 | 默认 | 说明 |
|---|---|---|
| `BUILD_TESTING` | ON | 构建 CTest 测试 |
| `FANUC_SNPX_BUILD_EXAMPLES` | ON | 构建命令行示例 |
| `BUILD_SHARED_LIBS` | OFF | CMake 标准选项；ON 生成共享库 |

子目录集成：

```cmake
add_subdirectory(path/to/fanuc_ucl snpx-build)
target_link_libraries(my_app PRIVATE fanuc_snpx::fanuc_snpx)
```

安装包集成：

```cmake
find_package(fanuc_snpx CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE fanuc_snpx::fanuc_snpx)
```

配置上层项目时，通过 `-DCMAKE_PREFIX_PATH=/absolute/path/to/install` 指向安装前缀。C++23 编译要求、头文件路径、线程库与 Windows 的 `ws2_32` 会通过目标传播。静态库/共享库须与应用使用兼容的编译器、标准库和运行时；PImpl 不代表跨任意 C++ ABI 兼容。

## 9. 验证与边界

协议测试覆盖：基础帧布局、固定/扩展请求与响应、每个截断长度、无效消息类型/长度/分片、粘连缓冲区、跨字节位打包、有符号小端字、载荷和端口边界、离线调用。驱动层另覆盖抽象继承与虚析构、兼容错误别名、端口类型/权限/边界校验、通过基类指针调用全部通用方法、所有位偏移解包、五种端口映射、最高寄存器地址、短响应错误、重连及超时传播；安装包消费者同时验证新旧 API。

集成测试通过基于 Asio 的本地 TCP 模拟控制器覆盖：拆分 INIT_ACK、完整握手、反序合并响应、多线程请求提交、位写入、ASG 注册/缓存冲突/读取/超时断开、重连清除缓存、序号回绕、请求超时、256 个在途请求耗尽、断开使待处理 future 完成、错误 INIT_ACK、握手超时、错误响应类型、未知序号、厂商错误帧与对端关闭。另覆盖主机名解析、拒绝连接后重连、空闲时取消期限、65534 字节扩展响应的分段读取、半帧超时、批量大帧写入期间析构和 future 跨 Client 生命周期完成。

CI 在 Linux/Windows 的 Debug/Release 构建并运行测试，安装后再通过独立消费者工程验证 `find_package` 与链接；Linux 另运行 AddressSanitizer 和 UndefinedBehaviorSanitizer。主工程测试有 CTest 级超时，断言不因 `NDEBUG` 被禁用。

不能由这些测试推出：所有机器人型号兼容、硬实时延迟、网络安全认证、复杂 ASG 数据类型正确性或厂商全部状态码支持。正式部署前，应针对目标控制器版本、选件和变量布局进行真实设备只读测试，再在独立的安全测试条件下验证写入。

## 10. 本次收敛范围

删除原 Rust 源码、Cargo/fuzz 工程、Python 绑定与测试、旧语言示例和演示媒体、其他协议、Rust/Python 格式化及发布流程、旧多协议变更日志。

保留并改造 C++ SNPX 源码、示例、测试、CI、开发容器和编辑器配置；保留许可证和项目历史，不修改既有版权归属。`SNPX.md` 与本说明仅引用当前 C++ API，不再把已删除实现当作规范来源。
