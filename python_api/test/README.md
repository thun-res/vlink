# python_api/test — VLink Python 绑定测试

本目录承载 VLink Python 公开模块 `vlink`（底层扩展为 `_vlink_nanobind`）的功能与契约测试。

> 与 `python_api/examples/` 的区别：本目录目标是 **验证绑定**（断言+回归），
> 而 `examples/` 用同样的代码风格 **讲解 API 用法**。两者风格上接近，但
> 用途不同——本目录文件不适合作为教学入门读物。

---

## 文件清单

| 文件 | 角色 | 大小 |
|---|---|---|
| `test_vlink.py`           | 基础烟雾测试，每条 API 一个 happy-path 用例 | ~930 行 / 22 个测试 |
| `test_vlink_full.py`      | 完整覆盖测试，含错误路径 / 复杂场景       | ~2190 行 / 45 个测试 |
| `test_vlink_coverage.py`  | 绑定覆盖率回归，确保关键 attr / method 仍可见 | ~750 行 / 27 个测试 |

三个文件相互独立，可以单独运行。三层覆盖的设计意图：

- `test_vlink.py`：CI 烟雾测试入口（最快，跑通 = 绑定可用）
- `test_vlink_full.py`：本地 / 夜间深度跑，验证 schema / security / discovery 等高阶子系统
- `test_vlink_coverage.py`：变更绑定时跑，确保关键 API surface 没有被无意删除

---

## 运行

```bash
# 烟雾测试（最常用）
python3 test_vlink.py

# 完整测试
python3 test_vlink_full.py

# 覆盖率回归
python3 test_vlink_coverage.py
```

每个文件的 `if __name__ == "__main__":` 块顺序调用所有 `test_*()` 函数。
测试不依赖 pytest——所有断言走原生 `assert`，方便在最小依赖环境运行。

---

## `test_vlink.py` 函数索引

| 函数 | 验证 |
|---|---|
| `test_bytes`             | `vlink.Bytes` 工厂方法与 buffer protocol |
| `test_uuid`              | UUID 生成 |
| `test_pubsub`            | `Publisher` / `Subscriber` 基本流程 |
| `test_rpc`               | `Server` / `Client` 同步 + 异步调用 |
| `test_field`             | `Setter` / `Getter` push + pull |
| `test_message_loop`      | `MessageLoop` + `Timer` |
| `test_thread_pool`       | `ThreadPool` |
| `test_utils`             | `utils` / `helpers` 子模块 |
| `test_quantize`          | `quantize.encode_int16` / `decode_int16`（min/max 与 extent 重载） |
| `test_qos`               | `Qos` 与 `QosProfile` |
| `test_zerocopy_header`   | `Header` POD |
| `test_zerocopy_raw_data` | `RawData` 序列化往返 |
| `test_zerocopy_camera_frame` | `CameraFrame` |
| `test_zerocopy_point_cloud` | `PointCloud` schema 协议与 `set_vertical` |
| `test_zerocopy_python_ownership_guards` | Python buffer/NumPy 视图的所有权与生命周期保护 |
| `test_zerocopy_point_cloud_compress` | `PointCloud` 压缩与解压往返 |
| `test_zerocopy_proxy_data` | `ProxyData` |
| `test_zerocopy_occupancy_grid` | `OccupancyGrid` |
| `test_zerocopy_tensor` | `Tensor`（含 set_dtype/set_shape 顺序） |
| `test_zerocopy_object_array` | `ObjectArray` + nested `Object` POD |
| `test_zerocopy_message_parser` | 以 `ObjectArray` 覆盖统一解析入口、字段反射、集合边界、输入生命周期与 64 位整数精度 |
| `test_zerocopy_audio_frame` | `AudioFrame` |

---

## `test_vlink_full.py` 关键覆盖

- CRC / hash 工具函数 buffer-protocol 等价性
- `Node.set_ser_type` 与 `SchemaType` 枚举往返
- `SchemaData` 显式注册与 `register_schema_callback` 动态分发
- `BagWriter.create` / `BagWriter.filter_get` / `push_schema` / split callback / 显式 `close()`
- `BagReader.detect_schema` / `check` / `reindex` / `play` / status callback
- `DiscoveryViewer` 显式实例与过滤
- `TriggerRecorder` 启停、触发落盘与超时等待，以及宿主加载并绑定 `BagPluginInterface` / `TriggerPluginInterface`
- `UrlRemap`
- Security 模型
- `Setter.set` 变化通知与 `Getter.listen` 去重
- Bag 显式 `Frame.timestamp` 写入与读回

---

## `test_vlink_coverage.py` 关键覆盖

回归用 attribute / method 列表，确保下列 API surface 不被删除：

- `Node` 类方法集合（init / deinit / set_ser_type / set_property / ...）
- `BagWriter.Config` 的 19 个可写字段（含 writer 生命周期级 `sync_mode` 同步写入开关）
- `BagWriter` 的方法集合
- `BagReader.detect_schema` 与 status / info 字段
- `SchemaData.is_valid_type` / `is_real_type` / `convert_type`
- `quantize` 子模块的 int16 线性量化辅助函数
- `MemoryPool.Config.batch_size` 的默认值、可写绑定及构造传递
- 各零拷贝类型的 enum 值齐全

---

## 与 examples 的关系

本目录的代码与 `python_api/examples/` 的 demo 在 API 调用模式上是**镜像**关系：

| 同主题 | `examples/` 的教学版本 | `test/` 的验证版本 |
|---|---|---|
| Pub/Sub | `examples/demo_vlink_communication.py::demo_pubsub_bytes` | `test/test_vlink.py::test_pubsub` |
| RPC | `examples/demo_vlink_communication.py::demo_rpc_sync` | `test/test_vlink.py::test_rpc` |
| Field | `examples/demo_vlink_communication.py::demo_field_push` | `test/test_vlink.py::test_field` |
| 零拷贝 | `examples/demo_vlink_communication.py::demo_pubsub_camera_frame` | `test/test_vlink.py::test_zerocopy_camera_frame` |
| Bag | `examples/demo_vlink_bag.py::demo_bag_simple_record_replay` | `test/test_vlink_full.py::test_bag_extended` |

如果你想"学会 + 验证"，可以先读 `examples/` 的对应函数，再跑 `test/` 的对应函数确认。
