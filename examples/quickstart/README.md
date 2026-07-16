# 🚀 quickstart — VLink 三种通信模型入门示例

quickstart 给出 VLink 三种通信模型（Event / Method / Field）的最小可运行单文件演示。三个示例均使用 `intra://` 进程内传输，不依赖任何外部中间件与配置文件，编译后即可直接运行。其目的是在最短篇幅内呈现 Publisher/Subscriber、Client/Server、Setter/Getter 的角色分工与最小调用顺序。

读完本目录后，可继续阅读 `../communication/` 下的进阶示例，覆盖定时器驱动发布、订阅者检测、连接检测、异步 reply、变化上报等更完整的 API 矩阵。

## 📑 子示例索引

| 示例 | 通信模型 | 关键类 | URL |
|------|----------|--------|-----|
| `hello_pubsub/` | Event（事件 / 发布订阅） | `vlink::Publisher<T>` / `vlink::Subscriber<T>` | `intra://hello/pubsub` |
| `hello_rpc/`    | Method（远程过程调用） | `vlink::Client<Req,Resp>` / `vlink::Server<Req,Resp>` | `intra://hello/rpc` |
| `hello_field/`  | Field（最新值 / 状态同步） | `vlink::Setter<T>` / `vlink::Getter<T>` | `intra://hello/field` |

## 📖 推荐阅读顺序

1. **`hello_pubsub/`**。Publisher/Subscriber 是 VLink 中最基础、覆盖面最广的通信模型，其余两种模型可视为它的特化。该示例同时演示 `MessageLoop::async_run()`、`Subscriber::attach()`、`Publisher::wait_for_subscribers()` 这一组在各示例中反复出现的基础调用。

2. **`hello_rpc/`**。Method 模型的关键区别在于：Server 端通过 `listen()` 的 `(req, resp)` 双参数回调原地填充响应；Client 端通过 `invoke()` 同步等待返回。该示例展示最常用的 `std::optional<Resp>` 形式。

3. **`hello_field/`**。Field 模型可理解为"带缓存的 Publisher/Subscriber"：Setter 的最近一次写入会自动同步给后加入的 Getter，因此该示例额外演示 `wait_for_value()`——晚加入的读者亦可读到当前状态。

## 🧩 共同前置知识

- C++ 模板与 lambda 捕获基础。
- `std::optional`、`std::chrono` 字面量（`100ms`、`2s` 等）的常规用法。
- VLink 通过 URL 协议前缀（`intra://`、`dds://`、`shm://` 等）选择传输后端，业务代码无需改动。

如尚未了解 VLink 整体架构，建议先阅读顶层 `doc/00-overview.md` 与 `doc/02-communication.md`。

## 🔗 参考

- `../communication/` — 同三种模型的进阶示例，覆盖定时器、连接检测、延迟统计、强制发布、异步 reply 等用法。
- `../serialization/` — VLink 自动选择序列化策略的机制（Bytes / POD / string / 自定义 operator）。
- `../url_guide/` — URL 各字段含义与跨后端切换。
- 顶层 `doc/02-communication.md` — 三种通信模型的完整规范。
