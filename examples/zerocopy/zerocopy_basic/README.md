# ⚡ zerocopy_basic — Loan API + `RawData`：vlink 零拷贝的两大原语

演示零拷贝的两个基础设施：**Loan API**（Publisher 从传输层 SHM 池借出内存，业务直接在借出的内存上写数据，发布时不再拷贝）与 **`vlink::zerocopy::RawData`**（带 header 的可序列化字节容器，提供 shallow / deep / move 三种拷贝语义）。

在 `shm://` / `shm2://` 等具备缓冲池的后端上零拷贝生效；`dds://`、`intra://` 等无池后端的 `is_support_loan()` 返回 `false`，业务须改用普通分配并正常发布。

## 🧭 核心 API 索引

| API | 用途 |
|-----|------|
| `pub.is_support_loan()` | 当前传输是否支持 loan（dds 返回 false，需走 `Bytes::create` 回退） |
| `pub.loan(size)` | 从池中借出一段内存，返回 loaned `Bytes` |
| `pub.return_loan(bytes)` | 归还未使用或发布失败的 loan（对已被后端消费的缓冲区为无害空操作） |
| `RawData::create(size)` | 分配内部缓冲 + header |
| `RawData::shallow_copy / deep_copy / move_copy` | 别名 / 深拷贝 / 移动所有权 |
| `RawData::header` | 公开字段：`seq` / `time_meas` / `time_pub` / `frame_id` |
| `RawData::operator>> / operator<<` | 与 `vlink::Bytes` 序列化互转 |
| `RawData::is_owner()` | 是否拥有底层内存（接收端为 false） |

## 🚀 最小示例

运行完整示例前须启动对应的共享内存运行时；`shm://` 默认使用 Iceoryx RouDi：

```bash
iox-roudi &
./build/output/bin/example_zerocopy_basic
```

loan 检测 + 借出 + 发布（无池后端回退）：

```cpp
vlink::Publisher<vlink::Bytes> pub("shm://zerocopy_basic/loan_demo");

if (pub.is_support_loan()) {
  vlink::Bytes buf = pub.loan(sizeof(SensorSample));
  auto* s = reinterpret_cast<SensorSample*>(buf.data());
  s->id = 1;
  if (!pub.publish(buf, true)) {          // force=true：示例未创建匹配订阅者
    pub.return_loan(buf);                 // 未进入后端发布路径时显式归还
  }
} else {
  pub.publish(vlink::Bytes::create(sizeof(SensorSample)), true);
}
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
| 大消息（图像、点云、地图）+ 高频 publish + `shm://` 后端 | 发布端直接写池内存用 `Publisher<Bytes>::loan()`；`RawData` 提供接收侧借用视图 |
| 跨进程传感器帧 | 用专用容器 `CameraFrame` / `PointCloud`（均以 `RawData` 为基），见 `doc/06-zerocopy.md` |
| 小消息或非共享内存后端 | 直接用普通 `Publisher<T>`，无需 loan |

## 📖 参考

| 资源 | 内容 |
|------|------|
| `doc/06-zerocopy.md` | 零拷贝机制全景、各容器 wire 格式与 Bytes 所有权语义 |
| `include/vlink/zerocopy/raw_data.h` | `RawData` 容器与 header 定义 |
