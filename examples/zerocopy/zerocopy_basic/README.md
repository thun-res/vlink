# ⚡ zerocopy_basic — Loan API + `RawData`：vlink 零拷贝的两大原语

演示零拷贝的两个基础设施：**Loan API**（Publisher 从传输层 SHM 池借出内存，业务直接在借出的内存上写数据，发布时不再拷贝）与 **`vlink::zerocopy::RawData`**（带 header 的可序列化字节容器，提供 shallow / deep / move 三种拷贝语义）。

在 `shm://` / `shm2://` 等具备缓冲池的后端上零拷贝生效；`dds://`、`intra://` 等无池后端退化为普通分配 + 序列化，loan API 仍可调用。

## 🧭 核心 API 索引

| API | 用途 |
|-----|------|
| `pub.is_support_loan()` | 当前传输是否支持 loan（dds 返回 false，需走 `Bytes::create` 回退） |
| `pub.loan(size)` | 从池中借出一段内存，返回 loaned `Bytes` |
| `pub.return_loan(bytes)` | 归还未使用的 loan（`publish` 会自动归还） |
| `sub.set_manual_unloan(bool)` | 由订阅端控制缓冲生命周期，回调内须显式 `return_loan` |
| `RawData::create(size)` | 分配内部缓冲 + header |
| `RawData::shallow_copy / deep_copy / move_copy` | 别名 / 深拷贝 / 移动所有权 |
| `RawData::header` | 公开字段：`seq` / `time_meas` / `time_pub` / `frame_id` |
| `RawData::operator>> / operator<<` | 与 `vlink::Bytes` 序列化互转 |
| `RawData::is_owner()` | 是否拥有底层内存（接收端为 false） |

## 🚀 最小示例

loan 检测 + 借出 + 发布（无池后端回退）：

```cpp
vlink::Publisher<vlink::Bytes> pub("shm://zerocopy_basic/loan_demo");

if (pub.is_support_loan()) {
  vlink::Bytes buf = pub.loan(sizeof(SensorSample));
  auto* s = reinterpret_cast<SensorSample*>(buf.data());
  s->id = 1;
  pub.publish(buf);                       // publish 后框架自动归还 loan
} else {
  pub.publish(vlink::Bytes::create(sizeof(SensorSample)));
}
```

订阅端手动归还（拿到消息后要异步留用时）：

```cpp
sub.set_manual_unloan(true);
sub.listen([&sub](const vlink::Bytes& msg) {
  sub.return_loan(msg);                   // 不归还会耗尽池并阻塞发布端
});
```

`RawData` 三种拷贝语义（性能 shallow > move > deep）：

```cpp
vlink::zerocopy::RawData src;
src.create(128);
src.header.seq = 77;

vlink::zerocopy::RawData shallow, deep, moved;
shallow.shallow_copy(src);               // 别名 src，is_owner=false
deep.deep_copy(src);                     // 独立缓冲
moved.move_copy(src);                    // 接管所有权，src 失效
```

## 🧮 选型判据

| 场景 | 方案 |
|------|------|
| 大消息（图像、点云、地图）+ 高频 publish + `shm://` 后端 | 用 loan 或 `RawData` 直接零拷贝 |
| 跨进程传感器帧 | 用专用容器 `CameraFrame` / `PointCloud`（均以 `RawData` 为基），见 `doc/06-zerocopy.md` |
| 小消息或非共享内存后端 | 直接用普通 `Publisher<T>`，无需 loan |

## 📖 参考

| 资源 | 内容 |
|------|------|
| `doc/06-zerocopy.md` | 零拷贝机制全景、各容器 wire 格式与 Bytes 所有权语义 |
| `include/vlink/zerocopy/raw_data.h` | `RawData` 容器与 header 定义 |
