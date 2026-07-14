# 🛤️ 4. 传输后端与 URL

URL 是业务代码与传输实现之间的唯一契约面：其 `scheme` 前缀决定**用哪条链路**投递消息，而消息类型、路径与调用代码保持不变。这构成本章的核心不变量——**URL 选链路，业务代码对后端透明**。本章确立 URL 契约的结构模型、各后端选型判据、各后端接入要点、混合桥接与 URL 重映射；以何种**行为**投递（可靠性、历史深度、持久化等 QoS 维度）由 URL 的 `?qos=` 参数承接，独立成章见 [QoS 配置](05-qos.md)。

![URL 格式](images/url-format.png)

---

## 🔗 4.1 URL 契约与概念模型

VLink 不自研传输协议，而在成熟后端之上提供一致的编程模型。一次通信由「通信模型 + URL + 核心方法」三要素确定，其中 URL 是业务代码与传输实现之间的唯一契约面：

```
<transport>://[<host>[:<port>]]/<path>[?<query>][#<fragment>]
|----------|  |--------------|  |-----|  |-------|  |--------|
   传输后端     主机与端口(可选)   路径     配置参数    传输提示
```

| 字段 | 语义 | 必填 | 示例 |
| --- | --- | :---: | --- |
| `transport` | 传输后端标识，决定底层协议与运行时 | 是 | `dds`、`shm`、`intra`、`zenoh` |
| `host` | 主机名或地址，可带端口 | 否 | `127.0.0.1:30490`、`vehicle` |
| `path` | 主题路径，标识通信端点 | 是 | `/chassis/speed` |
| `query` | 配置参数，`&` 分隔 | 否 | `domain=1&depth=10&qos=sensor` |
| `fragment` | 传输提示，部分后端使用 | 否 | `#queue`、`#1M`、`#tcp` |

由此得到一项贯穿全框架的不变量：**仅改变 `transport` 前缀即可迁移后端，路径、消息类型与调用代码不变。** 序列化、QoS、安全、零拷贝等能力均服从此契约——或由消息类型在编译期确定，或以查询参数、节点变体的形式接入，均不破坏「业务代码不感知后端」的约束。

```cpp
vlink::Publisher<Imu> pub("intra://sensor/imu");   // 进程内
vlink::Publisher<Imu> pub("shm://sensor/imu");     // 同机零拷贝
vlink::Publisher<Imu> pub("dds://sensor/imu");     // 跨机网络
vlink::Publisher<Imu> pub("zenoh://sensor/imu");   // 云边 / 跨 NAT
```

后端可用性由构建期决定：某后端依赖缺失或目标平台不支持时，对应模块不参与编译，运行期不可用。构建开关见 [快速开始](01-started.md)。

---

## 🚀 4.2 快速开始

以同机零拷贝后端 `shm://` 为例，发布端与订阅端共享同一路径，节点构造即可用：

```cpp
#include <vlink/vlink.h>

vlink::Publisher<float> pub("shm://sensor/temperature");
vlink::Subscriber<float> sub("shm://sensor/temperature");

sub.listen([](const float& t) { VLOG_I("recv: ", t); });
pub.wait_for_subscribers(std::chrono::milliseconds(500));
pub.publish(25.6f);
```

将前缀替换为 `intra://` 或 `dds://`，上述代码无需其它修改即可改为进程内或跨机运行。`shm://` 额外要求运行共享内存守护进程并初始化运行时，见 [§4.6.2](#-462-shm--共享内存零拷贝)。

---

## 🧭 4.3 后端选型

按部署拓扑与负载选择后端。优先采用稳定后端（`intra`、`shm`、`dds`、`ddsc`），Beta 后端用于特定平台或协议生态。由于 URL 契约的存在，下表选择可在开发、集成、量产阶段切换而不改业务代码；开发期可统一用 `intra://` 完成端到端验证。

![传输后端决策树](images/transport-decision-tree.png)

| 部署场景 | 推荐后端 | 状态 |
| --- | --- | :---: |
| 同进程模块间通信，要求最低延迟 | `intra://` | 稳定 |
| 同机跨进程大负载（相机帧、点云） | `shm://` | 稳定 |
| 局域网跨机，DDS 生态 | `dds://` / `ddsc://` | 稳定 |
| 同机跨进程零拷贝，进程自治（无守护进程） | `shm2://` | Beta |
| 云边协同、跨 NAT、物联网 | `zenoh://` | Beta |
| 车载以太网、AUTOSAR 服务化 | `someip://` | Beta |
| 带宽受限的物联网消息分发 | `mqtt://` | Beta |
| 同机标准 IPC，类 D-Bus | `fdbus://` | Beta |
| QNX 实时操作系统原生 IPC | `qnx://` | Beta |

---

## 📊 4.4 后端能力概览

全部后端均支持六种节点类型（Publisher/Subscriber、Server/Client、Setter/Getter），三种通信模型可自由混用。后端差异集中在通信范围、零拷贝能力、依赖与成熟度：

| 后端 | 通信范围 | 零拷贝 | 依赖 | 状态 |
| --- | --- | :---: | --- | :---: |
| `intra://` | 进程内 | 是 | 无 | 稳定 |
| `shm://` | 同机跨进程 | 是 | iceoryx + RouDi | 稳定 |
| `dds://` | 跨机 / 局域网 | 否 | Fast-DDS | 稳定 |
| `ddsc://` | 跨机 / 局域网 | 否 | CycloneDDS | 稳定 |
| `shm2://` | 同机跨进程 | 是 | iceoryx2-c | Beta |
| `ddsr://` | 跨机 / 局域网 | 否 | RTI Connext DDS | Beta |
| `ddst://` | 跨机 | 否 | TravoDDS（国产） | Beta |
| `zenoh://` | 跨机 / 云边 | 条件支持 | zenoh-c / zenoh-pico | Beta |
| `someip://` | 车载以太网 | 否 | vsomeip | Beta |
| `mqtt://` | 跨机 / 物联网 | 否 | Paho MQTT C | Beta |
| `fdbus://` | 同机 | 否 | fdbus | Beta |
| `qnx://` | 同机（QNX） | 否 | QNX SDK | Beta |

能力边界条件：

- **QoS**：`dds`/`ddsc`/`ddsr`/`ddst` 支持完整 DDS QoS；`shm`/`shm2`/`zenoh`/`mqtt` 支持部分配置；`intra`/`fdbus`/`qnx` 不涉及。作用域与生效模型见 [QoS 配置](05-qos.md)。
- **消息级加密**：`intra` 不支持；`shm`、各 DDS、`zenoh` 等支持，但 `dds://` 配合 CDR 原生类型时不经 VLink 加密管道。详见 [安全加密](07-security.md)。
- **零拷贝**：`shm`/`shm2` 始终零拷贝；`zenoh` 仅在启用 SHM 时提供借用。详见 [零拷贝](06-zerocopy.md)。

---

## ⚙️ 4.5 通用配置：高频查询参数

下列四个参数覆盖多数配置需求，在支持它们的后端上语义一致；后端特有参数见各小节：

| 参数 | 语义 | 示例 |
| --- | --- | --- |
| `?domain=` | 域 ID，不同域互相隔离、不可见 | `?domain=1` |
| `?depth=` | 历史 / 队列深度 | `?depth=10` |
| `?qos=` | 引用命名 QoS Profile | `?qos=sensor` |
| `?event=` | 次级事件过滤名，同一地址下再分流（DDS 系列不支持，改以独立 Topic 区分） | `?event=notify` |

并非每个后端都支持上述全部参数：`?event=` 见于 `intra`/`shm`/`shm2`/`zenoh`/`mqtt`/`fdbus`/`qnx`，DDS 系列无此字段；`?qos=` 仅 DDS 系列与 `zenoh` 支持（`mqtt` 的 `?qos=` 是 MQTT 自身的 0/1/2 级别，含义不同，见 [§4.6.7](#-467-mqtt--fdbus--qnxbeta)）。各后端实际接受的参数见对应小节。

两种等价的构造方式：URL 字符串，或参数与查询项一一对应的 `XxxConf` 对象（参数较多或需复用时更清晰）：

```cpp
vlink::DdsConf conf("vehicle/speed", /*domain=*/1, /*depth=*/10);
vlink::Publisher<float> pub(conf);  // 等价于 "dds://vehicle/speed?domain=1&depth=10"
```

`?qos=` 的常用方式是引用预置 Profile（如 `?qos=sensor`）；自定义 QoS 经 `XxxConf::register_qos("name", qos)` 注册后再在 URL 引用。Profile 选型与子策略语义见 [QoS 配置](05-qos.md)。

发布前可阻塞等待订阅者就绪，避免连接类后端（DDS/MQTT）的首条消息丢失；Client 侧对应 `wait_for_connected(timeout)` / `is_connected()`：

```cpp
vlink::Publisher<float> pub("dds://sensor/temperature");

if (pub.wait_for_subscribers(std::chrono::milliseconds(500))) {
    pub.publish(25.6f);
}

if (pub.has_subscribers()) {  // 非阻塞查询
    pub.publish(26.0f);
}
```

---

## 🔌 4.6 各后端接入要点

### 🧵 4.6.1 intra:// — 进程内通信

同一进程内各模块间通信。消息对象经回调直接传递，无序列化、无内核调用，延迟接近一次函数调用，无外部依赖。

```
intra://<address>[?event=<name>&pipeline=<N>][#queue|#direct]
```

| 参数 | 位置 | 语义 |
| --- | --- | --- |
| `address` | `<host>/<path>` | 主题地址，必填 |
| `event` | `?event=` | 次级事件过滤名 |
| `pipeline` | `?pipeline=` | 队列模式下的工作线程标识，`0` 选默认线程 |
| 投递模式 | `#queue` / `#direct` | 默认 `queue`；`direct` 在发布线程内直接回调 |

```cpp
#include <vlink/vlink.h>

vlink::Publisher<int> pub("intra://sensor/value");
vlink::Subscriber<int> sub("intra://sensor/value");
sub.listen([](const int& v) { VLOG_I("recv: ", v); });
pub.publish(42);
```

边界条件：`#direct` 模式下回调在 `publish()` 的调用线程内执行，需避免在回调中重入或持锁等待；仅限同进程，跨进程无效。

### 🗄️ 4.6.2 shm:// — 共享内存零拷贝

同机跨进程的零拷贝 IPC，适用于相机帧、点云等高频大负载传输。受底层限制，地址与事件字符串不超过 80 字符。

```
shm://<address>[?event=<name>&domain=<N>&depth=<N>&history=<N>&wait=<ms>]
```

| 参数 | 位置 | 语义 |
| --- | --- | --- |
| `address` | `<host>/<path>` | 服务 / 主题名，必填，≤80 字符 |
| `event` | `?event=` | 次级事件名，≤80 字符 |
| `domain` | `?domain=` | 域 ID，默认 0 |
| `depth` | `?depth=` | 队列容量覆写，默认 0（用 Iceoryx 默认值） |
| `history` | `?history=` | 历史重放计数，默认 0；字段节点默认 1 |
| `wait` | `?wait=<ms>` | 阻塞等待超时，`>0` 启用，仅 Pub/Sub 有效 |

依赖 Iceoryx RouDi 守护进程，所有 `shm://` 业务进程须在其后启动。可外部预先启动 `iox-roudi`，或用 VLink 内置代理一体化拉起（提供 `-c` 配置路径即内嵌 RouDi，兼远程监控）：

```bash
vlink-proxy -c /etc/vlink/iox.toml          # -c <PATH> 拉起内嵌 RouDi（兼远程监控）
vlink-proxy -c /etc/vlink/iox.toml -l 3     # -l 选内存策略（1 低 / 2 中(默认) / 3 高），点云等重载用 3
```

> 内置代理仅同机一个实例；完整参数与服务发现/监控见 [可观测性](12-observability.md) §12.12。

业务进程在创建节点前须完成运行时初始化，进程退出前清理；注册名在连接同一 RouDi 的所有进程间应保持唯一：

```cpp
#include <vlink/vlink.h>
#include <vlink/modules/shm_conf.h>

int main() {
    vlink::ShmConf::init_runtime("my_process");

    vlink::Publisher<float> pub("shm://sensor/temperature");
    pub.wait_for_subscribers();
    pub.publish(25.6f);

    vlink::ShmConf::deinit_runtime();
    return 0;
}
```

边界条件：`wait>0` 阻塞模式仅对 Publisher/Subscriber 有效，用于 RPC 或字段节点会导致构造失败。`ShmConf` 另提供 `init_roudi()`（单进程内嵌 RouDi）、`auto_init_roudi()`（自动择优）、`has_roudi_running()` 等运行时入口；零拷贝容器见 [零拷贝](06-zerocopy.md)。

![共享内存零拷贝数据流](images/shm-zerocopy-flow.png)

### 🆕 4.6.3 shm2:// — Iceoryx2 共享内存（Beta）

Iceoryx2 为下一代实现，进程自治管理共享内存，无需独立守护进程。每条消息的内存分配大小由 URL 片段 `#<size>` 预先确定，默认 128 字节，上限 32 MiB。

```
shm2://<address>[?event=<name>&domain=<N>&depth=<N>&history=<N>&wait=<ms>][#<size>]
```

参数与 `shm://` 同义（`depth` 为槽池容量、`history` 为历史重放计数，字段节点默认 1）；`address` 与 `event` 同样受 80 字符限制。`size` 支持 `B`/`K`/`M`/`G` 后缀（不区分大小写），取值范围 `(0, 32MiB]`，如 `#1K`、`#1M`、`#8M`。

```cpp
#include <vlink/vlink.h>

vlink::Publisher<vlink::Bytes> pub("shm2://lidar/pointcloud#4M");
auto buf = vlink::Bytes::create(4 * 1024 * 1024);
pub.publish(buf);

vlink::Subscriber<vlink::Bytes> sub("shm2://lidar/pointcloud#4M");
sub.listen([](const vlink::Bytes& data) {
    process_pointcloud(data.data(), data.size());
});
```

边界条件：`shm2://` 与 `shm://` 互不兼容，两者节点无法互通；消息大小须在 URL 片段中预先声明。

### 🛰️ 4.6.4 DDS 系列：dds:// / ddsc:// / ddsr:// / ddst://

四者共享同一套 DDS API，仅底层运行时与前缀不同，业务代码不变。均面向局域网多播发现，不提供 NAT 穿透：

| 前缀 | 底层运行时 | 定位 |
| --- | --- | --- |
| `dds://` | eProsima Fast-DDS | 默认主力跨机后端，支持扩展 QoS 与原生类型（`ddsf://` 是 `dds://` 的等价别名） |
| `ddsc://` | Eclipse CycloneDDS | Apache 协议开源生态 |
| `ddsr://` | RTI Connext DDS | 商业 DDS，需 RTI 许可证（Beta） |
| `ddst://` | TravoDDS | 自主可控替代方案（Beta） |

以 `dds://` 为例，其余替换前缀即可：

```
dds://<topic>[?domain=<N>&depth=<N>&qos=<name>]
```

| 参数 | 位置 | 语义 |
| --- | --- | --- |
| `topic` | `<host>/<path>` | DDS Topic 名，必填 |
| `domain` | `?domain=` | Domain ID，默认读环境变量 `VLINK_DDS_DOMAIN`，未设为 0 |
| `depth` | `?depth=` | History 深度覆写，默认 0（沿用 QoS 选定深度） |
| `qos` | `?qos=` | 命名 QoS Profile，与扩展 QoS 互斥 |

```cpp
#include <vlink/vlink.h>

vlink::Publisher<std::string> pub("dds://system/log?qos=event");
pub.publish("System started");

vlink::Subscriber<std::string> sub("dds://system/log?qos=event");
sub.listen([](const std::string& msg) { VLOG_I("recv: ", msg); });
```

边界条件：URL 中显式 `domain=` 优先于环境变量；不同 Domain 的节点互不发现。方法模型的响应 Topic 自动派生（在 Topic 名后追加 `___resp` 后缀），无需手动注册。`dds://`、`ddsr://`、`ddst://` 另支持 `?part=`/`?topic=`/`?pub=`/`?sub=`/`?writer=`/`?reader=` 形式的逐实体扩展 QoS（即 `DdsConf` 的 `qos_ext` 构造重载，与 `?qos=` 互斥）；CDR 原生类型注册的模板入口 `register_topic<T>()` / `register_url<T>()` 仅 `dds://` 提供；XML Profile 加载 `load_global_qos_file()` 与发现快照 `get_discovered_topics()` 由 `dds://`、`ddst://` 提供（`ddsr://` 不含）；`ddsc://` 最精简，仅支持命名 Profile（`?qos=`），无 `qos_ext`、无 XML 加载、无发现接口。QoS 设置入口见 [QoS 配置](05-qos.md)，环境变量见 [集成](13-integration.md)。

### 🌐 4.6.5 zenoh:// — Zenoh 协议（Beta）

面向云-边-端统一数据管理的协议，内置 NAT 穿透，支持 P2P 与路由两种模式，适用于跨多网络域（WiFi/5G/以太网）的物联网与云边协同。

```
zenoh://<address>[?event=<name>&domain=<N>&qos=<name>&depth=<N>&shm=<bool>][#<hint>]
```

| 参数 | 位置 | 语义 |
| --- | --- | --- |
| `address` | `<host>/<path>` | Key Expression，支持通配符如 `vehicle/*` |
| `domain` | `?domain=` | 会话 / 域标识 |
| `qos` | `?qos=` | 命名 QoS Profile |
| `depth` | `?depth=` | 队列深度 |
| `shm` | `?shm=` | 启用共享内存零拷贝借用，`1`/`true` 开启，需 SHM 构建 |
| `fragment` | `#<hint>` | 传输提示，如 `tcp` / `udp` / `unix` / `tcp/host:port` |

```cpp
#include <vlink/vlink.h>

vlink::Publisher<std::string> pub("zenoh://vehicle/123/telemetry?qos=event");
pub.publish("{\"speed\": 80.5}");

vlink::Subscriber<std::string> sub("zenoh://vehicle/123/telemetry?qos=event");
sub.listen([](const std::string& data) { VLOG_I("recv: ", data); });
```

边界条件：P2P 模式或路由器（`zenohd`）部署及 `VLINK_ZENOH_*` 环境变量见 [集成](13-integration.md)；`?shm=1` 借用语义见 [零拷贝](06-zerocopy.md)。SHM 路径另有 `?shm_mode=<lazy|init>`、`?shm_size=`、`?shm_threshold=`、`?shm_loan_threshold=`、`?shm_blocking=` 等细调键。嵌入式场景可用功能裁剪的 zenoh-pico。

### 🚗 4.6.6 someip:// — SOME/IP 车载以太网（Beta）

AUTOSAR 标准车载以太网协议，用于 ECU 间通信与 V2X。端点以数字 ID 体系标识（Service ID / Instance ID / Method、Event ID），而非字符串 Topic：

```
# 方法：someip://<service>/<instance>?method=<id>
# 事件：someip://<service>/<instance>?groups=<g1|g2>&event=<id>
# 字段：someip://<service>/<instance>?groups=<g1|g2>&event=<id>&field=1
```

| 参数 | 位置 | 语义 |
| --- | --- | --- |
| `service` | `<host>` | Service ID，16 位非零，十进制或 `0x` |
| `instance` | `<path>` | Instance ID，16 位非零 |
| `method` | `?method=` | Method ID，仅 Server/Client |
| `groups` | `?groups=g1\|g2` | Event Group 集合，`\|` 分隔 |
| `event` | `?event=` | Event ID |
| `field` | `?field=1` | 字段模式（Setter/Getter） |

```cpp
#include <vlink/vlink.h>

vlink::Publisher<float> pub("someip://0x1234/0x5678?groups=0x1&event=0x10");
pub.publish(80.5f);

vlink::Subscriber<float> sub("someip://0x1234/0x5678?groups=0x1&event=0x10");
sub.listen([](const float& v) { VLOG_I("recv: ", v); });
```

边界条件：依赖 vsomeip JSON 配置文件，路径由环境变量 `VSOMEIP_CONFIGURATION` 指定；事件与字段节点须同时设 `groups` 与 `event`，字段还需 `field=1`；vsomeip 可能要求网络权限（root 或 `CAP_NET_RAW`）。建议进程启动时调用 `SomeipConf::load_global_config_file()` 预加载配置。

### 📦 4.6.7 mqtt:// / fdbus:// / qnx://（Beta）

**mqtt://** — 面向物联网的轻量发布/订阅，适用于带宽受限、网络不稳定场景，依赖外部 MQTT Broker（Mosquitto、EMQX 等）。`?qos=` 为 MQTT 自身的 0/1/2 级别，`#tcp://ip:1883` 可覆盖 Broker 地址。

```cpp
vlink::Publisher<std::string> pub("mqtt://sensor/data");
pub.publish("{\"temp\": 25.6}");

vlink::Subscriber<std::string> sub("mqtt://sensor/data");
sub.listen([](const std::string& msg) { VLOG_I("recv: ", msg); });
```

**fdbus://** — 面向 Android/Linux 的轻量 IPC，语义类 D-Bus。`#svc`（经名称服务发现，默认）或 `#ipc`（直接 P2P）选择模式；`svc` 模式需系统中运行 fdbus 名称服务进程 `name_server`。

```cpp
vlink::Publisher<std::string> pub("fdbus://my_service");
pub.publish("hello from fdbus");

vlink::Subscriber<std::string> sub("fdbus://my_service");
sub.listen([](const std::string& msg) { VLOG_I("recv: ", msg); });
```

**qnx://** — QNX Neutrino RTOS 原生 IPC（Pulse / Message Passing），提供确定性实时延迟，适用于功能安全（ASIL）场景。仅在 QNX 目标平台可用，Linux 上不可用。

```cpp
vlink::Publisher<float>  pub("qnx://sensor/imu");
vlink::Subscriber<float> sub("qnx://sensor/imu");
sub.listen([](const float& v) { VLOG_I("recv: ", v); });
pub.publish(9.8f);
```

---

## 🔀 4.7 后端混合、桥接与重映射

### 🧩 4.7.1 混合与桥接

URL 契约不限制单进程内同时使用多个后端。两种典型组合：

**进程内加速 + 跨机分发**：本地以 `intra://` 零开销分发，同时以 `dds://` 发往远端。

```cpp
vlink::Publisher<SensorData> intra_pub("intra://sensor/raw");
vlink::Publisher<SensorData> dds_pub("dds://sensor/raw");

SensorData data = read_sensor();
intra_pub.publish(data);
dds_pub.publish(data);
```

**协议桥接**：订阅一个后端、转发至另一后端，打通不同网络段（如将同机共享内存相机帧转发到远端 DDS）。

```cpp
vlink::Subscriber<vlink::Bytes> shm_sub("shm://camera/frame");
vlink::Publisher<vlink::Bytes>  dds_pub("dds://remote/camera/frame");

shm_sub.listen([&dds_pub](const vlink::Bytes& frame) {
    dds_pub.publish(frame);
});
```

### 🔧 4.7.2 全局初始化

多数后端无需手动初始化：节点构造即可用，析构自动清理。少数后端在创建节点前需一次全局初始化：

| 后端 | 初始化 | 清理 |
| --- | --- | --- |
| `intra://` | 无需 | 无需 |
| `shm://` | `ShmConf::init_runtime("name")`（守护进程须先运行） | `ShmConf::deinit_runtime()` |
| `shm2://` | 无需（进程自治） | 无需 |
| `dds://` | 可选 `DdsConf::register_qos()` / `load_global_qos_file()` | 节点析构自动 |
| `ddsc://` | 可选 `DdscConf::register_qos()` | 节点析构自动 |
| `zenoh://` | 可选 `ZenohConf::register_qos()` | 节点析构自动 |
| `someip://` | 推荐 `SomeipConf::load_global_config_file()` | 节点析构自动 |
| `mqtt://` | 无需（依赖外部 Broker） | 节点析构自动 |
| `fdbus://` | 无需（依赖系统级名称服务） | 节点析构自动 |
| `qnx://` | 无需 | 节点析构自动 |

进程启动时可调用 `vlink::Url::global_init(...)` 预初始化指定的已链接后端；它不控制传输插件发现。`VLINK_URL_PLUGINS` 的完整值选择一种互斥模式：等于 `auto`（大小写不敏感）时，允许未链接的已知后端在对应 URL 首次使用时按固定名称 `vlink-<module>` 加载；为空或等于 `none`（大小写不敏感）时关闭插件加载；其他非空值仍按逗号或空格分隔的显式预加载列表解析，并在首次 URL 初始化时加载其中列出的已知模块。`auto` / `none` 必须是完整值，不能与模块列表混写。该设置在进程级插件管理器首次初始化时读取一次，此后修改无效。已链接后端始终优先，未知 transport / 自定义 URL scheme 不会通过该机制加载；插件机制只适用于共享模块，不会加载静态归档（Unix `.a` / Windows 静态 `.lib`），共享库搜索路径可用 `VLINK_PLUGIN_DIR` 配置。分包安装时，运行时组件已包含这些可加载名称，无需开发组件。

```bash
# 显式预加载固定列表
export VLINK_URL_PLUGINS="zenoh,ddsc"

# 或改用按需加载模式（不能与上面的列表同时生效）
# export VLINK_URL_PLUGINS=auto

# 或显式关闭插件加载
# export VLINK_URL_PLUGINS=none
```

### 🔁 4.7.3 URL 重映射

环境变量 `VLINK_URL_REMAP` 指向一个 JSON 文件，可在不改代码的前提下整体替换后端（如将全部 `dds://` 改走 `shm://`）。键为源 URL（或其子串），值为目标 URL；按声明顺序子串匹配，首个命中生效。

```json
{
    "dds://sensor/imu": "shm://sensor/imu",
    "intra://camera/front": "zenoh://camera/front"
}
```

```bash
export VLINK_URL_REMAP="/etc/vlink/remap.json"
```

亦可在代码中使用 `UrlRemap`：

```cpp
#include <vlink/extension/url_remap.h>

vlink::UrlRemap remap;
remap.load("/etc/vlink/remap.json");
const std::string& actual = remap.convert("dds://sensor/imu");  // -> "shm://sensor/imu"
```

边界条件：`VLINK_URL_REMAP` 的值是文件路径而非映射规则本身；VLink 首次创建 `Url` 时自动加载并对后续 URL 转换。详见 [集成](13-integration.md)。

---

## 📚 相关文档

| 主题 | 文档 |
| --- | --- |
| 概述与设计立场 | [概述](00-overview.md) |
| 构建开关与端到端验证 | [快速开始](01-started.md) |
| 通信模型与各原语接口 | [通信模型](02-communication.md) |
| 消息序列化与后端配合 | [消息序列化](03-serialization.md) |
| `?qos=` 参数、profile 选型与可靠性约束 | [QoS 配置](05-qos.md) |
| 零拷贝容器与共享内存借用 | [零拷贝](06-zerocopy.md) |
| 消息级加密与后端兼容性 | [安全加密](07-security.md) |
| 共享内存守护进程、监控代理与服务发现 | [可观测性](12-observability.md) |
| 环境变量、URL 重映射与扩展 | [集成](13-integration.md) |
