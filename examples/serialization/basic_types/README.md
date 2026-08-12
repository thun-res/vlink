# 🧩 basic_types — 开箱即用的消息类型

`Publisher<T>` / `Subscriber<T>` 的模板参数 `T` 即消息类型，VLink 按 `T` 在编译期自动选择序列化方式，应用层无需注册编解码器。本示例演示四种无需代码生成工具的类型：`vlink::Bytes`（原始字节）、`std::string`（文本/JSON）、POD 结构体（平凡可拷贝结构）和宏声明的 SOME/IP 结构。

![类型到序列化策略](./images/serialization-type-chain.png)

## 📋 核心 API

| API | 用途 |
|-----|------|
| `vlink::Publisher<T>(url)` | 创建某类型的发布者 |
| `vlink::Subscriber<T>(url)` | 创建某类型的订阅者 |
| `pub.publish(msg)` | 发布一条消息 |
| `sub.listen(cb)` | 注册回调 `void(const T&)`，仅可调用一次 |
| `vlink::Bytes::from_string(sv)` | 由字符串构造 Bytes（拷贝） |
| `vlink::Bytes::create(n)` | 分配 n 字节、未初始化的 Bytes |
| `vlink::Bytes::encode_to_base64 / decode_from_base64` | Base64 互转 |
| `vlink::Bytes::get_crc_32(b)` | 计算 CRC32 |
| `VLINK_SOMEIP_FIELDS(...)` | 按声明顺序生成 SOME/IP payload 编解码 |
| `VLINK_SOMEIP_ALIGNMENT(n)` | 配置顶层 payload 的 AUTOSAR alignment |
| `VLINK_SOMEIP_ENDIAN(endian)` | 配置顶层 payload 标量的大小端 |
| `VLINK_SOMEIP_ENDIAN_BIG` / `VLINK_SOMEIP_ENDIAN_LITTLE` | 显式配置大端或小端 payload 标量 |
| `VLINK_SOMEIP_LENGTH(field, n)` | 配置单个字段的长度宽度 |
| `VLINK_SOMEIP_ARRAY_LENGTH(field, ...)` | 从外到内配置多维数组的长度宽度 |
| `VLINK_SOMEIP_STRUCT_LENGTH(n)` | 配置结构体的长度宽度 |

## 🚀 最小可运行片段

```cpp
// std::string：直接收发，框架自动序列化
vlink::Subscriber<std::string> sub("intra://example/basic/string#direct");
sub.listen([](const std::string& msg) { VLOG_I("recv: ", msg); });

vlink::Publisher<std::string> pub("intra://example/basic/string#direct");
pub.publish(std::string("Hello, VLink!"));
```

本示例用 `#direct` 让回调在发布线程内同步完成。默认 `#queue` 则由 intra 自身的 pipeline 派发。

POD 结构体（平凡 + 标准布局）可直接作为 `T`：`vlink::Publisher<SensorReading>`，发布前用 `SensorReading r{}` 零初始化以避免填充字节不确定。`vlink::Bytes` 用法相同，并附带 Base64 / CRC32 等工具方法。

## 🧭 类型选型

- **`vlink::Bytes`**：应用层已有自定义二进制协议，零编解码开销。
- **`std::string`**：文本、JSON 或格式未知的载体。
- **POD 结构体**：同机同 ABI 的简单结构，最快路径；跨平台/跨语言请改用 Protobuf / FlatBuffers / CDR。
- **SOME/IP 结构**：用 `VLINK_SOMEIP_FIELDS(...)` 声明字段顺序，支持嵌套结构、`std::string`、`std::vector`、`std::array` 和 `vlink::Bytes`；大小端、alignment 与长度宽度的部署范围见[消息序列化](../../../doc/03-serialization.md#-35-someip-与自定义序列化器)。
- **其他复杂结构**：跨语言场景改用 Protobuf（见 [`../../samples/helloworld/`](../../samples/helloworld/)）或 FlatBuffers（见 [`../../samples/someip_flat/`](../../samples/someip_flat/)）。

## ▶️ 运行

```bash
./build/output/bin/example_basic_types
```

## 📚 相关文档

- [`../../base/bytes_basic/`](../../base/bytes_basic/) — `vlink::Bytes` 的完整 API
- [`../../samples/helloworld/`](../../samples/helloworld/) — Protobuf 序列化端到端样例
- [`../../samples/someip_flat/`](../../samples/someip_flat/) — FlatBuffers 序列化端到端样例
- 顶层 `doc/03-serialization.md` — 序列化机制全景
