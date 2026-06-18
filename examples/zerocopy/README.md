# zerocopy/ — 跨进程零拷贝数据通路

本目录展示 vlink 的零拷贝数据通路。真正意义上的零拷贝需要共享内存：`shm://`、`shm2://` 这类传输会从 SHM 池
中借出（loan）一段已经存在的内存，由发送方填写、订阅方直接映射，全程不发生用户态复制。其他传输（`dds://`、
`zenoh://`、`mqtt://` …）退化为普通分配 + 序列化路径。

## 子示例索引

| 示例 | 主题 | 关键类 |
|------|------|--------|
| `zerocopy_basic/` | Loan API 与 `RawData` 容器（vlink 零拷贝的两大原语） | `Publisher::loan` / `return_loan` / `vlink::zerocopy::RawData` |
| `zerocopy_camera_frame/` | 两进程相机生产/消费器 | `vlink::zerocopy::CameraFrame` |
| `zerocopy_point_cloud/` | LiDAR 风格变长 schema 点云容器 | `vlink::zerocopy::PointCloud` |
| `zerocopy_occupancy_grid/` | 两进程占据栅格生产/消费器 | `vlink::zerocopy::OccupancyGrid` |
| `zerocopy_tensor/` | 两进程 N 维张量生产/消费器（推理输入 / 输出） | `vlink::zerocopy::Tensor` |
| `zerocopy_object_array/` | 两进程 3D 检测对象数组生产/消费器 | `vlink::zerocopy::ObjectArray` |
| `zerocopy_audio_frame/` | 两进程音频帧生产/消费器 | `vlink::zerocopy::AudioFrame` |

## 推荐阅读顺序

先读 `zerocopy_basic`：理解 `is_support_loan()` 检测、`loan` / `return_loan` 的成对调用、`RawData` 的 header /
serialize / shallow vs deep vs move 拷贝语义。这是后续两个零拷贝容器的公共底座。

接着读 `zerocopy_camera_frame`。它把 `CameraFrame` 拆成一对独立的可执行文件，演示真正的生产/消费拓扑：
producer 进程把像素写入 SHM，consumer 进程映射进自己的地址空间，零拷贝完整跑通。

最后读 `zerocopy_point_cloud`。它展示 vlink 怎么把「按字段类型描述的可变 schema」嵌入到一个 256 字节的紧凑
struct 里，使得不同传感器的点云布局都能用同一个容器表达。

## 共同前置知识

- vlink 的 `Publisher<T>` / `Subscriber<T>` 用法。
- 共享内存模型：`shm://` 在 Linux 下基于 POSIX shm + memfd；如需启动 SHM Pool 需要确保 `/dev/shm` 可写。
- 「Owner」概念：零拷贝容器有 `is_owner()` 标志。本地构造为 `true`，通过 `operator<<` / `shallow_copy` 得到
  借用视图为 `false`，析构时不再释放底层内存。

## 配图

各示例 `images/` 包含对应布局图：

- `zerocopy_camera_frame/images/camera-frame-producer-consumer.png` — 两进程 SHM 数据通路
- `zerocopy_point_cloud/images/point-cloud-zerocopy.png` — PointCloud schema 与 wire 格式
- `zerocopy_occupancy_grid/images/occupancy-grid-producer-consumer.png` — OccupancyGrid 两进程数据通路
- `zerocopy_tensor/images/tensor-producer-consumer.png` — Tensor 两进程数据通路
- `zerocopy_object_array/images/object-array-producer-consumer.png` — ObjectArray 两进程数据通路
- `zerocopy_audio_frame/images/audio-frame-producer-consumer.png` — AudioFrame 两进程数据通路

`zerocopy_basic/` 没有图片，其概念性内容由 `RawData` 注释及 `doc/10-zerocopy.md` 覆盖。

## 参考

- `doc/10-zerocopy.md` — 零拷贝设计章节
- `include/vlink/zerocopy/raw_data.h` — `RawData` 容器
- `include/vlink/zerocopy/camera_frame.h` — `CameraFrame` 容器
- `include/vlink/zerocopy/point_cloud.h` — `PointCloud` 容器
- `include/vlink/zerocopy/occupancy_grid.h` — `OccupancyGrid` 容器
- `include/vlink/zerocopy/tensor.h` — `Tensor` 容器
- `include/vlink/zerocopy/object_array.h` — `ObjectArray` 容器
- `include/vlink/zerocopy/audio_frame.h` — `AudioFrame` 容器
