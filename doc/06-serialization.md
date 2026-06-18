# 6. 序列化

## 目录

- [6.1 概述](#61-概述)
- [6.2 自动类型推断机制](#62-自动类型推断机制)
- [6.3 所有序列化类型总览](#63-所有序列化类型总览)
- [6.4 各类型详细说明](#64-各类型详细说明)
  - [6.4.1 kBytesType — 原始字节直传](#641-kbytestype--原始字节直传)
  - [6.4.2 kProtoType — Protobuf 值类型](#642-kprototype--protobuf-值类型)
  - [6.4.3 kProtoPtrType — Protobuf 指针类型](#643-kprotoptrtype--protobuf-指针类型)
  - [6.4.4 kFlatTableType — FlatBuffers NativeTable](#644-kflattabletype--flatbuffers-nativetable)
  - [6.4.5 kFlatPtrType — FlatBuffers 表指针（零拷贝读）](#645-kflatptrtype--flatbuffers-表指针零拷贝读)
  - [6.4.6 kFlatBuilderType — FlatBuffers Builder](#646-kflatbuildertype--flatbuffers-builder)
  - [6.4.7 kCdrType — FastDDS CDR 编码](#647-kcdrtype--fastdds-cdr-编码)
  - [6.4.8 kStandardType — POD 值类型](#648-kstandardtype--pod-值类型)
  - [6.4.9 kStandardPtrType — POD 指针类型（零拷贝）](#649-kstandardptrtype--pod-指针类型零拷贝)
  - [6.4.10 kStringType — std::string](#6410-kstringtype--stdstring)
  - [6.4.11 kCharsType — C 字符串](#6411-kcharstype--c-字符串)
  - [6.4.12 kStreamType — 流序列化类型](#6412-kstreamtype--流序列化类型)
  - [6.4.13 kCustomType — 自定义序列化器](#6413-kcustomtype--自定义序列化器)
  - [6.4.14 kDynamicType — 动态类型](#6414-kdynamictype--动态类型)
- [6.5 自定义序列化器实现](#65-自定义序列化器实现)
- [6.6 Bytes 类详细介绍](#66-bytes-类详细介绍)
- [6.7 Protobuf 集成](#67-protobuf-集成)
- [6.8 FlatBuffers 集成](#68-flatbuffers-集成)
- [6.9 性能对比](#69-性能对比)
- [6.10 常见错误和避坑指南](#610-常见错误和避坑指南)

---

## 6.1 概述

VLink 序列化系统基于 **编译期类型推断**，通过 `constexpr if` 链在编译时确定每种消息类型应使用的编解码器。
应用层代码几乎不需要直接调用序列化 API，框架在 `publish()`、`listen()`、`invoke()`、`set()`、`get()` 等接口内部自动完成序列化和反序列化。

![序列化推断流程](images/serialization-flow.png)

### 6.1.1 类型映射总览

![序列化类型映射](images/serialization-types-map.png)

**关键设计原则：**

- 所有类型检测和分派均在编译期完成，无虚函数、无运行时类型判断。
- 不支持的类型（`kUnknownType`）在构造 Publisher/Subscriber/Setter/Getter/Client/Server 时触发 `static_assert` 编译错误，第一时间暴露问题。
- 同一套 API（`publish()`、`set()` 等）对所有序列化类型透明，切换序列化方式只需更换消息类型，无需修改通信代码。

**相关文档：**

- 构建与编译配置请参阅 [构建指南](01-build.md)
- Event / Method / Field 通信模型请参阅 [Event 模型](03-event-model.md)
- Bytes 等基础组件请参阅 [基础库](11-base-library.md)
- 传输后端选择请参阅 [传输后端](07-transport.md)

---

## 6.2 自动类型推断机制

`Serializer::get_type_of<T>()` 通过以下优先级链依次检测：

```
T == Bytes                          -> kBytesType
T 有 is_vlink_dynamic_data()        -> kDynamicType
T 有 serialize(Cdr&)/deserialize(Cdr&) -> kCdrType
T 有 SerializeToArray()/ParseFromArray() -> kProtoType
T* 指向有上述方法的消息             -> kProtoPtrType
T 继承 flatbuffers::NativeTable     -> kFlatTableType
T* 指向 flatbuffers::Table          -> kFlatPtrType
T 有 fbb_ 成员 + Finish()           -> kFlatBuilderType
T 有 operator>>(Bytes&)/operator<<(const Bytes&) -> kCustomType
T == std::string                    -> kStringType
T 可由 std::string 构造（非 string）-> kCharsType
T 是 trivial + standard_layout      -> kStandardType
T* 指向 trivial + standard_layout   -> kStandardPtrType
T 支持 std::stringstream << 和 >>（双向流） -> kStreamType
否则                                -> kUnknownType（编译报错）
```

**示例：编译期类型查询**

```cpp
#include <vlink/serializer.h>

constexpr auto t = vlink::Serializer::get_type_of<int>();
static_assert(t == vlink::Serializer::kStandardType, "int is standard-serialisable");

constexpr auto ts = vlink::Serializer::get_type_of<std::string>();
static_assert(ts == vlink::Serializer::kStringType, "");

static_assert(vlink::Serializer::is_supported(t), "");
```

### 6.2.1 Serializer 核心函数

| 函数                                       | 说明                                                                     |
| ------------------------------------------ | ------------------------------------------------------------------------ |
| `Serializer::get_type_of<T>()`             | 编译期返回 `T` 对应的 `Serializer::Type` 枚举值                          |
| `Serializer::is_supported(Type)`           | 判断该类型枚举是否受支持（仅 `kUnknownType` 返回 `false`）               |
| `Serializer::serialize(src, des)`          | 将 `src` 序列化到 `Bytes des`，返回 `bool`                               |
| `Serializer::deserialize(src, des)`        | 将 `Bytes src` 反序列化到 `des`，返回 `bool`                             |
| `Serializer::convert<SrcT, DesT>(src, des)` | 在两种类型之间转换（至少一端必须是 `Bytes`），返回 `bool`                |
| `Serializer::get_serialized_type<T>()`     | 返回 `T` 的序列化类型名称字符串（如 Protobuf fully-qualified name）       |
| `Serializer::get_serialized_size(src)`     | 返回可预估类型的序列化字节大小；大小不可预知或当前未实现预估的类型返回 `0`（包括 `kStandardType` / `kStandardPtrType`） |

---

## 6.3 所有序列化类型总览

| 类型常量            | 触发条件                                                    | 说明                           | 依赖库          |
| ------------------- | ----------------------------------------------------------- | ------------------------------ | --------------- |
| `kBytesType`        | `T == Bytes`                                                | 原始字节直传，零拷贝           | 无              |
| `kDynamicType`      | 有 `is_vlink_dynamic_data()` 成员                           | 动态类型，运行时字段定义       | 无              |
| `kCdrType`          | 有 `serialize(Cdr&)` 和 `deserialize(Cdr&)`                 | FastDDS CDR 编码，RTPS 标准    | eProsima FastCDR |
| `kProtoType`        | 有 `SerializeToArray()` 和 `ParseFromArray()`               | Protobuf 值类型                | protobuf        |
| `kProtoPtrType`     | 指向有上述方法的消息的指针                                  | Protobuf 指针（Arena 管理）    | protobuf        |
| `kFlatTableType`    | 继承 `flatbuffers::NativeTable`                             | FlatBuffers Object API         | flatbuffers     |
| `kFlatPtrType`      | 指向 `flatbuffers::Table` 子类的指针                        | FlatBuffers 零拷贝读           | flatbuffers     |
| `kFlatBuilderType`  | 有 `fbb_` 成员 + `Finish()`                                 | FlatBuffers Builder 模式       | flatbuffers     |
| `kCustomType`       | 有 `operator>>(Bytes&)` 和 `operator<<(const Bytes&)`       | 用户自定义编解码器             | 无              |
| `kStringType`       | `T == std::string`                                          | UTF-8 字符串                   | 无              |
| `kCharsType`        | 可由 `std::string` 构造（但不是 `std::string`）             | C 字符串字面量 / `char*`       | 无              |
| `kStandardType`     | `std::is_trivial_v && std::is_standard_layout_v`（非指针） | POD 结构体，直接内存拷贝       | 无              |
| `kStandardPtrType`  | 指向 trivial + standard_layout 类型的指针                   | POD 指针，零拷贝               | 无              |
| `kStreamType`       | 支持 `std::stringstream` 的 `<<` 和 `>>`（双向流）          | 流式文本编码                   | 无              |

> `Serializer::Type`（`include/vlink/serializer.h` 中 `enum Type : uint8_t`）共 14 个有效枚举（不含 `kUnknownType = 0`）。`kCustomType` 的枚举值为 `3`，但在 `get_type_of<T>()`（`include/vlink/internal/serializer-inl.h` 中的 `if constexpr` 链）里优先级低于 CDR / Protobuf / FlatBuffers —— 枚举值顺序不等于检测顺序。

---

## 6.4 各类型详细说明

### 6.4.1 kBytesType — 原始字节直传

**触发条件：** `T == vlink::Bytes`

`Bytes` 是 VLink 的原生字节容器。发布 `Bytes` 类型消息时，序列化层直接传递字节缓冲区，无任何额外编解码开销。这是最底层、最灵活的通信方式，适合自定义协议或透明代理场景。

```cpp
#include <vlink/vlink.h>

vlink::Publisher<vlink::Bytes> pub("shm://raw/channel");
auto buf = vlink::Bytes::create(256);
std::memcpy(buf.data(), payload, 256);
pub.publish(buf);

vlink::Subscriber<vlink::Bytes> sub("shm://raw/channel");
sub.listen([](const vlink::Bytes& msg) {
    process_raw(msg.data(), msg.size());
});
```

---

### 6.4.2 kProtoType — Protobuf 值类型

**触发条件：** `T` 具有 `SerializeToArray()` 和 `ParseFromArray()` 方法（通常继承自 `google::protobuf::MessageLite`）

Protobuf 是最常用的序列化方式，适合跨语言、跨平台的消息定义。VLink 使用 Protobuf 的二进制格式，通过 `SerializeToArray()` / `ParseFromArray()` 进行编解码。

```cpp
#include <vlink/vlink.h>
#include "my_message.pb.h"

vlink::Publisher<MyProtoMsg> pub("dds://my/topic");

MyProtoMsg msg;
msg.set_id(42);
msg.set_name("hello");
pub.publish(msg);

vlink::Subscriber<MyProtoMsg> sub("dds://my/topic");
sub.listen([](const MyProtoMsg& m) {
    std::cout << "id=" << m.id() << " name=" << m.name() << std::endl;
});
```

---

### 6.4.3 kProtoPtrType — Protobuf 指针类型

**触发条件：** `T` 是指向 Protobuf 消息的原始指针（如 `MyProtoMsg*`）

适合 Arena 分配模式，避免频繁的 new/delete 开销。VLink 通过指针取值后再序列化。

```cpp
google::protobuf::Arena arena;
MyProtoMsg* msg = google::protobuf::Arena::CreateMessage<MyProtoMsg>(&arena);
msg->set_id(1);

vlink::Publisher<MyProtoMsg*> pub("dds://arena/topic");
pub.publish(msg);
```

---

### 6.4.4 kFlatTableType — FlatBuffers NativeTable

**触发条件：** `T`（或 `shared_ptr<T>` 中的 T）继承 `flatbuffers::NativeTable`

FlatBuffers Object API 生成的 `*T` 类（Native Table）。序列化时调用 `Pack()` 将其打包到 `FlatBufferBuilder`，反序列化时调用 `UnPack()`。

```cpp
#include <vlink/vlink.h>
#include "my_message_generated.h"

vlink::Publisher<MyMessageT> pub("shm://flat/topic");

MyMessageT msg;
msg.value = 42;
msg.name = "hello";
pub.publish(msg);

vlink::Subscriber<MyMessageT> sub("shm://flat/topic");
sub.listen([](const MyMessageT& m) {
    std::cout << m.value << " " << m.name << std::endl;
});
```

---

### 6.4.5 kFlatPtrType — FlatBuffers 表指针（零拷贝读）

**触发条件：** `T` 是指向 `flatbuffers::Table` 子类的指针（如 `const MyMessage*`）

零拷贝读取 FlatBuffers 原始缓冲区，无需反序列化到中间对象，访问速度极快。指针直接指向接收缓冲区内的 FlatBuffers 表，生命期与底层 `Bytes` 一致。

```cpp
vlink::Subscriber<const MyMessage*> sub("shm://flat/topic");
sub.listen([](const MyMessage* m) {
    if (m) {
        std::cout << m->value() << std::endl;
    }
});

vlink::Publisher<MyMessageT> pub("shm://flat/topic");
```

---

### 6.4.6 kFlatBuilderType — FlatBuffers Builder

**触发条件：** `T` 有 `fbb_` 成员（`FlatBufferBuilder`）且有 `Finish()` 方法

VLink 检测到消息对象持有 `FlatBufferBuilder` 时，序列化路径会调用 `fbb_.Finish(src.Finish())`——即先调用用户的 `Finish()` 方法获取根偏移量（`flatbuffers::Offset<T>`），再由框架完成 FlatBuffers 的 `Finish()` 并取出缓冲区。

```cpp
struct MyFlatMsg {
    flatbuffers::FlatBufferBuilder fbb_;
    flatbuffers::Offset<MyMessage> root_;

    void Build(int val) {
        root_ = CreateMyMessage(fbb_, val);
    }

    flatbuffers::Offset<MyMessage> Finish() {
        return root_;
    }
};

vlink::Publisher<MyFlatMsg> pub("dds://flat/builder");

MyFlatMsg msg;
msg.Build(99);
pub.publish(msg);
```

> **注意：** VLink 的序列化路径内部会调用 `src.fbb_.Finish(src.Finish())`，即先调用用户的 `Finish()` 获取根偏移量，再由框架调用 `fbb_.Finish(offset)` 完成 FlatBuffers 构建。因此用户的 `Finish()` 方法**必须返回根偏移量**（`flatbuffers::Offset<T>`），而不是自己调用 `fbb_.Finish()`。

---

### 6.4.7 kCdrType — FastDDS CDR 编码

**触发条件：** `T` 有 `serialize(eprosima::fastcdr::Cdr&)` 和 `deserialize(eprosima::fastcdr::Cdr&)` 方法，或类名包含 `VLINK_FASTDDS_IDL_PREFIX` 前缀

CDR（Common Data Representation）是 DDS 标准的二进制格式，由 FastCDR 库实现。通常用于从 IDL 文件自动生成的消息类型。CDR 类型在 `dds://` 传输中有特殊快速路径优化（指针直传，无额外字节拷贝）。

```cpp
#include "MyMessage.h"
#include "MyMessagePubSubTypes.h"

vlink::DdsConf::register_topic<MyMessagePubSubType>("my_topic");

vlink::Publisher<MyMessage> pub("dds://my_topic");
MyMessage msg;
msg.value(42);
pub.publish(msg);
```

---

### 6.4.8 kStandardType — POD 值类型

**触发条件：** `std::is_trivial_v<T> && std::is_standard_layout_v<T>`，且 T 不是指针

对于简单的 C 风格结构体（Plain Old Data），VLink 直接进行 `sizeof(T)` 字节的内存拷贝，无任何编解码开销，是**速度最快**的序列化方式（除零拷贝外）。

```cpp
struct SensorData {
    float temperature;
    float humidity;
    uint64_t timestamp_us;
};

static_assert(std::is_trivial_v<SensorData>);
static_assert(std::is_standard_layout_v<SensorData>);

vlink::Publisher<SensorData> pub("shm://sensor/imu");

SensorData data{25.6f, 60.2f, get_timestamp()};
pub.publish(data);

vlink::Subscriber<SensorData> sub("shm://sensor/imu");
sub.listen([](const SensorData& d) {
    process(d.temperature, d.humidity);
});
```

**注意：** POD 类型不包含任何版本信息，结构体字段顺序、大小必须在发布端和订阅端完全一致，否则数据解析错误。不适合跨机器/跨架构的不同字节序场景。

---

### 6.4.9 kStandardPtrType — POD 指针类型（零拷贝）

**触发条件：** `T` 是指向 trivial + standard_layout 类型的指针

零拷贝 POD，指针被重解释后直接传递，无内存拷贝，适用于大型 POD 结构体（如相机帧、点云）的进程内或共享内存通信。

```cpp
struct LargeFrame {
    uint8_t pixels[1920 * 1080 * 3];
    uint64_t timestamp;
};

vlink::Publisher<LargeFrame*> pub("shm://camera/frame");
LargeFrame* frame = get_shm_buffer();
pub.publish(frame);

vlink::Subscriber<LargeFrame*> sub("shm://camera/frame");
sub.listen([](const LargeFrame* f) {
    if (f) {
        display_frame(f->pixels);
    }
});
```

---

### 6.4.10 kStringType — std::string

**触发条件：** `T == std::string`

UTF-8 字符串，内容直接复制到 `Bytes` 缓冲区，反序列化时从缓冲区重建 `std::string`。

```cpp
vlink::Publisher<std::string> pub("dds://log/messages");
pub.publish("System started successfully");

vlink::Subscriber<std::string> sub("dds://log/messages");
sub.listen([](const std::string& msg) {
    std::cout << msg << std::endl;
});
```

---

### 6.4.11 kCharsType — C 字符串

**触发条件：** 可由 `std::string` 构造（但不是 `std::string`），如 `const char*`

C 字符串字面量或 `char*` 类型。发布时转为 `std::string` 再序列化，接收端反序列化为 `std::string`。

```cpp
vlink::Publisher<const char*> pub("intra://log/raw");
pub.publish("hello from C string");

vlink::Subscriber<std::string> sub("intra://log/raw");
```

---

### 6.4.12 kStreamType — 流序列化类型

**触发条件：** `T` 支持 `std::stringstream` 的 `operator<<` 和 `operator>>`（双向），不是指针类型，且 **既不满足 `kStandardType` 也不满足更高优先级类型** 的检测条件。

在类型推断链中（参见 `include/vlink/internal/serializer-inl.h` 中 `get_type_of<T>()` 的 `if constexpr` 链），`kStandardType` / `kStandardPtrType` 会在 `kStreamType` **之前**检测。因此：

- **trivial + standard_layout 的算术类型**（`int`、`double` 等）——即使也能通过 `stringstream << / >>`——仍然会被优先推断为 `kStandardType`，走 memcpy 二进制路径。
- 只有**非 trivial 或非 standard_layout**（例如带 `std::string` 成员、带虚函数、带非 POD 成员），但额外实现了 stringstream 双向流的类型，才会落入 `kStreamType`（文本编码）。

```cpp
static_assert(vlink::Serializer::get_type_of<int>() == vlink::Serializer::kStandardType);

struct MyPod {
    float x, y, z;
};
static_assert(vlink::Serializer::get_type_of<MyPod>() == vlink::Serializer::kStandardType);

struct MyStreamMsg {
    std::string name;
    int x;
    friend std::ostream& operator<<(std::ostream& os, const MyStreamMsg& m) {
        return os << m.name << ' ' << m.x;
    }
    friend std::istream& operator>>(std::istream& is, MyStreamMsg& m) {
        return is >> m.name >> m.x;
    }
};
static_assert(vlink::Serializer::get_type_of<MyStreamMsg>() == vlink::Serializer::kStreamType);
```

---

### 6.4.13 kCustomType — 自定义序列化器

**触发条件：** `T` 有 `operator>>(vlink::Bytes&)` 和 `operator<<(const vlink::Bytes&)` 方法

用户完全控制序列化逻辑，适合：
- 外部系统已有的私有二进制协议
- 需要特殊压缩或加密处理的场景
- 不依赖任何第三方序列化库的场景

详见 [6.5 自定义序列化器实现](#65-自定义序列化器实现) 章节。

---

### 6.4.14 kDynamicType — 动态类型

**触发条件：** `T` 有 `is_vlink_dynamic_data()` 成员函数

动态类型允许在运行时定义字段结构，无需在编译期固定消息格式。适合调试工具、监控系统、协议桥接等场景。通常通过 VLink 的 `DynamicData` 类使用。

```cpp
#include <vlink/extension/dynamic_data.h>

vlink::Publisher<vlink::DynamicData> pub("dds://dynamic/topic");

vlink::DynamicData msg;
msg["speed"] = 80.0f;
msg["gear"] = 3;
pub.publish(msg);
```

---

## 6.5 自定义序列化器实现

实现自定义序列化器只需在类型上重载两个运算符：

```cpp
void operator>>(vlink::Bytes& out) const;

void operator<<(const vlink::Bytes& in);
```

### 6.5.1 完整示例

```cpp
#include <vlink/vlink.h>
#include <cstring>

struct MyCustomProtocol {
    uint32_t magic{0xDEADBEEF};
    uint16_t cmd{0};
    std::vector<uint8_t> payload;

    void operator>>(vlink::Bytes& out) const {
        size_t total = sizeof(magic) + sizeof(cmd) + sizeof(uint32_t) + payload.size();
        out = vlink::Bytes::create(total);

        uint8_t* ptr = out.data();
        std::memcpy(ptr, &magic, sizeof(magic));  ptr += sizeof(magic);
        std::memcpy(ptr, &cmd,   sizeof(cmd));    ptr += sizeof(cmd);

        uint32_t payload_size = static_cast<uint32_t>(payload.size());
        std::memcpy(ptr, &payload_size, sizeof(payload_size));  ptr += sizeof(payload_size);
        std::memcpy(ptr, payload.data(), payload.size());
    }

    void operator<<(const vlink::Bytes& in) {
        const uint8_t* ptr = in.data();
        std::memcpy(&magic, ptr, sizeof(magic));  ptr += sizeof(magic);
        std::memcpy(&cmd,   ptr, sizeof(cmd));    ptr += sizeof(cmd);

        uint32_t payload_size = 0;
        std::memcpy(&payload_size, ptr, sizeof(payload_size));  ptr += sizeof(payload_size);
        payload.assign(ptr, ptr + payload_size);
    }
};

static_assert(vlink::Serializer::get_type_of<MyCustomProtocol>() == vlink::Serializer::kCustomType);

vlink::Publisher<MyCustomProtocol> pub("dds://custom/channel");
MyCustomProtocol msg;
msg.cmd = 0x01;
msg.payload = {0xAA, 0xBB, 0xCC};
pub.publish(msg);

vlink::Subscriber<MyCustomProtocol> sub("dds://custom/channel");
sub.listen([](const MyCustomProtocol& m) {
    std::cout << "cmd=0x" << std::hex << m.cmd << std::endl;
});
```

### 6.5.2 注意事项

- `operator>>` 中必须确保 `out` 有足够大小，使用 `Bytes::create(size)` 分配。
- `operator<<` 不应假设 `in.size()` 固定，要做合法性检查。
- 两端的序列化/反序列化逻辑必须字节对齐，注意大小端问题。
- 若消息大小可变，在 `operator>>` 中动态计算所需字节数后再分配。

---

## 6.6 Bytes 类详细介绍

`vlink::Bytes` 是 VLink 序列化系统的底层数据载体，所有序列化后的消息都以 `Bytes` 形式在传输层流动。

### 6.6.1 设计特点

- **固定对象大小**：对象自身始终为 128 字节（96 字节内联栈缓冲 + 元数据）。
- **小缓冲优化（SBO）**：不超过 96 字节的数据直接存储在对象内，零堆分配。
- **内存池支持**：堆分配走 `vlink::MemoryPool`（分级 free-list 池），按 size class 分发，减少堆分配开销。
- **五种所有权模式**：

| 工厂方法                        | 是否拥有内存 | 拷贝行为 | 典型用途                        |
| ------------------------------- | ------------ | -------- | ------------------------------- |
| `Bytes::create(size)`           | 是           | 深拷贝   | 普通分配                        |
| `Bytes::shallow_copy(ptr, size)` | 否           | 指针别名 | 零拷贝包装外部缓冲区            |
| `Bytes::deep_copy(ptr, size)`   | 是           | 深拷贝   | 安全拷贝外部缓冲区              |
| `Bytes::loan_internal(ptr, size)` | 否（借用） | 指针别名 | Iceoryx 零拷贝 Chunk            |
| `Bytes::shallow_copy_ptr(ptr)`  | 否           | 指针别名 | 携带不透明指针（size == 0）     |

### 6.6.2 核心 API

```cpp
auto buf = vlink::Bytes::create(1024);
auto buf2 = vlink::Bytes::create(64, 4);

uint8_t* p = buf.data();
uint8_t* rp = buf.real_data();
size_t sz = buf.size();
size_t rsz = buf.real_size();
size_t cap = buf.capacity();

bool owned = buf.is_owner();
bool loaned = buf.is_loaned();
bool empty = buf.empty();

buf[0] = 0xFF;
buf.resize(2048);
buf.shrink_to(512);
buf.clear();

auto view = vlink::Bytes::shallow_copy(ext_ptr, ext_size);

auto copy = vlink::Bytes::deep_copy(ext_ptr, ext_size);

auto from_str = vlink::Bytes::from_string("hello");
std::string to_str = buf.to_string();
std::string_view sv = buf.to_string_view();

void* ptr = get_some_ptr();
auto bytes_ptr = vlink::Bytes::shallow_copy_ptr(ptr);
auto* recovered = bytes_ptr.to_ptr<MyStruct>();
```

### 6.6.3 内存池

```cpp
vlink::Bytes::init_memory_pool();

vlink::Bytes::release_memory_pool();
```

`Bytes` 的堆分配统一走 `vlink::MemoryPool`（参见 [11.4 节](11-base-library.md#114-内存池-memorypool)）。

### 6.6.4 工具方法

```cpp
auto compressed = vlink::Bytes::compress_data(buf.data(), buf.size());

if (vlink::Bytes::is_compress_data(compressed.data(), compressed.size())) {
    auto original = vlink::Bytes::uncompress_data(compressed.data(), compressed.size());
}

std::string b64 = vlink::Bytes::encode_to_base64(buf);
auto decoded = vlink::Bytes::decode_from_base64(b64);

uint32_t crc32 = vlink::Bytes::get_crc_32(buf);

uint64_t crc64 = vlink::Bytes::get_crc_64(buf);

auto reversed = vlink::Bytes::reverse_order(buf);

std::string hex = vlink::Bytes::convert_to_hex_str(buf.data(), buf.size());

bool ok = false;
auto parsed = vlink::Bytes::from_user_input("0x01020304", &ok);

bool le = vlink::Bytes::is_little_endian();
bool be = vlink::Bytes::is_big_endian();
```

### 6.6.5 偏移区（Offset）机制

`Bytes` 支持在数据前预留头部空间，传输层可在原地写入协议头，避免重新分配：

```cpp
auto buf = vlink::Bytes::create(payload_size, 8);

buf.data();
buf.size();

buf.real_data();
buf.offset();
```

---

## 6.7 Protobuf 集成

> Protobuf / FlatBuffers 的 CMake 集成配置请参阅 [构建指南](01-build.md) 第 1.6 节。

### 6.7.1 使用示例

```protobuf
syntax = "proto3";
package example;

message VehicleState {
    float speed = 1;
    int32 gear = 2;
    bool engine_on = 3;
    string vin = 4;
}
```

```cpp
#include <vlink/vlink.h>
#include "my_message.pb.h"

vlink::Publisher<example::VehicleState> pub("dds://vehicle/state");
example::VehicleState state;
state.set_speed(80.0f);
state.set_gear(3);
state.set_engine_on(true);
state.set_vin("LVHB1234567890000");
pub.publish(state);

vlink::Subscriber<example::VehicleState> sub("dds://vehicle/state");
sub.listen([](const example::VehicleState& s) {
    std::cout << "speed=" << s.speed() << " gear=" << s.gear() << std::endl;
});
```

### 6.7.2 Protobuf Arena 加速（kProtoPtrType）

```cpp
google::protobuf::ArenaOptions options;
options.initial_block_size = 4096;
google::protobuf::Arena arena(options);

vlink::Publisher<example::VehicleState*> pub("dds://vehicle/state");

example::VehicleState* state = google::protobuf::Arena::CreateMessage<example::VehicleState>(&arena);
state->set_speed(80.0f);
pub.publish(state);
```

---

## 6.8 FlatBuffers 集成

> Protobuf / FlatBuffers 的 CMake 集成配置请参阅 [构建指南](01-build.md) 第 1.6 节。

### 6.8.1 Schema 示例

```flatbuffers
namespace example;

table VehicleState {
    speed: float;
    gear: int;
    engine_on: bool;
    vin: string;
}

root_type VehicleState;
```

### 6.8.2 使用 Object API（kFlatTableType）

```cpp
#include <vlink/vlink.h>
#include "my_message_generated.h"

vlink::Publisher<example::VehicleStateT> pub("shm://vehicle/state");

example::VehicleStateT state;
state.speed = 80.0f;
state.gear = 3;
state.engine_on = true;
state.vin = "LVHB1234567890000";
pub.publish(state);

vlink::Subscriber<example::VehicleStateT> sub("shm://vehicle/state");
sub.listen([](const example::VehicleStateT& s) {
    std::cout << "speed=" << s.speed << std::endl;
});
```

### 6.8.3 使用零拷贝指针（kFlatPtrType）

```cpp
vlink::Publisher<example::VehicleStateT> pub("shm://vehicle/state");

vlink::Subscriber<const example::VehicleState*> sub("shm://vehicle/state");
sub.listen([](const example::VehicleState* s) {
    if (s) {
        std::cout << "speed=" << s->speed() << std::endl;
    }
});
```

---

## 6.9 性能对比

以下为各序列化方式在不同维度的对比（相对性能，具体数值依消息大小和硬件而定）：

| 序列化类型          | 编解码速度 | 消息大小效率 | 零拷贝 | 跨语言支持 | 版本兼容性 | 推荐场景                        |
| ------------------- | ---------- | ------------ | ------ | ---------- | ---------- | ------------------------------- |
| `kStandardType`     | 极快       | 极小         | 否     | 否         | 无         | 同架构同结构体 POD，高频数据    |
| `kStandardPtrType`  | 极快       | 极小         | 是     | 否         | 无         | 大型 POD，shm 零拷贝            |
| `kBytesType`        | 极快       | 取决于内容   | 是     | 是         | 无         | 透明代理，原始帧数据            |
| `kFlatPtrType`      | 极快       | 小           | 是     | 否         | 向前兼容   | 高性能只读 FlatBuffers          |
| `kFlatTableType`    | 快         | 小           | 否     | 是         | 向前兼容   | 高性能读写 FlatBuffers          |
| `kFlatBuilderType`  | 快         | 小           | 否     | 是         | 向前兼容   | 手动构建 FlatBuffers            |
| `kProtoType`        | 中等       | 中等（压缩） | 否     | 是（多语言）| 向前向后兼容 | 跨语言，含可选字段的消息      |
| `kProtoPtrType`     | 中等       | 中等         | 否     | 是         | 向前向后兼容 | Arena 模式，减少 new/delete    |
| `kCustomType`       | 取决于实现 | 取决于实现   | 否     | 否         | 手动维护   | 私有协议，外部系统              |
| `kStringType`       | 快         | 取决于内容   | 否     | 是         | N/A        | 文本日志，命令字符串            |
| `kCdrType`          | 快         | 中等         | 否     | 是（DDS）  | IDL 版本   | DDS 标准互操作，IDL 定义消息   |

**总结建议：**

- 最高性能（进程内/同机）：`kStandardType`（POD）或 `kBytesType` + `shm://`
- 高性能 + 结构化：`kFlatTableType`（FlatBuffers）
- 跨语言/跨版本：`kProtoType`（Protobuf）
- DDS 标准互操作：`kCdrType`（CDR）
- 原始控制/特殊协议：`kCustomType`

---

## 6.10 常见错误和避坑指南

### 6.10.1 编译错误：`<ValueT> is not a supported Serializer type`

**原因：** 消息类型不匹配任何已知序列化规则，`Serializer::get_type_of<T>()` 返回 `kUnknownType`。

```cpp
struct BadMsg {
    int x;
    std::vector<int> data;
};
vlink::Publisher<BadMsg> pub("shm://bad");
```

**解决方案：**

```cpp
struct GoodMsg {
    int x;
    std::vector<int> data;

    void operator>>(vlink::Bytes& out) const { }
    void operator<<(const vlink::Bytes& in)  { }
};
```

### 6.10.2 POD 类型跨架构字节序问题

```cpp
struct Timestamp {
    uint64_t nanoseconds;
};
```

### 6.10.3 FlatBuffers 零拷贝指针生命期

```cpp
const example::VehicleState* captured = nullptr;

vlink::Subscriber<const example::VehicleState*> sub("shm://state");
sub.listen([&captured](const example::VehicleState* s) {
    captured = s;
});

sub.listen([](const example::VehicleState* s) {
    float speed = s->speed();
    auto obj = s->UnPack();
});
```

### 6.10.4 Protobuf 序列化失败时的处理

```cpp
pub.publish(huge_proto_msg);
```

### 6.10.5 kCdrType 必须注册 TypeSupport

```cpp
vlink::DdsConf::register_topic<MyMessagePubSubType>("topic");
vlink::Publisher<MyMessage> pub("dds://topic");
```

### 6.10.6 自定义序列化器中错误的 Bytes 操作

```cpp
void operator>>(vlink::Bytes& out) const {
    out = vlink::Bytes::create(sizeof(x));
    std::memcpy(out.data(), &x, sizeof(x));
}
```

### 6.10.7 Protobuf 和 FlatBuffers 类型在同一 Topic 混用

```cpp
vlink::Publisher<MyProtoMsg> pub("dds://topic");
vlink::Subscriber<MyFlatMsg> sub("dds://topic");
```

---

**相关文档：**

- 零拷贝数据容器（`CameraFrame`、`PointCloud`、`RawData`、`OccupancyGrid`、`Tensor`、`ObjectArray`、`AudioFrame`）请参阅 [零拷贝与数据容器](10-zerocopy.md)
- 传输后端选择与 URL 格式请参阅 [传输后端与 URL](07-transport.md)
- Bytes 的压缩、Base64、CRC 等工具方法请参阅 [基础库](11-base-library.md)
- 安全加密（消息级 AES 加密）请参阅 [安全加密](09-security.md)
