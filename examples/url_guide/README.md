# 🌐 url_guide — VLink URL 书写指南

VLink 通过 URL 将“业务话题”与“传输后端”两层概念解耦：通信 API 仅依赖 URL 字符串。地址模型兼容的 topic 后端通常只需改写 scheme；SOME/IP、MQTT、FDBUS 等须按后端提供完整地址和参数。本目录聚焦 URL 本身的构成与书写规则，包含一个示例 —— `url_basics/`，演示 URL 解剖、字段语义、各传输的实例 URL 与运行时重映射。

各传输后端的端到端运行样例（共享内存、SOME/IP、Ping-Pong RPC、最小 Hello World）置于 `../samples/` 下，每个样例展示对应后端 URL 的真实写法、所需守护进程与可调参数。

## 📑 示例索引

| 示例 | 主题 | 关键类型 |
|------|------|---------|
| `url_basics/` | URL 解剖、字段语义、各传输实例 URL、传输无关 pub/sub、运行时重映射 | `vlink::Publisher` / `vlink::Subscriber` |

## 🧩 URL 通用形状

```
<transport>://<host>[/path][?key=value&key=value][#fragment]
```

| 字段 | 含义 |
|------|------|
| `transport` | 传输后端：`intra` / `shm` / `shm2` / `dds` / `ddsc` / `ddsr` / `ddst` / `zenoh` / `someip` / `fdbus` / `qnx` / `mqtt` |
| `host` | 主话题 / 服务标识符 |
| `path` | 二级话题路径 |
| `query` | 后端特定参数（`domain` / `depth` / `history` / `qos` / `event` / `field` / `method` …） |
| `fragment` | 模式提示（`queue` / `direct` / `svc` / broker URI 等，按后端约定） |

`query` 各 key 在不同后端的语义：

| 后端 | 常用 query / fragment |
|------|----------------------|
| `dds://` / `ddsc://` | `domain`、`depth`、`qos`；FastDDS `dds://` 另支持 `part` / `topic` / `pub` / `sub` / `writer` / `reader` XML profile key |
| `shm://` | `domain`、`depth`、`history`、`wait` |
| `someip://` | `groups`、`event`、`field`、`method`、`major`、`minor` |
| `mqtt://` | `qos`（MQTT QoS 0/1/2）；fragment 携带 broker 地址（`#tcp://broker:1883`） |
| `fdbus://` | `event`、`field`、`method`；fragment 携带 service 名（`#svc`） |

完整参数列表见 `doc/04-transport.md`。

## 🔁 运行时重映射（VLINK_URL_REMAP）

无需改动业务代码，通过环境变量 `VLINK_URL_REMAP` 指向一份 JSON 映射文件，框架在创建节点时将命中的 URL 映射为规则中给出的完整目标 URL：

```bash
export VLINK_URL_REMAP=/etc/vlink/remap.json
```

```json
{
  "dds://vehicle/speed": "shm://vehicle/speed",
  "ddsc://vehicle/pose": "dds://vehicle/pose"
}
```

应用层使用 `dds://vehicle/speed`，框架在节点创建时替换为 `shm://vehicle/speed`。典型场景：

| 场景 | 用途 |
|------|------|
| 部署切换 | 在进程启动前把指定 topic 从 DDS 映射到 shm |
| 测试环境 | 为需要替换的 DDS topic 逐条配置完整 `intra://` 目标 URL |
| 调试 | 将 topic 重定向到本地 fallback |

映射 key 只负责子串匹配，命中后不会保留输入 URL 中未匹配的部分，因此 `{"dds://": "shm://"}` 不能完成通用前缀替换。全局映射表在进程首次使用时加载一次；修改文件后需重启进程。需要在应用内动态更新时，应显式持有 `UrlRemap` 并在调用方完成同步后调用 `reload()`。

## 🗺️ 阅读顺序

1. **`url_basics/`** —— 理解 URL 字段构成与各传输的实例 URL。
2. **`../samples/<transport>/`** —— 进入各传输样例，查看真实运行时配置：
   - `../samples/shm_raw/` —— `shm://` 共享内存
   - `../samples/someip_flat/` —— SOME/IP
   - `../samples/ping_pong/` —— RPC 往返
   - `../samples/helloworld/` —— 最小端到端

## 🔗 前置与参考

| 资源 | 说明 |
|------|------|
| `../quickstart/` | Event / Method / Field 三种通信原语的基础用法 |
| `../communication/` | 三种模型的进阶用法 |
| `doc/04-transport.md` | 各传输后端的 URL 规则与 query 参数完整列表 |
| `doc/13-integration.md` | `VLINK_URL_REMAP` 等运行时环境变量 |
| `doc/00-overview.md` | URL 在 VLink 总体架构中的位置 |
