# VLink 示例项目总览

本目录收录 VLink 的官方示例代码，覆盖从入门到工程化场景的各种用法。每一个子目录都是一个独立、可编译的工程，聚焦一个具体主题。

示例代码本身不是单元测试，目的有三：

1. **教学** —— 为新接入 VLink 的开发者提供逐步可读的学习路径。
2. **参考实现** —— 真实工程场景下的最佳实践（生命周期、错误处理、QoS 选择、安全配置）。
3. **可运行验证** —— 在搭建完成的 VLink 环境上立即跑通的端到端样例，方便排查传输/中间件配置问题。

本 README 只作为目录索引与构建指引；具体的架构、传输层、序列化机制说明请参阅根目录 `README.md` 与 `doc/` 下的专题文档。

---

## 1. 目录总览（14 个分类 · 77 个示例）

构建覆盖范围由两个 CMake 选项控制：

- `ENABLE_EXAMPLES=ON`（默认 `OFF`）开启示例构建。
- `ENABLE_EXAMPLES_ALL=ON`（默认 `OFF`）在已启用 `ENABLE_EXAMPLES` 的基础上额外编译除 `samples/` 之外的全部分类。

只启用前者时，仅编译 `samples/` 内的端到端样例；想跑分类教程时必须同时把 `ENABLE_EXAMPLES_ALL` 打开。`samples/dds_idl` 默认在 `samples/CMakeLists.txt` 中被注释禁用（依赖 FastDDS 的 `fastddsgen` IDL 工具链），需要手动取消注释才会加入构建。

| 目录 | 工程数 | 前置 | 说明 |
|------|------:|------|------|
| `quickstart/` | 3 | 无（`intra://`） | 三种通信模型的最小可运行示例 |
| `base/` | 22 | 无 | Bytes、Logger、MessageLoop、Timer、ThreadPool、GraphTask、Cancellation 等基础工具 |
| `serialization/` | 4 | quickstart | 基本类型 / 自定义类型 / DynamicData / 进程内零拷贝 |
| `communication/` | 7 | quickstart | Event、Field、Method（sync、async、fire-forget）完整用法 |
| `url_guide/` | 1 | communication | URL 结构与重映射（各传输实例集中在 `samples/`） |
| `qos/` | 3 | url_guide | QoS 基础、历史深度、预设配置文件 |
| `security/` | 3 | communication | AES-128-GCM、自定义回调、RSA 混合加密（含 PSS 签名） |
| `zerocopy/` | 7 | base/bytes | `loan()`+`RawData`、`CameraFrame`、`PointCloud`、`OccupancyGrid`、`Tensor`、`ObjectArray`、`AudioFrame` |
| `recording/` | 4 | communication | Bag IO（`.vdb`）、MCAP（`.vcap`）、压缩 |
| `plugin/` | 3 | base | 插件加载、`RunablePluginInterface`、Schema 插件 |
| `proxy/` | 3 | communication | ProxyAPI 客户端、ProxyServer、可运行代理插件 |
| `c_api/` | 4 | quickstart | C 语言绑定：pub/sub、RPC、字段、应用层加密 |
| `node_features/` | 4 | communication | 生命周期、MessageLoop 绑定、属性、状态监控 |
| `samples/` | 9 | 视示例而定 | 围绕具体传输协议的端到端样例 |

---

## 2. 各目录详细说明

### 2.1 quickstart/ —— 快速入门（3 个工程）

面向零基础读者的最小示例，统一使用进程内传输（`intra://`），无需任何外部依赖即可跑通。

| 工程 | 说明 |
|------|------|
| `hello_pubsub` | 发布订阅（Event 模型）入门 —— `Publisher<T>::publish()` 与 `Subscriber<T>::listen()` 的最短路径 |
| `hello_rpc` | 请求响应（Method 模型）入门 —— `Client<Req,Resp>::invoke()` 与 `Server::listen()` |
| `hello_field` | 状态读写（Field 模型）入门 —— `Setter<T>::set()` 与 `Getter<T>::get()` |

### 2.2 base/ —— 基础库教程（22 个工程）

VLink 基础工具库的完整教程，覆盖字节、日志、事件循环、定时器、调度、并发同步、任务执行等。这些组件被三种通信原语内部广泛使用，掌握它们才能写出高质量的应用层代码。

| 工程 | 说明 |
|------|------|
| `bytes_basic` | `vlink::Bytes` 基本操作（创建、访问、所有权检查） |
| `bytes_advanced` | Bytes 高级用法（compress/uncompress、Base64、CRC32/64、字节序） |
| `bytes_zerocopy` | Bytes 零拷贝语义（`shallow_copy`、`loan_internal`、引用计数） |
| `uuid_basic` | UUID v4 生成与字符串/字节互转 |
| `logger_basic` | 日志基本配置（VLOG_I/D/W/E 与 CLOG_D 风格） |
| `logger_advanced` | 日志高级用法（级别过滤、自定义后端、Logger 插件接口） |
| `message_loop_basic` | MessageLoop 事件循环入门（`run`/`quit`、任务投递） |
| `message_loop_advanced` | 跨线程任务投递、`Schedule` 调度回调、`invoke_task` 同步等待 |
| `timer` | Timer 完整用法（attach/start/stop、单次触发、`kInfinite`、动态调整间隔） |
| `elapsed_timer` | `ElapsedTimer` 耗时测量（CPU 时间戳、不同精度） |
| `deadline_timer` | `DeadlineTimer` 截止时间检测（与 cancellation 协作） |
| `thread_pool` | 线程池任务调度、`TaskHandle`、shutdown 行为 |
| `multi_loop` | 多事件循环协同 |
| `schedule` | `Schedule::Config` 与回调式调度 |
| `cancellation` | 协作式取消（`CancellationSource`/`Token`/`Registration`） |
| `task_handle` | Tracked 任务投递与全状态机覆盖 |
| `graph_task` | 图任务（DAG 依赖执行、`precede`/`succeed`、条件分支） |
| `spin_lock` | 自旋锁实现与典型场景 |
| `object_pool` | 对象池复用、ResetPolicy 选择 |
| `memory_pool` | 分级金字塔内存池、tier 配置、统计 |
| `process` | 子进程启动与 I/O 通道管理 |
| `utils` | 路径、线程、信号等平台工具 |

### 2.3 serialization/ —— 序列化类型（4 个工程）

VLink 通信原语通过模板参数确定消息类型，框架根据类型 trait 自动选择序列化策略。这一组示例展示不同类型在 `Serializer::Type` 下的归属与编解码行为。

| 工程 | 说明 |
|------|------|
| `basic_types` | Bytes、POD 标量、std::string 等基本类型的序列化对照（合并自原 `bytes_type`+`pod_type`+`string_type`） |
| `custom_type` | 自定义类型 + `operator>>`/`operator<<` 接入 `kCustomType` |
| `dynamic_data` | `DynamicData` 动态类型 —— 运行时类型擦除容器 |
| `intra_data` | 进程内零拷贝类型 —— `intra://` 专用的引用传递优化 |

### 2.4 communication/ —— 通信模型（7 个工程）

三种通信模型的完整教程，从基础用法到高级特性。

| 工程 | 说明 |
|------|------|
| `event_basic` | Event 模型入门 —— Publisher.publish() + Subscriber.listen() |
| `event_advanced` | Event 高级用法 —— `wait_for_subscribers`、`detect_subscribers`、批量发布、过滤回调 |
| `field_basic` | Field 模型入门 —— `Setter.set()` 写入 + `Getter.get()` 轮询读取 |
| `field_advanced` | Field 高级用法 —— change 回调（`Getter::listen`）、`wait_for_value` 阻塞读取、`std::optional` 处理 |
| `method_sync` | Method 同步调用 —— `Client.invoke(req, resp, timeout)` 阻塞等待 |
| `method_async` | Method 异步调用 —— 回调式 + `std::future` 模式 |
| `method_fire_forget` | Method 单向调用 —— `Client.send(req)` 不等待响应 |

### 2.5 url_guide/ —— URL 书写指南（1 个工程）

VLink URL 是连接通信原语与传输后端的纽带，统一形如 `<scheme>://<host>/<topic>[?param=value]`。各 transport 的具体 URL 形式集中在 `samples/`（如 `samples/shm_raw`、`samples/someip_flat`），本目录仅保留一份通用结构介绍。

| 工程 | 说明 |
|------|------|
| `url_basics` | URL 的组成、解析、UrlRemap 重映射机制 |

### 2.6 qos/ —— QoS 配置（3 个工程）

服务质量（Quality of Service）参数配置。VLink 的 QoS 模型与 DDS 对齐，但同时映射到非 DDS 后端（shm、someip、fdbus、mqtt 等）。

| 工程 | 说明 |
|------|------|
| `qos_basics` | QoS 基本概念与默认配置（Reliability、History、Durability、PublishMode、Liveliness） |
| `qos_history_depth` | 历史深度（消息队列长度）的影响 |
| `qos_profiles` | 预设 QoS 配置文件（`QosProfile::kEvent` / `kMethod` / `kField` 等） |

### 2.7 security/ —— 加密与认证（3 个工程）

通信加密。VLink 提供 `Security*` 前缀的通信原语变体（`SecurityPublisher` / `SecuritySubscriber` / `SecurityClient` 等），构造时通过第二参数 `const Security::Config&` 一次性传入对称密钥、RSA 密钥或自定义回调。

| 工程 | 说明 |
|------|------|
| `security_basic` | `Security::Config::key` 对称原始密钥 + AES-128-GCM；`passphrase` + PBKDF2 派生 |
| `security_custom` | 通过 `Config::encrypt_callback` / `decrypt_callback` 绕过内置 AEAD |
| `security_rsa` | RSA-OAEP 混合加密 + 可选 RSA-PSS 签名认证 |

### 2.8 zerocopy/ —— 零拷贝传输（7 个工程）

`shm://` 等支持借出（loan）的传输后端可以避免序列化过程中的拷贝。本组示例演示 loan API 与多种典型零拷贝数据类型。

| 工程 | 说明 |
|------|------|
| `zerocopy_basic` | `loan()`/`return_loan()` API + `RawData` 容器（合并自原 `zerocopy_loan` + `zerocopy_raw_data`） |
| `zerocopy_camera_frame` | `vlink::zerocopy::CameraFrame` —— 摄像头帧零拷贝传递 |
| `zerocopy_point_cloud` | `vlink::zerocopy::PointCloud` —— 点云数据零拷贝传递 |
| `zerocopy_occupancy_grid` | `vlink::zerocopy::OccupancyGrid` —— 2D 占据栅格零拷贝传递 |
| `zerocopy_tensor` | `vlink::zerocopy::Tensor` —— N 维张量零拷贝传递 |
| `zerocopy_object_array` | `vlink::zerocopy::ObjectArray` —— 3D 检测对象数组零拷贝传递 |
| `zerocopy_audio_frame` | `vlink::zerocopy::AudioFrame` —— 音频帧零拷贝传递 |

### 2.9 recording/ —— 数据录制与回放（4 个工程）

通信数据的录制、存储与回放，用于离线分析、回归测试、问题复现。

| 工程 | 说明 |
|------|------|
| `record_basic` | 录制与回放入门 —— `BagWriter` + `BagReader` |
| `record_bag` | `.vdb` 格式的写入、读取、时间/URL 过滤、`BagProcessor` |
| `record_mcap` | MCAP 格式支持（`VCAPWriter` / `VCAPReader`） |
| `record_compression` | 录制数据压缩（Zstd） |

### 2.10 plugin/ —— 插件系统（3 个工程）

VLink 插件化扩展机制，支持运行时动态加载模块（dlopen + `VLINK_PLUGIN_REGISTER` 宏）。

| 工程 | 说明 |
|------|------|
| `plugin_basic` | 插件基本加载与使用（`vlink::Plugin::load<T>()`） |
| `plugin_runnable` | `RunablePluginInterface` runnable 插件模式 |
| `plugin_schema` | Schema 插件（protobuf descriptor + flatbuffers reflection） |

### 2.11 proxy/ —— 代理 API/服务（3 个工程）

通过代理层访问 VLink 通信节点，适用于跨语言或远程场景（例如可视化前端、跨进程 debugger）。

| 工程 | 说明 |
|------|------|
| `proxy_api_basic` | `vlink::ProxyAPI` 客户端用法（Controller / Listener 角色） |
| `proxy_server_basic` | `vlink::ProxyServer` 代理服务端搭建 |
| `proxy_runnable_plugin` | 把 `RunablePluginInterface` 插件交给 `ProxyServer::Config::runnable_list` 加载并由 ProxyServer 统一驱动 |

### 2.12 c_api/ —— C 语言 API（4 个工程）

C 语言绑定，覆盖三种通信模型 + 应用层加密。适用于嵌入式系统、FFI 场景、第三方语言绑定。

| 工程 | 说明 |
|------|------|
| `c_pubsub` | C API 发布订阅 |
| `c_rpc` | C API 远程调用 |
| `c_field` | C API 字段读写 |
| `c_security` | C API 应用层加密（独立 `vlink_security_handle_t` + `vlink_create_secure_*`） |

### 2.13 node_features/ —— 高级节点特性（4 个工程）

VLink 通信原语共同的基类 `Node<ImplT, SecT>` 暴露的几个不常用但关键的能力。

| 工程 | 说明 |
|------|------|
| `lifecycle` | 节点 init/deinit/interrupt 生命周期 |
| `message_loop_binding` | 把节点绑定到外部 MessageLoop |
| `properties` | `set_property`/`get_property`（QoS、SchemaType、discovery 开关） |
| `status_monitoring` | `Status` / `StatusDetail` 回调与监控 |

### 2.14 samples/ —— 传输协议专项示例（9 个工程）

针对各传输后端的完整可运行示例。默认配置下（`ENABLE_EXAMPLES=ON` 而 `ENABLE_EXAMPLES_ALL=OFF`）只编译这一组。详见 [samples/README.md](samples/README.md)。

| 工程 | 传输协议 | 序列化格式 | 通信模型 | 进程模式 |
|------|----------|------------|----------|----------|
| `helloworld` | 多后端可切换 | Protobuf | Method + Event | 多进程（Server + Client） |
| `ping_pong` | 多后端可切换 | Bytes | Event（双向延迟测量） | 多进程（Ping + Pong） |
| `shm_raw` | `shm://` | Bytes | Method + Event + Field + Security | 单进程 |
| `dds_dynamic` | `dds://` | DynamicData + Protobuf | Method + Event | 单进程 |
| `dds_idl` | `dds://` | FastDDS IDL (CDR) | Method + Event | 单进程（默认禁用） |
| `ddsc_proto` | `ddsc://` | Protobuf | Method + Event | 单进程 |
| `pub_sub_fbs` | `ddsc://` | FlatBuffers | Event | 多进程（Pub + Sub） |
| `fdbus_proto` | `fdbus://` | Protobuf | Method + Event + Field | 单进程 |
| `someip_flat` | `someip://` | FlatBuffers | Method + Event + Field | 单进程 |

---

## 3. 推荐学习路径

根据目标和经验水平选择合适路径。所有分类都已经包含可运行源码；若只想先看默认构建产物，建议从 `samples/` 开始，再按主题回看对应分类。

### 3.1 路径 1：初学者入门

```
quickstart/ → base/ → serialization/ → communication/
```

**目标读者**：第一次接触 VLink 的开发者。

**学习要点**：

1. **quickstart/**：通过 3 个最小示例（hello_pubsub / hello_rpc / hello_field）理解 VLink 的三种通信模型。使用 `intra://`，无需部署外部中间件。
2. **base/**：掌握 VLink 基础设施 —— Bytes 字节操作、MessageLoop 事件循环、Timer 定时器、Logger 日志系统。这些组件在所有传输示例中被大量使用。
3. **serialization/**：学习 VLink 支持的各种数据类型 —— 原始字节、POD 标量、字符串、自定义类型、DynamicData。理解模板参数如何决定序列化策略。
4. **communication/**：深入三种通信模型的完整用法 —— 同步/异步 RPC、事件批量发布、字段监听、Fire & Forget。

**预计耗时**：2-3 天。

### 3.2 路径 2：URL 与传输协议

```
url_guide/ → qos/ → samples/
```

**目标读者**：负责对接不同传输后端的集成工程师。

**学习要点**：

1. **url_guide/**：理解 VLink URL 的统一格式（`<scheme>://<host>/<topic>[?params]`），掌握 UrlRemap 重映射机制。
2. **qos/**：按业务场景配置 QoS —— 可靠性、历史深度、预设配置。
3. **samples/**：在真实传输协议上运行端到端样例。重点看：
   - `helloworld/helloworld_common.h` —— 通过环境变量切换 6 种传输协议
   - `ping_pong/ping_pong_common.h` —— 同样模式的延迟测量版本
   - 各协议专项示例的 URL 差异

**实践建议**：通过 `helloworld` 的环境变量切换机制，用同一套代码依次跑 DDS、SHM、DDSC、FDBus 等后端，对比部署差异和性能特征。

**预计耗时**：2-3 天。

### 3.3 路径 3：高级功能

```
security/ → zerocopy/ → recording/ → proxy/
```

**目标读者**：需要安全通信、高性能传输或数据回放的高级开发者。

**学习要点**：

1. **security/**：通信加密 —— `Security*` 变体原语、`Security::Config` 配置、自定义回调。
2. **zerocopy/**：零拷贝传输 —— `loan()`/`return_loan()` API、`RawData`/`CameraFrame`/`PointCloud` 容器。
3. **recording/**：数据录制回放 —— `.vdb` 与 `.vcap`（MCAP）格式、Zstd 压缩。
4. **proxy/**：代理服务 —— 跨语言访问、远程代理、`RunablePluginInterface` 集成。

**实践建议**：`samples/shm_raw/` 完整演示了 Security 变体的六种通信原语，是学习安全通信最好的起点。

**预计耗时**：3-4 天。

### 3.4 路径 4：系统集成

```
c_api/ → plugin/ → node_features/
```

**目标读者**：把 VLink 集成进既有系统的架构师。

**学习要点**：

1. **c_api/**：C 语言绑定接入非 C++ 项目（嵌入式、FFI、其他语言）。
2. **plugin/**：插件机制实现模块化、运行时加载。
3. **node_features/**：节点生命周期、属性配置、状态监控等高级特性。

**实践建议**：同时参考 `samples/` 中各示例的 CMakeLists.txt，了解 `find_package(vlink COMPONENTS ...)` 与 `vlink_generate_cpp()` 的标准用法。

**预计耗时**：2-3 天。

---

## 4. 构建说明

### 4.1 从 VLink 源码树构建全部示例

```bash
cd /work/vlink

# 配置 Release 构建，启用全量 examples
cmake -DCMAKE_BUILD_TYPE=Release \
      -DENABLE_EXAMPLES=ON \
      -DENABLE_EXAMPLES_ALL=ON \
      -B build -S .

# 编译所有示例（缺依赖的示例会自动跳过，不会让构建失败）
cmake --build build -j$(nproc)

# 编译产物
ls build/output/bin/example_* build/output/bin/sample_*
```

若系统中安装过其他版本的 VLink，运行示例前请先把当前 build 的库路径加进 `LD_LIBRARY_PATH`，避免加载到 `/usr/local/lib` 中的旧库：

```bash
export LD_LIBRARY_PATH=$PWD/build/output/lib:$LD_LIBRARY_PATH
# 或者
source build/output/vlink-setup.sh
```

每个 `samples/*` 的 CMakeLists 都包含依赖检测逻辑。例如 `shm_raw` 会检测 `vlink::shm` 目标是否存在，不存在则打印 `Skip example_shm_raw.` 并跳过，不会让构建失败。

### 4.2 单独构建 examples 目录（基于已安装 VLink）

```bash
cd /work/vlink/examples
cmake -B build -S . \
      -DCMAKE_PREFIX_PATH=<vlink-install-path> \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### 4.3 单独构建某个示例

```bash
cd /work/vlink/examples/samples/helloworld
cmake -B build -S . -DCMAKE_PREFIX_PATH=<vlink-install-path>
cmake --build build
```

### 4.4 控制构建范围

顶层构建通过两个选项控制范围：

- `ENABLE_EXAMPLES=ON` 启用示例构建。
- `ENABLE_EXAMPLES_ALL=ON` 在默认 `samples/` 之外继续启用其余 13 个分类。

若只想调整 `samples/` 内部的子示例，在 `examples/samples/CMakeLists.txt` 中注释/取消注释对应的 `add_subdirectory()`：

```cmake
add_subdirectory(helloworld)
add_subdirectory(dds_dynamic)
# add_subdirectory(dds_idl)    # 默认注释 —— 需 FastDDS IDL 工具链
add_subdirectory(ddsc_proto)
add_subdirectory(pub_sub_fbs)
add_subdirectory(someip_flat)
add_subdirectory(shm_raw)
add_subdirectory(fdbus_proto)
add_subdirectory(ping_pong)
```

`dds_idl` 默认被注释掉，因为它需要 FastDDS 的 `fastddsgen` IDL 代码生成工具链，需手动启用。

---

## 5. 运行示例时的环境变量

VLink 示例广泛使用环境变量来切换传输协议与配置运行参数。`samples/helloworld` 和 `samples/ping_pong` 通过 `samples/common_transport.h` 共享同一套环境变量约定。

### 5.1 helloworld 环境变量

| 变量 | 说明 | 可选值 | 默认 |
|------|------|--------|------|
| `METHOD_TRANSPORT` | RPC 传输选择 | `dds`、`ddsc`、`shm`、`someip`、`fdbus`、`qnx` | `dds` |
| `EVENT_TRANSPORT` | 事件传输选择 | 同上 | `dds` |
| `METHOD_URL` | 直接指定完整 RPC URL（覆盖 `METHOD_TRANSPORT`） | 任意合法 VLink URL | 未设置 |
| `EVENT_URL` | 直接指定完整事件 URL（覆盖 `EVENT_TRANSPORT`） | 任意合法 VLink URL | 未设置 |

### 5.2 ping_pong 环境变量

| 变量 | 说明 | 可选值 | 默认 |
|------|------|--------|------|
| `PING_TRANSPORT` | Ping 端传输选择 | `dds`、`ddsc`、`shm`、`someip`、`fdbus`、`qnx` | `dds` |
| `PONG_TRANSPORT` | Pong 端传输选择 | 同上 | `dds` |
| `PING_URL` | 直接指定 Ping URL（覆盖 `PING_TRANSPORT`） | 任意合法 VLink URL | 未设置 |
| `PONG_URL` | 直接指定 Pong URL（覆盖 `PONG_TRANSPORT`） | 任意合法 VLink URL | 未设置 |

### 5.3 使用方式

```bash
# 方式 1：命令行前缀（仅对当前命令生效）
METHOD_TRANSPORT=shm EVENT_TRANSPORT=shm ./build/output/bin/sample_helloworld_server

# 方式 2：export 预设（对当前终端所有后续命令生效）
export METHOD_TRANSPORT=ddsc
export EVENT_TRANSPORT=ddsc
./build/output/bin/sample_helloworld_server

# 方式 3：直接指定完整 URL（最高优先级，覆盖对应的 *_TRANSPORT）
METHOD_URL="someip://0x01/0x02?method=0x1" \
EVENT_URL="someip://0x01/0x02?groups=0x1&event=0x2" \
    ./build/output/bin/sample_helloworld_server
```

---

## 6. 各传输后端前置条件

| 传输后端 | URL 前缀 | 前置守护进程 | 额外依赖库 | CMake 组件名 |
|----------|----------|-------------|-----------|-------------|
| 进程内 | `intra://` | 无 | 无 | `vlink::vlink` |
| FastDDS | `dds://` | 无 | FastDDS | `vlink::dds` |
| CycloneDDS | `ddsc://` | 无 | CycloneDDS | `vlink::ddsc` |
| RTI DDS | `ddsr://` | 无 | RTI Connext DDS（商用许可） | `vlink::ddsr` |
| TravoDDS | `ddst://` | 无 | TravoDDS | `vlink::ddst` |
| 共享内存（Iceoryx） | `shm://` | `iox-roudi` | Iceoryx | `vlink::shm` |
| 共享内存（Iceoryx2） | `shm2://` | 无（Iceoryx2 无中央守护） | Iceoryx2 | `vlink::shm2` |
| SOME/IP | `someip://` | `vsomeipd`（vsomeip routing manager） | vsomeip + FlatBuffers | `vlink::someip` |
| FDBus | `fdbus://` | `fdb_name_server` | FDBus | `vlink::fdbus` |
| QNX | `qnx://` | 无 | QNX OS | `vlink::qnx` |
| Zenoh | `zenoh://` | 无 | Zenoh | `vlink::zenoh` |
| MQTT | `mqtt://` | MQTT Broker | MQTT 客户端库 | `vlink::mqtt` |

---

## 7. VLink 六种通信原语速查

| 原语 | 通信模型 | 方向 | 创建方式 | 核心方法 |
|------|---------|------|---------|---------|
| `Publisher<T>` | Event | 发送方 | `Publisher<T> pub(url)` | `publish(msg)`、`wait_for_subscribers()`、`has_subscribers()` |
| `Subscriber<T>` | Event | 接收方 | `Subscriber<T> sub(url)` | `listen(callback)`、`set_manual_unloan(bool)` |
| `Server<Req, Resp>` | Method | 服务端 | `Server<Req, Resp> srv(url)` | `listen(cb)`、`listen_for_reply(cb)`、`reply(id, resp)` |
| `Client<Req, Resp>` | Method | 客户端 | `Client<Req, Resp> cli(url)` | `invoke(req)`、`send(req)`、`async_invoke(req)`、`wait_for_connected()` |
| `Setter<T>` | Field | 写入方 | `Setter<T> setter(url)` | `set(value)` |
| `Getter<T>` | Field | 读取方 | `Getter<T> getter(url)` | `get()` 返回 `std::optional<T>`、`wait_for_value()`、`listen(cb)` |

每种原语都有对应的 Security 变体（如 `SecurityPublisher<T>`）。生产环境通常先构造 `Security::Config`（例如 `cfg.key = "..."` 或 `cfg.passphrase = "..."`），然后作为 `SecurityXxx` 构造函数的第二参数传入：

```cpp
vlink::Security::Config cfg;
cfg.key = "my-secret-key-16";

vlink::SecurityPublisher<MyMsg> pub("shm://topic", cfg);
// 或：
vlink::SecurityPublisher<MyMsg> pub("shm://topic", cfg, vlink::InitType::kWithoutInit);
```

省略第二参数时使用内置默认安全槽位。运行期不再提供公共的 `enable_security()` / `security()` 接口 —— Security 必须在构造时一次性指定。

---

## 8. 代码规范

所有示例遵循 VLink 项目的代码规范：

- **代码风格**：Google C++ Style，行宽 120 字符（`.clang-format`）
- **静态分析**：开启 clang-tidy 严格检查，警告视为错误（`.clang-tidy`）
- **C++ 标准**：C++17（少数 base 示例按需启用 C++20 特性）
- **格式化工具**：`tools/format.sh`（clang-format + cmake-format）
- **代码检查**：`tools/check.sh`（cpplint，行宽 120）
- 示例代码**不**包裹 `NOLINTBEGIN`/`NOLINTEND` 块（该豁免仅限 `test/`）

---

## 9. 项目统计

| 指标 | 数值 |
|------|------|
| 分类目录总数 | 14 |
| 示例工程总数 | 77 |
| 全量构建覆盖工程数 | 76（`samples/dds_idl` 默认禁用，需手动启用 FastDDS IDL 工具链） |
| 覆盖传输协议 | intra、shm、shm2、dds、ddsc、ddsr、ddst、zenoh、someip、mqtt、fdbus、qnx（共 12 种 transport，部分依赖编译选项） |
| 覆盖序列化格式 | Protobuf、FlatBuffers、DDS IDL (CDR)、Bytes、DynamicData、自定义 op<< |
| 覆盖通信模型 | Method (RPC)、Event (Pub/Sub)、Field (Get/Set)、Fire & Forget |
