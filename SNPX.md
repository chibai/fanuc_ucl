# FANUC SNP-X / HMI 协议说明

本文按 `fanuc_ucl` 仓库中的 `hmi` 实现说明 SNP-X 通信过程。这里的 SNP-X 指本库实现的 HMI/SRTP 风格 TCP 子集，不等同于对 FANUC 全部 SNP-X 服务、控制器型号或选件的完整规范。具体可访问的端口、系统变量和写权限仍取决于控制器配置。

实现入口主要位于 `src/hmi/mod.rs`、`src/hmi/runner.rs`、`src/hmi/proto/wire.rs`、`src/hmi/proto/ports.rs` 和 `src/hmi/proto/asg.rs`。

## 1. 通信模型

- 传输层：TCP，控制器默认监听端口 `60008`。
- 字节序及序列化：小端、固定宽度整数编码；本库通过 bincode 编码帧，但帧头和报文体遵循代码中显式定义的 SNP-X 字段布局。
- 连接方向：客户端连接控制器，客户端发送请求，控制器按请求序号返回响应。
- 并发模型：独立 I/O 线程维护 TCP 流、拼接不完整帧、发送队列并按序号匹配响应。TCP 是字节流，单次 `read` 不保证恰好对应一帧。
- 权限与安全：该连接没有 TLS 或应用层身份验证。应在隔离、受控的机器人网络中使用；写 DO、GO、AO、寄存器或命令前，必须确认控制器状态、外围设备和安全措施。软件写入不能替代急停、安全门或其他安全回路。

此协议面向 HMI 数据读写，不是实时轨迹接口。高频运动控制请使用仓库中的 STMO/RMI 等相应接口。

## 2. 建立连接与握手

`HmiDriver::connect` 会连接到 `<控制器 IP>:60008`，默认连接超时为 1 秒，然后按顺序进行以下握手。整个握手使用同一个连接超时预算。

1. **TCP 建连**：创建非阻塞 TCP socket 和 I/O 线程，启用 `TCP_NODELAY`。
2. **INIT**：客户端发送 `Message::INIT`，初始序号为 `0`。该帧头的 `pkt_type` 为 `InitTx`，`msg_type` 为 `Init`；报文体带 `Init` segment。
3. **INIT_ACK**：控制器必须返回与本库 `Message::INIT_ACK` 完全相等的消息。该帧使用 `InitRx` 和 `InitFinish`。不匹配即握手失败并关闭连接。
4. **MAGIC 探测**：客户端序号递增到 `1`，发送 `Magic` 请求（`service_request = 0x4F`、`segment = 0x01`）。驱动等待该序号的响应。
5. **清理 ASG 表**：客户端发送 `GlobalByte` 写请求，内容为 ASCII `CLRASG`，并等待响应。该步骤清除控制器端此前的 ASG 映射。
6. **就绪**：上述步骤成功后 `connect` 才返回成功。其后的普通读写各自分配序号，并通过返回的 handle 等待响应。

任何握手步骤超时或失败都会使本次连接断开。断开后重新连接时，控制器端会再次执行 `CLRASG`；若复用同一个 `HmiDriver` 对象，建议重新创建对象并注册所需 ASG 项，避免沿用此前对象中的本地 ASG 缓存。

## 3. 帧格式

### 3.1 公共帧头：42 字节

帧头字段依次按小端编码。下表偏移是相对于一帧起始位置，`u16`/`u32` 分别占 2/4 字节。

| 偏移 | 大小 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 2 | `pkt_type` | 包类型，如 `InitTx=0x0000`、`InitRx=0x0001`、`Transmit=0x0002`、`Receive=0x0003` |
| 2 | 2 | `seq1` | 序号字段之一；本实现使用其低 8 位匹配响应 |
| 4 | 2 | `text_len` | 扩展报文有效载荷长度；普通报文为 0 |
| 6 | 2 | `unk1` | 保留/未解释字段 |
| 8 | 4 | `operation1` | 操作类型，如 `Init=0`、`InitFinish=1`、`Writing=0x100`、`Reading=0x200` |
| 12 | 4 | `unk2` | 保留/未解释字段 |
| 16 | 4 | `operation2` | 第二个操作类型字段 |
| 20 | 4 | `unk3` | 保留/未解释字段 |
| 24 | 2 | `unk4` | 保留/未解释字段 |
| 26 | 3 | `time` | 时间字段；当前模板置零 |
| 29 | 1 | `unk5` | 保留/未解释字段 |
| 30 | 1 | `seq2` | 另一个序号字段；客户端请求同时设置 `seq1` 和 `seq2` |
| 31 | 1 | `msg_type` | 消息类型，见下表 |
| 32 | 4 | `mbox_src` | mailbox 源地址 |
| 36 | 4 | `mbox_dst` | mailbox 目标地址；常规模板为 `0x00000E10` |
| 40 | 1 | `pkt_num` | 分片序号；当前实现使用单包 `1` |
| 41 | 1 | `total_pkt_num` | 总分片数；当前实现使用单包 `1` |

`msg_type` 的本库定义：

| 值 | 类型 | 用途 |
|---:|---|---|
| `0x00` | `Init` | INIT 握手帧 |
| `0xC0` | `Req` | 固定长度请求 |
| `0xD4` | `Resp` | 固定长度响应/确认 |
| `0x80` | `ExtReq` | 带可变长度载荷的请求 |
| `0x94` | `ExtResp` | 带可变长度载荷的响应 |
| `0xD1` | `Error` | 错误类型；当前消息解码器不将它作为普通请求/响应解码 |

### 3.2 报文体与帧总长

普通帧由 **42 字节帧头 + 14 字节报文体 = 56 字节** 组成。固定请求 (`Req`) 的 14 字节报文体为：

| 字段 | 大小 | 含义 |
|---|---:|---|
| `service_request` | 1 | 服务代码 |
| `segment` | 1 | 内存/数据段 |
| `target_index` | 2 | 目标地址或位索引 |
| `target_size` | 2 | 读取/写入项目数 |
| `payload` | 6 | 内嵌数据；不足部分补零 |
| `unk` | 2 | 保留字段 |

`Resp` 也为 14 字节：`unk1 (2)`、`payload (6)`、PLC 状态（程序号 1、权限级别 1、最后扫描时间 2、状态标志 2）。写请求和握手主要通过响应是否到达确认；普通写入的 typed handle 不会额外验证回读值。

扩展请求/响应的固定部分同样为 14 字节，之后紧跟载荷。扩展帧长度为：

```text
frame_length = 56 + text_len
```

扩展报文中 `text_len` 是后续载荷的**字节数**。当前实现把扩展分片字段设为 `pkt_num = 1`、`total_pkt_num = 1`，也就是单个 TCP 帧承载整段扩展数据。短写入（编码后不超过 6 字节）使用 `Req`，更长的写入使用 `ExtReq`；读取请求使用固定请求帧，较长读取结果由控制器以扩展响应返回。

### 3.3 序号与响应匹配

序号是 8 位值，会在 `0..255` 间回绕。驱动将完整发送的请求按序号登记为等待响应，控制器响应到达后按序号找到对应 handle。若同一序号仍有未完成请求，发送队列会在真正发送前为新请求换用空闲序号；全部 256 个序号都占用时，新请求暂缓发送。因此调用读写 API 返回 handle 并不表示控制器已经应答，须等待 handle 或检查其状态。

接收端会将 TCP 字节追加到缓冲区：缓冲区不足 42 字节时等待更多数据；有完整帧头后按 `56 + text_len` 判断完整帧是否到齐；随后可在同一批数据中连续解析多帧。响应只按序号路由，不依赖 TCP `read` 边界。

## 4. 服务代码、数据段与端口映射

本库常用的服务代码：

| 服务 | 值 | 用途 |
|---|---:|---|
| `ReadSysMemory` | `0x04` | 读取系统内存/数据端口 |
| `WriteSysMemory` | `0x07` | 写入系统内存/数据端口 |
| `Magic` | `0x4F` | 握手后的 Magic 探测 |

本库支持的数据段包括 DI/DO 位区、寄存器和全局字节命令等。注意 FANUC 对输入/输出段的命名与主机读写对象容易混淆；`DigitalInput` 和 `DigitalOutput` 的代码映射如下：

| API 数据端口 | SNP-X segment | 基本类型 | 说明 |
|---|---:|---|---|
| `DigitalInput` | `OutputBit=0x48` | 位 | 输入信号；从位区读取 |
| `DigitalOutput` | `InputBit=0x46` | 位 | 输出信号；可读写 |
| `RobotInput` / `RobotOutput` | `OutputBit` / `InputBit` | 位 | 各自基址偏移 5000 |
| `UopInput` / `UopOutput` | `OutputBit` / `InputBit` | 位 | 各自基址偏移 6000 |
| `SopInput` / `SopOutput` | `OutputBit` / `InputBit` | 位 | 各自基址偏移 7000 |
| `WeldInput` / `WeldOutput` | `OutputBit` / `InputBit` | 位 | 各自基址偏移 8000 |
| `WireStickInput` / `WireStickOutput` | `OutputBit` / `InputBit` | 位 | 各自基址偏移 8400 |
| `GroupInput` | `AnalogOutput=0x0C` | 小端 `i16` | 群组输入读取 |
| `GroupOutput` | `AnalogInput=0x0A` | 小端 `i16` | 群组输出读写 |
| `AnalogInput` | `AnalogOutput` | 小端 `i16` | 基址偏移 1000 |
| `AnalogOutput` | `AnalogInput` | 小端 `i16` | 基址偏移 1000 |
| `Register` | `Registers=0x08` | 小端 `i16` | 数值寄存器 `R[]` |
| `Command` | `GlobalByte=0x38` | 字节串 | 发送 `CLRASG`、`SETASG ...`、`CLRALM` 等命令 |

端口 API 的索引约定由具体端口类型决定：例如 `DigitalInput` 的索引为零起始（`DI[0]` 是第一个位），而 `DigitalOutput`、`GroupOutput` 和 `Register` 使用从 1 开始的索引；单索引端口传 `0` 会报错。读取布尔数组时，驱动将读取范围扩展到字节边界后再裁剪为所请求的位数；布尔位在字节内按低位到高位（bit 0 到 bit 7）排列。调用时应以端口类型和控制器 I/O 配置为准，不要仅凭信号名称推算 wire index。

常见编码规则：

- 位数组按位打包，读取响应也按位解包。
- `i16` 寄存器按小端两字节编码。
- 命令字符串按其 UTF-8 字节发送；数组命令之间用回车字节 `0x0D` 分隔。
- 输入端口通常只读。Rust 的 `write_array_unsafe` / `write_unsafe` 和 Python 的 `write_unsafe` 会绕过该限制；这些方法不会自动使输入成为受支持或安全的写入目标。

## 5. ASG（变量映射）流程

ASG 将控制器变量映射到连续的 `R[]` 寄存器区，之后通过普通 `ReadSysMemory` / `WriteSysMemory` 寄存器请求访问。

1. `connect` 握手先发送 `CLRASG`，清空控制器端映射。
2. 调用 `register_asg` / `register_sysvar_asg` 时，驱动生成变量名、变量类型、寄存器长度和可选切片范围。
3. 驱动为首个条目分配地址 `R[1]`，后续条目放在本地已分配区域末尾。
4. 驱动通过 `GlobalByte` 发送 `SETASG <address> <size> <variable> <multiply>`，并等待控制器确认。
5. 返回的 `AsgVarInterface` 保存映射信息。读取时从对应 `R[]` 地址读取，再根据类型/偏移解码；写入时将类型值编码成寄存器字并写入相同区域。

常用 ASG 类型及默认占用寄存器数如下。`Short`、`Byte` 和 `Bool` 的系统变量映射仍按此实现规定的两个寄存器传输，而不直接套用单字节/双字节普通 port 编码。

| 类型 | 默认寄存器数 |
|---|---:|
| `i32`、`i16`、`i8`、`f32`、`bool` | 2 |
| `String` | 40 |
| `PositionData` | 50 |
| `AlarmData` | 100 |
| `ProgramStatus` | 18 |

注册同名变量时，当前 `HmiDriver` 会返回已有本地条目而不重新发送 `SETASG`。控制器端 ASG 项的数量和可映射对象仍受控制器限制；文档中的表仅描述本库的数据布局。

## 6. Rust 示例

下面示例读取 DO1、写入 DO1，并映射一个布尔系统变量。请仅在已确认 DO1 可安全切换的测试环境运行写入部分。

```rust
use std::time::Duration;

use fanuc_ucl::hmi::{DigitalOutput, HmiDriver, SysVarArgs};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut driver = HmiDriver::new([10, 0, 0, 1]);
    driver.connect(Some(Duration::from_secs(1)), &[], &[])?;

    let do1 = driver
        .read::<DigitalOutput>(1)?
        .wait_timeout(Duration::from_millis(100))?;
    println!("DO1 = {do1}");

    // 写入返回响应 handle；等待它以确认控制器已应答。
    driver
        .write::<DigitalOutput>(1, true)?
        .wait_timeout(Duration::from_millis(100))?;

    let enable = driver.register_asg(
        SysVarArgs::<bool> {
            var_name: "$MCGR_CFG.$ENABLE".to_string(),
            ..Default::default()
        },
        Duration::from_millis(100),
    )?;
    let enabled = enable
        .read(&driver)?
        .wait_timeout(Duration::from_millis(100))?;
    println!("HSPO enabled = {enabled}");

    driver.disconnect(false)?;
    Ok(())
}
```

## 7. Python 示例

```python
from fanuc_ucl import hmi


def main():
    driver = hmi.HmiDriver("10.0.0.1")
    driver.connect(timeout_secs=1.0)

    do1 = driver.read(hmi.DigitalOutput, 1).wait_timeout(0.1)
    print(f"DO1 = {do1}")

    # 写入会改变控制器输出；仅在确认安全后执行。
    driver.write(hmi.DigitalOutput, 1, True).wait_timeout(0.1)

    enable = driver.register_sysvar_asg(
        bool,
        name="$MCGR_CFG.$ENABLE",
        timeout_secs=0.1,
    )
    print(f"HSPO enabled = {enable.read(driver).wait_timeout(0.1)}")

    driver.disconnect()


if __name__ == "__main__":
    main()
```

Python 的 `read` / `write` 使用数据端口类作为第一个参数。`register_sysvar_asg` 的 Python 接口根据传入类型（如 `bool`、`int`、`float`、`str`）选择 ASG 编解码类型；Rust 则由 `SysVarArgs<T>` 的类型参数确定。

## 8. C++23 实现

仓库提供一个无第三方库依赖的独立 C++23 实现，位于 `cpp/`。它包含 SNP-X 帧编解码、带超时的 TCP 连接和 INIT/MAGIC/CLRASG 握手、后台 I/O 线程、8 位序号响应匹配、常用位/字端口读写，以及 ASG 命令和寄存器访问。接口使用 `std::expected` 表示同步错误，使用 `std::future` 表示异步请求响应；TCP socket 由平台原生 socket API 提供。预定义输入端口为只读，写入 API 会拒绝这些端口。

```sh
cmake -S cpp -B build/cpp
cmake --build build/cpp
ctest --test-dir build/cpp --output-on-failure
```

完整可执行示例在 `cpp/examples/snpx_example.cpp`。`Client::read` 返回原始 SNP-X 响应；用 `payload_bytes(response)` 取响应载荷，再按数据类型调用 `decode_bits` 或 `decode_words`。位读取的载荷从驱动为读取范围对齐后的首字节开始，所以调用 `decode_bits` 时传入所请求起始位相对于该首字节的位偏移；例如 DO1 对齐后从 bit 0 开始。ASG 接口接受变量名和寄存器长度，返回起始 R 地址，调用方需按映射变量的数据布局自行编码/解码寄存器。

该实现的协议覆盖范围与前述限制一致，未实现完整厂商 SNP-X 服务集合、TLS、控制器错误帧语义或多分片扩展报文。

## 9. 常见错误与排查

| 错误/现象 | 含义与建议 |
|---|---|
| `Timeout` | TCP 建连或响应等待超时。检查控制器 IP、端口、网络策略和控制器 SNP-X/HMI 配置，并适当增加超时。 |
| `NotConnected` | I/O 线程或 TCP 连接已结束；检查 `has_connection_errored()`，必要时重新建连。 |
| `ZeroIndex` | 对从 1 开始的端口传入了索引 0。 |
| `IndexOutOfRange` | 端口地址/范围超出协议允许的 16 位地址空间。 |
| `MalformedResponse` | 响应载荷短于该类型要求的最小长度，或无法按预期布局解码。 |
| 连接成功但变量读写失败 | 确认该端口/系统变量受控制器支持、索引约定正确、ASG 类型和切片匹配，并且控制器允许相应操作。 |
| 收到异常或没有匹配请求的响应 | 响应必须带有正在等待的序号；检查是否有其他客户端共享连接或控制器侧异常。 |

调用 `wait_timeout` 会将响应超时作为错误返回；不要把“请求已排队”或“write API 已返回 handle”当作控制器已成功执行的证据。
