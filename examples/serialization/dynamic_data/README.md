# dynamic_data — 同话题携带多种类型：`vlink::DynamicData`

`vlink::DynamicData` 是 vlink 提供的"类型擦除容器"：把类型名作为字符串嵌入数据中，让**同一个 URL 上的同一对 Publisher/Subscriber 可以承载多种不同类型的消息**。订阅端通过 `get_type()` 判断是什么类型，再用 `as<T>()` / `convert(T&)` 提取出来。

本示例覆盖：

- `DynamicData::load(name, value)` 写入与 `as<T>()` / `convert(T&)` 读取。
- 单话题多类型订阅（Temperature / Pressure / Status）。
- 通过 `operator>>` / `operator<<` 显式做 wire 格式 round-trip。
- 实例相等比较 `operator==` / `operator!=`。

读完本示例你能掌握：

- 何时该用 `DynamicData`，何时不该用。
- DynamicData 的 wire 布局（前 20 字节类型名 + 序列化 payload）。
- 在 Subscriber 回调里如何按 `get_type()` 分发处理。

## 背景与适用场景

`DynamicData` 解决"一个 topic 上想跨多种类型"的工程需求。典型场景：

- 异构事件总线：很多种事件（不同字段），但订阅者都想在同一个 topic 上接收并按类型分发。
- 调试 / 监控 / 录制：想把多种业务消息汇总到一条管道。
- 跨语言 / 跨进程的命令通道：command name 作为字符串嵌入，类型由 name 推断。

什么时候**不该**用：

- 类型已知且固定 —— 用 `Publisher<T>` / `Subscriber<T>` 强类型路径，编译期检查更安全。
- 性能极敏感 —— DynamicData 比强类型多一层类型名拷贝和 string 比较。
- 序列化格式与外部协议绑定（Protobuf / FlatBuffers / IDL）—— 直接走对应路径。

DynamicData 的内部表示：

```
+-------------------+-----------------------+
| 20 bytes type name| serialized payload    |
+-------------------+-----------------------+
```

类型名以 NUL 结尾、固定 20 字节（含 NUL）；payload 用 `Serializer::serialize<T>` 编码。因此**类型名最长 19 个字符**。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `vlink::DynamicData` | 默认构造 | 空容器 |
| `DynamicData::load` | `template <typename T> void load(std::string_view name, const T& value)` | 写入类型名 + 值 |
| `DynamicData::as` | `template <typename T> T as() const` | 取出为指定类型（返回值拷贝） |
| `DynamicData::convert` | `template <typename T> void convert(T& out) const` | 取出到引用 |
| `DynamicData::get_type` | `std::string_view get_type() const` | 取嵌入的类型名 |
| `DynamicData::operator>>` | `void operator>>(Bytes&) const` | 编码到 Bytes（wire 格式） |
| `DynamicData::operator<<` | `void operator<<(const Bytes&)` | 从 Bytes 还原 |
| `DynamicData::operator==` | `bool operator==(const DynamicData&) const` | 类型名 + payload 字节比较 |

## 代码导读

### 1. 基础 load / as

```cpp
vlink::DynamicData dd_str;
dd_str.load("StringMsg", std::string("Hello from DynamicData!"));
VLOG_I("[Basic] type=", dd_str.get_type(), " value=\"", dd_str.as<std::string>(), "\"");

vlink::DynamicData dd_temp;
dd_temp.load("Temperature", Temperature{36.6, 42});
Temperature restored_temp{};
dd_temp.convert(restored_temp);
VLOG_I("[Basic] type=", dd_temp.get_type(), " celsius=", restored_temp.celsius,
       " sensor_id=", restored_temp.sensor_id);
```

`load("Temperature", Temperature{36.6, 42})` 把类型名 `"Temperature"` 和值一起塞进 `dd_temp`。`as<T>()` 是按值返回；`convert(T&)` 是按引用填回，避免一次拷贝。

### 2. 单话题多类型

```cpp
vlink::Subscriber<vlink::DynamicData> sub("dds://example/dynamic/multi");
sub.attach(&loop);
sub.listen([&msg_count](const vlink::DynamicData& dd) {
  msg_count++;
  std::string type_name(dd.get_type());

  if (type_name == "Temperature") {
    auto t = dd.as<Temperature>();
    VLOG_I("[Sub] #", msg_count, " Temperature celsius=", t.celsius, " sensor=", t.sensor_id);
  } else if (type_name == "Pressure") {
    auto p = dd.as<Pressure>();
    VLOG_I("[Sub] #", msg_count, " Pressure hpa=", p.hpa, " sensor=", p.sensor_id);
  } else if (type_name == "Status") {
    VLOG_I("[Sub] #", msg_count, " Status=\"", dd.as<std::string>(), "\"");
  }
});

vlink::Publisher<vlink::DynamicData> pub("dds://example/dynamic/multi");
pub.attach(&loop);

vlink::Timer timer(&loop, 50, 1);
timer.start([&pub]() {
  vlink::DynamicData dd1;
  dd1.load("Temperature", Temperature{22.5, 1});
  pub.publish(dd1);

  vlink::DynamicData dd2;
  dd2.load("Pressure", Pressure{1013.25, 2});
  pub.publish(dd2);

  vlink::DynamicData dd3;
  dd3.load("Status", std::string("all_sensors_ok"));
  pub.publish(dd3);
});
```

同一对 `Publisher<DynamicData>` / `Subscriber<DynamicData>` 上分别发了 Temperature、Pressure、Status 三种类型；订阅端用 `get_type()` 分发处理。

### 3. 显式 wire round-trip

```cpp
vlink::DynamicData dd_wire;
dd_wire.load("TestType", Temperature{100.0, 99});

vlink::Bytes wire_bytes;
dd_wire >> wire_bytes;

vlink::DynamicData dd_from_wire;
dd_from_wire << wire_bytes;

Temperature from_wire{};
dd_from_wire.convert(from_wire);
VLOG_I("[Wire] size=", wire_bytes.size(), " type=", dd_from_wire.get_type(),
       " celsius=", from_wire.celsius, " sensor=", from_wire.sensor_id);
```

显式 `>> Bytes` 演示 wire 编码：返回的 `wire_bytes.size()` = 20（type name）+ `sizeof(Temperature)`（payload）= 36 字节。

### 4. 相等比较

```cpp
vlink::DynamicData a;
vlink::DynamicData b;
a.load("Test", 42);
b.load("Test", 42);
VLOG_I("[Eq] same=", a == b);

b.load("Test", 99);
VLOG_I("[Eq] differ=", a != b);
```

`operator==` 比较类型名和 payload 字节；适合做单元测试断言。

## 运行

```bash
./build/output/bin/example_dynamic_data
```

预期输出（节选）：

```
[Basic] type=StringMsg value="Hello from DynamicData!"
[Basic] type=Temperature celsius=36.6 sensor_id=42
[Sub] #1 Temperature celsius=22.5 sensor=1
[Sub] #2 Pressure hpa=1013.25 sensor=2
[Sub] #3 Status="all_sensors_ok"
[Sub] #4 Temperature celsius=-5 sensor=3
[Wire] size=36 type=TestType celsius=100 sensor=99
[Eq] same=1
[Eq] differ=1
[Summary] received=4
```

URL 用 `dds://`；切到 `intra://` 也能跑。

## 常见陷阱

1. **类型名超过 19 字节**：被截断或不能存下。命名 `Temperature_v1_high_resolution_long` 太长，应缩成 `TempV1HR` 之类。
2. **load 后再 load**：第二次会覆盖第一次；不能"追加"多个值到同一个 DynamicData。
3. **as<T>() 类型错配**：如果 `dd` 里实际是 `Temperature` 但你 `dd.as<Pressure>()`，行为按 `Serializer::deserialize` 派发 —— 对 POD 来说就是 memcpy 同样大小到不同类型，得到的字段值乱。生产代码必须先 `get_type()` 判断再 `as<>`。
4. **跨进程时类型名要约定**：发送方和接收方对"Temperature"这个字符串的含义必须一致；否则订阅端拿到 22 字节 payload 不知道怎么解。
5. **DynamicData 序列化开销**：每条消息多 20 字节固定开销，高频通信要权衡。

## 设计要点

- DynamicData 在 vlink 内部用 `Serializer::Type::kDynamicType` 标识；自动派发已经处理 wire 格式细节。
- 类型名固定 20 字节是为了不引入变长前缀，减少跨语言实现复杂度。
- 与 Protobuf 的 `Any` 类型对比：DynamicData 是 vlink 私有格式，无 schema 约束；Protobuf Any 自带类型 URL，更适合跨语言。
- DynamicData 不替代 Method 模型：它仅仅是承载多类型的"事件信封"。

## 配图

无专属配图。整体序列化派发链见 `../basic_types/images/serialization-type-chain.png`。

## 参考

- `../basic_types/` — 基础类型（Bytes / string / POD）
- `../custom_type/` — 自定义 operator>>/<<
- `samples/dds_dynamic/` — DynamicData 在真实 DDS 后端的端到端样例
- `vlink/include/vlink/extension/dynamic_data.h` — DynamicData 接口
- 顶层 `doc/06-serialization.md` — 序列化机制全景
