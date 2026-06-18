# communication — 三种通信模型的进阶示例

`communication/` 在 `quickstart/` 的"最小可运行"基础上，分别按 Event / Method / Field 三大模型展开覆盖更完整的 API 矩阵：连接检测、强制发布、多订阅者扇出、延迟统计、异步 reply、变化上报、晚加入同步等。所有示例使用 `dds://` 或 `ddsc://` 作为主要传输（部分用 `zenoh://`），让示例尽量贴近真实分布式场景。

## 子示例索引

| 示例 | 主题 | 关键类 |
|------|------|--------|
| `event_basic/`        | Timer 周期发布 + `register_terminate_signal` 优雅退出 | `Publisher`, `Subscriber`, `Timer`, `MessageLoop` |
| `event_advanced/`     | 订阅者检测、`publish(force=true)`、多订阅者扇出、延迟统计 | `Publisher::detect_subscribers`, `Subscriber::set_latency_and_lost_enabled` |
| `field_basic/`        | `Setter`/`Getter` 基础 + 晚加入 `wait_for_value` + `listen` 回调 | `Setter`, `Getter` |
| `field_advanced/`     | `set_change_reporting` 去重、多 Getter 扇出、延迟统计 | `Getter::set_change_reporting`, `Getter::get_latency` |
| `method_sync/`        | 同步 RPC 的三种 invoke 形式 | `Client::invoke`, `Server::listen(ReqRespCallback)` |
| `method_async/`       | `listen_for_reply` + `reply`、callback / future 异步调用 | `Server::listen_for_reply`, `Client::async_invoke` |
| `method_fire_forget/` | 单向 RPC：`Server<Req>` + `Client::send()` | `Server<ReqT>`, `Client::send` |

## 推荐阅读顺序

**Event 模型一组（`event_basic` → `event_advanced`）**。基础示例展示 `Timer` 周期驱动发布 + SIGINT/SIGTERM 优雅退出（`Utils::register_terminate_signal`），是真实业务中最常见的"采集 → 发布 → 长期运行 → ctrl-c 退出"骨架。进阶示例叠加 `detect_subscribers` 异步通知、`publish(force=true)` 强制发送、三个订阅者扇出、`set_latency_and_lost_enabled` 端到端延迟统计四组高级特性。

**Field 模型一组（`field_basic` → `field_advanced`）**。基础示例覆盖最常用的 `set` / `get` / `listen` / `wait_for_value` 四方面；进阶示例引入 `set_change_reporting` 抑制重复写入触发的回调、多个 `Getter` 同步同一 URL、`get_latency` / `get_lost` 统计延迟与丢包。

**Method 模型一组（`method_sync` → `method_async` → `method_fire_forget`）**。三个示例展示 RPC 的五种调用形态：同步 invoke 的三种变形（output ref / optional / custom timeout）；异步 invoke 的两种变形（callback / future）；以及无响应的 `send()`。`method_async` 同时演示 Server 端的"延迟应答"机制 `listen_for_reply` + `reply(req_id, resp)`，这是适配数据库查询、外部 HTTP 调用等需要异步处理的请求时的关键 API。

## 共同前置知识

- `../quickstart/` —— 必须先看，理解三种模型的最小骨架。
- `../url_guide/url_basics/` —— URL 各字段语义、跨后端切换。
- `../serialization/basic_types/` —— 默认序列化规则（POD vs string vs Bytes）。
- `vlink/include/vlink/publisher.h` / `subscriber.h` / `client.h` / `server.h` / `setter.h` / `getter.h` —— 各模型完整公开 API。

## 配图

![Communication models overview](event_basic/images/communication-models-overview.png)

上图位于 `event_basic/images/`，给出 Event/Method/Field 三种模型的角色与数据流总览，建议结合 `event_basic` 一起阅读。

## 参考

- 顶层 `doc/03-event-model.md`、`doc/04-method-model.md`、`doc/05-field-model.md` —— 三种模型的完整规范。
- `../qos/` —— DDS-family 后端上的 QoS 配置；与本目录示例可组合使用。
- `../url_guide/` —— URL 解析与跨后端切换。
