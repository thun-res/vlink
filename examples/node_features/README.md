# node_features — vlink 通信原语的高级节点特性

vlink 的六种通信原语（Publisher / Subscriber / Server / Client / Setter / Getter）共同继承自模板基类 `Node<ImplT, SecT>`，从这个基类得到一组**所有原语共用的**节点级能力。本目录通过 4 个示例覆盖这些"调一次但很关键"的特性。

读完本目录你能掌握：

- 节点生命周期管理（构造期 init / 运行期 deinit / 中断 / 重新 init）。
- 把节点绑定到指定 `MessageLoop` 控制回调线程。
- 通过 string key 的 property 系统配置 QoS / SchemaType / discovery 开关。
- 注册状态变化 handler 获取 publication_matched / sample_lost 等 DDS 标准事件。

## 子示例索引

| 示例 | 主题 | 关键 API |
|------|------|---------|
| `lifecycle/` | `kWithoutInit` + 手动 `init`/`deinit` + `interrupt` + `has_inited` | `Node::init`、`Node::deinit`、`Node::interrupt` |
| `message_loop_binding/` | `attach(&loop)` 控制回调线程；多节点共享 / 独立 loop | `Node::attach` |
| `properties/` | `set_property` / `get_property` / `set_ser_type` / `set_discovery_enabled` | 同名 |
| `status_monitoring/` | `register_status_handler` 监听 DDS 标准事件 + `get_cpu_usage` | 同名 |

## 推荐阅读顺序

1. **`lifecycle/`** —— 必看。理解 `kWithoutInit` 延迟初始化模式：构造对象时不创建底层 transport，等 `set_property` 配置完才 `init()`。
2. **`properties/`** —— 紧跟 lifecycle，学习在 init 之前怎么配置 QoS、schema、discovery 可见性。
3. **`message_loop_binding/`** —— 几乎所有 vlink 示例都用到 `attach(&loop)`；这里集中讲它的语义。
4. **`status_monitoring/`** —— 在生产部署里做监控埋点必看。

## 共同前置知识

- `../quickstart/` —— 六种通信原语的基础。
- `../base/message_loop_basic/` —— MessageLoop。
- `../qos/qos_basics/` —— QoS 字段对照（properties 的可调 key）。

## "在 init 之前能改、之后不能改"的字段

| 字段 | 影响 |
|------|------|
| QoS 子策略（reliability / history / durability / publish_mode 等） | 决定 transport 行为 |
| `ser_type` / `schema_type` | discovery 元数据；用于 bag / proxy |
| `discovery_enabled` | 是否被 ProxyServer / DiscoveryViewer 看到 |

这些都通过 `properties/` 里演示的 string property 系统配置。

## 配图

各示例下 `images/` 包含对应示意图：

- `lifecycle/images/node-lifecycle.png` —— 节点状态机
- `message_loop_binding/images/node-loop-binding.png` —— 多节点 attach 共享 / 独立 loop
- `properties/images/node-properties-config.png` —— properties 配置流程
- `status_monitoring/images/status-detail-events.png` —— DDS 标准事件类型

## 参考

- 顶层 `doc/02-node-lifecycle.md` —— 节点生命周期
- 顶层 `doc/03-message-loop.md` —— MessageLoop
- 顶层 `doc/04-qos.md` —— QoS
- `vlink/include/vlink/node.h` —— Node 基类接口
