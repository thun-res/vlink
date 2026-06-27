# 🧩 node_features — 节点级通用能力

vlink 的六种通信原语（Publisher / Subscriber / Server / Client / Setter / Getter）均继承自模板基类 `Node<ImplT, SecT>`，由该基类提供一组所有原语共用的节点级能力：生命周期控制、延迟初始化、阻塞唤醒等。本目录通过示例覆盖这类"调用频次低但影响关键"的接口。

掌握内容：

- 节点生命周期管理：延迟初始化、`init` / `deinit`、`interrupt`、`has_inited`。
- `kWithoutInit` 模式——在创建底层 transport 之前完成 QoS / property 配置。
- 阻塞接口（`wait_for_*`）的中断语义与可重入初始化。

## 📁 子示例索引

| 示例 | 主题 | 关键 API |
|------|------|----------|
| [`lifecycle/`](./lifecycle/) | `kWithoutInit` 延迟初始化、`init`/`deinit`、`interrupt`、`has_inited` | `Node::init`、`Node::deinit`、`Node::interrupt`、`Node::has_inited` |

## 🔑 在 init 之前可配置、之后冻结的字段

下列字段在 `init()` 期间被底层 transport 读取，须在 init 之前通过 property 系统设置；init 之后修改多数不再生效。

| 字段 | 影响 |
|------|------|
| QoS 子策略（reliability / history / durability / publish_mode 等） | transport 行为 |
| `ser_type` / `schema_type` | discovery 元数据，供 bag / proxy 使用 |
| `discovery_enabled` | 是否对 ProxyServer / DiscoveryViewer 可见 |

## 🖼️ 配图

- [`lifecycle/images/node-lifecycle.png`](./lifecycle/images/node-lifecycle.png) —— 节点状态机

## 🔗 前置与参考

| 主题 | 位置 |
|------|------|
| 六种通信原语基础 | [`../quickstart/`](../quickstart/) |
| MessageLoop | [`../base/message_loop_basic/`](../base/message_loop_basic/) |
| QoS 字段对照 | [`../qos/qos_basics/`](../qos/qos_basics/) |
| 节点生命周期专文 | 顶层 `doc/02-communication.md` |
| Node 基类接口 | `include/vlink/node.h` |
