# intra_data — 进程内零拷贝消息：`VLINK_INTRA_DATA_DECLARE` 宏

本示例演示 vlink 提供的"进程内零拷贝"消息封装：通过 `VLINK_INTRA_DATA_DECLARE(ValueT, DeclName)` 宏在编译期生成一个携带类型信息的 `shared_ptr<IntraDataType<ValueT>>` 包装类型。**当通信发生在同一进程内（`intra://`）时**，订阅者收到的是和发布者完全相同的 shared_ptr，零序列化、零拷贝。

当通信链路切换到跨进程传输（如 `dds://`、`shm://`）时，框架会自动 fallback 到 `operator>>` / `operator<<` 进行真正的序列化。

读完本示例你能掌握：

- `VLINK_INTRA_DATA_DECLARE` 宏生成了什么，应用层如何使用。
- 进程内零拷贝的工程意义（大对象、共享 shared_ptr、避免反序列化）。
- 同一类型如何在 intra:// 走零拷贝、在跨进程走 wire 序列化。
- 共享所有权和"发布后不可变"约定。

## 背景与适用场景

适用场景：

- 同一进程内的多模块共享大对象（如完整传感器帧、图像、张量）。
- 进程内总线 / 调度器模式：模块之间用 vlink 通信，但都在同一进程，需要避免序列化开销。
- 既要支持"同进程零拷贝"，又要支持"跨进程序列化"的统一抽象。

不适合：

- 通信必须跨进程（直接用 zerocopy/CameraFrame、PointCloud 等专用容器；它们也支持 shm 零拷贝）。
- 大量小消息 —— shared_ptr 引用计数本身有原子开销，小消息可能反而比直接拷贝慢。
- 多个订阅者在不同线程并发修改 —— 进程内零拷贝意味着所有订阅者共享一份数据；要么约定不可变，要么自己加锁。

## 核心 API

| API | 签名/形式 | 说明 |
|-----|----------|------|
| `VLINK_INTRA_DATA_DECLARE` | `VLINK_INTRA_DATA_DECLARE(ValueT, DeclName)` 宏 | 生成 `DeclName` 类型（=`std::shared_ptr<IntraDataType<ValueT>>`）+ `DeclName::create()` 工厂 |
| `DeclName::create` | `static DeclName create()` | 构造并返回 shared_ptr 句柄；内部新分配 `IntraDataType<ValueT>` |
| `DeclName::operator->` | 通过 shared_ptr 提供 | 访问内部 `IntraDataType` 实例（含 `value` 字段） |
| `IntraDataType::value` | 类型 `ValueT` | 实际承载的数据 |
| `IntraDataType::operator>>` | `bool operator>>(Bytes&) const` | 跨进程时调用 ValueT 的 operator>> 编码 |
| `IntraDataType::operator<<` | `bool operator<<(const Bytes&)` | 同样跨进程反向 |
| `IntraDataType::get_serialized_size` | `size_t` | 编码后字节数 |
| `IntraDataType::kValueType` | `static constexpr Serializer::Type` | 内部 ValueT 推导出的类型 |

## 代码导读

### 1. 定义底层类型 + 宏展开

```cpp
struct MyStruct {
  int32_t id;
  float temperature;
  char label[32];

  void operator>>(vlink::Bytes& out) const {
    out = vlink::Bytes::create(sizeof(MyStruct));
    std::memcpy(out.data(), this, sizeof(MyStruct));
  }

  void operator<<(const vlink::Bytes& in) {
    if (in.size() >= sizeof(MyStruct)) {
      std::memcpy(this, in.data(), sizeof(MyStruct));
    }
  }
};

VLINK_INTRA_DATA_DECLARE(MyStruct, MyIntra)
```

宏 `VLINK_INTRA_DATA_DECLARE(MyStruct, MyIntra)` 展开生成（大致）：

```cpp
class MyIntraType : public IntraDataBase {
  MyStruct value;
  static MyIntraType create();
  bool operator>>(Bytes& out) const { return value.operator>>(out); }
  bool operator<<(const Bytes& in) { return value.operator<<(in); }
  static constexpr Serializer::Type kValueType = ...;
};

using MyIntra = std::shared_ptr<MyIntraType>;
```

所以 `MyIntra::create()` 返回一个 shared_ptr。

### 2. 创建与字段访问

```cpp
auto data = MyIntra::create();
data->value.id = 42;
data->value.temperature = 36.6F;
std::strncpy(data->value.label, "sensor_A", sizeof(data->value.label) - 1);
VLOG_I("[Create] id=", data->value.id, " temp=", data->value.temperature, " label=", data->value.label);
VLOG_I("[Create] kValueType=", static_cast<int>(MyIntraType::kValueType),
       " type=", MyIntraType::get_serialized_type());
```

`data->value` 是 `MyStruct` 实例；`kValueType` 是编译期常量，能 debug 时打出来确认 vlink 选了哪个序列化策略。

### 3. 跨进程 fallback：手动 round-trip

```cpp
auto original = MyIntra::create();
original->value.id = 100;
original->value.temperature = 25.0F;
std::strncpy(original->value.label, "motor_B", sizeof(original->value.label) - 1);

vlink::Bytes wire;
bool ok = (*original) >> wire;
VLOG_I("[Serialize] ok=", ok, " size=", wire.size(), " expected=", original->get_serialized_size());

auto restored = MyIntra::create();
ok = (*restored) << wire;
VLOG_I("[Deserialize] ok=", ok, " id=", restored->value.id, " temp=", restored->value.temperature,
       " label=", restored->value.label);
```

跨进程时框架会自动调用这两个 operator；这里手动模拟，用于在单元测试里验证 wire 格式可逆。

### 4. intra:// 上零拷贝 pub/sub

```cpp
const std::string topic_url = "intra://example/intra_data/struct#direct";

vlink::Subscriber<MyIntra> sub(topic_url);
sub.listen([&](const MyIntra& typed) {
  received_count++;

  if (typed) {
    VLOG_I("[Sub] #", received_count, " id=", typed->value.id, " temp=", typed->value.temperature,
           " label=", typed->value.label);
  }
});

vlink::Publisher<MyIntra> pub(topic_url);
pub.wait_for_subscribers();

for (int i = 1; i <= 5; ++i) {
  auto frame = MyIntra::create();
  frame->value.id = i;
  frame->value.temperature = 20.0F + static_cast<float>(i);
  std::snprintf(frame->value.label, sizeof(frame->value.label), "reading_%d", i);
  pub.publish(frame);
}
```

`Publisher<MyIntra>` / `Subscriber<MyIntra>` 看起来和普通 pub/sub 没区别；但因为 URL 是 `intra://`，框架知道这是进程内通信，**不会序列化**，直接把 shared_ptr 传给订阅者。

URL 后缀 `#direct` 是 vlink intra 后端的内部约定，指示走 direct（同步派发）路径。

### 5. 共享所有权语义

```cpp
auto shared = MyIntra::create();
shared->value.id = 999;
MyIntra alias = shared;  // 引用计数 +1，不拷贝数据
VLOG_I("[Share] use_count=", shared.use_count(), " same_object=", shared.get() == alias.get());
```

`MyIntra` 本身就是 `shared_ptr`，所以拷贝它只增加引用计数；底层 `IntraDataType` 实例仍是同一份。

## 运行

```bash
./build/output/bin/example_intra_data
```

预期输出（节选）：

```
[Create] id=42 temp=36.6 label=sensor_A
[Create] kValueType=3 type=MyStruct  
[Serialize] ok=1 size=40 expected=40
[Deserialize] ok=1 id=100 temp=25 label=motor_B
[Sub] #1 id=1 temp=21 label=reading_1
[Sub] #2 id=2 temp=22 label=reading_2
[Sub] #3 id=3 temp=23 label=reading_3
[Sub] #4 id=4 temp=24 label=reading_4
[Sub] #5 id=5 temp=25 label=reading_5
[Sub] total=5
[Share] use_count=2 same_object=1
IntraData example complete.
```

URL 必须是 `intra://` 才能享受零拷贝；改成 `dds://` 程序仍能跑，但会走 operator>>/<< 序列化。

## 常见陷阱

1. **发布后修改 payload**：订阅者可能正在并发读 `frame->value.label`；发布之后不要再修改，按"不可变"对待。需要修改就 create 一个新 shared_ptr。
2. **shared_ptr 循环引用**：MyIntra 内部如果再含 shared_ptr<MyIntra>，会形成循环；要么用 weak_ptr，要么打破链。
3. **跨进程传输时性能下降**：intra 用零拷贝，但 dds/shm 走 operator>>，吞吐差几个数量级；不要假设 IntraData 一定快。
4. **使用未实现 operator>>/<< 的 ValueT**：宏会生成跨进程 fallback 调用 ValueT 的 operator，没实现就编译失败 / 跨进程乱码。
5. **URL `#direct` 后缀拼写错误**：影响 intra 后端选路，仍会跑但可能走 queue 而不是 direct。

## 设计要点

- `VLINK_INTRA_DATA_DECLARE` 是宏，原因是要在用户代码生成新类型；C++17 没有更优雅的方式。
- shared_ptr 引用计数是原子的，吞吐瓶颈可能在引用计数本身而非数据传递。
- intra 路径的零拷贝由 vlink 内部的 channel + lockfree queue 实现；多个订阅者只递增引用计数，不复制 payload。
- 与 `zerocopy/CameraFrame` 等的区别：CameraFrame 走 `shm://` 共享内存零拷贝（跨进程），IntraData 只在进程内零拷贝；两者解决不同层次的问题。

## 配图

无专属配图。整体序列化派发链见 `../basic_types/images/serialization-type-chain.png`。

## 参考

- `../custom_type/` — 实现 ValueT 所需的 operator>>/<<
- `../../zerocopy/` — 共享内存零拷贝容器（CameraFrame、PointCloud）
- `vlink/include/vlink/impl/intra_data.h` — IntraData 宏与基类
- 顶层 `doc/06-serialization.md` — 序列化机制
- 顶层 `doc/10-zerocopy.md` — 零拷贝机制
