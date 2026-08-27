# 📡 communication —— 三种通信模型的进阶示例

本类目在 `quickstart/` 的最小骨架之上，按 Event / Method / Field 三大模型分别展开更完整的 API 矩阵：连接检测、强制发布、多订阅者扇出、端到端延迟与丢包统计、变化上报、晚加入同步等。示例以 `dds://` / `ddsc://` 作为主要传输，贴近分布式部署场景；地址模型兼容的 topic 后端通常可只替换 scheme，专用协议须填写完整合法 URL。异步应答（回调 / future）与服务端延迟应答 `listen_for_reply` + `reply(req_id, resp)` 的完整规范见顶层 `doc/02-communication.md`。

![通信模型总览](../../doc/images/readme-communication-models.png)

## 🧩 子示例索引

| 示例 | 模型 | 主题 | 关键接口 |
|------|------|------|----------|
| `event_advanced/` | Event | 订阅者检测、`publish(force=true)`、多订阅者扇出、延迟/丢包统计 | `Publisher::detect_subscribers`、`Subscriber::set_latency_and_lost_enabled` |
| `field_advanced/` | Field | 变化上报去重、晚加入同步、多 Getter 扇出与统计 | `Getter::set_change_reporting`、`Getter::get_latency` |
| `method_sync/` | Method | 同步 RPC 的三种 `invoke` 形态 | `Client::invoke`、`Server::listen(ReqRespCallback)` |

## 📖 阅读顺序

| 顺序 | 示例 | 说明 |
|------|------|------|
| 1 | `event_advanced/` | 在最小 pub/sub 之上叠加 `detect_subscribers` 异步通知、`publish(force=true)` 强制下发、多订阅者扇出、端到端延迟统计 |
| 2 | `field_advanced/` | 在 `set` / `get` / `listen` / `wait_for_value` 之上叠加 `set_change_reporting` 去重、多 Getter 同步、`get_latency` / `get_lost` 统计 |
| 3 | `method_sync/` | 同步 `invoke` 的三种重载（输出引用 / `optional` / 自定义超时） |

## 📚 前置知识

| 入口 | 作用 |
|------|------|
| `../quickstart/` | 三种模型的最小可运行骨架，先看 |
| `../url_guide/url_basics/` | URL 各字段语义与跨后端切换 |
| `../serialization/basic_types/` | 默认序列化规则（POD / string / Bytes / SOME/IP 宏结构） |
| `include/vlink/publisher.h`、`subscriber.h`、`client.h`、`server.h`、`setter.h`、`getter.h` | 各模型完整公开 API |

## 🔗 参考

- 顶层 `doc/02-communication.md` —— 三种通信模型的完整规范。
- `../qos/` —— DDS-family 后端上的 QoS 配置，可与本类目示例组合。
- `../url_guide/` —— URL 解析与跨后端切换。
