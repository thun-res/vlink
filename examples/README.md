# VLink 示例

本目录收录 VLink 的官方示例，每个子目录是一个独立、可编译的工程，聚焦一个主题。示例用于教学与端到端验证，不替代单元测试；完整说明见根目录 [README](../README.md) 与 [doc/](../doc/)。

当前共 14 个分类、29 个示例工程。

## 🔧 构建

推荐经 [VKit](https://github.com/thun-res/vkit) 构建整个工作区。若在 VLink 源码树中单独构建示例：

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_EXAMPLES=ON -DENABLE_EXAMPLES_ALL=ON
cmake --build build -j
```

- `ENABLE_EXAMPLES=ON`（默认 OFF）：仅编译 `samples/` 下的端到端样例。
- `ENABLE_EXAMPLES_ALL=ON`：在此基础上编译其余 13 个分类教程。

产物位于 `build/output/bin/`（`example_*` 与 `sample_*`）。缺少依赖的示例会自动跳过而不致构建失败。

## 📂 分类与示例

| 分类 | 示例 | 内容 |
| --- | --- | --- |
| [quickstart](quickstart) | `hello_pubsub` · `hello_rpc` · `hello_field` | 三种通信模型的最小可运行示例（`intra://`，无外部依赖） |
| [base](base) | `bytes_basic` · `logger_basic` · `message_loop_basic` · `timer` | Bytes、Logger、MessageLoop、Timer 等基础组件 |
| [serialization](serialization) | `basic_types` | Bytes / POD / `std::string` 等类型的自动序列化 |
| [communication](communication) | `event_advanced` · `field_advanced` · `method_sync` | 三模型的进阶用法：多订阅扇出、change 回调、同步 RPC |
| [url_guide](url_guide) | `url_basics` | URL 结构与运行时重映射 |
| [qos](qos) | `qos_basics` | QoS 基本配置与预定义 profile |
| [security](security) | `security_basic` | 消息级加密：对称 key seed、PBKDF2 与密钥不匹配演示 |
| [zerocopy](zerocopy) | `zerocopy_basic` | `loan` / `return_loan` 借贷接口与 `RawData` |
| [recording](recording) | `record_basic` | 节点级与全局消息录制 |
| [plugin](plugin) | `plugin_basic` | 插件加载与调用 |
| [proxy](proxy) | `proxy_api_basic` | ProxyAPI 客户端 |
| [c_api](c_api) | `c_pubsub` | C 语言绑定：发布订阅 |
| [node_features](node_features) | `lifecycle` | 节点 init / deinit / interrupt 生命周期 |
| [samples](samples) | `helloworld` · `ping_pong` · `shm_raw` · `someip_flat` · `ddsc_proto` · `dds_dynamic` · `fdbus_proto` · `pub_sub_fbs` · `dds_idl` | 围绕具体传输协议的端到端样例 |

## 🚀 端到端样例（samples）

针对各传输后端的完整可运行示例。默认构建（`ENABLE_EXAMPLES=ON`）仅编译此分类。

| 工程 | 传输 | 序列化 | 通信模型 | 说明 |
| --- | --- | --- | --- | --- |
| `helloworld` | 多后端可切换 | Protobuf | Method + Event | 推荐入口 |
| `ping_pong` | 多后端可切换 | Bytes | Event（双向） | 延迟测量 |
| `shm_raw` | `shm://` | Bytes | Method + Event + Field + Security | 全六原语 + 加密 |
| `someip_flat` | `someip://` | FlatBuffers | Method + Event + Field | 车载 SOME/IP 场景 |
| `ddsc_proto` | `ddsc://` | Protobuf | Method + Event | CycloneDDS + Protobuf |
| `dds_dynamic` | `dds://` | DynamicData | Method + Event | 异构类型同话题 |
| `fdbus_proto` | `fdbus://` | Protobuf | Method + Event + Field | FDBus 三模型 |
| `pub_sub_fbs` | `ddsc://` | FlatBuffers | Event | `UserT` / 零拷贝 `User*` |
| `dds_idl` | `dds://` | CDR | Method + Event | FastDDS 原生 IDL（默认禁用） |

`helloworld` 与 `ping_pong` 通过环境变量切换后端（如 `METHOD_TRANSPORT` / `EVENT_TRANSPORT`），用同一份代码验证不同传输。`dds_idl` 默认不构建（需 `fastddsgen` 工具链）；详见 [samples/README.md](samples/README.md)。

各后端的守护进程与依赖前置见 [传输后端与 URL、QoS](../doc/04-transport.md)。
