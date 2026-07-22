# ⚡ zerocopy/ — 跨进程零拷贝数据通路

本目录展示 vlink 的零拷贝数据通路。真正意义上的零拷贝需要共享内存：`shm://`、`shm2://` 这类传输会从 SHM 池
中借出（loan）一段已经存在的内存，由发送方填写、订阅方直接映射，全程不发生用户态复制。`dds://`、`mqtt://`
等无池传输走普通分配 + 序列化路径；`zenoh://` 在启用且运行时能取得 Zenoh SHM provider 时也可支持 loan。调用方
必须以 `is_support_loan()` 的运行时结果为准。

## 📂 子示例索引

| 示例 | 主题 | 关键类 |
|------|------|--------|
| `zerocopy_basic/` | Loan API 与 `RawData` 容器（vlink 零拷贝的两大原语） | `Publisher::loan` / `return_loan` / `vlink::zerocopy::RawData` |

> `CameraFrame` / `PointCloud` / `OccupancyGrid` / `Tensor` / `ObjectArray` / `AudioFrame` 等零拷贝容器不再单独提供示例目录，
> 其数据结构、wire 格式与用法统一归并到 `doc/06-zerocopy.md` 专文，请直接查阅该章节。

## 🧩 概念基础

零拷贝建立在三个公共概念之上，`zerocopy_basic` 全部覆盖，构成所有零拷贝容器的底座：

| 概念 | 说明 |
|------|------|
| Loan API | `is_support_loan()` 检测后借出；`publish` 消费 loan，发布返回 `false` 时调用方应 `return_loan`（对已消费缓冲为无害空操作） |
| `RawData` 容器 | 带 header 的可序列化字节容器，提供 shallow / deep / move 三种拷贝语义 |
| Owner 标志 | 零拷贝容器的 `is_owner()`：本地构造为 `true`，借用视图（`operator<<` / `shallow_copy`）为 `false`，析构不释放底层内存 |

## 🔧 共同前置知识

- vlink 的 `Publisher<T>` / `Subscriber<T>` 用法。
- 共享内存模型：`shm://` 使用 Iceoryx 的 POSIX 共享内存机制；Linux 上启动 SHM Pool 需确保 `/dev/shm` 可写并运行 RouDi。

## 📖 参考

| 资源 | 内容 |
|------|------|
| `doc/06-zerocopy.md` | 零拷贝机制全景、各容器 wire 格式与配图 |
| `include/vlink/zerocopy/raw_data.h` | `RawData` 容器 |
| `include/vlink/zerocopy/camera_frame.h` | `CameraFrame` 容器 |
| `include/vlink/zerocopy/point_cloud.h` | `PointCloud` 容器 |
| `include/vlink/zerocopy/occupancy_grid.h` | `OccupancyGrid` 容器 |
| `include/vlink/zerocopy/tensor.h` | `Tensor` 容器 |
| `include/vlink/zerocopy/object_array.h` | `ObjectArray` 容器 |
| `include/vlink/zerocopy/audio_frame.h` | `AudioFrame` 容器 |
