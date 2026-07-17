# 🌐 url_basics — URL 解剖与跨传输切换

VLink 用 URL 将“业务话题”与“传输后端”解耦：同一通信 API 可用于多种传输，但 URL 的地址和参数必须满足目标后端契约。`intra://`、`shm://`、`dds://` 等 topic 后端常可保留地址只改 scheme；SOME/IP 等专用协议需重写完整 URL。本示例演示 URL 的字段构成、各传输的实例写法，以及传输无关的 pub/sub 调用。

## 🧩 URL 通用形状

```
<transport>://<host>[/path][?key=value&...][#fragment]
```

| 字段 | 含义 |
|------|------|
| `transport` | 后端选择：`intra` / `shm` / `shm2` / `dds` / `ddsc` / `ddsr` / `zenoh` / `someip` / `mqtt` / `fdbus` |
| `host` + `path` | 在该后端内寻路的话题 / 服务 / 字段名 |
| `query` | 后端可调参数，如 `domain=42&depth=10&qos=sensor` |
| `fragment` | 后端特定提示，如 mqtt 的 broker、fdbus 的服务名、intra 的派发模式 |

实例：

| URL | 含义 |
|-----|------|
| `intra://sensor/lidar` | 进程内传输，话题 `sensor/lidar` |
| `dds://vehicle/speed?domain=42&depth=10&qos=sensor` | FastDDS，Domain 42，KeepLast(10)，`kSensor` QoS 预设 |
| `someip://4660/22136?groups=1&event=16` | SOME/IP 事件端点，service 4660 / instance 22136（十进制，等价 hex `0x1234` / `0x5678`），event group 1，event 16 |

## 🚀 同代码跨传输

URL 直接传入 `Publisher` / `Subscriber` 构造函数；以下地址模型兼容的后端可只改 scheme：

```cpp
vlink::Subscriber<std::string> sub("intra://demo/url_basics");
sub.listen([](const std::string& msg) { VLOG_I("Received:", msg); });

vlink::Publisher<std::string> pub("intra://demo/url_basics");
pub.wait_for_subscribers();
pub.publish("Hello from url_basics example!");
```

将 `intra://demo/url_basics` 改为 `dds://demo/url_basics` 或 `shm://demo/url_basics` 时，调用代码与 topic path 可保持不变；进程范围、发现、QoS、依赖以及复制 / 借贷行为以后端实现为准。

## 🔁 运行时换后端

设置环境变量 `VLINK_URL_REMAP` 指向一份 JSON 映射文件，框架首次使用 URL 时加载规则。规则的 key 用于子串匹配，命中后返回完整 value，并非通用的前缀替换；需要切换多个 topic 时应逐条写出完整目标 URL。全局映射文件变更需重启进程，详见 `doc/13-integration.md`。

## ⚠️ 边界条件

| 条件 | 说明 |
|------|------|
| `path` 以 `/` 开头 | 写 `/lidar` 而非 `lidar`，否则 host/path 边界判定异常 |
| `query` 顺序无意义 | `domain=1&depth=16` 与 `depth=16&domain=1` 等价 |
| `fragment` 语义随后端而变 | mqtt 为 broker 地址，fdbus 为服务名，intra 为派发模式 |
| 消息类型须被后端支持 | 换后端不改业务代码，但例如 someip 推荐 FlatBuffers 而非裸 POD |

## 🧭 模型与传输正交

URL 决定传输后端，与所用通信模型正交：Event（Publisher / Subscriber）、Method（Client / Server）、Field（Setter / Getter）共用同一套 URL。先按业务选模型，再按部署环境选 `transport://` 前缀。

## 🔗 参考

| 资源 | 说明 |
|------|------|
| `doc/04-transport.md` | 各传输后端的 URL 规则与 query / fragment 完整列表 |
| `doc/13-integration.md` | `VLINK_URL_REMAP` 等运行时配置 |
| `../../samples/` | 各传输的端到端样例（shm_raw、someip_flat、ping_pong、helloworld） |
| `../../README.md` | 示例总览 |
