# 📼 recording — 通信数据的录制与回放

VLink 提供录制与回放基础设施，可将受支持的通信原语消息持久化为 bag 文件，用于事后回放、离线分析与回归测试。`intra://` 不支持节点级录制；DDS CDR 按完整封装字节录制与回放。其余限制见顶层 `doc/09-recording.md`。支持两种 bag 格式：

- **`.vdb`**：SQLite 后端的 VLink 原生格式，支持复杂查询、分片、压缩与 WAL。
- **`.vcap` / `.vcapx`**：MCAP 格式，与 Foxglove、ROS 2 工具链兼容。

本类目示范节点级录制的最简形态：在受支持的 Publisher / Subscriber 等原语上调用 `set_record_path()`，或通过 `VLINK_BAG_PATH` 环境变量启用进程级 writer。全局 writer 可记录经过序列化 Bytes 路径的普通 `intra://` 消息和 DDS CDR，但不会记录绕过 Bytes 的 `IntraData` 直通消息。`BagWriter` / `BagReader` 直接读写、读取过滤、多 bag 合并，以及 MCAP 格式与压缩选择器（None / Zstd / LZ4 / LZAV）的取舍，属于进阶主题，详见顶层 `doc/09-recording.md`。

## 📑 子示例索引

| 示例 | 主题 | 关键 API |
|------|------|----------|
| `record_basic/` | 节点级 `set_record_path()` 与全局 `VLINK_BAG_PATH` 环境变量 | `set_record_path`、`BagWriter::global_get` |

## 🔗 共同前置知识

| 依赖 | 说明 |
|------|------|
| `../communication/` | 三种通信原语的基础用法。 |
| `../serialization/` | 录制内容的序列化形态决定回放方式。 |
| `doc/09-recording.md` | 录制系统完整设计文档。 |

## ⚖️ 录制 vs 监控 vs 调试

| 场景 | 工具 | 文件存储 | 实时性 |
|------|------|----------|--------|
| 长期录制（小时—天） | `BagWriter`（`.vdb` / `.vcap`） | 是 | 持久化 |
| 短期调试（秒—分钟） | ProxyAPI，见 `../proxy/` | 否 | 实时 |
| 实时监控告警 | 业务层 metric + Logger | 通常否 | 实时 |

## 🖼️ 配图

`record_basic/images/recording-flow.png` —— 节点录制的内部数据流。

压缩管线与 MCAP 文件结构示意图随专文一并维护，见顶层 `doc/09-recording.md`。

## 📦 示例数据集

无需自行采集即可体验回放：官方提供一份预录 bag，可从 GitHub Release 下载。

| 文件 | 大小 | 时长 | 内容 |
|------|------|------|------|
| `sample_r2b.vdb` | ~18 MB | ~10 s / 707 帧 | IMU + LiDAR 点云 + 相机压缩帧 |

Topic 明细：

| URL | 类型 | 帧数 |
|-----|------|------|
| `dds://sensor/imu/d455?qos=sensor` | `vmsgs.proto.sensor.Imu` | 303 |
| `shm://sensor/lidar/pandar_xt_32_0` | `vlink::zerocopy::PointCloud` | 100 |
| `shm://sensor/camera/compressed/d455_1` | `vlink::zerocopy::CameraFrame` | 304 |

采集自 NVIDIA r2b（Robotics-2-Benchmark）公开数据集 `r2b_storage`，转录为 VLink bag 格式；使用前请遵循原数据集许可证。

```bash
# 下载
curl -L -o sample_r2b.vdb \
  https://github.com/thun-res/vmsgs/releases/download/sample-data/sample_r2b.vdb

# 查看录制信息
vlink-bag info sample_r2b.vdb

# 播放录制信息
vlink-player sample_r2b.vdb
```

## 📚 参考

- 顶层 `doc/09-recording.md` —— 录制系统完整章节
- `include/vlink/extension/bag_writer.h` / `bag_reader.h` —— BagWriter / BagReader 接口
- `include/vlink/extension/vcap_writer.h` / `vcap_reader.h` —— VCAP 接口
- `include/vlink/extension/bag_processor.h` —— 多 bag 合并
