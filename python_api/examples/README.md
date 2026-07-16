# python_api/examples — VLink Python 教学示例

本目录收录 **教学性** 的 Python 脚本，目的是帮助读者从零理解 VLink 的 Python
绑定如何使用。每个文件都是 **独立可运行** 的——可以直接 `python3 xxx.py`
执行，看到每个 demo 的 `[OK]` 行。

> 与 `python_api/test/` 的区别：`test/` 验证绑定契约（断言+回归），
> 而 `examples/` 用同样的代码风格 **讲清楚 API 的使用方式**，注释解释
> "WHY"，每个函数都可以直接复制成新脚本的起点。

---

## 文件清单

| 文件 | 主题 | 行数 |
|---|---|---|
| `demo_vlink_communication.py` | 三种通信模型完整演示 | ~1100 |
| `demo_vlink_bag.py`    | bag 记录与回放（数据闭环）| ~700 |

---

## `demo_vlink_communication.py` 章节速查

| Section | 演示函数 | 主题 |
|---|---|---|
| **1. Event · 原始字节** | `demo_pubsub_bytes` | Python `bytes` 通过 `Publisher` / `Subscriber` |
| | `demo_pubsub_vlink_bytes` | `vlink.Bytes` 类（`create` / `from_bytes`） |
| **2. Event · Schema 负载** | `demo_pubsub_protobuf` | Protobuf 消息（依赖 `google.protobuf` 与 `*_pb2.py`） |
| | `demo_pubsub_flatbuffers` | FlatBuffers 消息（依赖 `flatbuffers` 与 `flatc --python` 产物） |
| **3. Event · 零拷贝容器** | `demo_pubsub_raw_data` | `RawData` |
| | `demo_pubsub_camera_frame` | `CameraFrame` |
| | `demo_pubsub_point_cloud` | `PointCloud`（含 schema 编码 size_num/type_num 与 `set_vertical`） |
| | `demo_pubsub_proxy_data` | `ProxyData`（路由元数据封装） |
| | `demo_pubsub_occupancy_grid` | `OccupancyGrid`（占据栅格） |
| | `demo_pubsub_tensor` | `Tensor`（NN 输入输出） |
| | `demo_pubsub_object_array` | `ObjectArray` + 嵌套 `Object` POD |
| | `demo_pubsub_audio_frame` | `AudioFrame`（PCM 音频帧） |
| **4. Method · RPC** | `demo_rpc_sync` | `Client.invoke(req)` 同步调用 |
| | `demo_rpc_async` | `Client.invoke_async(req, cb)` 异步回调 |
| | `demo_rpc_with_zerocopy` | RPC 负载是 `Tensor`（模拟推理服务） |
| **5. Field · 状态同步** | `demo_field_push` | `Getter.listen(cb)` 推式订阅 |
| | `demo_field_pull` | `Getter.get()` 拉式读取 Getter 已接收的最新值 |

通用骨架（Section 1-3）：

```text
1. 先建 Subscriber 并 listen()，再建 Publisher（避免 wait_for_subscribers 错过匹配）
2. publisher.wait_for_subscribers(timeout_ms=2000)
3. 序列化 -> publish()
4. threading.Event 等待回调累计完成
5. publisher.deinit() / subscriber.deinit()
```

---

## `demo_vlink_bag.py` 章节速查

| Demo | 主题 |
|---|---|
| `demo_bag_simple_record_replay` | 最小化记录/回放循环（5 条消息往返） |
| `demo_bag_with_compression` | `LZAV` 压缩配置（`.vdb` 后端；MCAP 后端使用 Zstd 或 none） |
| `demo_bag_zerocopy_record_replay` | 记录 `CameraFrame` + `OccupancyGrid`，回放时分主题反序列化 |
| `demo_bag_playback_control` | `play / jump / pause / resume`，`rate=0.0` 最快回放 |
| `demo_bag_filter_urls` | `Config.filter_urls` 只回放指定主题子集 |
| `demo_bag_inspect_only` | `get_info()` 元数据只读检视（不回放） |
| `demo_bag_cursor_read` | `open_cursor()` + 迭代器同步拉取帧 |

`BagReader` 与默认异步 `BagWriter` 生命周期：

```text
1. cls.create(path, ...)            # 工厂；失败返回 None
2. 注册回调（register_*_callback）
3. async_run()                      # 启动后台线程
4. push(...) / play(...)            # 业务调用
5. BagWriter: wait_for_idle(timeout_ms) # 等待已接受写任务完成
6. quit() + wait_for_quit(timeout_ms)   # 停止后台循环
7. BagWriter: close() + 检查 fail()     # 完成 metadata/footer/manifest
```

默认未绑定 bag 插件的异步模式下，`BagWriter.push()` 返回非负时间戳表示队列已接受该帧；返回负值表示因队列或内存限制被拒绝，已接受的写入不会被新帧顶替。绑定插件时返回值遵循插件转发语义。

`BagWriter.Config.sync_mode=True` 时无需 `async_run()`、`wait_for_idle()` 或 `quit()`；创建后直接 `push()`，最后 `close()` 并检查 `fail()`。

`TriggerRecorder` 的 bag 插件由宿主显式加载并绑定，插件库名不再放进
`TriggerRecorder.Config`：

```python
plugin_host = vlink.Plugin()
bag_plugin = plugin_host.load_bag_plugin("my_bag_plugin", dir_name="plugins")
if bag_plugin is None:
    raise RuntimeError("failed to load bag plugin")

config = vlink.TriggerRecorder.Config()
recorder = vlink.TriggerRecorder(config)
recorder.bind_bag_interface(bag_plugin)
```

`load_bag_plugin()` 按 `BagPluginInterface` 接口版本 2.0 加载；返回对象可直接交给
`bind_bag_interface()`，`clear_bag_interface()` 用于恢复默认的采集时间顺序。`on_reset()` / `flush()`
属于插件与 C++ 宿主之间的内部生命周期，Python 侧不直接调用。reader 仅在自然完成的回放轮次调用
`flush()`；若在边界排空前观察到 `stop()` / `jump()`，下一会话通过 `on_reset()` 丢弃保留尾帧。

触发生命周期插件（`TriggerPluginInterface`）同样由宿主加载并绑定，`load_trigger_plugin()`
会在加载后调用一次插件的 `init(config)`：

```python
trigger_plugin = plugin_host.load_trigger_plugin("my_trigger_plugin", config="{}")
if trigger_plugin is None:
    raise RuntimeError("failed to load or init trigger plugin")

recorder.bind_trigger_interface(trigger_plugin)
```

`load_trigger_plugin()` 按 `TriggerPluginInterface` ABI 2.0 加载并调用 `init(config)`，加载或
`init()` 任一失败都返回 `None`；`clear_trigger_interface()` 用于解绑。两类插件都必须在
`async_run()` 之前或 recorder 停止后绑定。

---

## 运行

```bash
# 通信教学
python3 demo_vlink_communication.py

# Bag 教学
python3 demo_vlink_bag.py
```

依赖：
- VLink Python 绑定模块 `_vlink_nanobind`（由 `_vlink_nanobind` CMake target 构建；工程名为 `vlink-python_api`）
- `demo_pubsub_protobuf` 需要 `google.protobuf` 及 `protoc --python_out=. test/idl/test.proto` 生成的 `test_pb2.py`
- `demo_pubsub_flatbuffers` 需要 `flatbuffers` Python 模块及 `flatc --python test/idl/test.fbs` 生成的 `fbs/Message.py`

未安装的可选依赖不会让 demo 失败——对应函数打印 `[SKIP]` 并继续执行其余 demo。

---

## 复用这些代码

每个 demo 函数都被设计成 **可整段复制**：
- 去掉 `def demo_xxx():` 一行 + 缩进，就是一个独立脚本
- 在脚本顶部加 `import _vlink_nanobind as _vlink` 即可
- URL 中的 `intra://demo/...` 替换成你的实际 topic
- 主题约定遵循 VLink URL 方案：`<transport>://<topic_path>`，
  传输前缀可换为 `shm://`、`shm2://`、`dds://`、`zenoh://` 等

如果你的代码需要 **跨进程** 通讯（不是本地 `intra://`）：对于地址兼容的话题，可把 URL scheme
换成 `shm://`（共享内存，支持 transport loan）或 `dds://`（DDS 局域网/广域网），
demo 中的所有 Publisher / Subscriber / Server / Client / Setter / Getter
调用结构不需要改动；具体复制语义、依赖与 QoS 随后端而异。

---

## 命名约定

| 前缀 | 含义 |
|---|---|
| `demo_pubsub_*` | Event model 演示（Section 1-3） |
| `demo_rpc_*`    | Method model 演示（Section 4） |
| `demo_field_*`  | Field model 演示（Section 5） |
| `demo_bag_*`    | Bag 记录 / 回放演示 |

---

## 学习路径建议

1. **入门**：先读 `demo_pubsub_bytes`，理解 Pub/Sub 五步骨架
2. **进阶**：读 `demo_pubsub_camera_frame`（零拷贝） + `demo_rpc_sync`（RPC）
3. **应用**：读 `demo_bag_simple_record_replay`，串起 record→replay 数据闭环
4. **生产化**：参考 `demo_pubsub_protobuf` 与 `demo_bag_with_compression`，
   学会跨语言 schema 与 long-running 流的压缩参数
