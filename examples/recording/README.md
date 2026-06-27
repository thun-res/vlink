# 📼 recording — 通信数据的录制与回放

VLink 提供完整的录制与回放基础设施，可将通信原语上流经的全部消息持久化为 bag 文件，用于事后回放、离线分析与回归测试。支持两种 bag 格式：

- **`.vdb`**：SQLite 后端的 VLink 原生格式，支持复杂查询、分片、压缩与 WAL。
- **`.vcap` / `.vcapx`**：MCAP 格式，与 Foxglove、ROS 2 工具链兼容。

本类目示范节点级录制的最简形态：在 Publisher / Subscriber 等原语上调用 `set_record_path()`，或通过 `VLINK_BAG_PATH` 环境变量录制全进程流量。`BagWriter` / `BagReader` 直接读写、读取过滤、多 bag 合并，以及 MCAP 格式与压缩选择器（None / Zstd / LZ4 / LZAV）的取舍，属于进阶主题，详见顶层 `doc/09-recording.md`。

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

## 📚 参考

- 顶层 `doc/09-recording.md` —— 录制系统完整章节
- `include/vlink/extension/bag_writer.h` / `bag_reader.h` —— BagWriter / BagReader 接口
- `include/vlink/extension/vcap_writer.h` / `vcap_reader.h` —— VCAP 接口
- `include/vlink/extension/bag_processor.h` —— 多 bag 合并
