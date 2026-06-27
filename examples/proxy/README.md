# 🛰️ Proxy — 远程监控与控制代理

VLink Proxy 提供跨进程 / 跨机器观察、录制、回放与注入 VLink topic 的能力，典型用途：

- 可视化工具（Foxglove、自建 dashboard）实时观察消息流。
- 远程录制与回放：dispatcher 运行于另一台机器。
- 调试期注入测试消息。

Proxy 由两端组成：

| 组件 | 角色 | 职责 |
|------|------|------|
| `ProxyServer` | 守护进程 | 运行于目标进程或同机；负责 topic discovery、token 握手、心跳、数据转发 |
| `ProxyAPI` | 客户端 | 连接 Server 后按角色（Controller / Listener）发送 Control 或被动接收 Data |

本目录聚焦客户端接入：`ProxyAPI` 的 Config / Role / Mode 与回调注册。Server 端守护进程的配置与转发机制，以及 Runnable 插件接入 Server 的进阶用法，见顶层 `doc/12-observability.md`。

## 📑 示例索引

| 示例 | 主题 | 关键类 |
|------|------|--------|
| [`proxy_api_basic/`](./proxy_api_basic/) | ProxyAPI 客户端配置、角色、模式、回调 | `vlink::ProxyAPI` |

## 🔗 前置知识

- [`../communication/`](../communication/) —— Publisher / Subscriber 进阶用法；最小 pub/sub 见 [`../quickstart/`](../quickstart/)。
- [`../plugin/plugin_basic/`](../plugin/plugin_basic/) —— 插件接口（Runnable 列表相关）。
- DDS 基础：Proxy 当前依赖 DDS 系列传输。

## 🖼️ 配图

`proxy_api_basic/images/proxy-api-client-flow.png` —— ProxyAPI 连接与命令流。

## 📚 参考

- 顶层 `doc/12-observability.md` —— Proxy 系统完整设计（含 ProxyServer 与 Runnable 接入）。
- `include/vlink/external/proxy_api.h` —— ProxyAPI 接口定义。
- `include/vlink/external/proxy_server.h` —— ProxyServer 接口定义。
