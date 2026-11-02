# FANUC SNP-X · C++23

专注于 FANUC SNPX（SNP-X）HMI 数据访问的 C++23 客户端，默认使用 TCP `60008`。工程仅保留 SNPX：帧编解码、连接握手、异步请求、位/字端口读写和 ASG 变量映射。

**依赖选型：网络使用原生 POSIX Socket / Windows Winsock2；并发、错误和格式化使用 C++23 标准库；不使用 JSON 库。** SNPX 使用二进制帧，JSON 不属于其传输格式。

## 构建

需要 CMake ≥ 3.25，以及支持 `std::expected`、`std::format`、`std::jthread` 的编译器和标准库。推荐 GCC 13+ / libstdc++ 13+，或 Visual Studio 2022 17.8+。Linux 和 Windows 已配置 CI；其他工具链需自行验证标准库支持。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

也可使用 `cmake -S cpp -B build/cpp` 独立构建库。

## 使用

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

详细说明见 [C++23 架构与依赖说明](docs/ARCHITECTURE.md)，线上字节格式和端口表见 [SNPX 协议说明](SNPX.md)。

## 范围与安全

这是非官方 SNPX TCP 子集实现，不是 FANUC 全量协议或安全认证组件。未实现 TLS、身份认证、多分片重组、自动重连及复杂 ASG 数据类型的自动编解码。不提供实时运动控制。

仅用于隔离的工业网络。写入输出、寄存器和命令前，必须确认控制器配置、权限和设备安全；软件不能替代硬件安全回路。模拟控制器测试不等于真实机器人型号兼容性认证。

原有 Rust/Python 绑定、其他协议、示例及发布流程已移除；本工程不承诺与旧 API 兼容。保留原始 [Apache-2.0 许可证](LICENSE)。
