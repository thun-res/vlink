# zerocopy_basic — Loan API + `RawData` 容器：vlink 零拷贝的两大原语

本示例覆盖 vlink 零拷贝传输的两个基础设施：

- **Loan API**：`Publisher::loan(size)` / `Publisher::return_loan(bytes)` —— Publisher 从传输层 SHM 池中借出一段内存，业务直接在借出的内存上填数据，发布时框架不再拷贝。
- **`vlink::zerocopy::RawData`**：vlink 内置的"带 header 的可序列化字节容器"，支持 shallow_copy / deep_copy / move_copy 三种语义。

合并自原 `zerocopy_loan` + `zerocopy_raw_data`，提供完整的零拷贝基础形态。

读完本示例你能掌握：

- 在不同传输上检测 loan 支持。
- loan / return_loan 的正确成对使用。
- `set_manual_unloan(true)` 让 Subscriber 推迟自动归还。
- `RawData` 的 header 字段、序列化、所有权传递语义。

## 背景与适用场景

零拷贝的工程意义：避免把消息从应用堆复制到 SHM、又从 SHM 复制回应用堆。在大数据场景（图像帧、点云、地图）下，拷贝是真正的瓶颈。

适用：

- `shm://` / `shm2://` 传输上的大消息。
- `Bytes` / `RawData` 形式的载荷。
- 高频 publish 路径（每秒 ≥ 100 次大消息）。

非 SHM 传输（`dds://`、`zenoh://`、`mqtt://` 等）退化为普通分配 + 序列化路径；loan API 仍可调，但内部会用普通 malloc。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `Publisher::is_support_loan` | `bool is_support_loan() const` | 当前传输是否支持 loan |
| `Publisher::loan` | `Bytes loan(size_t size)` | 借出一段内存，返回 loaned Bytes |
| `Publisher::return_loan` | `void return_loan(const Bytes&)` | 归还借出 |
| `Subscriber::set_manual_unloan` | `void set_manual_unloan(bool)` | Subscriber 是否手动归还 |
| `RawData::create` | `void create(size_t)` | 分配内部缓冲 + header |
| `RawData::shallow_copy` | `void shallow_copy(const RawData&)` | 别名 |
| `RawData::deep_copy` | `void deep_copy(const RawData&)` | 深拷贝 |
| `RawData::move_copy` | `void move_copy(RawData&&)` | 移动 |
| `RawData::header` | `Header` 公开字段 | seq / time_meas / time_pub / frame_id |
| `RawData::operator>>` / `operator<<` | const / mut | 与 `vlink::Bytes` 互转 |
| `RawData::is_owner` | `bool` | 是否拥有底层内存 |

## 代码导读

### 1. loan 支持检测

```cpp
vlink::Publisher<vlink::Bytes> pub("shm://example/loan");
if (pub.is_support_loan()) {
  VLOG_I("transport supports loan");
}
```

### 2. loan + return_loan + 发送

```cpp
auto buf = pub.loan(1024);
// 在 buf.data() 上直接写数据
std::memset(buf.data(), 0x42, buf.size());
pub.publish(buf);
// 框架在 publish 后内部归还；如果走非 loan 路径，return_loan 被自动调
```

### 3. Subscriber 手动归还

```cpp
vlink::Subscriber<vlink::Bytes> sub("shm://example/loan");
sub.set_manual_unloan(true);          // 不自动归还
sub.listen([&sub](const vlink::Bytes& msg) {
  // 使用 msg；msg 的内存仍由 SHM 池持有
  sub.return_loan(msg);              // 显式归还
});
```

业务场景：拿到消息后要"留着一会儿"再处理（如缓存进队列稍后异步处理），用 manual_unloan 推迟归还，避免 SHM 池被占满。

### 4. RawData 基础

```cpp
vlink::zerocopy::RawData rd;
rd.create(256);
rd.header.seq = 100;
rd.header.time_pub = vlink::ElapsedTimer::get_cpu_timestamp(vlink::ElapsedTimer::kMilli);
std::strncpy(rd.header.frame_id, "lidar", sizeof(rd.header.frame_id) - 1);
std::memset(rd.data(), 0x77, rd.size());
```

### 5. shallow / deep / move

```cpp
vlink::zerocopy::RawData a, b;
a.create(128);

b.shallow_copy(a);                    // b 别名 a；is_owner=false
VLOG_I("same memory=", b.data() == a.data());

vlink::zerocopy::RawData c;
c.deep_copy(a);                       // c 深拷贝
VLOG_I("distinct memory=", c.data() != a.data());

vlink::zerocopy::RawData d;
d.move_copy(std::move(a));            // d 接管 a
```

### 6. RawData 跨 publish

```cpp
vlink::Publisher<vlink::zerocopy::RawData> pub("shm://example/rawdata");
vlink::Subscriber<vlink::zerocopy::RawData> sub("shm://example/rawdata");
sub.listen([](const vlink::zerocopy::RawData& msg) {
  VLOG_I("got seq=", msg.header.seq, " size=", msg.size(), " owner=", msg.is_owner());
});

vlink::zerocopy::RawData out;
out.create(64);
out.header.seq = 1;
pub.publish(out);
```

接收端 `is_owner() == false`：数据来自 wire，不是本地分配。

## 运行

```bash
# 需要 iox-roudi 后台跑（shm 后端依赖）
iox-roudi &

./build/output/bin/example_zerocopy_basic
```

预期输出（节选）：

```
transport supports loan
loaned 1024 bytes
manual unloan: returned
RawData seq=100 frame_id=lidar size=256
shallow: same memory=1
deep:    distinct memory=1
move ok
got seq=1 size=64 owner=0
```

## 常见陷阱

1. **loan 后忘记 publish 或 return_loan**：SHM 池可能被耗尽；loan 必须配对消化。
2. **manual_unloan=true 但忘记 return_loan**：与上面同理。
3. **非 SHM 传输上 loan**：行为正确但不是零拷贝；按业务可接受。
4. **RawData::shallow_copy 后源对象析构**：别名变野指针 —— 同 Bytes::shallow_copy 的生命周期约束。
5. **header.frame_id 长度**：固定 16 字节含 NUL，超长被截断。

## 设计要点

- loan API 屏蔽传输细节：业务代码只调 `loan` / `publish` / `return_loan`，框架决定是否真的零拷贝。
- RawData 是"带元信息的 Bytes 升级版"，header 包含序号、时间戳、frame_id 等通用字段。
- 三种 copy 语义按业务需求选；性能从快到慢：shallow > move > deep。

## 配图

无专属配图。整体零拷贝架构见 `doc/10-zerocopy.md`。

## 参考

- `../zerocopy_camera_frame/` — 摄像头帧 producer/consumer
- `../zerocopy_point_cloud/` — 点云容器
- `../../base/bytes_zerocopy/` — Bytes 层面的所有权语义
- `vlink/include/vlink/zerocopy/raw_data.h` — RawData 接口
- 顶层 `doc/10-zerocopy.md` — 零拷贝机制全景
