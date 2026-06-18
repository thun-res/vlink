# url_basics — VLink URL 解剖 + `UrlParser` 用法

VLink 通过 URL 把"业务话题"与"传输后端"两层概念解耦：同一份业务代码只要改 URL 前缀，就能在 `intra://`、`dds://`、`shm://`、`someip://` 等十几种传输上运行，无需改任何调用代码。本示例覆盖：

- URL 各字段的语义（transport / host / path / query / fragment）。
- 通过 `vlink::UrlParser` 解析 URL；从字典构造 URL；带 override 的克隆。
- 各传输后端的典型 URL 实例（intra / shm / dds / ddsc / zenoh / someip / mqtt / fdbus / qnx）。
- 静态分类器 `vlink::Url::is_local_type` 等。
- 一对 Publisher/Subscriber 在 `intra://` 上跑通的最小演示，强调"传输无关"。

读完本示例你能掌握：

- 怎么写正确的 URL。
- URL 中的 query / fragment 在不同后端各代表什么。
- `UrlParser` 适合做什么、不适合做什么。
- 静态分类器 `vlink::Url::*` 在不构造 Node 的前提下做 URL 性质判断。

## 背景与适用场景

URL 是 vlink 用户对接通信框架的**唯一交集**：所有节点都通过 URL 指定要绑定的话题/服务/字段。URL 串的组成决定：

- 走哪个传输后端（transport 字段）。
- 在该后端里寻路什么资源（host + path）。
- 后端特定的可调参数（query 字符串）。
- 路由模式 / 服务前缀等附加信息（fragment）。

通用形状：

```
<transport>://<host>[/path][?key=value&...][#fragment]
```

例如：

- `intra://sensor/lidar?event=scan&pipeline=4#direct` —— 进程内传输，话题 `sensor/lidar`，附带分流参数和 direct 模式。
- `dds://vehicle/speed?domain=42&depth=10&qos=sensor` —— FastDDS，DDS Domain 42，KeepLast(10)，使用 `kSensor` QoS 预设。
- `someip://4660/22136?groups=1|2&event=16&field=1` —— SOME/IP，service ID 4660 / instance ID 22136；十六进制是 `0x1234` / `0x5678`。

`UrlParser` 是 vlink 内部用的 URL 解析器，应用层一般不需要直接用 —— Publisher 等节点构造时框架会自动解析。但调试、做测试、做配置加载时直接用 `UrlParser` 很方便。

URL 重映射通过 `VLINK_URL_REMAP` 环境变量配置（JSON 文件），运行时把 URL 前缀替换；详见 `doc/21-environment-vars.md`。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `vlink::UrlParser` | `explicit UrlParser(const std::string& url)` | 构造时解析 |
| `UrlParser::get_transport` | `std::string get_transport() const` | 取 transport 字段 |
| `UrlParser::get_host` | `std::string get_host() const` | 取 host |
| `UrlParser::get_path` | `std::string get_path() const` | 取 path |
| `UrlParser::get_port` | `std::string get_port() const` | 取 port（少数 URL 有） |
| `UrlParser::get_query` | `std::string get_query() const` | 取 query 字符串原文 |
| `UrlParser::get_query_dictionary` | `std::map<std::string, std::string> get_query_dictionary() const` | 解析后的 key=value 字典 |
| `UrlParser::get_fragment` | `std::string get_fragment() const` | 取 `#` 后内容 |
| `UrlParser::to_string` | `std::string to_string() const` | 重新拼回 URL 字符串 |
| `UrlParser(components, category, ...)` | 构造 | 从字典构造 URL |
| `UrlParser(other, overrides)` | 构造 | 基于现有 URL 克隆，按字典覆盖某些字段 |
| `vlink::Url::is_local_type` | `static bool is_local_type(const std::string&)` | 是否为本机传输（intra/shm/qnx） |
| `vlink::Url::is_intra_type` / `is_shm_type` | 同形 | 各自具体判定 |
| `vlink::Url::get_sort_index` | `static int get_sort_index(const std::string&)` | URL 在 vlink 内部用于排序时的 index |

## 代码导读

### 1. 每种传输的 URL 解剖

```cpp
void show(const std::string& url_str) {
  vlink::UrlParser parser(url_str);

  VLOG_I("URL:", url_str);
  VLOG_I("  transport=", parser.get_transport(), " host=", parser.get_host(),
         " path=", parser.get_path(), " port=", parser.get_port());
  VLOG_I("  query=", parser.get_query(), " fragment=", parser.get_fragment());

  const auto& dict = parser.get_query_dictionary();

  if (!dict.empty()) {
    for (const auto& [key, value] : dict) {
      VLOG_I("    ", key, "=", value);
    }
  }

  VLOG_I("  reconstructed=", parser.to_string());
}

int main() {
  show("intra://sensor/lidar?event=scan&pipeline=4#direct");
  show("shm://vehicle/speed?domain=1&depth=16&history=5&wait=1");
  show("dds://vehicle/speed?domain=42&depth=10&qos=sensor");
  show("ddsc://navigation/path?domain=1&qos=reliable");
  show("zenoh://robot/arm/joint1?domain=0&qos=sensor");
  show("someip://4660/22136?groups=1|2&event=16&field=1");
  show("mqtt://home/temperature?qos=1#tcp://192.168.1.100:1883");
  show("fdbus://audio/volume?event=level_changed#svc");
  show("qnx://sensor/radar?event=target_detected");
}
```

各传输的 URL 风格差异在于 query 里的可调参数 + fragment 的语义；`UrlParser` 一视同仁地拆字段，是否合法由对应后端校验。

### 2. 从字典构造 URL

```cpp
std::map<vlink::UrlParser::Component, std::string> components;
components[vlink::UrlParser::Component::kTransport] = "dds";
components[vlink::UrlParser::Component::kHost] = "vehicle";
components[vlink::UrlParser::Component::kPath] = "/telemetry/gps";
components[vlink::UrlParser::Component::kQuery] = "domain=5&qos=sensor";
components[vlink::UrlParser::Component::kFragment] = "";

vlink::UrlParser built(components, vlink::UrlParser::Category::kHierarchical, true);
VLOG_I("Built URL:", built.to_string());
```

当 URL 字段来自配置文件 / 数据库 / GUI 选择时，从字典构造比手工拼字符串更安全（避免漏掉 `?` 或 `&` 分隔符）。

### 3. 克隆并覆盖 query

```cpp
vlink::UrlParser original("dds://vehicle/speed?domain=0&qos=sensor");
std::map<vlink::UrlParser::Component, std::string> overrides;
overrides[vlink::UrlParser::Component::kQuery] = "domain=99&qos=best";

vlink::UrlParser modified(original, overrides);
VLOG_I("Original:", original.to_string(), " Modified:", modified.to_string());
```

典型用法：基础 URL 写死在配置里，运行时根据环境（开发 / 仿真 / 生产）覆盖某些字段。

### 4. 同 API 跨传输

```cpp
vlink::Subscriber<std::string> sub("intra://demo/url_basics");
sub.listen([](const std::string& msg) { VLOG_I("Received:", msg); });

vlink::Publisher<std::string> pub("intra://demo/url_basics");
pub.wait_for_subscribers();
pub.publish("Hello from url_basics example!");
```

把 URL 改为 `dds://demo/url_basics` 或 `shm://demo/url_basics`，代码不变就能切换后端。

### 5. 静态分类器

```cpp
VLOG_I("is_local_type('intra://x'):", vlink::Url::is_local_type("intra://x"));
VLOG_I("is_intra_type('intra://x'):", vlink::Url::is_intra_type("intra://x"));
VLOG_I("is_shm_type('shm://x'):", vlink::Url::is_shm_type("shm://x"));
VLOG_I("get_sort_index('intra://x'):", vlink::Url::get_sort_index("intra://x"));
VLOG_I("get_sort_index('dds://x'):", vlink::Url::get_sort_index("dds://x"));
```

这些 helper 不构造 Node、不做 IO，纯字符串判定。适合在节点构造之前做"URL 性质"分支决策。

## 运行

```bash
./build/output/bin/example_url_basics
```

预期输出（节选）：

```
URL:intra://sensor/lidar?event=scan&pipeline=4#direct
  transport=intra host=sensor path=/lidar port=
  query=event=scan&pipeline=4 fragment=direct
    event=scan
    pipeline=4
  reconstructed=intra://sensor/lidar?event=scan&pipeline=4#direct
URL:shm://vehicle/speed?domain=1&depth=16&history=5&wait=1
  ...
Built URL:dds://vehicle/telemetry/gps?domain=5&qos=sensor
Original:dds://vehicle/speed?domain=0&qos=sensor Modified:dds://vehicle/speed?domain=99&qos=best
Received:Hello from url_basics example!
is_local_type('intra://x'):1
is_shm_type('shm://x'):1
get_sort_index('intra://x'):...
```

无前置依赖：本示例只用了进程内传输。

## 常见陷阱

1. **path 必须以 `/` 开头**：vlink 期望 `/lidar` 而不是 `lidar`；写错时 host/path 边界判断会乱。
2. **query 顺序无意义**：`domain=1&depth=16` 与 `depth=16&domain=1` 等价。
3. **fragment 各传输的含义不同**：`intra` 的 `#direct` 表示派发模式；`mqtt` 的 `#tcp://192.168.1.100:1883` 表示 broker；`fdbus` 的 `#svc` 表示服务名。
4. **SOME/IP 的 service/instance 是数字**：`someip://4660/22136` —— 4660 是 host，22136 是 path 第一段；都是十进制写法，对应 hex `0x1234`/`0x5678`。十六进制 URL `someip://0x1234/0x5678` 也支持，但不能混写。
5. **URL 改了后端但 ABI 不兼容**：换后端后业务代码不需要改，但消息类型必须能被对应后端支持（如 someip 强烈推荐 FlatBuffers 而非 raw POD）。

## 设计要点

- URL 不携带类型信息 —— 接收端必须知道 `T` 是什么才能正确反序列化；vlink 用模板参数显式约束。
- query 字段在不同后端的可识别 key 不同；详见 `doc/07-transport.md` 与各 `samples/<transport>/README.md`。
- `Url::is_*` 仅基于字符串前缀判定，不验证后端是否真的可用（vlink 编译时可能没启用对应组件）。
- `UrlRemap` 是一层运行时映射：URL `dds://vehicle/speed` 可被替换为 `shm://vehicle/speed`，用于灰度切换、调试。

## 配图

无专属配图。URL 在 vlink 总体架构中的位置见 `doc/00-whitepaper.md`。

## 参考

- `../../samples/` — 每种传输的端到端样例（DDS、shm、SOME/IP、FDBus 等）
- `../../qos/` — query 中 `qos=...` 参数对应的 QoS 预设
- `vlink/include/vlink/impl/url_parser.h` — `UrlParser` 接口
- 顶层 `doc/07-transport.md` — 各传输后端的 URL 规则与可调参数完整列表
- 顶层 `doc/21-environment-vars.md` — `VLINK_URL_REMAP` 等运行时配置
