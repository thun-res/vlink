# 🧩 node_features — 节点级通用能力

vlink 的六种通信原语（Publisher / Subscriber / Server / Client / Setter / Getter）均继承自模板基类 `Node<ImplT, SecT>`，由该基类提供一组所有原语共用的节点级能力：生命周期控制、延迟初始化、阻塞唤醒等。本目录通过示例覆盖这类"调用频次低但影响关键"的接口。

掌握内容：

- 节点生命周期管理：延迟初始化、`init` / `deinit`、`interrupt`、`has_inited`。
- `kWithoutInit` 模式——在创建底层 transport 之前完成后端 property、序列化类型和发现上报等初始化配置；endpoint QoS 由 URL / Conf 提供。
- 阻塞接口（`wait_for_*`）的中断语义与可重入初始化。

## 📁 子示例索引

| 示例 | 主题 | 关键 API |
|------|------|----------|
| [`lifecycle/`](./lifecycle/) | `kWithoutInit` 延迟初始化、`init`/`deinit`、`interrupt`、`has_inited` | `Node::init`、`Node::deinit`、`Node::interrupt`、`Node::has_inited` |

## 🔑 配置入口与初始化时机

下列配置应在 transport 初始化前确定，但入口并不都是 property 系统：

| 配置 | 正确入口 | 影响 |
|------|----------|------|
| Endpoint QoS | URL `?qos=&depth=`、后端 `Conf` / `register_qos()` | transport 投递行为 |
| 后端支持的节点属性 | `set_property()`，例如 DDS participant 的 `dds.*` | 后端初始化参数 |
| `ser_type` / `schema_type` | `set_ser_type()` | discovery/bag/proxy 元数据；DDS raw/CDR 模式与 CDR 类型名还决定原生 endpoint 类型 |
| `discovery_enabled` | `set_discovery_enabled()` | 是否对 DiscoveryReporter / Viewer 可见 |

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
