# custom_type — 通过 `operator>>` / `operator<<` 接入自定义序列化

本示例演示 vlink 的"自定义序列化"路径 —— 用户自己实现 `void operator>>(vlink::Bytes& out) const` 和 `void operator<<(const vlink::Bytes& in)`，vlink 在编译期识别这两个 operator 后，把该类型归类为 `kCustomType`（优先级高于 `kStandardType` 兜底）。

读完本示例你能掌握：

- 自定义类型如何通过 `operator>>` / `operator<<` 接入 vlink 序列化派发。
- 固定长度 vs 变长 payload 的两种典型编码格式。
- 怎么用 `Serializer::serialize` / `deserialize` 在单元测试里做 round-trip 校验。

## 背景与适用场景

`kStandardType` 适合简单 POD 结构，但有几种情况无法用它：

1. **结构里包含 `std::string`、`std::vector`、`std::map` 等非 trivial 类型** —— `is_trivial_v<T>` 为 false，编译期就走不到 `kStandardType` 分支。
2. **需要紧凑编码**：直接 memcpy POD 会保留 padding 字节（如 `int` 后接 `double` 之间的 4 字节空洞），跨平台/跨语言传输浪费带宽。
3. **需要版本兼容**：自己控制编码格式，加字段、加 magic、加 version 都可行。
4. **需要按字段做加密 / 压缩 / 摘要**：在 `operator>>` 里串入业务逻辑。

`kCustomType` 给了应用层完全控制权 —— vlink 只负责传 `Bytes`，编解码由你决定。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `T::operator>>` | `void operator>>(vlink::Bytes& out) const` | 把 `*this` 写到 `out`；约定先 `out = Bytes::create(...)`，再填入数据 |
| `T::operator<<` | `void operator<<(const vlink::Bytes& in)` | 从 `in` 还原 `*this`；失败时把字段留默认值即可，不要抛异常 |
| `vlink::Serializer::get_type_of<T>` | `constexpr Type` | 实现了 operator>>/<< 的类型会被识别为 `kCustomType` |
| `vlink::Serializer::serialize<T>` | `void serialize(const T& msg, Bytes& out)` | 内部调 `msg.operator>>(out)` |
| `vlink::Serializer::deserialize<T>` | `void deserialize(const Bytes& in, T& out)` | 内部调 `out.operator<<(in)` |

## 代码导读

### 1. 固定长度类型 Vec3

```cpp
struct Vec3 {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;

  void operator>>(vlink::Bytes& out) const {
    out = vlink::Bytes::create(sizeof(float) * 3);
    std::memcpy(out.data(), &x, sizeof(float));
    std::memcpy(out.data() + sizeof(float), &y, sizeof(float));
    std::memcpy(out.data() + sizeof(float) * 2, &z, sizeof(float));
  }

  void operator<<(const vlink::Bytes& in) {
    if (in.size() >= sizeof(float) * 3) {
      std::memcpy(&x, in.data(), sizeof(float));
      std::memcpy(&y, in.data() + sizeof(float), sizeof(float));
      std::memcpy(&z, in.data() + sizeof(float) * 2, sizeof(float));
    }
  }
};

static_assert(vlink::Serializer::get_type_of<Vec3>() == vlink::Serializer::kCustomType);
```

注意：

- 编码大小是 `12` 字节，没有 padding，比 `kStandardType` 的 memcpy(sizeof(Vec3))` 在跨平台/跨语言上更稳。
- `operator<<` 里只在 `in.size() >= 12` 时才解码；否则保持成员默认值。**不抛异常**是 vlink 的约定。
- `static_assert` 在编译期就让你确认 vlink 选了 `kCustomType`。

### 2. 变长类型 NamedValue

```cpp
struct NamedValue {
  std::string name;
  int32_t code = 0;
  double value = 0.0;

  void operator>>(vlink::Bytes& out) const {
    uint32_t name_len = static_cast<uint32_t>(name.size());
    size_t total = sizeof(uint32_t) + name_len + sizeof(int32_t) + sizeof(double);
    out = vlink::Bytes::create(total);

    uint8_t* ptr = out.data();
    std::memcpy(ptr, &name_len, sizeof(uint32_t));
    ptr += sizeof(uint32_t);

    if (name_len > 0) {
      std::memcpy(ptr, name.data(), name_len);
      ptr += name_len;
    }

    std::memcpy(ptr, &code, sizeof(int32_t));
    ptr += sizeof(int32_t);
    std::memcpy(ptr, &value, sizeof(double));
  }

  void operator<<(const vlink::Bytes& in) {
    if (in.size() < sizeof(uint32_t)) return;

    const uint8_t* ptr = in.data();
    uint32_t name_len = 0;
    std::memcpy(&name_len, ptr, sizeof(uint32_t));
    ptr += sizeof(uint32_t);

    if (in.size() < sizeof(uint32_t) + name_len + sizeof(int32_t) + sizeof(double)) return;

    name.assign(reinterpret_cast<const char*>(ptr), name_len);
    ptr += name_len;
    std::memcpy(&code, ptr, sizeof(int32_t));
    ptr += sizeof(int32_t);
    std::memcpy(&value, ptr, sizeof(double));
  }
};
```

编码格式：`[u32 name_len][name bytes][i32 code][f64 value]`。这是 vlink 内部、Protobuf 等都用的"长度前缀 + payload + 后续字段"格式。

`operator<<` 里两次做边界检查：先确认能读出 `name_len`，再确认整个消息够长。生产代码可以更细致：拒绝过长的 `name_len`（防止恶意消息 DOS）、检查内嵌字段范围等。

### 3. 通过 Publisher/Subscriber 传输

```cpp
vlink::Subscriber<Vec3> vec3_sub("dds://example/custom/vec3");
vec3_sub.attach(&loop);
vec3_sub.listen([&vec3_count](const Vec3& v) {
  vec3_count++;
  VLOG_I("[Vec3] #", vec3_count, " x=", v.x, " y=", v.y, " z=", v.z);
});

vlink::Publisher<Vec3> vec3_pub("dds://example/custom/vec3");
vec3_pub.attach(&loop);

// ... 同样为 NamedValue ...

vlink::Timer timer(&loop, 50, 1);
timer.start([&]() {
  vec3_pub.publish(Vec3{1.0F, 2.0F, 3.0F});
  vec3_pub.publish(Vec3{-0.5F, 100.0F, -999.0F});

  NamedValue nv1;
  nv1.name = "temperature";
  nv1.code = 100;
  nv1.value = 36.6;
  nv_pub.publish(nv1);

  // ... 测试空串、超长串 ...
});
```

业务代码完全不用关心 `kCustomType` —— 像普通模板参数一样使用 `Publisher<Vec3>`、`Subscriber<NamedValue>`。

### 4. 单元测试式 round-trip

```cpp
NamedValue original;
original.name = "test_field";
original.code = 777;
original.value = 2.71828;

vlink::Bytes buf;
vlink::Serializer::serialize(original, buf);

NamedValue restored;
vlink::Serializer::deserialize(buf, restored);
VLOG_I("[RoundTrip] name_match=", original.name == restored.name,
       " code_match=", original.code == restored.code, " value_match=", original.value == restored.value);
```

`Serializer::serialize` / `deserialize` 是单元测试里验证自定义 operator 是否对称的标准做法。

## 运行

```bash
./build/output/bin/example_custom_type
```

预期输出（节选）：

```
[Vec3] #1 x=1 y=2 z=3
[Vec3] #2 x=-0.5 y=100 z=-999
[NamedValue] #1 name="temperature" code=100 value=36.6
[NamedValue] #2 name="" code=-1 value=0
[NamedValue] #3 name="a_very_long_sensor_name..." code=42 value=3.14159
[RoundTrip] name_match=1 code_match=1 value_match=1
[Summary] Vec3=2 NamedValue=3
```

URL 用 `dds://`；切到 `intra://` 也能跑。

## 常见陷阱

1. **operator>> 不分配 Bytes**：必须 `out = vlink::Bytes::create(total);` 再写；否则 `out.data()` 指向空缓冲。
2. **operator<< 抛异常**：vlink 默认假定反序列化失败时把字段保持默认值；抛异常会让 Subscriber 回调线程崩溃。
3. **operator>> 与 operator<< 不对称**：编码写了 12 字节但解码只读 8 字节 —— 不会立刻报错，但接收端会缺字段。`Serializer::serialize` + `deserialize` round-trip 测试可以提前发现。
4. **变长字段没做边界检查**：恶意消息可以让 `name_len` 巨大，分配大块内存做 DOS。生产代码要限制字段最大长度。
5. **operator>> 不是 const**：vlink 的 trait 检查需要 const operator>>。

## 设计要点

- 自定义类型的优先级高于 POD：即便 `is_trivial_v<T>` 为 true，只要有 operator>>/<<，vlink 就选 `kCustomType`。
- `Bytes::create(n)` 不会清零；`operator>>` 里必须把所有字节都写一遍，否则接收端解到的字节带 indeterminate 值。
- 跨语言通信时 operator>> 的编码格式要和对方约定（推荐用 LSB-first 整数 + 显式 fixed-width 类型）；本示例的 native endian memcpy 仅适用于同 ABI 主机。
- 想要更强的自描述能力（schema 验证、版本演进、跨语言）建议用 Protobuf / FlatBuffers / CDR，见 `samples/helloworld/` 等。

## 配图

无专属配图。整体序列化派发链见 `../basic_types/images/serialization-type-chain.png`。

## 参考

- `../basic_types/` — Bytes / string / POD 三种基础路径
- `../dynamic_data/` — 单话题携带多种类型
- `../intra_data/` — 进程内零拷贝
- `samples/helloworld/` — Protobuf 序列化
- `samples/pub_sub_fbs/` — FlatBuffers 序列化
- `vlink/include/vlink/serializer.h` — Serializer 接口
- 顶层 `doc/06-serialization.md` — 序列化机制全景
