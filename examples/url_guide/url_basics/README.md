# 🌐 url_basics — URL 解剖与跨传输切换

VLink 用一串 URL 将"业务话题"与"传输后端"解耦：同一份业务代码只需改写 URL 前缀，即可在 `intra://`、`shm://`、`dds://`、`someip://` 等十余种传输上运行，调用代码不变。本示例演示 URL 的字段构成、各传输的实例写法，以及传输无关的 pub/sub 调用。

## 🧩 URL 通用形状

```
<transport>://<host>[/path][?key=value&...][#fragment]
```

| 字段 | 含义 |
|------|------|
| `transport` | 后端选择：`intra` / `shm` / `dds` / `ddsc` / `zenoh` / `someip` / `mqtt` / `fdbus` / `qnx` … |
| `host` + `path` | 在该后端内寻路的话题 / 服务 / 字段名 |
| `query` | 后端可调参数，如 `domain=42&depth=10&qos=sensor` |
| `fragment` | 后端特定提示，如 mqtt 的 broker、fdbus 的服务名、intra 的派发模式 |

实例：

| URL | 含义 |
|-----|------|
| `intra://sensor/lidar` | 进程内传输，话题 `sensor/lidar` |
| `dds://vehicle/speed?domain=42&depth=10&qos=sensor` | FastDDS，Domain 42，KeepLast(10)，`kSensor` QoS 预设 |
| `someip://4660/22136?event=16` | SOME/IP，service 4660 / instance 22136（十进制，等价 hex `0x1234` / `0x5678`） |

## 🚀 同代码跨传输

URL 直接传入 `Publisher` / `Subscriber` 构造函数，换后端仅改前缀：

```cpp
vlink::Subscriber<std::string> sub("intra://demo/url_basics");
sub.listen([](const std::string& msg) { VLOG_I("Received:", msg); });

vlink::Publisher<std::string> pub("intra://demo/url_basics");
pub.wait_for_subscribers();
pub.publish("Hello from url_basics example!");
```

将 `intra://demo/url_basics` 改为 `dds://demo/url_basics` 或 `shm://demo/url_basics`，代码与行为不变，仅传输路径不同。

## 🔁 运行时换后端

设置环境变量 `VLINK_URL_REMAP` 指向一份 JSON 映射文件，框架启动时整体替换 URL 前缀，无需改动业务代码，用于灰度切换、调试、开发与生产环境互换。详见 `doc/13-integration.md`。

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
