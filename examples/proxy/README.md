# proxy — vlink 远程监控与控制代理

vlink Proxy 提供"远程观察 / 录制 / 回放 / 注入"vlink topics 的能力，用于：

- 可视化工具（Foxglove、自家 dashboard）实时观察消息流。
- 远程录制 / 回放（dispatcher 跑在另一台机器上）。
- 调试时注入测试消息。

Proxy 由两部分组成：

- **`ProxyServer`**：守护进程，跑在被观察的目标进程或同机；做 topic discovery、token 握手、心跳、数据转发。
- **`ProxyAPI`**：客户端，连接 Server 后按角色（Controller / Listener）发送 Control 或被动接收 Data。

读完本目录你能掌握：

- ProxyAPI 客户端的 Config / Role / Mode / 回调注册。
- ProxyServer 的配置、连接握手、心跳、转发机制。
- 怎么把 RunablePlugin 接入 ProxyServer 让它自动管理 lifecycle。

## 子示例索引

| 示例 | 主题 | 关键类 |
|------|------|--------|
| `proxy_api_basic/` | ProxyAPI 客户端配置、角色、模式、回调 | `vlink::ProxyAPI` |
| `proxy_server_basic/` | ProxyServer daemon：discovery + 握手 + 心跳 + 转发 | `vlink::ProxyServer` |
| `proxy_runnable_plugin/` | 通过 ProxyServer 加载 Runnable 插件 | `ProxyServer::Config::runnable_list` |

## 推荐阅读顺序

1. **`proxy_server_basic/`** —— 先看 Server 端，理解它在 vlink 生态中的位置。
2. **`proxy_api_basic/`** —— Client 端配置与 Role/Mode 选择。
3. **`proxy_runnable_plugin/`** —— 进阶：把 Runnable 插件交给 Server 管理。

## 共同前置知识

- `../communication/` —— Publisher / Subscriber 基础。
- `../plugin/plugin_runnable/` —— RunablePlugin 接口（runnable_list 用到）。
- DDS 基础（Proxy 当前依赖 DDS-family 传输）。

## 配图

各示例下 `images/` 包含对应流程图：

- `proxy_api_basic/images/proxy-api-client-flow.png` —— ProxyAPI 连接与命令流
- `proxy_server_basic/images/proxy-server-overview.png` —— ProxyServer 内部组件结构
- `proxy_runnable_plugin/images/proxy-runnable-plugin.png` —— Runnable 插件 + ProxyServer 协作

## 参考

- 顶层 `doc/16-proxy.md` —— Proxy 系统完整设计
- `vlink/include/vlink/external/proxy_api.h` —— ProxyAPI 接口
- `vlink/include/vlink/external/proxy_server.h` —— ProxyServer 接口
