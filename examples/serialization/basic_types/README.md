# basic_types — VLink 自动序列化派发：Bytes / std::string / POD 结构

本示例演示 VLink 在编译期通过模板特化自动选择序列化策略的机制。VLink 三种通信原语（Publisher/Subscriber/Server/Client/Setter/Getter）的模板参数 `T` 决定消息类型，框架根据 `T` 的特征在编译期推导出对应的 `Serializer::Type`，从而决定如何把 `T` 编解码为 `Bytes`。

本示例分三段，分别演示 vlink 中最基础的三种序列化路径：

1. **`vlink::Bytes`（kBytesType）**：原始字节，没有编解码开销，作为"应用层自定义协议"的载体。
2. **`std::string`（kStringType）**：长度前缀 + payload；最常见的文本/JSON/未知格式载体。
3. **POD 结构（kStandardType）**：编译期判定 `std::is_trivial_v<T> && std::is_standard_layout_v<T>`，按 `memcpy(sizeof(T))` 编解码。

读完本示例你能掌握：

- 三种最常用类型在 vlink 里的序列化行为差异。
- `Serializer::get_type_of<T>()` 编译期类型查询的用法（debug 和 `static_assert` 验证）。
- 手动调用 `Serializer::serialize` / `deserialize` 做单元测试 round-trip。

## 背景与适用场景

应用层把数据放到 VLink 上的"消息"位置时，必须选一种序列化形态。vlink 的 `Serializer` 在编译期为每种 `T` 自动选定策略，应用层几乎不需要写"如何编码"的代码 —— 只要 `T` 满足某个 trait，对应的策略就被启用：

- `Bytes` 等同的类型 → `kBytesType`，零开销。
- `std::string` / `std::string_view` 等 → `kStringType`，按长度前缀 + 原始字节。
- 满足 `is_trivial && is_standard_layout` 的结构 → `kStandardType`，按 `memcpy` 编码。
- 自定义类型实现了 `operator>>` / `operator<<` → `kCustomType`（见 `../custom_type/`）。
- Protobuf 消息 → `kProtoType`（见 `samples/helloworld/`）。
- FlatBuffers 表 → `kFlatTableType`（见 `samples/pub_sub_fbs/`）。

本示例只覆盖前三种，因为它们不依赖任何外部代码生成（Protobuf / FlatBuffers 都需要工具链）。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `vlink::Serializer::get_type_of<T>` | `constexpr Type get_type_of<T>()` | 编译期返回 `T` 对应的 `Serializer::Type` 枚举 |
| `vlink::Serializer::serialize<T>` | `void serialize(const T& msg, Bytes& out)` | 把 `T` 编码到 `out`（按其类型策略） |
| `vlink::Serializer::deserialize<T>` | `void deserialize(const Bytes& in, T& out)` | 反向 |
| `vlink::Serializer::get_serialized_size<T>` | `size_t get_serialized_size(const T& msg)` | 编码后字节数（含长度前缀等元数据） |
| `vlink::Bytes::from_string` | `static Bytes from_string(std::string_view)` | 从字符串构造 Bytes（拷贝） |
| `vlink::Bytes::create` | `static Bytes create(size_t size)` | 分配指定大小、未初始化的 Bytes |
| `vlink::Bytes::encode_to_base64` / `decode_from_base64` | 同名 | Base64 互转 |
| `vlink::Bytes::get_crc_32` | `static uint32_t get_crc_32(const Bytes&)` | CRC32 校验 |

## 代码导读

### 1. 编译期类型查询

```cpp
static_assert(vlink::Serializer::get_type_of<vlink::Bytes>() == vlink::Serializer::kBytesType);
static_assert(vlink::Serializer::get_type_of<std::string>() == vlink::Serializer::kStringType);
static_assert(vlink::Serializer::get_type_of<SensorReading>() == vlink::Serializer::kStandardType);
static_assert(vlink::Serializer::get_type_of<int>() == vlink::Serializer::kStandardType);
```

这些 `static_assert` 在编译期通过 —— vlink 在你写 `Publisher<SensorReading>` 时已经知道该走 `kStandardType` 策略。

`int` 也被识别为 `kStandardType`：它是 trivial + standard-layout 的标量。

### 2. Bytes 消息流（零开销）

```cpp
vlink::Subscriber<vlink::Bytes> sub("dds://example/basic/bytes");
sub.attach(loop);
sub.listen([&received](const vlink::Bytes& msg) {
  received++;
  VLOG_I("[Bytes] #", received, " size=", msg.size(), " text=", msg.to_string());
});

vlink::Publisher<vlink::Bytes> pub("dds://example/basic/bytes");
pub.attach(loop);

pub.publish(vlink::Bytes::from_string("Hello VLink Bytes!"));
pub.publish(vlink::Bytes{0x48, 0x65, 0x6C, 0x6C, 0x6F});  // 字符串字面量的字节列表

auto buf = vlink::Bytes::create(8);
std::memset(buf.data(), 0xAB, buf.size());
pub.publish(buf);

pub.publish(vlink::Bytes{});  // 空 Bytes 也是合法消息
```

`Bytes` 走 `kBytesType`：发布时不做任何编码，订阅端 `listen` 回调收到的是同一份字节序列。这是性能最优的载体，适用于应用层有自己定义二进制协议的场景。

### 3. std::string 消息流

```cpp
vlink::Subscriber<std::string> sub("dds://example/basic/string");
sub.attach(loop);
sub.listen([&received](const std::string& msg) {
  received++;
  VLOG_I("[String] #", received, " len=", msg.size(), " text=\"", msg, "\"");
});

vlink::Publisher<std::string> pub("dds://example/basic/string");
pub.publish(std::string("Hello, VLink!"));
pub.publish(std::string("UTF-8 supported"));
pub.publish(std::string(""));
pub.publish(std::string(200, 'A'));
pub.publish(std::string(R"({"key":"value","count":42})"));

std::string original = "round-trip test payload";
vlink::Bytes buf;
vlink::Serializer::serialize(original, buf);

std::string restored;
vlink::Serializer::deserialize(buf, restored);
VLOG_I("[String] round-trip match=", original == restored);
```

`std::string` 走 `kStringType`：编码格式是 4 字节长度 + 原始 byte 数组。空串、200 字符长串、JSON 文本都能完整通过。

最后几行用 `Serializer::serialize` / `deserialize` 显式做一次单元测试 round-trip，验证编解码可逆。

### 4. POD 结构消息流

```cpp
struct SensorReading {
  uint32_t sensor_id;
  double temperature;
  double humidity;
  int64_t timestamp_us;
  uint8_t status;
  uint8_t reserved[7];
};

static_assert(std::is_trivial_v<SensorReading>);
static_assert(std::is_standard_layout_v<SensorReading>);

vlink::Subscriber<SensorReading> sub("dds://example/basic/pod");
sub.listen([&received](const SensorReading& r) {
  received++;
  VLOG_I("[POD] #", received, " id=", r.sensor_id, " temp=", r.temperature,
         " humidity=", r.humidity, " status=", static_cast<int>(r.status));
});

vlink::Publisher<SensorReading> pub("dds://example/basic/pod");

SensorReading reading{};   // 零初始化，避免 padding 含未定义字节
reading.sensor_id = 42;
reading.temperature = 23.5;
...
pub.publish(reading);

VLOG_I("[POD] sizeof(SensorReading)=", sizeof(SensorReading),
       " serialized_size=", vlink::Serializer::get_serialized_size(reading));
```

`SensorReading` 编译期被判定为 `kStandardType`，因此编码就是 `memcpy(&reading, buf.data(), sizeof(reading))`。`reserved[7]` 字段显式补齐，让 sizeof 可控。

注意 `SensorReading reading{}` —— **零初始化非常重要**：未初始化的 padding 字节在不同进程会不一致，导致接收端反序列化的字节比较看起来"不一样"。一个普遍的工程实践是：所有 `kStandardType` 结构在使用前必须 `T t{}` 或 `std::memset(&t, 0, sizeof(t))`。

## 运行

```bash
./build/output/bin/example_basic_types
```

预期输出（节选）：

```
--- Bytes (kBytesType) ---
[Bytes] #1 size=18 text=Hello VLink Bytes!
[Bytes] #2 size=5 text=Hello
[Bytes] #3 size=8 text=...
[Bytes] #4 size=0 text=
[Bytes] base64=Vkxpbms= decoded=VLink crc32=2716623793
--- std::string (kStringType) ---
[String] #1 len=13 text="Hello, VLink!"
[String] #2 len=15 text="UTF-8 supported"
[String] #3 len=0 text=""
[String] #4 len=200 text="AAAA..."
[String] #5 len=27 text="{"key":"value","count":42}"
[String] round-trip match=1
--- POD struct (kStandardType) ---
[POD] #1 id=42 temp=23.5 humidity=65.2 status=1
[POD] sizeof(SensorReading)=32 serialized_size=32
=== Basic types demo complete ===
```

`sizeof(SensorReading)` 等于 32：4(sensor_id) + 4(padding) + 8(temperature) + 8(humidity) + 8(timestamp_us) + 1(status) + 7(reserved) - 注意编译器为 double 8 字节对齐插入了一个 4 字节 padding 在 sensor_id 后。

URL 用 `dds://`；切到 `intra://example/basic/...` 在无 DDS 环境也能跑。

## 常见陷阱

1. **POD 结构未零初始化**：`SensorReading r;` 与 `SensorReading r{};` 在不同进程间会因 padding 不同导致字节比较失败。永远写 `T t{}`。
2. **POD 结构含指针/容器**：`std::string`、`std::vector`、`std::unique_ptr` 会让 `is_trivial_v<T> == false`，不能走 `kStandardType`；这种类型需要 `operator>>` / `operator<<`（见 `custom_type/`）。
3. **`Bytes` 跨进程传输**：如果传输后端是 `intra://`，订阅者收到的 Bytes 可能和发布者**共享底层缓冲**（zerocopy），改一份会影响另一份。要安全要么改用 `Bytes::deep_copy` 要么走非 intra 后端。
4. **空 string / 空 Bytes 是合法消息**：传输层不会过滤；接收端 listen 回调会被触发，`msg.size() == 0`。
5. **`get_serialized_size()` 对 string 包含长度前缀**：`std::string("abc")` 的 serialized_size 不是 3，而是 4(长度) + 3(payload) = 7。

## 设计要点

- `kStandardType` 的"零开销 memcpy"前提是收发两端 ABI 相同（同一台机器同一个编译器版本）；跨平台或多语言通信请改用 Protobuf / FlatBuffers / CDR。
- `kStringType` 的长度前缀是 4 字节，因此单条字符串最大 ~4GB；超过这个限制要分片。
- `Bytes` 内部维护引用计数 + 可选所有权；`Bytes::shallow_copy` 是 zerocopy 的关键。详见 `base/bytes_basic/` 与 `base/bytes_zerocopy/`。

## 配图

![Serialization dispatch chain](./images/serialization-type-chain.png)

图中展示 `Serializer::get_type_of<T>()` 的编译期 if-constexpr 链：从最具体的类型（Bytes、string、Proto Message 等）逐级回落到最通用的 `kStandardType` 兜底。

## 参考

- `../custom_type/` — 自定义 `operator>>` / `operator<<` 接入 `kCustomType`
- `../dynamic_data/` — 同一话题携带多种类型的 `DynamicData`
- `../intra_data/` — `intra://` 进程内零拷贝消息
- `base/bytes_basic/`、`base/bytes_advanced/` — `vlink::Bytes` 完整 API
- `vlink/include/vlink/serializer.h` — Serializer 完整接口
- 顶层 `doc/06-serialization.md` — 序列化机制全景
