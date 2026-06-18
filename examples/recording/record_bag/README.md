# record_bag — 直接操作 `BagWriter` + `BagReader` 的完整 API

`record_basic` 演示了"在通信原语上挂 bag 路径"这种最简形态。本示例进一步直接操作 `BagWriter` / `BagReader` API：手工 push 消息、配置压缩 / 分片 / WAL、读取并按时间/URL 过滤、用 `BagProcessor` 合并多个 bag 时序输出。

覆盖完整 bag 读写流程。

读完本示例你能掌握：

- BagWriter 的完整 API：`create` / `push` / Config 各字段 / split 回调。
- BagReader 的完整 API：`get_info` / play / output 回调 / Config 过滤。
- `filter_get` 共享 writer 实例。
- 多 bag 时序合并：`BagProcessor`。

## 背景与适用场景

适用：

- 离线录制工具（不在业务进程中跑，独立 bag merger）。
- 数据回放仿真器：精确控制时间戳、过滤特定 topic。
- 录制后处理：合并多个 bag、按 URL 切分、转换格式。

不适合：

- 业务进程内简单录制（用 `record_basic/` 的 `set_record_path`）。

## 核心 API

### BagWriter

| API | 签名 | 说明 |
|-----|------|------|
| `BagWriter::create` | `static std::shared_ptr<BagWriter> create(const std::string& path, const Config& = {})` | 工厂；按扩展名选 vdb / vcap |
| `BagWriter::push` | `int64_t push(const vlink::Frame& frame, bool immediate = false)` | 写一条消息；`frame.timestamp < 0` 表示由 writer 自动分配时间戳 |
| `BagWriter::Config` | `{ compress, split_by_size, wal_mode, tag_name, ... }` | 写入配置 |
| `BagWriter::filter_get` | `static std::shared_ptr<BagWriter> filter_get(const std::string& path)` | 同路径共享实例 |
| `BagWriter::register_split_callback` | `void register_split_callback(cb, before_open)` | 分片切换回调 |

### BagReader

| API | 签名 | 说明 |
|-----|------|------|
| `BagReader::create` | `static std::shared_ptr<BagReader> create(const std::string& path, bool read_only = true)` | 打开 |
| `BagReader::get_info` | `BagInfo get_info() const` | 元信息（含 URL 列表、消息数、时间范围） |
| `BagReader::register_output_callback` | `void` | 回放回调 |
| `BagReader::register_finish_callback` | `void` | 完成回调 |
| `BagReader::play` | `void play(const Config&)` | 启动回放 |
| `BagReader::Config` | `{ rate, times, begin_time, end_time, filter_urls, skip_blank }` | 回放配置 |
| `BagReader::seek` | `bool seek(int64_t timestamp, bool force_play = false)` | 跳到指定时间 |

### BagProcessor

| API | 签名 | 说明 |
|-----|------|------|
| `vlink::Frame` | `{ timestamp, url, ser_type, schema_type, action_type, data }` | 全局统一帧类型；单一 `timestamp` 随帧带出 |
| `BagProcessor::push` | `void push(int64_t data_timestamp, const vlink::Frame& frame)` | 按 `data_timestamp`（数据面时间，重排键）滑窗重排推入一帧 |
| `BagProcessor::register_output_callback` | `void(const vlink::Frame&)` | 重排后输出回调 |

## 代码导读

### 1. 写 50 条消息

```cpp
auto writer = vlink::BagWriter::create("/tmp/example_bag.vdb");

for (int i = 0; i < 50; ++i) {
  int64_t ts = vlink::ElapsedTimer::get_cpu_timestamp(vlink::ElapsedTimer::kMilli) + i * 10;
  std::string text = "msg_" + std::to_string(i);
  vlink::Bytes payload = vlink::Bytes::from_string(text);

  std::string url = (i % 2 == 0) ? "intra://topic_a" : "intra://topic_b";

  vlink::Frame frame;
  frame.timestamp   = ts;
  frame.url         = url;
  frame.ser_type    = vlink::Serializer::kStringType;
  frame.schema_type = vlink::SchemaType::kRaw;
  frame.action_type = vlink::ActionType::kPublish;
  frame.data        = payload;
  writer->push(frame);
}
writer.reset();   // 析构 flush
```

### 2. Config: 压缩 + 分片 + WAL + tag

```cpp
vlink::BagWriter::Config cfg;
cfg.compress = vlink::CompressType::kLzav;
cfg.split_by_size = 10 * 1024 * 1024;   // 每 10MB 切分
cfg.wal_mode = true;                     // SQLite WAL 模式
cfg.tag_name = "experiment_42";

auto compressed_writer = vlink::BagWriter::create("/tmp/compressed.vdb", cfg);
```

### 3. filter_get 共享 writer

```cpp
auto a = vlink::BagWriter::filter_get("/tmp/shared.vdb");
auto b = vlink::BagWriter::filter_get("/tmp/shared.vdb");
// a 与 b 共享同一 writer 实例；refcount 管理 lifetime
```

### 4. 读元信息

```cpp
auto reader = vlink::BagReader::create("/tmp/example_bag.vdb");
auto info = reader->get_info();
MLOG_I("size={} total_msgs={} begin={} end={}", info.size_bytes, info.total_messages,
       info.begin_time, info.end_time);
for (const auto& [url, stat] : info.url_stats) {
  MLOG_I("  url={} count={}", url, stat.count);
}
```

### 5. 基础回放

```cpp
reader->register_output_callback([](const vlink::Frame& frame) {
  VLOG_I("ts=", frame.timestamp, " url=", frame.url, " size=", frame.data.size());
});
reader->register_finish_callback([]() { VLOG_I("playback finished"); });

vlink::BagReader::Config cfg;
cfg.rate = 1.0;          // 1x 实时速度
cfg.times = 1;
reader->play(cfg);
```

### 6. 时间 + URL 过滤

```cpp
vlink::BagReader::Config cfg;
cfg.rate = 2.0;                      // 2x 加速回放
cfg.begin_time = 1000;
cfg.end_time = 5000;
cfg.filter_urls = {"intra://topic_a"};
cfg.skip_blank = true;
reader->play(cfg);
```

### 7. 多 bag 时序合并

```cpp
vlink::BagProcessor proc;
proc.register_output_callback([](const vlink::Frame& f) { /* 按 data_timestamp 有序输出 */ });

// 把 readerA、readerB 的内容推进 processor（合并回放把录制时间当作 data_timestamp 重排键）
auto fan_in = [&proc](const vlink::Frame& frame) { proc.push(/*data_timestamp=*/frame.timestamp, frame); };
readerA->register_output_callback(fan_in);
readerB->register_output_callback(fan_in);
readerA->play({});
readerB->play({});
```

processor 按 ts 单调输出；适合"多个并行子系统录制 → 全局时序回放"场景。

## 运行

```bash
./build/output/bin/example_record_bag
```

预期产物 `/tmp/example_bag.vdb`、`/tmp/compressed.vdb` 等。

预期输出（节选）：

```
=== writer 50 messages ===
  pushed 50 messages to /tmp/example_bag.vdb
=== writer config ===
  compressed.vdb written with LZAV + 10MB split
=== reader info ===
  size=4096 total_msgs=50 begin=... end=...
  url=intra://topic_a count=25
  url=intra://topic_b count=25
=== basic playback ===
  ts=... url=intra://topic_a size=5
  ts=... url=intra://topic_b size=5
  ...
  playback finished
=== filtered playback (2x rate) ===
  only topic_a messages
=== BagProcessor merge ===
  time-ordered output
```

## 常见陷阱

1. **push 时 ts 必须单调**：reader 假定 ts 递增；乱序 push 会让 seek / 时间范围过滤错乱。
2. **WAL 模式 + 写满磁盘**：vdb 是 SQLite，磁盘满会失败；用 `register_split_callback` 监控并切换。
3. **filter_get 共享 writer 析构**：每个 shared_ptr 都引用一份；最后一个析构时才真 close。
4. **rate > 1 时 output 回调跑得快**：业务回调若慢会阻塞回放；按需要降 rate 或异步处理。
5. **filter_urls 大小写**：URL 精确匹配，包括大小写。

## 设计要点

- BagWriter 是 MessageLoop 派生：push 投递到内部队列后立即返回。
- VDB 内部用 SQLite + 自定义 schema；vcap 用 MCAP 标准格式。
- BagProcessor 在有序 deque 中按 `push()` 传入的 `data_timestamp`（数据面时间，重排键）插入排序，滑窗满足后顺序输出；帧自身只携带单一 `timestamp` 原样带出。

## 配图

无专属配图。

## 参考

- `../record_basic/` — 节点级简单录制
- `../record_mcap/` — VCAP / MCAP 格式
- `../record_compression/` — 压缩对比
- `vlink/include/vlink/extension/bag_writer.h` / `bag_reader.h` / `bag_processor.h` — 完整接口
- 顶层 `doc/12-bag-recording.md` — 录制系统章节
