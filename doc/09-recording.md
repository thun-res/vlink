# 📹 9. 录制与回放

录制与回放将 VLink 的通信消息持久化到磁盘文件，并在离线时按原始时序或指定速率重放，面向调试复现、数据集采集、仿真回灌与离线分析等场景。它提供两套对称接口：写入端 `BagWriter` 与读取端 `BagReader`。读取端及默认异步写入模式在独立后台线程运行；写入端也可显式选择同步落盘。

![Bag 录制与回放架构](images/bag-architecture.png)

本章先建立录制管线的数据模型与生命周期，再给出写入、回放、合并的接口与边界条件。命令行等价工具 `vlink-bag` 见 [10-cli-tools.md](10-cli-tools.md)，图形化回放器 `vlink-player` 见 [11-visualization.md](11-visualization.md)。

---

## 📐 9.1 概念与数据模型

录制管线的基本单元是 `Frame`：一条消息在写入与回放中流经的统一结构。写入端将序列化后的 payload 连同元数据填入 `Frame` 推入 `BagWriter`；回放端从文件按时间戳逐帧重建 `Frame` 并交给输出回调。

| `Frame` 字段 | 类型 | 写入端语义 | 回放端语义 |
| --- | --- | --- | --- |
| `timestamp` | `int64_t` | 微秒时间戳；`< 0` 由 writer 按内部时钟自动分配，`>= 0`（含 `0`）原样记录 | 由 reader 填为相对录制起点的微秒偏移 |
| `url` | `std::string` | 消息所属 topic URL，不得为空 | 经插件 URL 重映射后的回放 URL |
| `data` | `vlink::Bytes` | 序列化后的 payload | 浅视图，仅在回调内有效，需外带先复制 |
| `ser_type` | `std::string` | 序列化类型名（如 `"demo.proto.PointCloud"`），原样存盘 | 由 reader 从元数据回填 |
| `schema_type` | `vlink::SchemaType` | schema 家族：`kProtobuf` / `kFlatbuffers` / `kZeroCopy` / `kRaw` / `kCdr` | 由 reader 从元数据回填 |
| `action_type` | `vlink::ActionType` | 消息动作，通常 `kPublish` | 原样回传 |

`ser_type` 标识 payload 的精确序列化类型，应用层据此选择解码器；`schema_type` 是供工具快速分派的粗粒度家族标签。两者关系与完整取值见 [03-serialization.md](03-serialization.md)。

默认异步写入与回放的生命周期均为"创建 → 启动后台线程 → 驱动数据 → 等待退出"：

![录制与回放流程](images/bag-record-playback-flow.png)

| 阶段 | 录制 | 回放 |
| --- | --- | --- |
| 创建 | `BagWriter::create(path, cfg)` | `BagReader::create(path)` |
| 注册 | — | `register_output_callback(cb)` |
| 启动 | `async_run()` | `async_run()` |
| 驱动 | `push(frame)` | `play(cfg)` |
| 退出 | `wait_for_idle()` + `quit()` + `wait_for_quit()` + `close()` | `auto_quit` 自停 或 `stop()` / `quit()`，再 `wait_for_quit()` |

> **约束**：默认异步 writer 必须在任何 `push()` 前调用 `async_run()`；`sync_mode=true` 的 writer 不启动循环，直接 `push()` 后 `close()`。reader 必须在 `play()` 前调用 `async_run()`。
> `BagWriter::close()` 在队列退出后写入最终 metadata、footer 或 split manifest；如需在对象析构前确认这些写入成功，应显式调用并检查 `fail()`。
> 若 writer 绑定了写侧 `BagPluginInterface`，停止生产后须先调用 `clear_bag_interface()`，让插件在循环仍可接收任务时 `flush()` 尾部帧；随后再按表中顺序等待队列、退出并 `close()`。未绑定插件时无需此步。

---

## 💾 9.2 存储格式选择

`create()` 按文件扩展名分派具体后端，更换格式仅需改扩展名；扩展名不在下表之内时 `create()` 返回空指针。两种格式共用同一套 `Config`、`push()`、`play()` 与 `register_output_callback()`，业务代码不区分后端。

| 扩展名 | 后端 | `kCompressAuto` 选用的编解码器 | 适用判据 |
| --- | --- | --- | --- |
| `.vdb` / `.vdbx` | SQLite 容器 | LZAV | 默认。读写快、占盘小，适合长时录制与回灌 |
| `.vcap` / `.vcapx` | MCAP 容器 | Zstandard | 需被 Foxglove Studio 直接打开可视化时（见 [§9.11](#-911-mcap-与-foxglove)） |

> 压缩默认关闭（`compress` 字段默认 `kCompressNone`）；上表第三列是把 `compress` 置为 `kCompressAuto` 后各后端实际选用的编解码器（见 [§9.4](#-94-录制配置)）。VDB 后端需编译时启用 `ENABLE_SQLITE`，MCAP 的 Zstandard 压缩需启用 `ENABLE_ZSTD`（二者默认均为 `ON`）。

---

## ✍️ 9.3 录制

最小录制流程为：创建 writer、启动后台线程、填充 `Frame` 推入。

```cpp
#include <vlink/extension/bag_writer.h>

auto writer = vlink::BagWriter::create("/data/recording.vdb");
writer->async_run();

vlink::Frame frame;
frame.timestamp   = -1;
frame.url         = "dds://sensors/lidar";
frame.ser_type    = "demo.proto.PointCloud";
frame.schema_type = vlink::SchemaType::kProtobuf;
frame.action_type = vlink::ActionType::kPublish;
frame.data        = payload;
writer->push(frame);

writer->wait_for_idle();
writer->quit();
writer->wait_for_quit();
writer->close();
if (writer->fail()) {
    // 处理异步写入或最终化失败
}
```

`push()` 的写入策略由创建 writer 时的 `Config::sync_mode` 固定，writer 生命周期内不能逐帧切换：

| 配置 / 方法 | 语义 |
| --- | --- |
| `sync_mode=false`（默认） | `push()` / `push_schema()` 异步入队，须先启动 writer 后台循环 |
| `sync_mode=true` | 所有帧、schema 及插件输出都在产生它们的线程同步写入，无需后台循环 |
| `*writer << frame` | 与 `push(frame)` 使用相同的 writer 级策略；写失败时置位 `fail()` |

`push()` 线程安全，可直接在通信回调内调用。未绑定 bag 插件的异步模式下，返回非负值表示该帧已被队列接受；队列或内存上限触发拒绝时返回负值，已接受的写入不会为新帧让位。绑定插件时，返回值遵循插件转发语义，见 [§9.12](#-912-多文件合并回放)。同步模式会阻塞到落盘，可能违反实时截止期，应在创建 writer 时明确选择，不能与异步写入混用。

### 9.3.1 Schema 嵌入

录制 Protobuf / FlatBuffers 等带 schema 的消息时，可将 schema 描述符一并写入文件，供离线自省与可视化解析消息结构：

```cpp
vlink::SchemaData schema;
schema.name        = "sensors.LidarPoint";
schema.encoding    = "protobuf";
schema.schema_type = vlink::SchemaType::kProtobuf;
schema.data        = proto_file_descriptor_bytes;
writer->push_schema(schema);
```

亦可用 `register_schema_callback()` 注册解析器，在某 `ser_type` 首次出现时再懒加载其 schema。

---

## ⚙️ 9.4 录制配置

`Config` 控制压缩、文件分割与录制标签，未设字段保持默认。下表为高频字段，完整字段见 `bag_writer.h`。

```cpp
vlink::BagWriter::Config config;
config.compress        = vlink::BagWriter::kCompressAuto;
config.split_by_size   = 1024LL * 1024 * 1024;
config.split_by_time   = 60LL * 1000;
config.max_split_count = 10;
config.tag_name        = "test_run_001";

auto writer = vlink::BagWriter::create("/data/recording.vdbx", config);
```

| 字段 | 默认 | 语义 |
| --- | --- | --- |
| `compress` | `kCompressNone` | `kCompressAuto` 由后端选压缩（VDB 用 LZAV、MCAP 用 Zstd）；`kCompressNone` 关闭 |
| `split_by_size` | 1 GiB | 按文件大小分割阈值（字节），`0` 关闭 |
| `split_by_time` | `0` | 按时间分割间隔（毫秒），`0` 关闭 |
| `max_split_count` | `0` | 分包文件保留上限；`0` 不限制，超限后删除 manifest 中最旧的分包 |
| `tag_name` | 空 | 录制标签，写入文件头供检索 |
| `sync_mode` | `false` | `true` 全程同步直写且不启动 VDB 周期 cache flush；`false` 全程经后台队列并启用周期 flush |

`cache_size` 始终只表示 VDB 的事务提交字节阈值或 VCAP 的 chunk 大小，不承担模式开关语义。

`max_split_count` 仅对 `.vdbx` / `.vcapx` 分包容器生效。轮转时优先保留新分包；达到上限后从 manifest
移除并删除最旧分包。若上限清理所需的 manifest 更新或文件删除失败，本次轮转失败；删除失败时会恢复原清单，不会继续写入新分包。

分割产生新文件时可注册回调获知文件名。第二参数 `before` 决定回调在新文件打开前还是后触发：

```cpp
writer->register_split_callback(
    [](int split_index, const std::string& split_filename) {
        VLOG_I("split #", split_index, " -> ", split_filename);
    },
    /*before=*/false);
```

---

## ▶️ 9.5 回放

最小回放流程为：创建 reader、注册输出回调、启动后台线程、调用 `play(cfg)`。

```cpp
#include <vlink/extension/bag_reader.h>

auto reader = vlink::BagReader::create("/data/recording.vdb");

reader->register_output_callback(
    [](const vlink::Frame& frame) {
        VLOG_I("ts=", frame.timestamp, " url=", frame.url,
               " size=", frame.data.size());
    });

reader->async_run();

vlink::BagReader::Config cfg;
cfg.rate      = 1.0;
cfg.times     = 1;
cfg.auto_quit = true;   // 播完后停止后台循环线程，使 wait_for_quit() 返回
reader->play(cfg);

reader->wait_for_quit();
```

> **约束**：`wait_for_quit()` 仅在循环线程退出后返回。一次性回放须置 `cfg.auto_quit = true`，否则播完后线程继续运行、`wait_for_quit()` 永不返回；常驻回放器则不设此标志，改由 `stop()` / `quit()` 收尾。

输出回调在 reader 的后台线程触发；reader 自动从文件元数据回填 `frame.ser_type` / `schema_type`，应用层据此选择反序列化方式（`frame.data` 的浅视图语义见 [§9.1](#-91-概念与数据模型) `Frame` 表）。

除输出回调外，可注册状态、就绪与完成回调：

| 回调 | 触发时机 | 入参 |
| --- | --- | --- |
| `register_output_callback(cb)` | 每回放一帧 | `const Frame&` |
| `register_status_callback(cb)` | 状态变迁 | `Status`（`kStopped` / `kPaused` / `kPlaying`） |
| `register_ready_callback(cb)` | 文件打开并索引完成 | 无 |
| `register_finish_callback(cb)` | 回放会话结束 | `bool is_interrupted` |

---

## 🎚️ 9.6 回放参数与控制

`play()` 接收 `Config`，常用字段如下：

| 字段 | 默认 | 语义 |
| --- | --- | --- |
| `rate` | `1.0` | 速率倍数，`2.0` 加速、`0.5` 慢放 |
| `times` | `1` | 循环次数；`<= 0`（含 `vlink::BagReader::kInfinite`，其值为 `-1`）表示无限循环 |
| `begin_time` / `end_time` | `0` / `0` | 回放时间窗（毫秒，相对录制起点），`0` 表示从头 / 到尾 |
| `filter_urls` | 空 | URL 白名单，空集合表示回放全部 |
| `auto_quit` | `false` | 播完后停止后台循环线程 |

```cpp
vlink::BagReader::Config cfg;
cfg.rate        = 2.0;
cfg.times       = vlink::BagReader::kInfinite;
cfg.begin_time  = 10 * 1000LL;
cfg.end_time    = 30 * 1000LL;
cfg.filter_urls = {"dds://sensors/lidar"};
cfg.auto_quit   = true;
reader->play(cfg);
```

回放控制方法作用于运行中的 reader：

| 方法 | 语义 |
| --- | --- |
| `pause()` / `resume()` | 暂停 / 继续 |
| `pause_to_next()` | 单步：发一帧后再次暂停 |
| `jump(begin_time, rate, times, force_to_play)` | 跳转到指定时间戳（毫秒）并应用新参数 |
| `stop()` | 中止会话并回退到起点 |
| `get_status()` / `get_timestamp()` | 查询当前状态 / 当前时间戳 |

文件损坏时可异步修复，三者均返回 `std::future<bool>`：`check()` 校验完整性、`reindex()` 重建索引、`fix(/*rebuild=*/false)` 修复（`true` 为从头重建）。

---

## ⏩ 9.7 游标顺序读取

除 `play()` 的定时回放外，`BagReader` 另提供一条**同步顺序读取**通道：游标。它按录制顺序逐帧把数据交还调用线程，**无后台线程、无帧间延时**，也不依赖 `async_run()` / `play()`，适合离线批处理、转码、抽帧统计等不需要按真实时序节流的场景。

```cpp
auto reader = vlink::BagReader::create("/data/recording.vdb");

vlink::BagReader::Config cfg;
cfg.begin_time  = 10 * 1000LL;
cfg.end_time    = 30 * 1000LL;
cfg.filter_urls = {"dds://sensors/lidar"};
reader->open_cursor(cfg);            // 也可用无参 open_cursor() 全量读取

vlink::Frame frame;
while (*reader >> frame) {           // 等价于 while (reader->read_next(frame))
    VLOG_I("ts=", frame.timestamp, " url=", frame.url,
           " size=", frame.data.size());
}
```

| 方法 | 语义 |
| --- | --- |
| `open_cursor(config)` | 按 `config` 打开（或重置）游标；成功返回 `true`，打开失败置位 `fail()` 并返回 `false` |
| `open_cursor()` | 等价于以默认 `Config` 打开，全量、不过滤、按录制顺序遍历 |
| `read_next(out)` | 读下一帧填入 `out`；到末尾置位 `eof()`、出错置位 `fail()`，二者均返回 `false` |
| `*reader >> frame` | `read_next()` 的流式别名，返回 `*this`，可 `while (reader >> frame)` |
| `eof()` / `fail()` | 是否已到文件末尾 / 上次游标操作是否失败 |
| `operator bool()` | 游标是否仍可读（未到末尾且未失败），供流式循环判停 |

`read_next()` / `operator>>` 在首次调用时会自动以默认 `Config` 懒打开游标，因此仅在需要过滤或设置时间窗时才必须显式 `open_cursor()`；再次调用 `open_cursor()` 会重绕游标、重新套用 `config` 并清除 `eof()` / `fail()` 状态。

> **行为约束**：游标仅 `Config::filter_urls` 与 `begin_time` / `end_time` 时间窗生效，`rate` / `times` / `auto_*` / `skip_blank` 等定时回放参数一律被忽略；已绑定的 `BagPluginInterface` 的 URL 重映射与排除仍然生效，但其 `on_read()` 拦截（仅回放路径触发）不会运行。游标为单线程，不要与同一 reader 上正在进行的 `play()` 会话并发驱动。`frame.data` 是浅视图，仅在下一次 `read_next()` / `operator>>` 前有效，需外带先复制。

---

## 🔎 9.8 读取元数据

打开文件后无需启动回放即可读取元数据，用于核对录制时长、消息量与各 URL 的频率：

```cpp
const auto& info = reader->get_info();
VLOG_I("file: ", info.file_name);
VLOG_I("duration: ", info.total_duration / 1000, " s");
VLOG_I("messages: ", info.message_count);

for (const auto& meta : info.url_metas) {
    VLOG_I("  ", meta.url, " count=", meta.count,
           " freq=", meta.freq, " Hz ser=", meta.ser_type);
}
```

`Info::url_metas` 每条对应一个录制 URL，含 `count`（消息数）、`freq`（平均频率，Hz）、`ser_type` 与 `schema_type` 等字段，适合在回放前驱动解码器分派。各 `ser_type` 取值与对应 `schema_type` 的完整对照见 [03-serialization.md](03-serialization.md)。

---

## 🔗 9.9 与通信 API 集成

最省事的录制方式是节点自带的 **drop-in** 接口 `Node::set_record_path(path)`：在任意 `Publisher` / `Subscriber` / `Client` / `Server` / `Setter` / `Getter` 上设置后，流经该节点的消息自动写入指定 bag，无需手动序列化或操作 `BagWriter`。

```cpp
vlink::Subscriber<LidarPoint> sub("dds://sensors/lidar");
sub.set_record_path("/data/lidar.vdb");   // 后缀须为 .vdb/.vdbx/.vcap/.vcapx
```

> **约束**：`intra://` 节点**不支持节点级 `set_record_path()`**——调用会触发致命日志（`VLOG_F`，抛出 `RuntimeError`）。对支持的传输，路径会交给 `BagWriter::filter_get()`；后缀不在 `.vdb`/`.vdbx`/`.vcap`/`.vcapx` 之内时返回空指针，节点录制静默关闭。DDS CDR 以包含 encapsulation header 的完整字节负载录制和回放。`VLINK_BAG_PATH` 全局 writer 可捕获经过 Bytes 序列化路径的普通 intra 消息与 DDS CDR，但不捕获 `IntraData` 直通消息；需要在录制前转码、过滤或重排时，应使用下面的手动 `BagWriter` 接管。

需要完全掌控帧内容时，把 `BagWriter` 注入订阅回调，回放时将 reader 输出转回 `Publisher`，是与业务通信结合的标准用法。

录制端：`Subscriber` 回调内序列化后推入 writer。

```cpp
#include <vlink/vlink.h>
#include <vlink/extension/bag_writer.h>

auto writer = vlink::BagWriter::create("/data/lidar.vdb");
writer->async_run();

vlink::Subscriber<LidarPoint> sub("dds://sensors/lidar");
sub.listen([&](const LidarPoint& msg) {
    vlink::Bytes bytes;
    vlink::Serializer::serialize(msg, bytes);

    vlink::Frame frame;
    frame.timestamp   = -1;
    frame.url         = "dds://sensors/lidar";
    frame.ser_type    = "demo.proto.PointCloud";
    frame.schema_type = vlink::SchemaType::kProtobuf;
    frame.action_type = vlink::ActionType::kPublish;
    frame.data        = bytes;
    writer->push(frame);
});
```

回放端：reader 输出回调内反序列化后经 `Publisher` 重新发出。

```cpp
auto reader = vlink::BagReader::create("/data/lidar.vdb");
vlink::Publisher<LidarPoint> pub("dds://sensors/lidar");

reader->register_output_callback([&](const vlink::Frame& frame) {
    if (frame.url == "dds://sensors/lidar") {
        LidarPoint msg;

        if (vlink::Serializer::deserialize(frame.data, msg)) {
            pub.publish(msg);
        }
    }
});

reader->async_run();

vlink::BagReader::Config cfg;
cfg.rate = 1.0;
reader->play(cfg);
```

---

## 🌐 9.10 进程级自动录制

设置环境变量 `VLINK_BAG_PATH` 可让进程在不改代码的前提下自动开启全局录制：

```bash
export VLINK_BAG_PATH=/data/auto_record.vdb   # 后缀须为 .vdb/.vdbx/.vcap/.vcapx
./my_vlink_app
```

代码中经 `vlink::BagWriter::global_get()` 取得进程级共享 writer（未设环境变量时返回空指针）；按路径取或建共享 writer 用 `vlink::BagWriter::filter_get("/data/x.vdb")`。环境变量完整说明见 [13-integration.md](13-integration.md)。

---

## 📊 9.11 MCAP 与 Foxglove

将录制内容导入 Foxglove Studio 可视化时，把扩展名改为 `.vcap`，其余代码不变（格式特性见 [§9.2](#-92-存储格式选择)）：

```cpp
auto writer = vlink::BagWriter::create("/data/recording.vcap");
```

MCAP 格式的文件头内嵌 schema 与 channel 元数据并支持随机访问。配合 [§9.3.1](#931-schema-嵌入) 写入 schema 后，可在 Foxglove 中直接解析消息结构。

---

## 🧩 9.12 多文件合并回放

录制按大小或时间分割会产生多个文件；多源或乱序数据需按真实数据时间重新排序时，用 `vlink::BagProcessor` 做时间滑窗重排。它维护一个缓冲窗口，将多个 reader 汇入的帧按 `push(data_timestamp, frame)` 传入的 data-plane time 升序输出，给"迟到但更早"的帧一个排到已缓存帧之前的机会。仅当窗口内最旧帧与最新帧的 data-plane time 跨度达到 `Config::min_cache_time`（默认 `500` ms）才释放最旧帧；墙钟时间绝不推进该窗口，生产者静默后的尾帧仅由显式 `flush()` 排空。`BagProcessor::Config` 另有两个可调字段：`max_cache_size`（默认 256 MiB，缓冲帧总载荷字节上限）与 `max_jump_time`（默认 1 h，data-plane time 单次跳变的绝对上限）；超过该阈值时，该帧按上一有效数据时间加录制时间 `Frame::timestamp` 的增量回退，避免异常数据时间跨越污染排序轴。输出时 `Frame::timestamp` 会映射到排序后的 data-plane-time 轴，并在单个 flush 段内保持严格单调递增；`flush()` 同步排空并重置时间锚点，`reset()` 则等待正在执行的输出回调结束、丢弃缓存而不输出并重置全部锚点。如果某帧无法提取 data-plane time，可传入负值，processor 会按上一帧的数据时间叠加 `Frame::timestamp` 差值补齐（无前序锚点时保持 `-1` 并排在最前）。`BagProcessor` 的输出回调在其独立 worker 线程触发，不得在该回调内调用 `flush()` / `reset()`，也不得让 `push()` 与边界操作并发。processor 只重排帧并映射 `Frame::timestamp`，不会改写 payload 内的时间字段；来自不同时间基准的 `time_meas` 即使各自单调，也不能据此判断全 topic 的 payload 字段全局单调。

```cpp
#include <vlink/extension/bag_reader.h>
#include <vlink/extension/bag_processor.h>
#include <vlink/vlink.h>

int64_t extract_data_timestamp(const vlink::Bytes& data);  // parse from your payload header

int main() {
    vlink::Logger::init("merge-playback");

    vlink::Publisher<vlink::Bytes> pub("dds://replay/merged");
    vlink::BagProcessor processor;

    processor.register_output_callback(
        [&](const vlink::Frame& f) { pub.publish(f.data); });

    std::vector<std::shared_ptr<vlink::BagReader>> readers;

    for (const auto& file : {"/data/drive_0.vdb", "/data/drive_1.vdb", "/data/drive_2.vdb"}) {
        auto reader = vlink::BagReader::create(file);
        reader->register_output_callback(
            [&](const vlink::Frame& frame) {
                const int64_t data_timestamp = extract_data_timestamp(frame.data);
                processor.push(data_timestamp, frame);
            });
        reader->async_run();
        readers.push_back(reader);
    }

    vlink::BagReader::Config cfg;
    cfg.rate      = 1.0;
    cfg.auto_quit = true;   // 每个 reader 播完后自停，使下面的 wait_for_quit() 能返回

    for (auto& reader : readers) {
        reader->play(cfg);
    }

    for (auto& reader : readers) {
        reader->wait_for_quit();
    }

    processor.flush();

    return 0;
}
```

`BagProcessor` 同时是 `BagPluginInterface` 派生插件的基本构件，用于录制前 / 回放前的重排与转码：写侧由 `BagWriter::bind_bag_interface()` 在落盘前调用插件的 `on_write()`，读侧由 `BagReader::bind_bag_interface()` 在回放前调用 `on_read()`，二者均经插件内部的 `do_callback()` 重新发出。reader 在每个顶层 `play()` / `jump()` 读会话开始、ready 回调之前调用插件 `on_reset()`；重排插件应在此调用 `BagProcessor::reset()`，确保 `stop()` / `jump()` 留下的缓存和时间锚点不会污染新会话。只有自然完成的回放轮次才调用 `flush()`，保留尾帧并隔离下一轮；若在边界排空前观察到 `stop()` / `jump()`，则跳过 `flush()`，由下一会话的 `on_reset()` 丢弃尾帧。`on_read()` 收到的帧已填充经 `convert_url_meta()` 处理后的有效 `ser_type` / `schema_type`；若 `on_read()` 另行更改输出 URL，保留类型表示 payload 类型不变，若要按新 URL 的已知元数据重新解析则须先清空 `ser_type` 并把 `schema_type` 设为 `kUnknown`，转码时则应显式填写新类型。`BagPluginInterface` 接口版本为 2.0，加载与完整生命周期见 [13-integration.md](13-integration.md)。

---

## 🚨 9.13 触发录制与内存打点

前述录制均为持续录制：`BagWriter` 收到的每一帧都落盘，磁盘占用随录制时长线性增长。另有一类需求是长期运行、但只保留少量关键事件前后的片段——例如量产车辆在碰撞、急刹、接管等异常发生时才需要留存现场数据。为此提供触发录制引擎 `vlink::TriggerRecorder`：它在内存中滚动缓冲，仅在事件触发时把触发点前后的窗口落盘，磁盘占用与触发次数相关而与运行时长无关，是行车记录仪 / 事件数据记录器（EDR）模式。

| 维度 | 持续录制（`BagWriter`） | 触发录制（`TriggerRecorder`） |
| --- | --- | --- |
| 落盘时机 | 每帧即时落盘 | 仅事件触发时落盘触发点前后窗口 |
| 磁盘占用 | 随录制时长线性增长 | 与触发次数相关，与时长无关 |
| 数据留存 | 全时段完整留存 | 仅关键事件前后片段 |
| 典型场景 | 调试复现、数据集采集、仿真回灌 | 长期运行的异常事件采集（EDR） |

**机制**　引擎经服务发现订阅总线上全部话题的原始 `Bytes`，为每个 URL 维护一个滚动的内存环形缓冲，仅保留最近一段历史（不落盘）。收到触发时，取触发点前 `pre` 毫秒加触发点后 `post` 毫秒窗口内的帧，在内存中按采集时刻（capture time，帧到达引擎的单调时刻）稳定排序后写入 bag 文件（`.vdb` 或 `.vcap`），并按保留上限对历史文件轮转。采集回调是热路径，运行在传输分发线程上，仅取 URL 级锁并对 payload 做一次拷贝，摊销为 O(1)。

**两类插件（不可混淆）**　引擎通过两个不同接口、两个不同方法接受两类插件：

- **bag 重排插件**（`BagPluginInterface`，仅经 `bind_bag_interface()` 绑定）位于落盘写入路径内部：其 `on_write()` 从每帧 payload 解析真实的**数据面时间**（data-plane time），并据此做滑窗重排后再持久化——与在线 `BagWriter` 的机制完全一致（见 [§9.12](#-912-多文件合并回放)）。`TriggerRecorder` 不接收插件库名或搜索目录，也不自行动态加载；宿主须先直接创建接口实例，或用 `Plugin` 加载共享库，再把所得 `shared_ptr<BagPluginInterface>` 绑定给 recorder，并保证其所需生命周期。**未绑定该接口时，引擎按采集时刻顺序落盘**，无需解析 payload。TriggerRecorder 自身的后台循环直接同步写入 bag；插件工作线程及 `flush()` 尾帧也绕过 `BagWriter` 任务队列，以免一次 dump 瞬间生成第二份排队窗口。
- **触发插件**（`TriggerPluginInterface`，经 `bind_trigger_interface()` 绑定）只观察引擎生命周期——最重要的是 `on_dump_finished()`，即一份 bag 写完后上传或归档的入口；它从不改写帧。动态加载宿主在绑定前调用 `init(config)`；程序化绑定方如需参数，应自行先调用 `init()`。

**恒定保留**　每个 URL 恒定保留 `effective_pre + max_post_all + 2 * retention_guard` 时长的历史（`only_back` 的 `effective_pre` 为 0，其他 URL 为各自的 `pre`），其中 `max_post_all` 为所有 URL 中最大的生效 `post` 窗口。该全局最大值只决定环形缓冲的保留时长；单次 dump 在受理时筛选参与 URL，并按这些 URL 的最大有效 `post` 调度落盘，其中有效值为触发请求 `post` 与 URL 配置 `post` 的较小值。该值大于 0 时等待它加 `retention_guard`，为 0 或没有 URL 命中时立即调度。这样采集热路径无需判断"当前是否有触发在进行"，代价是内存全局耦合：单个 URL 配置过大的 `post` 会抬高每个 URL 的保留时长与内存占用，内存紧张时应约束 `post`。启用 bag 重排插件时，其滑窗会在落盘期间额外持有部分窗口副本，峰值内存可接近窗口大小的两倍。

**per-URL 窗口**　每个 URL 可独立覆盖默认的 `pre` / `post` 窗口、单包上限与该 URL 的缓冲字节上限，并可设 `only_front`（仅录触发前）或 `only_back`（仅录触发后）。据此可为不同话题裁剪保留策略，例如相机保留触发前 60 s、触发后 5 s，雷达仅保留触发前 15 s，制动信号仅录触发后一段；各 URL 窗口互不相同。全局另支持 URL 白 / 黑名单（精确匹配）、压缩、字节上限溢出策略（淘汰最旧帧 / 丢弃新帧），以及 dump 输入流控——`sleep_interval` / `sleep_time_ms` 每向写入链路提交一定字节即休眠一次；无异步 bag 插件时这直接约束同步写盘节奏，带 worker/重排插件时仅约束投喂速度，`flush()` 尾帧不保证同样的 IO 节流。

```cpp
#include <chrono>
#include <iostream>
#include <thread>

#include <vlink/base/plugin.h>
#include <vlink/extension/bag_plugin_interface.h>
#include <vlink/extension/trigger_recorder.h>

vlink::TriggerRecorder::Config config;
config.dump_dir        = "/data/edr";
config.default_pre_ms  = 15'000;   // 默认触发前 15 s
config.default_post_ms = 0;        // 默认不录触发后数据

vlink::TriggerRecorder::UrlConfig radar;
radar.pre_ms  = 15'000;            // 该 URL 触发前 15 s
radar.post_ms = 0;                 // 该 URL 不录触发后
config.url_overrides["dds://radar/points"] = radar;

vlink::TriggerRecorder recorder(   // 构造即校验配置并获取全部资源,失败抛 RuntimeError
    config,
    [](const std::string& url, vlink::InitType type) {
      return vlink::TriggerRecorder::RawSub::create_shared(url, type);
    });

// 可选：插件库名与搜索路径属于宿主配置；由宿主加载后显式绑定
vlink::Plugin bag_plugin_loader;
if (auto bag_plugin = bag_plugin_loader.load<vlink::BagPluginInterface>("edr_reorder", 2, 0)) {
  recorder.bind_bag_interface(bag_plugin);
}

recorder.async_run();
recorder.invoke_task([]() {}).wait();  // 等待 on_begin() 完成

// 外部事件发生时,落盘触发点前后窗口
vlink::TriggerRecorder::TriggerParams params;
params.reason = "hard-brake";      // 写入 bag 标签
std::string dump_path;
if (recorder.dump(params, dump_path)) {
  std::cout << dump_path << std::endl;
  while (recorder.is_dumping()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

// 退出时
recorder.quit();
recorder.wait_for_quit();
```

`RawSubFactory` 按引擎传入的 URL 和 `InitType` 创建订阅器，并可在返回前设置 `dds.ip` 等必须早于 `init()` 生效的宿主侧 transport 属性，但不应自行初始化或监听；getter 语义、丢帧统计、schema、发现开关、`init()` 与 `listen()` 仍由引擎统一处理。factory 写在宿主编译单元中，使 URL 的头文件内联分派能看到宿主所链接 transport target 传播的 `VLINK_SUPPORT_*`。若宿主没有直接链接对应共享后端，可在进程首次初始化 URL 前设置 `VLINK_URL_PLUGINS=auto` 允许已知 transport 按需加载，或把该变量设为模块列表进行显式预加载；为空或设为 `none` 时关闭插件加载，详见 [传输后端与 URL](04-transport.md)。

`TriggerRecorder` 继承 `MessageLoop`，与其他 VLink 循环类一样直接使用基类的 `async_run()`、`quit()` 和 `wait_for_quit()`；等待 post 窗口与写盘都在它自身的循环线程上执行。`dump()` 为非阻塞：它记下触发时刻后异步完成"等待本次参与 URL 的最大有效 `post` 窗口 → 重排 → 写盘"，其间若再次触发则被拒绝（落盘串行化）。两参数重载在任务成功入队时同步返回已经确定的输出路径，包括自动命名的防覆盖后缀。若已接受的 dump 尚在等待 post 窗口，`quit()` 遵循 `MessageLoop` 语义终止该延时任务；需要保留该文件时，应先等待 `is_dumping()` 变为 `false` 再退出。`TriggerParams` 可临时缩小本次的 `pre` / `post`（只能相对配置缩小，因环形缓冲仅按配置时长保留历史）、指定输出路径 / 文件名，并用可同时设置的 URL 精确 `whitelist` / `blacklist` 及不区分大小写的子串做两层过滤；精确名单先保留白名单再剔除黑名单，`black_mode` 仅切换子串过滤的白名单 / 黑名单模式。落盘产物即标准 bag 文件，可由 `BagReader`、游标读取或图形化 `vlink-player` 进一步处理。

命令行等价工具 `vlink-trigger`（`daemon` 常驻缓冲 / `dump` 发起触发落盘）及其完整 JSON 配置项见 [10-cli-tools.md](10-cli-tools.md)。

---

## 📦 9.14 完整示例

### 9.14.1 录制

每 1 GiB 分割并开启压缩，捕获终止信号后退出录制循环。

```cpp
#include <atomic>

#include <vlink/extension/bag_writer.h>
#include <vlink/base/logger.h>
#include <vlink/base/utils.h>

int main() {
    vlink::Logger::init("bag-demo");

    vlink::BagWriter::Config config;
    config.compress           = vlink::BagWriter::kCompressAuto;
    config.split_by_size      = 1024LL * 1024 * 1024;
    config.split_name_by_time = true;
    config.tag_name           = "field_test_2026";

    auto writer = vlink::BagWriter::create("/data/field_test.vdb", config);
    writer->async_run();

    std::atomic_bool running{true};
    vlink::Utils::register_terminate_signal([&](int) { running.store(false); });

    int seq = 0;
    while (running.load()) {
        vlink::Bytes data = vlink::Bytes::create(256);
        std::memset(data.data(), seq & 0xFF, 256);

        vlink::Frame frame;
        frame.timestamp   = -1;
        frame.url         = "intra://sensor/imu";
        frame.ser_type    = "raw";
        frame.schema_type = vlink::SchemaType::kRaw;
        frame.action_type = vlink::ActionType::kPublish;
        frame.data        = data;
        writer->push(frame);
        seq++;

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    writer->wait_for_idle();
    writer->quit();
    writer->wait_for_quit();
    writer->close();

    if (writer->fail()) {
        VLOG_E("recording finalization failed");
        return 1;
    }

    VLOG_I("recording saved");
    return 0;
}
```

### 9.14.2 回放

```cpp
#include <vlink/extension/bag_reader.h>
#include <vlink/base/logger.h>

int main(int argc, char* argv[]) {
    vlink::Logger::init("bag-play");

    if (argc < 2) {
        VLOG_F("usage: ", argv[0], " <bag_file>");
    }

    auto reader = vlink::BagReader::create(argv[1]);

    const auto& info = reader->get_info();
    VLOG_I("file: ", info.file_name, " messages: ", info.message_count);

    reader->register_output_callback(
        [](const vlink::Frame& frame) {
            VLOG_D("ts=", frame.timestamp, " url=", frame.url,
                   " size=", frame.data.size());
        });

    reader->async_run();

    vlink::BagReader::Config cfg;
    cfg.rate      = 1.0;
    cfg.times     = 1;
    cfg.auto_quit = true;
    reader->play(cfg);

    reader->wait_for_quit();
    return 0;
}
```

> 可运行示例参见 `examples/recording/`：`record_basic` 演示节点级 `set_record_path()`（见 [§9.9](#-99-与通信-api-集成)）的 drop-in 录制。

---

## 📚 相关文档

- 命令行录制 / 回放工具 `vlink-bag`（record/play/info/clone/merge/check/reindex/fix/tag）：[10-cli-tools.md](10-cli-tools.md)
- 命令行触发录制工具 `vlink-trigger`（daemon/dump）：[10-cli-tools.md](10-cli-tools.md)（引擎 `TriggerRecorder` 见本章 [§9.13](#-913-触发录制与内存打点)）
- 图形化回放器 `vlink-player`：[11-visualization.md](11-visualization.md)
- 序列化类型与 schema：[03-serialization.md](03-serialization.md)
- 录制相关环境变量、`BagPluginInterface` 插件的加载与改写钩子：[13-integration.md](13-integration.md)（`BagProcessor` 时间滑窗重排见本章 [§9.12](#-912-多文件合并回放)）
