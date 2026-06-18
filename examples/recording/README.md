# recording — 通信数据的录制与回放

vlink 提供完整的"录制 + 回放"基础设施，可以把通信原语上的所有消息持久化到 bag 文件，事后做回放、分析、回归测试。支持两种 bag 格式：

- **`.vdb`**：SQLite 后端的 vlink 自家格式，适合复杂查询、分片、压缩、WAL。
- **`.vcap` / `.vcapx`**：MCAP 格式，与 Foxglove、ROS 2 工具链兼容。

读完本目录你能掌握 vlink 录制系统的所有 API：

- 简单录制：在 Publisher/Subscriber 上 `set_record_path()`。
- 直接操作 BagWriter / BagReader：自由控制写入字段、读取过滤、合并多 bag。
- MCAP 格式：用 Foxglove Studio 直接打开 vlink 录制。
- 压缩：对比 None / Zstd / LZ4 / LZAV 的吞吐与压缩率。

## 子示例索引

| 示例 | 主题 | 关键类 |
|------|------|--------|
| `record_basic/` | 节点级 `set_record_path()` + 全局 `VLINK_BAG_PATH` 环境变量 | `Publisher::set_record_path` 等 |
| `record_bag/` | 直接 `BagWriter` + `BagReader` API（写、读、过滤、合并） | `BagWriter`、`BagReader`、`BagProcessor` |
| `record_mcap/` | MCAP 格式（Foxglove 兼容） | `VCAPWriter`、`VCAPReader` |
| `record_compression/` | None / Zstd / LZ4 / LZAV 压缩对比 | `BagWriter::Config::compress` |

## 推荐阅读顺序

1. **`record_basic/`** —— 必看。理解节点级录制最简形态（业务代码 1 行就能开启录制）。
2. **`record_bag/`** —— 深入 BagWriter / BagReader 完整 API：手工 push、读取过滤、时间合并。
3. **`record_mcap/`** —— vcap 格式与 vdb 的差异，何时选 MCAP。
4. **`record_compression/`** —— 在 IO 吞吐和 CPU 压缩之间权衡。

## 共同前置知识

- `../communication/` —— 三种通信原语的基础。
- `../serialization/` —— 录制内容的序列化形态决定怎么回放。
- 顶层 `doc/12-bag-recording.md` —— 录制系统完整设计文档。

## 录制 vs 监控 vs 调试

vlink 几种"看消息"机制比较：

| 场景 | 工具 | 文件存储 | 实时性 |
|------|------|--------|------|
| 长期录制（小时-天） | `BagWriter` (`.vdb` / `.vcap`) | 是 | 持久化 |
| 短期调试（秒-分钟） | `proxy/` ProxyAPI | 否 | 实时 |
| 实时监控告警 | 业务层 metric + Logger | 通常否 | 实时 |

## 配图

各示例 `images/` 包含对应流程图：

- `record_basic/images/recording-flow.png` —— 节点录制的内部数据流
- `record_compression/images/compression-pipeline.png` —— 压缩管线
- `record_mcap/images/mcap-format-flow.png` —— MCAP 文件结构

## 参考

- 顶层 `doc/12-bag-recording.md` —— 录制系统完整章节
- `vlink/include/vlink/extension/bag_writer.h` / `bag_reader.h` —— BagWriter / BagReader 接口
- `vlink/include/vlink/extension/vcap_writer.h` / `vcap_reader.h` —— VCAP 接口
- `vlink/include/vlink/extension/bag_processor.h` —— 多 bag 合并
