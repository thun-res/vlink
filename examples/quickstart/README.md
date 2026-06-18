# quickstart — VLink 三种通信模型的入门最小示例

quickstart 目录给出 VLink 三种通信模型（Event / Method / Field）的最短可运行单文件演示。三个示例均使用 `intra://` 进程内传输，不依赖任何外部中间件、不依赖任何配置文件，编译完即可直接运行。它们的目的是让初学者在 30 行左右的代码内立刻看清楚 Publisher/Subscriber、Client/Server、Setter/Getter 的角色分工与最小调用顺序。

读完这三个示例后，可以继续阅读 `../communication/` 下的进阶示例，那里覆盖了定时器驱动发布、订阅者检测、`detect_connected`、异步 reply、变化上报等更完整的 API 矩阵。

## 子示例索引

| 示例 | 通信模型 | 关键类 | URL |
|------|----------|--------|-----|
| `hello_pubsub/` | Event（事件 / 发布订阅） | `vlink::Publisher<T>` / `vlink::Subscriber<T>` | `intra://hello/pubsub` |
| `hello_rpc/`    | Method（远程过程调用） | `vlink::Client<Req,Resp>` / `vlink::Server<Req,Resp>` | `intra://hello/rpc` |
| `hello_field/`  | Field（最新值 / 状态同步） | `vlink::Setter<T>` / `vlink::Getter<T>` | `intra://hello/field` |

## 推荐阅读顺序

1. **先看 `hello_pubsub/`**。Publisher/Subscriber 是 VLink 中最基础也是覆盖面最广的通信模型，理解了它，后续两种模型本质上是它的特化。该示例同时演示了 `MessageLoop::async_run()`、`Subscriber::attach()`、`Publisher::wait_for_subscribers()` 这一组在所有示例里反复出现的基础调用。

2. **再看 `hello_rpc/`**。Method 模型的关键不同点在于：Server 端通过 `listen()` 的 `(req, resp)` 双参数回调原地填充响应；Client 端通过 `invoke()` 同步等待返回。`hello_rpc` 展示了最常用的 `std::optional<Resp>` 形式。

3. **最后看 `hello_field/`**。Field 模型可以理解为"带缓存的 Publisher/Subscriber"——Setter 的最近一次写入会自动同步给后加入的 Getter，因此该示例额外演示了 `wait_for_value()` 这种"晚加入也能读到当前状态"的语义。

## 共同前置知识

- C++17 模板与 lambda 捕获基础。
- `std::optional`、`std::chrono` 字面量（`100ms`, `2s` 等）的常规用法。
- 一个最小的概念：VLink 通过 URL 的协议前缀（`intra://`、`dds://`、`shm://` 等）选择传输后端，业务代码无需改动。

如果还没看过 VLink 的整体架构，建议阅读顶层 `doc/00-whitepaper.md` 和 `doc/03-event-model.md` / `doc/04-method-model.md` / `doc/05-field-model.md`。

## 参考

- `../communication/` — 同三种模型的进阶示例，覆盖定时器、连接检测、延迟统计、强制发布、异步 reply 等高级用法。
- `../serialization/` — 介绍 VLink 自动选择序列化策略的机制（Bytes / POD / string / 自定义 operator）。
- `../url_guide/` — URL 各字段含义与跨后端切换。
- 顶层 `doc/03-event-model.md`、`doc/04-method-model.md`、`doc/05-field-model.md` — 各模型的完整规范。
