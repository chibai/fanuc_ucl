# FANUC SNP-X · C++23

面向多品牌机械臂数据访问的 C++23 驱动接口，目前提供 FANUC SNPX（SNP-X）实现，默认使用 TCP `60008`。FANUC 实现支持帧编解码、连接握手、异步请求、位/字端口读写和 ASG 变量映射；尚未实现其他品牌协议。

**依赖选型：网络统一使用 Boost.Asio；错误和格式化使用 C++23 标准库；不使用 JSON 库。** Asio 异步 I/O、`co_await` 协程和定时器替代平台 socket 代码与轮询；公共接口保留 `std::future<std::expected<…>>`。SNPX 使用二进制帧，JSON 不属于其传输格式。`std::asio` 不属于 C++23 标准库，本项目使用 `boost::asio`。

## 构建

需要 CMake ≥ 3.25、Boost ≥ 1.74 的头文件，以及支持协程、`std::expected`（含单子操作）、`std::format`、`std::to_underlying`、`std::jthread` 的编译器和标准库。推荐 GCC 13+ / libstdc++ 13+，或 Visual Studio 2022 17.8+。Linux 和 Windows 已配置 CI；其他工具链需自行验证标准库支持。

Ubuntu 可先安装 `sudo apt-get install cmake libboost-dev`。Windows 可设置 `BOOST_ROOT` 指向 Boost 安装目录，或使用 `-DCMAKE_TOOLCHAIN_FILE=…/vcpkg.cmake` 和 vcpkg 的 `boost-asio` 包。Asio 与 Boost.System 使用头文件模式，无需单独链接 Boost 二进制库。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

也可使用 `cmake -S cpp -B build/cpp` 独立构建库。

## VS Code（Ubuntu 26.04 / G++ 15.2 / C++23）

仓库自带 `.vscode` 配置，使用系统 `/usr/bin/g++-15`、Ninja 和 GDB。先安装工具：

```sh
sudo apt-get update
sudo apt-get install g++-15 cmake ninja-build gdb libboost-dev
g++-15 --version
code .
```

确认编译器版本为预期的 15.2；配置指定 GCC 15 的可执行文件，并不锁定发行版提供的补丁版本。打开**仓库根目录**，安装 VS Code 推荐的 **C/C++** 和 **CMake Tools** 扩展。

- **Ctrl+Shift+B**：配置并构建 Debug；产物位于 `build/Debug/cpp/`。
- **F5**：默认构建并通过 GDB 调试协议测试；也可在“运行和调试”中选择本地模拟控制器集成测试或 SNP-X 示例。示例会询问控制器地址，**连接会清空 ASG 表**，不要未经确认连接真实设备。
- **终端 → 运行任务**：选择 `CMake: test Debug`、`CMake: build Release`、`CMake: test Release` 或对应清理任务；Release 产物位于 `build/Release/cpp/`。
- **CMake Tools**：选择 GCC 15 工具包及 Debug/Release 构建类型；其配置与任务共用对应构建目录。智能提示由 CMake 提供实际编译参数，并生成 `compile_commands.json`；保存 C++ 文件时使用 C/C++ 扩展自带的 clang-format（LLVM 风格）。

若编译器安装路径不同，同时修改 `.vscode/settings.json` 和 `.vscode/tasks.json` 中的 `/usr/bin/g++-15`。更换已有构建目录的编译器或生成器前，先运行 **CMake: Delete Cache and Reconfigure**。这些工作区配置面向上述本机环境；现有 Ubuntu 24.04 Dev Container 默认只有 GCC 13，使用它时也需相应调整编译器路径。

## 使用

### 品牌无关接口

`robot::RobotDriver` 是抽象基类，`fanuc::FanucDriver` 继承它并复用现有 SNP-X 客户端。业务代码依赖基类，后续品牌实现同一接口即可替换：

```cpp
#include <fanuc_snpx/fanuc_driver.hpp>
#include <iostream>
#include <memory>

int main() {
    std::unique_ptr<robot::RobotDriver> driver =
        std::make_unique<fanuc::FanucDriver>();
    if (auto result = driver->connect("10.0.0.1", 60008); !result) {
        std::cerr << result.error().message << '\n';
        return 1;
    }
    auto bits = driver->read_bits(robot::Port::digital_output, 0);
    if (!bits) {
        std::cerr << bits.error().message << '\n';
        return 1;
    }
    std::cout << "DO1 = " << (*bits)[0] << '\n';
}
```

通用接口统一采用 **0 起始索引**，读写同步等待设备响应，返回 `robot::Result<T>`；目前抽象连接、数字 I/O、模拟 I/O 和 16 位寄存器读写，不包含运动控制。通过基类连接时必须显式指定端口，直接调用 `FanucDriver::connect(host)` 则默认使用 `60008`。新增品牌只需包含 `robot/robot_driver.hpp` 并继承 `robot::RobotDriver`，不应依赖 SNP-X 消息类型。

### FANUC 专有接口（保持兼容）

需要异步请求、ASG、UOP 或原始报文时，继续使用 `fanuc::snpx::Client`；其接口、端口索引规则与原有行为保持不变：

```cpp
#include <fanuc_snpx/client.hpp>
#include <iostream>

int main() {
    fanuc::snpx::Client client;
    if (auto result = client.connect("10.0.0.1"); !result) {
        std::cerr << result.error().message << '\n';
        return 1;
    }
    auto response = client.read(fanuc::snpx::digital_output, 1).get();
    if (!response) {
        std::cerr << response.error().message << '\n';
        return 1;
    }
    auto bits = fanuc::snpx::decode_bits(fanuc::snpx::payload_bytes(*response), 0, 1);
    if (!bits) {
        std::cerr << bits.error().message << '\n';
        return 1;
    }
    std::cout << "DO1 = " << (*bits)[0] << '\n';
}
```

命令行示例：`./build/cpp/snpx_example 10.0.0.1`。示例只读取 DO1，不切换物理输出；**建立连接仍会发送 `CLRASG`，清空控制器 ASG 表**。

## 集成

在上层 CMake 工程中通过 `add_subdirectory` 引入，链接 `fanuc_snpx::fanuc_snpx`；也可安装后使用 `find_package(fanuc_snpx CONFIG REQUIRED)`：

```sh
cmake --install build --config Release --prefix install
```

接口功能、参数约定和完整使用示例见 [C++ API 参考](docs/API.md)；架构与依赖说明见 [C++23 架构说明](docs/ARCHITECTURE.md)，线上字节格式和端口表见 [SNPX 协议说明](SNPX.md)。

## 范围与安全

这是非官方 SNPX TCP 子集实现，不是 FANUC 全量协议或安全认证组件。未实现 TLS、身份认证、多分片重组、自动重连及复杂 ASG 数据类型的自动编解码。不提供实时运动控制。

仅用于隔离的工业网络。写入输出、寄存器和命令前，必须确认控制器配置、权限和设备安全；软件不能替代硬件安全回路。模拟控制器测试不等于真实机器人型号兼容性认证。

原有 Rust/Python 绑定、其他协议、示例及发布流程已移除；本工程不承诺与旧 API 兼容。保留原始 [Apache-2.0 许可证](LICENSE)。
