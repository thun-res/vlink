# VLink

![](doc/images/vlink.svg)

![](https://img.shields.io/badge/version-v2.1.0-informational.svg) ![](https://img.shields.io/badge/language-C++17-informational.svg) ![](https://img.shields.io/badge/license-Apache%202.0-informational.svg) ![](https://img.shields.io/badge/platform-Linux%20|%20QNX%20|%20Android%20|%20macOS%20|%20Windows-informational.svg)

[![CI Lint](https://img.shields.io/github/actions/workflow/status/thun-res/vlink/ci-lint.yml?branch=master&label=CI%20Lint)](https://github.com/thun-res/vlink/actions/workflows/ci-lint.yml) [![CI Test](https://img.shields.io/github/actions/workflow/status/thun-res/vlink/ci-test.yml?branch=master&label=CI%20Test)](https://github.com/thun-res/vlink/actions/workflows/ci-test.yml) [![coverage](https://img.shields.io/endpoint?url=https://thun-res.github.io/vlink/coverage/badge.json)](https://thun-res.github.io/vlink/coverage/) [![benchmark](https://img.shields.io/endpoint?url=https://thun-res.github.io/vlink/bench/badge.json)](https://thun-res.github.io/vlink/bench/)

[English](README.en.md) | 中文 · [官方网站](https://vlink.work) · [文档](doc/00-overview.md)

VLink 是面向自动驾驶与具身智能的高性能 C++17 通信中间件，定位为 ROS 2 的全场景替代方案。它以一套类型安全的统一 API 覆盖进程内、共享内存、车载以太网与跨机网络的全部通信需求，使通信后端的更换退化为一次 URL 前缀的修改，而业务代码不随之改动。

当前版本支持 12 种传输后端、14 种序列化格式、3 种通信模型与 6 个核心原语，并提供安全加密、录制回放、服务发现、10 个 CLI 工具及 Foxglove / Rerun 可视化桥接。

![VLink 架构](doc/images/readme-architecture.png)

---

## 🧩 核心抽象：URL 即通信契约

VLink 的全部设计围绕一个中心抽象：一次通信由"通信模型 + URL + 核心方法"三要素确定，后端是 URL 前缀的实现细节，对业务逻辑不可见。

```
<scheme>://<topic_name>[?参数]
```

`scheme` 决定后端，更换后端只需更换前缀，业务代码不变：

```cpp
vlink::Publisher<Imu> pub("intra://sensor/imu");  // 进程内
vlink::Publisher<Imu> pub("shm://sensor/imu");    // 同机零拷贝
vlink::Publisher<Imu> pub("dds://sensor/imu");    // 跨机网络
```

由此得到三项工程收益：业务逻辑与传输实现解耦；用 `intra://` 即可在单进程内完成端到端测试；部署拓扑由配置而非源码决定。

---

## 📡 通信模型

VLink 提供三种通信模型，对应六个核心原语，按通信语义选择，与后端无关。

![三种通信模型](doc/images/readme-communication-models.png)

**事件 Event —— 发布 / 订阅**

```cpp
vlink::Publisher<Imu> publisher("dds://sensor/imu");
publisher.publish(msg);

vlink::Subscriber<Imu> subscriber("dds://sensor/imu");
subscriber.listen([](const Imu& msg) { process(msg); });
```

**方法 Method —— 请求 / 响应**

```cpp
vlink::Server<Req, Resp> server("dds://calc/add");
server.listen([](const Req& req, Resp& resp) { resp.set_sum(req.left() + req.right()); });

vlink::Client<Req, Resp> client("dds://calc/add");
if (auto resp = client.invoke(req, std::chrono::seconds(3))) { use(*resp); }
```

**字段 Field —— 状态同步**

```cpp
vlink::Setter<Status> setter("shm://vehicle/status");
setter.set(status);

vlink::Getter<Status> getter("shm://vehicle/status");
getter.listen([](const Status& s) { use(s); });
```

| 模型 | 语义 | 原语 | 典型场景 |
| --- | --- | --- | --- |
| 事件 Event | 发布/订阅 | `Publisher<T>` / `Subscriber<T>` | 传感器数据、感知结果广播 |
| 方法 Method | 请求/响应 | `Client<Req,Resp>` / `Server<Req,Resp>` | 地图查询、参数读写、服务调用 |
| 字段 Field | 状态同步 | `Setter<T>` / `Getter<T>` | 车辆状态、配置参数、标定值 |

---

## 🚌 传输后端

| 前缀 | 底层 | 通信范围 | 零拷贝 | 状态 |
| --- | --- | --- | :---: | :---: |
| `intra://` | 内置队列 | 进程内 | 是 | 稳定 |
| `shm://` | Iceoryx | 同机跨进程 | 是 | 稳定 |
| `dds://` | Fast-DDS | 跨机网络 | 否 | 稳定 |
| `ddsc://` | CycloneDDS | 跨机网络 | 否 | 稳定 |
| `shm2://` | Iceoryx2 | 同机跨进程 | 是 | Beta |
| `ddsr://` | RTI Connext | 跨机网络 | 否 | Beta |
| `ddst://` | 国产 DDS | 跨机网络 | 否 | Beta |
| `zenoh://` | Zenoh | 跨机 / 云边 | 否 | Beta |
| `someip://` | vsomeip | 车载以太网 | 否 | Beta |
| `mqtt://` | Paho MQTT | 云端 | 否 | Beta |
| `fdbus://` | FDBus | 同机 | 否 | Beta |
| `qnx://` | QNX IPC | 同机（QNX） | 否 | Beta |

URL 语法、查询参数与各后端要点见 [传输后端与 URL](doc/04-transport.md)。

---

## 🚀 快速开始

![Quick Start](doc/images/quickstart-workflow.gif)

推荐使用 [VKit](https://github.com/thun-res/vkit) 构建。VKit 将多仓库源码拉取、跨平台工具链分发、分层组件编排与打包整合为统一流水线，在 Linux / QNX / Android / macOS / Windows 上以相同命令完成构建，且仅依赖 Bash 与 CMake。

```bash
git clone https://github.com/thun-res/vkit.git && cd vkit
make import_full      # 拉取 middleware 源码：vmsgs 与 vlink
make                  # 编译、部署并生成 runtime 包
```

亦可在已有 CMake 工程中单独集成 VLink：

```cmake
find_package(vlink REQUIRED COMPONENTS shm dds)
target_link_libraries(my_app PRIVATE vlink::vlink vlink::shm vlink::dds)
```

完整构建、交叉编译与集成方式见 [构建与集成](doc/01-started.md)。

---

## 🧰 工程生态

VLink 由三个协同的仓库构成：

| 仓库 | 职责 | 地址 |
| --- | --- | --- |
| VLink | 通信中间件本体（本仓库） | <https://github.com/thun-res/vlink> |
| VKit | 跨平台构建与发布套件 | <https://github.com/thun-res/vkit> |
| VMsgs | 标准消息定义库（感知/规划/定位/地图等领域，Protobuf / FlatBuffers） | <https://github.com/thun-res/vmsgs> |

---

## 📚 文档

> 📄 **[VLink 技术白皮书](doc/00-whitepaper.md)** —— 行业背景、技术论证、横向对比与性能分析的完整学术论述。

以下为面向使用的学习路径（16 篇），建议顺序阅读。

**入门**

| 文档 | 内容 |
| --- | --- |
| [0. 概述与设计哲学](doc/00-overview.md) | 定位、核心抽象（URL 即通信契约）、架构与能力全景 |
| [1. 快速上手](doc/01-started.md) | VKit 构建、第一个程序、示例导航 |

**核心**

| 文档 | 内容 |
| --- | --- |
| [2. 通信模型](doc/02-communication.md) | 节点基础与 Event / Method / Field 三模型 |
| [3. 消息序列化](doc/03-serialization.md) | 类型驱动的自动序列化 |

**传输与能力**

| 文档 | 内容 |
| --- | --- |
| [4. 传输后端与 URL](doc/04-transport.md) | 12 种后端与 URL 规范 |
| [5. QoS 配置](doc/05-qos.md) | 服务质量策略与 profile |
| [6. 零拷贝](doc/06-zerocopy.md) | 借贷接口与感知数据容器 |
| [7. 安全加密](doc/07-security.md) | 消息级加密与密钥管理 |
| [8. 基础库](doc/08-base-library.md) | Logger / MessageLoop / Timer / ThreadPool 等 |
| [9. 录制与回放](doc/09-recording.md) | Bag / MCAP 格式与接口 |

**工具与运维**

| 文档 | 内容 |
| --- | --- |
| [10. CLI 工具](doc/10-cli-tools.md) | 10 个命令行工具 |
| [11. 可视化（Viewer / WebViz）](doc/11-visualization.md) | 桌面 Viewer 与 Foxglove / Rerun 桥接 |
| [12. 服务发现与代理监控](doc/12-observability.md) | 拓扑发现与跨网段观测 |

**集成与参考**

| 文档 | 内容 |
| --- | --- |
| [13. 集成与扩展](doc/13-integration.md) | C API、插件系统、环境变量 |
| [14. 速查与故障排查](doc/14-reference.md) | 单页 API/URL/QoS/CLI 参考与症状索引 |
| [15. 测试与贡献规范](doc/15-contributing.md) | 测试框架与贡献流程 |

---

## 📊 项目报告

VLink 在每次版本发布时自动运行基准测试与覆盖率统计，并发布完整报告。

| 报告 | 在线报告 | Wiki 摘要 |
| --- | --- | --- |
| 🚀 性能基准（vlink-bench） | [在线查看](https://thun-res.github.io/vlink/bench/) | [Benchmarks](https://github.com/thun-res/vlink/wiki/Benchmarks) |
| 🧪 代码覆盖率 | [在线查看](https://thun-res.github.io/vlink/coverage/) | [Coverage](https://github.com/thun-res/vlink/wiki/Coverage) |

---

## 💻 平台支持

| 平台 | 架构 | 编译器 | 状态 |
| --- | --- | --- | :---: |
| Linux | x86_64 / aarch64 | GCC 9+ / Clang 10+ | 稳定 |
| QNX 7.x / 8.x | aarch64 / x86_64 | QCC（QNX SDP） | 稳定 |
| Android | aarch64 / x86_64 | NDK Clang r25+ | 稳定 |
| Windows 10+ | x86_64 | MSVC 2019+ / MinGW | 稳定 |
| macOS 10.15+ | x86_64 / arm64 | AppleClang 12+ | Beta |

---

## 📁 项目结构

```
vlink/
├── include/vlink/   公共头文件（6 原语 + 基础库 + 扩展 + 零拷贝）
├── src/             核心库实现
├── modules/         12 种传输后端实现
├── cli/             10 个命令行工具
├── proxy/           ProxyServer / ProxyAPI
├── viewer/          Qt 桌面可视化工具
├── webviz/          Foxglove / Rerun 桥接
├── c_api/           C API（供 Python / Rust 等 FFI 调用）
├── python_api/      nanobind Python 绑定
├── examples/        使用示例（14 个分类，29 个工程）
├── test/            测试套件
├── doc/             文档
└── CMakeLists.txt   顶层 CMake 入口
```

---

## 🤝 贡献

VLink 由 Thun Lu 维护，欢迎 PR、issue 与文档改进。提交前请阅读 [贡献规范](doc/15-contributing.md)。

## 📜 许可证

[Apache License 2.0](LICENSE) —— 可自由用于商业项目。

Copyright (C) 2026 Thun Lu. All rights reserved.
