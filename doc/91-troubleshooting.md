# 91. 故障排查手册 · Troubleshooting

> 按"症状 → 可能原因 → 如何确认 → 如何处置"结构组织。搜索关键词请用浏览器 Ctrl+F。
>
> 万能第一步：`vlink-check diag`。它会一次性跑 40+ 项自检（见 [§91.1](#911-先跑-vlink-check-diag)）。
>
> **找不到你的问题？** 直接跳到 [§91.24 按错误字符串速查](#9124-按错误字符串速查reverse-lookup)。

目录：

1. [先跑 vlink-check diag](#911-先跑-vlink-check-diag)
2. [构建期问题](#912-构建期问题)
3. [启动期问题](#913-启动期问题)
4. [通信建立不起来 / 收不到数据](#914-通信建立不起来--收不到数据)
5. [性能问题（延迟抖、丢样本、CPU 高）](#915-性能问题延迟抖丢样本cpu-高)
6. [SHM / Iceoryx 专属](#916-shm--iceoryx-专属)
7. [DDS 专属](#917-dds-专属)
8. [Bag 录制回放](#918-bag-录制回放)
9. [C API / Python / 跨语言](#919-c-api--python--跨语言)
10. [CLI 工具异常](#9110-cli-工具异常)
11. [还没排出来？](#9111-还没排出来)
12. [每个 CLI 工具错误目录](#9112-每个-cli-工具错误目录)
13. [每个 Transport 专属 init 错误](#9113-每个-transport-专属-init-错误)
14. [运行时异常分类速查](#9114-运行时异常分类速查)
15. [Discovery 深入](#9115-discovery-深入)
16. [平台专属陷阱](#9116-平台专属陷阱)
17. [QoS + 安全兼容性](#9117-qos--安全兼容性)
18. [容器 / K8s / VPN 部署](#9118-容器--k8s--vpn-部署)
19. [插件加载失败](#9119-插件加载失败)
20. [Bag 损坏 / 恢复](#9120-bag-损坏--恢复)
21. [日志调优](#9121-日志调优)
22. [版本 / ABI 不匹配](#9122-版本--abi-不匹配)
23. [性能反模式 & 调试技巧](#9123-性能反模式--调试技巧)
24. [按错误字符串速查（Reverse Lookup）](#9124-按错误字符串速查reverse-lookup)

---

## 91.1 先跑 `vlink-check diag`

诊断命令会按顺序检查（源代码：`cli/check/check.cc`）。按分组整理如下：

| 分组           | 自检项 | 失败时候的大致含义 |
|----------------|---|---|
| 构建 / 主机    | `Check vlink version...` / `Check platform info...` / `Check hostname...` / `Check machine id...` / `Check cpu cores...` | 用于问题上报时收集的基础身份信息 |
| IP / DDS       | `Check available IP addresses...`                | 机器上只有 `lo` 或无 IP，所有跨机 transport 都不会工作 |
| IP / DDS       | `Check VLink DDS IP available...`                | `VLINK_DDS_IP` 设置了但不在本机 IP 列表里 |
| IP / DDS       | `Check VLink DDS interface...`                   | `VLINK_DDS_IP` 对应的网卡找不到 |
| IP / DDS       | `Check VLink DDS interface MTU...`               | 网卡 MTU < 1500：大报文会被分片甚至丢弃 |
| IP / DDS       | `Check VLink multicast address...`               | 路由表里没有 `239.255.0.100` —— **服务发现会失败** |
| IP / DDS       | `Check DDS multicast address...`                 | DDS 默认组播 `239.255.0.1` 未配通 |
| IP / DDS       | `Check VLINK_DDS_DOMAIN range...`                | 域 ID 超出 `[0,232]`，多数 DDS 栈会拒绝 |
| 内核 / 网络    | `Check net.core.rmem_max/wmem_max...`            | UDP 收/发缓冲太小：高码率 DDS/Zenoh 会丢样本 |
| 内核 / 网络    | `Check rp_filter...`                             | `rp_filter=1` 会静默丢弃异步网卡的多播回包 |
| 进程限额       | `Check RLIMIT_NOFILE...`                         | 文件描述符上限太低：DDS 会开不出足够 socket |
| 进程限额       | `Check RLIMIT_MEMLOCK...`                        | 锁页内存太小：SHM 零拷贝池会分配失败 |
| 文件系统       | `Check available space for log dir...`           | 日志盘不足（建议 >1 GB 余量） |
| 文件系统       | `Check log dir writable...`                      | 日志目录不可写（权限 / 只读挂载） |
| 文件系统       | `Check /dev/shm space...`                        | 共享内存池空间不足：SHM/shm2 / iceoryx 无法创建段 |
| 文件系统       | `Check VLINK_PLUGIN_DIR / PROTO_DIR / FBS_DIR`   | 环境变量指向的目录不存在或不是目录 |
| 文件系统       | `Check VLINK_QOS_CONFIG / FASTDDS_QOS_FILE / CYCLONEDDS_URI` | QoS / 后端配置文件路径无效 |
| 文件系统       | `Check VLINK_LOG_LEVEL value...`                 | 日志级别不在 `[0,6]` 范围（含 `6=Off`），会回退默认值 |
| 运行时健康     | `Check cpu usage...`                             | 当前负载过高（>90% 触发 FAIL）；后续测量不准 |
| 运行时健康     | `Check memory usage...`                          | 内存紧张（>90% 触发 FAIL）；SHM 分配可能失败 |
| 运行时健康     | `Check singleton lock...`                        | 单例锁目录存在争用，可能有残留进程或权限问题 |
| 运行时健康     | `Check time sync daemon...`                      | 没有 chrony/ntpd/systemd-timesyncd：bag 时间戳会乱序 |
| 运行时健康     | `Check iox-roudi running...`                     | RouDi 未运行，`shm://` 透明传输会失败 |
| 进程           | `Check proxy/bag/dump/eproto/efbs running...`    | 相关 CLI 是否已运行；排查重复启动或互相抢资源 |
| 进程           | `Check monitor/viewer/player/analyzer running...` | 同上 |
| 进程           | `Check bench/webviz running...`                  | 检查 `vlink-bench` 以及 `webviz` / `vlink-webviz` 进程；当前 WebViz 后端 `vlink-foxglove` / `vlink-rerun` 不在该诊断项内 |
| 进程           | `Check others running...`                        | 通过 `vlink-list -nc` 统计当前是否还有 vlink 用户进程 |

看到 `FAILED` 行的 `[DETAIL]` 列：直接给出了失败原因字串，参考下面各节处理。

**`vlink-check diag` 可选参数：**

- `-s` / `--summary`：执行完毕后打印 `PASSED / WARNING / FAILED` 统计，便于 CI 汇总；
- `-f SUBSTRING` / `--filter SUBSTRING`：只跑标题包含子串的诊断项，例如 `-f dds` 只关心 DDS 相关检查；
- `-a` / `--all`：附加打印所有 `VLINK_ENABLE_*` 编译选项状态。

**`vlink-check env`** 会把 `cli/check/check.cc` 内显式列出的那组 `VLINK_*` 环境变量逐条打印出来（是否已设置、当前值），并在末尾输出汇总行。清单项按编译时传输模块开关动态裁剪：未启用 `VLINK_SUPPORT_ZENOH` / `VLINK_SUPPORT_SHM2` / `VLINK_SUPPORT_MQTT` / `VLINK_SUPPORT_DDS*` / `VLINK_SUPPORT_SOMEIP` / `VLINK_SUPPORT_INTRA` 等模块时，相关 env 段会自动隐藏。`VLINK_BENCH_*` 由 `vlink-bench` 读取，不在 `vlink-check env` 的内置清单中。完整清单仍以 [`doc/21-environment-vars.md`](21-environment-vars.md) 为准。可用 `-p PREFIX` 按前缀过滤，例如 `vlink-check env -p VLINK_ZENOH_` 只看 Zenoh 专属变量。`-b/--available` 只显示已设置项。

**`vlink-check test`** 分两段：
- Part 1 在 `intra://` 上跑 Event / Method / Field 三种 paradigm 的最小往返（对应 `Publisher/Subscriber`、`Client/Server`、`Setter/Getter`）。
- Part 2 按 `VLINK_SUPPORT_*` 宏遍历当前构建进产物的所有传输后端（`intra / shm / shm2 / dds / ddsc / ddsr / ddst / zenoh / someip / mqtt / fdbus / qnx`），按后端能力运行 Event / Method / Field 子测试；SOME/IP 与 MQTT 当前只覆盖 Event。部分后端会先检查外部依赖，例如 `shm://` 通过 `vlink::ShmConf::auto_init_roudi(true)` 探测或尝试启动 RouDi，`mqtt://` 会检查 Broker URL 是否为空，`fdbus://` 会检查 name server。当前 SOME/IP 分支不检查 `VLINK_SOMEIP_CFG`，会直接尝试运行对应 Event 测试。

退出码 = **FAILED** 的子测试数；WARNING 只告警、不计入。

---

## 91.2 构建期问题

### 91.2.1 编译时 `static_assert: is_supported(type)` / `kUnknownType`

**原因**：你给 `Publisher<T>` / `Subscriber<T>` 等传了一个 `Serializer` 不认识的类型。

**如何确认**：看看 `T` 是否满足 [06-serialization.md](06-serialization.md#62-自动类型推断机制) 14 种触发条件中的某一个。

**修复**：
- POD：确保 `std::is_trivial_v<T> && std::is_standard_layout_v<T>` 都是 `true`（不能带虚函数、不能有非 trivial 的成员）
- Protobuf：用 `message` 类型，确保有 `SerializeToArray()` / `ParseFromArray()`
- FlatBuffers：用 object-API（继承 `flatbuffers::NativeTable`）或 builder 模式
- 自定义：实现 `operator>>(Bytes&)` 和 `operator<<(const Bytes&)` 让它匹配 `kCustomType`

### 91.2.2 `Could not find package vlink::<module>`

**原因**：你 `find_package(vlink REQUIRED COMPONENTS shm dds)` 但 VLink 构建时 `SKIP_SHM=ON` 或依赖缺失导致该 module 没编出来。

**确认**：跑 `vlink-info -l` 查看 `Modules:` 行（列出实际编译进来的传输模块名，例如 `intra;shm;dds;ddsc;...`）；构建期可在 CMake 配置日志里看 `SKIP_SHM` / `SKIP_DDS` / ... 这类 `option(SKIP_<NAME>)` 的取值。

**修复**：重新构建 VLink 时显式 `-DSKIP_SHM=OFF`，或在你的项目里改用 `find_package(vlink REQUIRED COMPONENTS all)` 自动适配当前安装。

### 91.2.3 找不到 `iceoryx_posh` / `fastdds` / 其它 third-party

**原因**：Conan 或系统包管理没装上这些依赖。

**修复**：用 Conan：`conan install . --build=missing`。或参考 [01-build.md](01-build.md) 逐个装。

### 91.2.4 Protobuf 版本冲突

**症状**：`link error: multiple definition of google::protobuf::...`。

**原因**：libprotobuf-lite 与 libprotobuf 混用；或 VLink 用系统 protobuf、你的项目用 vendor protobuf。

**修复**：两边统一。查 `pkg-config --modversion protobuf` 和 `grep 'find_package(Protobuf' /path/to/YourCMake.txt`。

### 91.2.5 `-Wconversion` / `-Werror` 开着的时候 VLink 头报一堆 warning

**原因**：VLink 头大量使用 `int64_t` 与 `size_t` 互转；对 `-Wconversion` 严苛的项目会爆。

**修复**：在 link 到 VLink 的 target 上临时 `target_compile_options(... PRIVATE -Wno-conversion)` 屏蔽该警告源。

### 91.2.6 `clang-tidy` 报一堆 `readability-identifier-naming` 在 VLink 头

**原因**：你在项目里开了全局 clang-tidy，但它把 VLink 头的 `kCamelCase` 判为违规。

**修复**：`.clang-tidy` 的 `HeaderFilterRegex` 只限你自己的代码，排除 VLink 安装路径（`/usr/local/include/vlink/.*`）。

### 91.2.7 `undefined reference to vlink::...`

**原因**：链接时缺 `vlink::vlink` 基础库，或只链了 module 没链 core。

**修复**：
```cmake
target_link_libraries(app PRIVATE vlink::vlink vlink::shm vlink::dds)
#                              ^^^^^^^^^^^^^^^
#                              必须！modules 依赖 core
```

---

## 91.3 启动期问题

### 91.3.1 构造 Publisher 直接抛异常（`vlink::Exception::RuntimeError`）

**常见根因**（源：`src/impl/url_parser.cc`）：
- URL 格式错误 —— 具体错误消息见 [§91.19](#9119-插件加载失败) 和 [§91.24 速查](#9124-按错误字符串速查reverse-lookup)：
  - `"The URL transport prefix cannot be empty."` → URL 不含 `xxx://`
  - `"Invalid character found in the URL transport prefix."` → scheme 含非法字符
  - `"A path is required on a hierarchical URL, even an empty path."` → 少了 path 部分
  - `"Bad key in the query string!"` → query 里写错参数名
- 指定的 transport module 未编进这次构建（例如用 `ddsr://` 但 `SKIP_DDSR=ON`）—— 运行时由 `url.cc` 抛 `"Unsupported plugin module"`
- DDS domain 冲突、多播端口被占用、或 Iceoryx RouDi 还没起

**确认**：
```bash
# 开启详细日志
export VLINK_LOG_LEVEL=0
./your_app 2>&1 | head -50
```

**修复**：按日志指示排查；或用 `InitType::kWithoutInit` 延迟 init、在外层捕获异常后重试。

### 91.3.2 进程一启动就挂死（没日志、没报错）

**常见根因**：
- `shm://` 但 SHM 守护进程没起（见 [§91.6](#916-shm--iceoryx-专属)）
- 多播网卡绑错：Linux 上路由默认走 eth0，但服务发现想走 eth1
- 使用了 `kWithInit` 的 Server/Client 且构造函数里阻塞等待连接
- Iceoryx RouDi 在运行但 mempool 耗尽（`loan()` 返回并阻塞）

**确认**：
```bash
strace -f -p PID                  # 看是否卡在 recvfrom / sem_wait
ls /dev/shm/                      # 看有无 iceoryx* 段
ps aux | grep -E "iox-roudi|vlink-proxy"
```

**修复**：
- **首选**：起 `vlink-proxy -c`（内嵌 iox-roudi + 针对 VLink 载荷分级的 chunk 内存池：默认 `-l 2` Middle = 7 档 / `-l 3` High = 8 档 / `-l 1` Low = 6 档；自带远程监控能力）—— 详见 [§91.6.1](#9161-shm-endpoint-报错启动不了)
- 多播网卡：用 `VLINK_DDS_IP=IP`（DDS 家族）或 `VLINK_ZENOH_MULTICAST_IF=IFACE`（zenoh）显式指定网卡；`VLINK_DDS_BIND=SCHEME` 只是在 URL 层选择 DDS 后端（如 `ddsc`/`ddst`），`VLINK_INTRA_BIND=SCHEME` 是把 `intra://` 重定向到其它后端（值是 scheme 名，如 `shm`/`dds`，不是网卡名），见 [21-environment-vars.md](21-environment-vars.md)
- 改用 `InitType::kWithoutInit` + 显式 `init()` + 超时控制

### 91.3.3 `Failed to load plugin` / `Unsupported plugin module`

**源码引用**：`src/impl/url.cc` 的 URL 插件加载路径，以及 `src/base/plugin.cc:268-395`（加载/创建）和 `src/base/plugin.cc:467-493`（插件 id / 版本校验）。

**原因**：`VLINK_URL_PLUGINS` 或 `VLINK_PLUGIN_DIR` 指向的 `.so` 找不到 / ABI 不匹配 / 不是 VLink 插件。

**修复**：检查文件存在；`ldd plugin.so` 看依赖；插件符号必须用 `VLINK_PLUGIN_EXPORT` 宏导出。完整插件错误见 [§91.19](#9119-插件加载失败)。

### 91.3.4 `Program has started.`（vlink-proxy 拒绝启动）

**源码**：`proxy/proxy.cc:110`。

**原因**：本机已有 `vlink-proxy` 在跑（同 domain id）。

**修复**：`ps aux | grep vlink-proxy`；找到旧实例 kill 掉，或改用不同的 `-d` 域。

---

## 91.4 通信建立不起来 / 收不到数据

### 91.4.1 `pub.has_subscribers()` 永远为 `false`

**检查清单**（从最常见排起）：
1. URL 是否**完全一致**？`shm://a/b` 和 `shm://a/b?qos=sensor` **不是同一个 endpoint**（domain 可能变）
2. `vlink-list -n` 能否看到两端都在？两端是否看对方？
3. Domain 是否一致？两端 URL 的 `?domain=X` 或 `VLINK_DDS_DOMAIN` 一致？
4. 安全模式是否匹配？一端 `kWithSecurity`、另一端 `kWithoutSecurity` —— **不通**
5. Serializer 类型是否匹配？一端用 Protobuf、另一端用 FlatBuffers —— 即使消息在线上看起来相似也匹配不上
6. 多播路由：`vlink-check diag` 中的 "VLink multicast address" 必须 PASSED

### 91.4.2 跨机器看不到对方

**高频根因**：
- 防火墙挡了 UDP（Linux 上 `sudo ufw status`；关：`sudo ufw disable`；或精准开放 239.255.0.0/24）
- 多播未启用：`ip link show eth0 | grep MULTICAST` 应该有 `MULTICAST`
- 容器网络：Docker `--net=host` 下 OK，`bridge` 下多播默认不通
- VPN / 跨子网：DDS 不路由 UDP 多播；跨 NAT 场景可用 `zenoh://`（内置 NAT 穿透），或在局域网两端各配置静态单播对端 IP（`VLINK_DDS_PEER` / `dds_impl` 的 `peer_ip`）

**修复**：
```bash
# 临时验证多播可达
iperf -s -u -B 239.255.0.100 -i 1     # 接收端
iperf -c 239.255.0.100 -u -T 1 -t 3   # 发送端
```

### 91.4.3 订阅回调偶尔少触发

**看两点**：
- QoS `History` depth：设置太小（如 `KeepLast(1)`）对突发流量会丢；`kSensor` 默认 10
- `set_latency_and_lost_enabled(true)` 后查 `get_lost().lost` —— 明确告诉你丢了几条

### 91.4.4 `SecurityPublisher` 与 `SecuritySubscriber` 连通但乱码

**原因**：双端 `Security::Config` 不一致（不同的 `key` / `passphrase`+`pbkdf2_salt`，一端使用空配置默认安全槽位而另一端显式配置，或一端配 RSA 密钥另一端配自定义回调）。

**修复**：两端通过 `SecurityXxx` 构造函数（第二参数）传入完全等价的 `Security::Config`，或双端都省略该参数使用内置默认安全槽位。AES-128-GCM 会在 tag 校验失败时返回 `false`，日志会出现 GCM authentication failed / RSA unwrap failed 等条目。

### 91.4.5 `Server::listen` 被调用了两次

**原因**：同一 Server 对 `listen` / `listen_for_reply` 多次注册，后者覆盖前者。

**修复**：一个 Server 只注册一个 callback；需要多条逻辑在 callback 里分发。

---

## 91.5 性能问题（延迟抖、丢样本、CPU 高）

### 91.5.1 P99 延迟明显高于 P50

**做法**：
```bash
# 1) 用 vlink-bench 基准对比
vlink-bench run --preset quick --report terminal

# 2) 订阅端开启延迟统计
sub.set_latency_and_lost_enabled(true);
VLOG_I("latency=", sub.get_latency(), " ns");
```

**可能原因**：
- CPU 没 isolcpu / governor 不是 performance
- MessageLoop 被阻塞任务挡住（避免在回调里做重活；用 ThreadPool 转出去）
- `History::kKeepAll` 让 Reliable 重传无限等待
- NUMA 错位：pub/sub 在不同 NUMA node，memcpy 穿桥延迟高
- 中断亲和性：网卡 IRQ 没 pin 到专用核

### 91.5.2 吞吐上不去

- 检 QoS：`Reliability=Reliable` + `PublishMode=Sync` 会序列化到单一网络栈栏；改 `ASync`
- 大消息：用 `zerocopy`（`loan()`），别让序列化层 memcpy
- DDS：检查 `block_time` 是否太长（默认 100 ms）；`VLINK_DDS_BUF` 调高 socket buffer
- MTU：大消息分片多 → 调 `VLINK_DDS_MTU` 或开 Reliable+Heartbeat 延长

### 91.5.3 CPU 占用异常高

- 关掉调试日志：`export VLINK_LOG_LEVEL=3`（Warn）或更高
- 关闭剖析器：`export VLINK_PROFILER_ENABLE=0`
- 用 `vlink-monitor` 看哪个 endpoint 的 `cpu_usage` 最高
- 序列化类型走 `kStreamType`（文本）性能远差于 `kStandardType`（二进制）或 `kBytesType`（直通）

### 91.5.4 延迟统计本身带来抖动

`set_latency_and_lost_enabled(true)` 需要每条消息附带时间戳并在订阅端做计算。生产环境建议关掉，只在调试时开。

---

## 91.6 SHM / Iceoryx 专属

### 91.6.1 `shm://` endpoint 报错启动不了

**原因**：SHM 后端依赖一个共享内存守护进程（Iceoryx RouDi）提前启动。如果该进程没起，所有 `shm://` 端点都会构造失败。

**✅ 首选修复：`vlink-proxy -c`**

VLink 自带的 `vlink-proxy` 工具在带 `-c` 时，会在自己进程里**内嵌启动 iox-roudi**，按 `-l` 选择分级 chunk 预分配内存池：默认 `-l 2`（Middle，7 档，等价 `proxy/etc/proxy_roudi.toml`）；点云/重载场景可切到 `-l 3`（High，8 档，等价 `proxy_roudi_large.toml`）；嵌入式轻量端侧 `-l 1`（Low，6 档，等价 `proxy_roudi_small.toml`）。下表展示 High 档（`-l 3`）的内存池布局：

| Chunk 容量 | 预分配个数 | 适配的典型载荷 |
|---:|---:|---|
| 1 KB | 10000 | 控制指令 / 心跳 / 小型状态消息 |
| 16 KB | 1000 | 小型结构化消息 / 配置 |
| 128 KB | 500 | IMU / GPS / CAN 批次 / 压缩后小帧 |
| 1 MB | 200 | 普通图像帧 / 压缩点云切片 |
| 3 MB | 100 | 高分辨率图像 |
| 6 MB | 50 | 多相机帧 / 中型点云 |
| 13 MB | 30 | 大尺寸点云 / 稀疏张量 |
| 24 MB | 20 | 超大负载 / 原始图像批 |

核心思路：**小 chunk 多、大 chunk 少**，既覆盖控制指令到原始点云的全量程载荷，又不会让少量大消息把小消息路径的 chunk 抢空。绝大多数 VLink 应用无需自己配 iox-roudi：

```bash
# 最省心的一行：-c 使用内置默认配置（推荐）
vlink-proxy -c

# 指定自定义 iox-roudi TOML 配置
vlink-proxy -c /path/to/custom_roudi.toml

# -l 选内存策略：1=Low / 2=Middle（默认）/ 3=High
vlink-proxy -c -l 3

# -m on/off 控制 Iceoryx 监控
vlink-proxy -c -m off

# 验证
ls /dev/shm | grep iox     # 应能看到 iceoryx* 段
```

`vlink-proxy` 同时提供跨机器/远程拓扑可视化能力（详见 [16-proxy.md](16-proxy.md)），一个进程就把"SHM 守护 + 远程监控"都带起来。

**⚠️ 备选：独立跑 `iox-roudi`**

仅当你有明确的部署限制（例如不能运行 `vlink-proxy`）时才需要这样做：

```bash
iox-roudi -c /path/to/iceoryx_config.toml &
```

需要自己维护 iox-roudi 配置文件；默认内存池往往太小不够 VLink 大消息用，**建议直接复制** `proxy/etc/proxy_roudi_large.toml` 作为起点。

<a id="shm-loan-failed"></a>

### 91.6.2 `loan()` 返回空 / 报 `Failed to loan buffer, size: ...`

**源码**：`modules/shm/shm_factory.cc:667, 704, 883, 951, 1119, 1161`。

**原因**：共享内存池配置不够大，或你一直 loan 但忘了 `publish`（泄露）。

**修复**：
- 用 `vlink-proxy -c -l 3`（High 内存策略，最大档位）
- 或者 `vlink-proxy -c /path/to/your.toml` 自定义更大的 mempool
- 检查每次 `loan()` 后是否都有对应 `publish()` 或显式 `return_loan()`
- 开 `set_manual_unloan(true)` 时一定要在回调里显式 `sub.return_loan(b)`（由订阅端归还，`Bytes` 自身没有该方法）

### 91.6.3 `Shm roudi is not supported.` 错误

**源码**：`modules/shm/shm_factory.cc:374`。

**原因**：代码路径里错误地假设可以用外部 RouDi；实际 VLink 的 shm module 只支持"代码侧 init_runtime 连到已经运行的 iox-roudi"。

**修复**：确保 RouDi（最好是 `vlink-proxy -c` 内嵌的那个）已在跑；业务进程里不要调 `init_roudi`。

### 91.6.4 `shm2://` 与 `shm://` 能否混用

**不能**。Iceoryx v1 和 v2 是完全独立的共享内存域。两端必须统一用同一个版本。
注意：`vlink-proxy -c` 内嵌的是 Iceoryx v1（对应 `shm://`）；`shm2://` 无需守护进程（Iceoryx2 无中心 daemon）。

### 91.6.5 `ShmConf: Input string length is too long.`

**源码**：`modules/shm/shm_conf.cc:60`。

**原因**：URL path 或 service name 超过 128 字符上限（Iceoryx 硬限制）。

**修复**：缩短路径；用 hash 替代长路径名。

### 91.6.6 `shm://` 同机多个应用启动时随机出错

**原因**：RouDi 的 segment 名字与 app 名字关联；如果两个 app 叫同样名字会冲突。

**修复**：每个进程用不同的 `process_name`（通过 `ShmConf::init_runtime(name)`），或确保 `argv[0]` 唯一。

---

## 91.7 DDS 专属

### 91.7.1 `dds://` 和 `ddsc://` 能互通吗

**RTPS wire 协议层互通**（FastDDS 和 CycloneDDS 都符合 OMG RTPS），但要保证：
- Domain ID 一致
- QoS 兼容（Reliability、Durability 方向正确）
- VLink 的 topic 命名要一致（URL path → DDS topic name 映射）

### 91.7.2 Multiple nodes with same DDS participant name

**原因**：VLink 为每个进程起一个 DDS Participant，但某些场景（同进程起多个 Publisher/Subscriber）共享同一个 Participant。正常。

### 91.7.3 DDS Security（RTPS SECURE）与 VLink Security

两者是正交的：**VLink Security 在 serializer 层加密 payload**，**DDS Security 在 RTPS 层加密报文**。可同时开启，但处理成本叠加。

### 91.7.4 kCdrType 的"安全例外"

`dds://` + `kCdrType` 跳过 VLink 的序列化层，直接交由 DDS 做 CDR 编码 —— 这意味着 **VLink 的 `kWithSecurity` 模板也对这条路径无效**。代码会在构造时直接打印 `"Cdr type does not support security."` 并退化为明文。如需加密，要么改走 Protobuf 等，要么开 DDS Security。

### 91.7.5 `Topic ... has no registered typesupport.`

**源码**：`modules/dds/dds_factory.cc:205`。

**原因**：你用了 CDR 类型但没 `register_type()` 注册到 DDS。

**修复**：在 SchemaPlugin 中 register schema，或直接用 Protobuf/FlatBuffers 让 VLink 自动处理。

### 91.7.6 `ssl.* properties are set but CycloneDDS was built without DDS_HAS_SSL support.`

**源码**：`modules/ddsc/ddsc_factory.cc:638`。

**原因**：你的 CycloneDDS 包编译时没带 OpenSSL。

**修复**：换个带 SSL 的 CycloneDDS 发行版；或者 `unset VLINK_SSL_*` 先跑起来。

### 91.7.7 RTI Connext（ddsr://）报 `DDS_DomainParticipantFactory_create_participant failed.`

**源码**：`modules/ddsr/ddsr_proxy.cc:57`。

**原因**：RTI license 过期、domain ID 不合法、`NDDSHOME` 没设、网络初始化失败。

**修复**：先确认 RTI 环境（`rtiddspingpub` 能否跑起来）；再回到 VLink。

### 91.7.8 TravoDDS（ddst://）上 CDR 完全被禁

**源码**：所有 `modules/ddst/ddst_*_impl.cc` 都有 `"Cdr type does not support security."`。

**结论**：`ddst://` + security 组合下不要用 CDR 类型。FlatBuffers / Protobuf 都可。

---

## 91.8 Bag 录制回放

### 91.8.1 `.vdb` 打不开 / 损坏（`Database version is incompatible.`）

**源码**：`src/extension/vdb_reader.cc:1747`。

**原因**：
- 录制时用的 VLink 版本太新，当前读取端太旧
- 或 bag 文件在 writer 还未落盘前 kill -9 导致 SQLite 头损坏

**修复**：
- 用 `vlink-bag check file.vdb` 检查完整性
- 用 `vlink-bag fix old.vdb`（可选 `-y/--rebuild` 进入重建模式，原地写入；无 `-o` 选项）尝试修复损坏段落
- 升级 VLink 到与录制端相同/更高的版本

### 91.8.2 MCAP 文件打不开（`Mcap version is incompatible.`）

**源码**：`src/extension/vcap_reader.cc:848`。

**原因**：MCAP 规范升级；你的 VLink 太旧。

**修复**：升级 VLink；或用 Foxglove Studio 之类的第三方工具直接读（因为 `.vcap` 就是 MCAP）。

### 91.8.3 回放速率跟不上原始速率

- 用 `-r 0.5` 放慢；用 `-r 2.0` 加速
- 解码开销大（Protobuf 大消息）：先 `vlink-bag clone SRC DST`（省略 `-p`，即不开压缩）得到无压缩版本再回放
- IO 瓶颈：NVMe → HDD 差一个数量级；存档用 HDD 没问题，回放建议放 SSD

### 91.8.4 `split_by_size` 和 `split_by_time` 能同时使用吗

可以；任一条件触发都会切。时间单位是**毫秒**。

### 91.8.5 VCAP（.vcap）与 MCAP 标准兼容吗

**是** —— .vcap 内部就是 MCAP。可以用 Foxglove 直接打开，也可以 `vlink-bag2mcap` 转一步（主要是改扩展名和补 schema 注册）。

### 91.8.6 `Cache size is full, waiting to consume.` 刷屏

**源码**：`src/extension/bag_processor.cc:98`。

**原因**：回放端消费速度跟不上读取速度；缓冲环已满。

**修复**：降低 `-r` 速率；或下调 QoS history depth（因为回调占用时间太长）。

### 91.8.7 `The number of messages has reached the upper limit`

**源码**：`src/extension/vdb_writer.cc:1373` 附近。

**原因**：达到 `max_row_count`（需先 `enable_limit=true`）；writer 按策略丢旧数据或丢新数据。

**修复**：加大 `max_row_count`，或启用 `split_by_size` / `split_by_time` 让文件自动切分，而不是丢数据。

### 91.8.8 `The compile macro VLINK_ENABLE_SQLITE is not turned on.`

**源码**：`src/extension/vdb_writer.cc:371`、`vdb_reader.cc:179`。

**原因**：VLink 构建时 `ENABLE_SQLITE=OFF`，.vdb 读写不可用。

**修复**：重新构建时打开 `-DENABLE_SQLITE=ON`；或只用 .vcap/MCAP。

---

## 91.9 C API / Python / 跨语言

### 91.9.1 `VLINK_RET_RUNTIME_ERROR` 是什么意思

`c_api.h:179` 定义：C++ 构造或初始化过程中抛出了异常。
常见触发：
- URL 格式错（`VLINK_RET_INVALID_ERROR` 单独用于空指针/坏 handle）
- Transport module 运行时初始化失败（例如 DDS 域启动失败）

**调试**：临时把 `VLINK_LOG_LEVEL=0` 看 C++ 层的异常 what() 信息。

### 91.9.2 Python 侧 `ImportError: cannot import _vlink_nanobind`

- 先确认装了 `ENABLE_PYTHON_API=ON` 构建出的 `_vlink_nanobind.so`
- `LD_LIBRARY_PATH` 包含安装目录的 `lib/python`
- Python 版本需与构建时一致（nanobind 是 ABI 强绑定）

### 91.9.3 `VLINK_RET_MEMORY_ERROR` 是什么

`vlink_get()` 之类：调用者给的 buffer 太小。先 `vlink_get(..., NULL, &size)` 查需要多少，再 allocate 重试。

### 91.9.4 `VLINK_RET_TRANSFER_ERROR`

publish / listen / invoke 操作失败。对 publisher：多数情况是没有订阅者（先 `vlink_has_subscribers` 检查）或者 transport 层 send 失败（网络/shm 问题）。

### 91.9.5 C API handle 用完忘了销毁，进程退出时卡住

**原因**：`vlink_destroy_*` 没调用；析构时 C++ 端试图等待 deinit，但 handle 已 leak。

**修复**：每个 `vlink_create_*` 必须有对应的 `vlink_destroy_*`；放入 RAII wrapper / `finally` 块。

---

## 91.10 CLI 工具异常

### 91.10.1 `vlink-list` 看不到任何节点

→ 跑 `vlink-check diag`。十有八九是 "VLink multicast address" FAILED（路由表没有 `239.255.0.100`）。

**临时 workaround**（Linux）：
```bash
sudo ip route add 239.255.0.100/32 dev eth0
```

### 91.10.2 `vlink-eproto sub` 报 `Must set proto dir`

- `-d /path/to/protos` 指定 proto 目录
- 或 `export VLINK_PROTO_DIR=/path/to/protos`
- 或运行 `vlink-eproto import /path/to/protos`（一次性写入 `$HOME/.vlink_proto_dir`，后续所有 shell 自动生效；`vlink-efbs` 同理用 `vlink-efbs import`）
- 或注册 `VLINK_SCHEMA_PLUGIN`（schema 插件 — 见 [19-extensions.md](19-extensions.md)）

### 91.10.3 `vlink-monitor` TUI 花屏

终端不支持某些控制序列。用 `TERM=xterm-256color`；或关闭分页 `--no-pager`（`vlink-bench` 里也一样）。

### 91.10.4 `vlink-bench run` 的某些 case 被 SKIPPED

这是设计：`intra://` + `mode=process` 会自动跳（跨进程 intra 无意义）；`quick` 预设也会做分层抽样。加 `--verbose` 看每个被跳过的原因。

### 91.10.5 `vlink-bag record` 报 `Sync mode and task depth cannot be set at the same time`

**源码**：`cli/bag/bag.cc:2832`。

**原因**：`-s/--sync_mode` 与 `--max_task_depth` 冲突（sync 不需要队列）。

**修复**：二选一。

---

## 91.11 还没排出来？

1. 收集：`vlink-info -l` 输出 + `vlink-check diag` 全文 + 一段有 `VLINK_LOG_LEVEL=0` 的日志
2. 精简复现：最小 pub/sub 双端程序 + URL
3. 到项目 issue 里提交，标签 `triage`

---

## 91.12 每个 CLI 工具错误目录

以下所有错误都是 CLI 工具 `std::cerr` 直接输出的 —— grep 你看到的字串可以精确定位到源码行。

<a id="cli-bag-errors"></a>

### 91.12.1 `vlink-bag`（`cli/bag/bag.cc`）

| 错误字串 | 行 | 常见原因 |
|---|---|---|
| `The target file not exists.` | 1150+ | 指定的 bag 文件不存在 / 路径错 |
| `Invalid time (input error).` | 1180+ | `--start` / `--duration` 时间戳格式错 |
| `Invalid time (duration error).` | 1246+ | duration ≤ 0 |
| `Invalid datatime.` / `Invalid systime.` | 1188+ | 时区 / 格式不支持 |
| `Can't find any urls to play.` | 1361 | bag 元数据缺失或空 URL 列表 |
| `The source file is same as target file.` | 1603 | clone 时源 = 目标 |
| `Tag name can not be empty.` | 2295 | `tag` 子命令缺名字 |
| `Sync mode and task depth cannot be set at the same time` | 见 `cli/bag/bag.cc` `--sync_mode` 校验段 | `-s` 和 `--max_task_depth` 冲突 |
| `Sync mode and memory size cannot be set at the same time` | 同上 | `-s` 和 `--max_memory_size` 冲突 |
| `You cannot use diff time formats at the same time` | 见 `cli/bag/bag.cc` 时间参数互斥校验段 | `-b` 用了多个冲突格式 |
| `BagWriter force to quit.` | 787 | 收到 SIGINT 或 OOM，writer 被强制终止 |
| `Failed to load plugin (...)` | 1259 | schema 插件缺失（见 [§91.19](#9119-插件加载失败)） |
| `filesystem_error` 回溯 | 多处 | 权限 / 磁盘满 / 路径不存在 |

### 91.12.2 `vlink-check`（`cli/check/check.cc`）

参见 [§91.1 的自检表](#911-先跑-vlink-check-diag) + 每项 FAILED 原因：
- `Empty IP address` → 机器无网卡或全部 down
- `Only find lo` → 物理网卡未启用
- `VLINK_DDS_IP is empty` → 你没设这个变量（WARN）
- `IP is invalid` → 你设的 `VLINK_DDS_IP` 不在本机 IP 列表
- `Cannot find 239.255.0.100` / `Cannot find 239.255.0.1` → 路由表缺多播项
- `Unsupported command: ip route` → 系统没有 `ip` 命令（古老发行版）
- `List running failed` → 无权限读 `/proc` 或 ps 不可用

### 91.12.3 `vlink-dump`（`cli/dump/`）

> 源码已拆分为多文件：`dump.cc`（main + 实时导出）、`dump_slice.cc`（切片/扫描）、`dump_extract.cc`（数据提取）、`dump_schema.cc`（schema 管理）。

| 错误 | 说明 |
|---|---|
| `A target URL is required for -t <type>. Use '*' only with slice/scan when selecting all topics.` | dump/export 模式必须指定具体 URL，不能省略或用 `*` |
| `CSV/JSON mode requires -c to specify fields.` | CSV/JSON 输出必须 `-c field1,field2,...` |
| `Expression (-x) requires -c to specify fields as variables.` | 用表达式必须先声明变量 |
| `Expression support requires exprtk library.` | 该 VLink 构建关闭了 exprtk；用 `-DENABLE_EXPRTK=ON` 重新构建即可启用表达式功能 |
| `Option --native is only valid for live dump/export modes.` | `--native` 不能与 slice/scan 混用 |
| `Option --native is only valid without -f/--bag_file.` | `--native` 与 `-f` 互斥 |
| `Option -b/--begin_time is only valid with bag input.` | `-b` 只对离线 bag 有效 |
| `Unknown type: ...` | `-t` 输出格式不认识，后附所有支持的格式列表 |
| `Failed to compile expression: ...` | `-x` 表达式语法错 |
| `Warning: No proto_dir or fbs_dir set, only zerocopy types will work.` | 忘了设 `-d` 或 `--fbs_dir` |
| `Option <name> is only valid with <modes> (current: -t <type>).` | 选项与当前 `-t` 模式不兼容（通用格式） |
| `Slice/scan mode requires -f/--bag_file.` | 切片/扫描需要 bag 文件输入 |
| `Slice mode requires one of: --window (-w), --segments, or --event.` | 切片需要指定切段方式 |
| `Slice mode accepts only one segment source: --window, --segments, or --event.` | 三选一，不能同时指定多个 |
| `Option --event requires -c/--condition to bind expression variables.` | 事件表达式需要字段变量 |
| `Option -c/--condition contains an empty field.` | `-c` 中有空字段（多余逗号） |
| `Option --url_filter requires at least one non-space keyword.` | URL 关键字不能全是空格 |
| `Use either positional url '...' or --urls/--url_filter, not both.` | 位置参数 URL 与 `--urls`/`--url_filter` 互斥 |
| `Option --black requires --urls or --url_filter.` | 黑名单模式需要先指定过滤条件 |
| `Use either --urls for exact URL matches or --url_filter for keyword matches, not both.` | 精确匹配与关键字匹配互斥 |
| `Option --quality_check is only valid with -t scan.` | 质量检查只在 scan 模式可用 |
| `Option --dropout_threshold requires --quality_check.` | 掉点阈值需要启用质量检查 |
| `Scan mode requires --event or --quality_check.` | scan 模式至少需要一个扫描条件 |
| `Option --export_csv requires -c/--condition.` | CSV 导出需要指定字段 |

### 91.12.4 `vlink-eproto` / `vlink-efbs`（`cli/eproto/eproto.cc`, `efbs/efbs.cc`）

共享的错误：
- `Cannot pub intra url.` → intra:// 不能跨进程 pub
- `Url is empty.` / `ser_type and encoding do not match.`
- `Must set proto dir [-d], set env 'VLINK_PROTO_DIR', run 'vlink-eproto import <dir>', or load VLINK_SCHEMA_PLUGIN.`（`vlink-efbs` 对应 `'VLINK_FBS_DIR'` 与 `'vlink-efbs import <dir>'`）
- `Proto txt file does not exist.` / `load_text_for_file failed.`
- `Blob content must be hex bytes.`
- `Cannot find proto.` / `Cannot find ser.`
- `Create root msg failed.`
- `JSON mode is not supported for zerocopy message types.`
- `Unsupported schema_type for eproto sub: ...`

### 91.12.5 `vlink-info`

- `Cannot find vlink-options.txt. Searched:` → `vlink-options.txt` 不在默认搜索路径（`cli/info/info.cc:76`），错误后随即列出所有候选路径
- `Cannot open options for path [...]` → 文件存在但无读权限（`cli/info/info.cc:88`）

### 91.12.6 `vlink-proxy`（`proxy/proxy.cc`）

- `Invalid domain id.` (L138) → `-d` 超出 0–255（`vlink-proxy` 自身校验范围；注意 `vlink-check diag` 针对 `VLINK_DDS_DOMAIN` 另外限制在 0–232）
- `Invalid input for [-m, --iox_monitoring].` (L159) → `-m` 只接受 on/off
- `RouDi for shm is not supported.` (L167) → SHM direct 模式要求内嵌 RouDi；确保 `-c` 已给
- `Program has started.` (L110) → 本机已有一个 proxy 实例
- **`-x/--max_packet_size` 设成 `0` 后所有非空消息被丢弃** → 过滤逻辑 `if (bytes.size() > real_max_packet_size) return;`（`proxy_server.cc:1161`）没有 `0 = unlimited` 的特判；CLI 默认是 `4.0` MiB，若需放行大包请显式设置足够大的 MiB 值（例如 `-x 64`）。头文件注释中写的 "0 = unlimited" 与当前实现不符

### 91.12.7 ProxyAPI 嵌入（`proxy/proxy_api/proxy_api.cc`）

- `ProxyApi: Non-controller nodes cannot send control.` (L309)
- `ProxyApi: Non-controller nodes cannot send data.` (L344)
- `ProxyApi: send_data requires url, ser, and a known schema.` (L351)
- `ProxyApi: send_data metadata does not match direct publisher.` (L373)

---

## 91.13 每个 Transport 专属 init 错误

### 91.13.1 `intra://`

- `IntraFactory: Intra does not support setting thread count.` — 别给 intra pool 设 `set_thread_count()`
- `IntraConf: Unknown node type, cannot parse protocol.` — URL path 为空
- `Two identical service requests.` — 同一进程里对同一 `intra://` URL 起了两个 Server

### 91.13.2 `shm://` / `shm2://`

见 [§91.6](#916-shm--iceoryx-专属)。额外：
- `ShmConf: Wait mode must be Event(Pub/Sub).` (`shm_conf.cc:116`) — shm 的 RPC 不支持阻塞等待
- `Shm2Factory: Bad event service name: ...` / `Failed to open or create event service '...'` — iceoryx2 服务名非法或冲突
- `Shm2Factory: Failed to init config.` — iceoryx2 TOML 加载失败
- `Shm2Factory: Failure in WaitSet::wait_and_process, ret=... result=...` — 事件循环层失败

### 91.13.3 `dds://` (Fast-DDS)

- `DdsConf: Invalid qos name: '...' (reserved or already registered).` — qos profile 名冲突
- `DdsConf: Unknown node type, cannot parse protocol.` — URL path 错
- `DdsFactory: Topic ... has no registered typesupport.` — schema 没注册
- `DdsFactory: Topic ... does not support BuiltIn::Raw.` — 当前 Fast-DDS 包不支持 raw 类型
- `DdsFactory: Method conf topic error.` / `... req and resp cannot be equal.` — RPC topic 规划错

### 91.13.4 `ddsc://` (CycloneDDS)

- 多数类似 dds，额外：
- `DdscFactory: Failed to take data.` — `dds_take()` 返回失败
- `DdscFactory: ssl.* properties are set but CycloneDDS was built without DDS_HAS_SSL support.` — CycloneDDS 包不带 TLS

### 91.13.5 `ddsr://` (RTI Connext)

- `DDS_DomainParticipantFactory_create_participant failed.` — 多半 license / network 错
- `DDS_DomainParticipant_create_topic failed.` — topic 已存在或命名冲突
- 完整错误链在 `modules/ddsr/ddsr_proxy.cc:57-254`

### 91.13.6 `ddst://` (TravoDDS)

- `DdstConf: Invalid qos name: '...' (reserved).` — qos 名非法
- `DdstFactory: Method conf topic error.` — RPC 路径错
- `DdstFactory: Failed to write data, does not support ptr type.` — CDR ptr 不被支持
- `Cdr type does not support security.` — 所有 6 种原语都会打（见 [§91.17](#9117-qos--安全兼容性)）

### 91.13.7 `zenoh://`

- `ZenohFactory: Failed to invoke [zc_init_log_from_env_or].`
- `ZenohFactory: Failed to invoke [z_config_default].`
- `ZenohFactory: Found invalid qos.` — qos 配置非法
- `ZenohFactory: Failed to invoke server [z_view_keyexpr_from_str].` / `[z_declare_queryable].`

配置可通过 `VLINK_ZENOH_CONFIG=JSON5`（zenoh-c 仅接受 JSON5 配置）、`VLINK_ZENOH_MODE=router|peer|client`、`VLINK_ZENOH_PEER=ENDPOINT` 等调节。

### 91.13.8 `someip://` (vsomeip3)

- `SomeipFactory: Someip does not support zero thread count.` — `set_thread_count(0)` 非法
- `SomeipFactory: Failed to init app.` — vsomeip 应用名非法或 JSON config 缺失
- `Two identical service requests.` — 同一 service+instance+method 注册多次

**env**：`VLINK_SOMEIP_CFG` 指定 vsomeip JSON 路径。

### 91.13.9 `mqtt://`

- `MqttFactory: Failed to invoke [MQTTClient_create], rc=...`
- `MqttFactory: Failed to invoke [MQTTClient_connect], broker=... rc=...`
- `MqttFactory: Failed to subscribe to topic '...', rc=...`

`rc` 的含义见 Paho MQTT C 文档（-1 = no connect，-2 = connect refused，等）。

### 91.13.10 `fdbus://`

- `FdbusFactory: Fdbus does not support zero thread count.`
- `FdbusFactory: Server bind failed.` — FDBus 名称服务未运行或该名字已被占

### 91.13.11 `qnx://`

- `QnxFactory: Qnx does not support zero thread count.`
- `QnxFactory: Server name_attach failed.` — 服务名冲突
- `QnxFactory: Server MsgReceive failed.` — 通道异常
- `QnxFactory: Client MsgSend failed.` — 服务端挂了 / 不可达
- `QnxFactory: Client name_attach failed.` — 客户端注册失败

---

## 91.14 运行时异常分类速查

### 91.14.1 Publisher 热路径

| 错误 | 位置 | 诊断 |
|---|---|---|
| `Failed to loan buffer, size: ...` | `shm_factory.cc:667+` | SHM pool 耗尽（见 [§91.6.2](#shm-loan-failed)） |
| `Failed to send, error: ...` | `shm_factory.cc:694+` | IPC channel 断 / 订阅端死 |
| `write_data() type mismatch, expected raw bytes but received ptr type.` | `dds_factory.cc:546` | 你发了原生对象进 raw-bytes topic |
| `write_cdr_data() type mismatch, expected ptr type but received raw bytes.` | `dds_factory.cc:564` | 你发了 bytes 进 CDR topic |
| `VCAPWriter: Writer is not open.` | `vcap_writer.cc:711, 775` | bag writer 还没 init |

### 91.14.2 Subscriber 热路径

- `Failed to take sample, error: ...` (`shm_factory.cc:1345`) — ring buffer 损坏或空
- `DdscFactory: Failed to take data.` — CycloneDDS 读失败（QoS 不匹配最常见）
- `VDBReader: Is busy.` (`vdb_reader.cc:*`) — 并发读 VDB（不支持）

### 91.14.3 Server / Client 热路径

- `QnxFactory: Server name_attach failed.` — 名字冲突
- `FdbusFactory: Server bind failed.` — 同上
- `DdsServer: Cannot find request id.` — 收到陌生 req_id 的 reply（协议乱）
- `Cdr type does not support security.` — 12 处都会打，见 [§91.17](#9117-qos--安全兼容性)

### 91.14.4 Setter / Getter

- `Bytes: Cannot shrink_to on non-owned Bytes.` — 对 shallow_copy / loan buf 改尺寸
- `Bytes: Cannot resize on non-owned Bytes.` — 同上
- `Bytes: Cannot deep copy self.` / `Cannot shallow copy self.` — 自复制
- `Bytes: Cannot move self.` — 自移动

### 91.14.5 Bag 内部（VDB + MCAP）

- `VDBWriter: Sqlite not open.` — 底层 DB 未初始化 / 被删
- `VDBReader: Invalid start_timestamp.` — `begin_time` 超出 bag 时间段
- `VDBReader: Cannot find any data for play.` — 过滤后结果为空
- `VDBReader: Incomplete data detected.` — bag 文件被截断
- `VCAPWriter: Compress is not supported without cache_size > 0.` — MCAP 压缩需 buffering

### 91.14.6 Base 组件

- `Timer: Callback is null for call_once.` / `Callback is not set.` — 启动定时器前没注册 cb
- `Timer: MessageLoop is null.` / `is not attached.` — 没 attach 到 loop 就 start
- `WheelTimer: Timeout too large (rounds overflow).` — 超出 wheel 可表达范围；改普通 Timer
- `WheelTimer: Failed to allocate a unique key.` — 并发冲突；重试或减压

---

## 91.15 Discovery 深入

**Discovery 地址**：`239.255.0.100` UDP 多播（`src/extension/discovery_reporter.cc`）。

### 91.15.1 `Failed to set multicast interface to loopback.` (`discovery_reporter.cc:206`)

- 内核不允许绑 loopback multicast（容器 / 某些 seccomp 配置）
- 修复：检查 `/proc/sys/net/ipv4/conf/lo/mc_forwarding`；或启用 `VLINK_DISCOVER_NATIVE=1` 切到单机走别的路径

### 91.15.2 `Failed to set multicast, please add address [...] to target device...` (`discovery_viewer.cc:563-573`)

- 路由表没有 239.255.0.100 的出口
- 修复：`sudo ip route add 239.255.0.100/32 dev eth0`（持久化改 `/etc/network/interfaces` 或 NetworkManager）

### 91.15.3 `Global discovery reporter is disabled.` (`discovery_reporter.cc:122`)

- `VLINK_DISCOVER_DISABLE=1` 被设上了
- 修复：`unset VLINK_DISCOVER_DISABLE` 或显式设为 0

<a id="discovery-url-ser-type-too-long"></a>

### 91.15.4 `Url is too long [...]`  /  `Ser type is too long [...]`

**源码**：`discovery_reporter.cc:306, 311`。

**原因**：URL 或 ser_type 超过 300 字符（Discovery 消息内 buffer 上限，见 `src/extension/discovery_reporter.cc:305-311`）。

**修复**：缩短 URL；用 hash 替代长路径名。

### 91.15.5 `DiscoveryViewer` 拿不到 `cpu_usage`

- `VLINK_PROFILER_ENABLE=0` 把剖析器关了
- 修复：打开剖析器或接受 cpu_usage=0

---

## 91.16 平台专属陷阱

### 91.16.1 Windows 相关（`#ifdef _WIN32`）

- `SysSharemem: CreateFileMapping failed.` — 权限 / quota / 名字冲突
- `SysSharemem: MapViewOfFile failed.` — 32-bit 进程 VA 耗尽；换 64-bit
- `SysSemaphore: CreateSemaphore failed.` / `WaitForSingleObjectEx failed.` / `ReleaseSemaphore failed.` — 命名信号量或 kernel 对象异常
- `Process: MultiByteToWideChar failed` — UTF-8 命令含非法字节序列
- 多播路由用 `route print`（不是 `ip route`）
- 服务发现的多播路径与 Linux 不完全一致；如 `netstat -an | findstr 239.255`

### 91.16.2 macOS（`#ifdef __APPLE__`）

- `SysSemaphore: dispatch_semaphore_create failed.` / `dispatch_semaphore_wait timeout or failed.` — macOS 用 GCD 语义
- Multicast 路由用 `netstat -rn`
- **`shm://` 不可用**：macOS 不支持 Iceoryx RouDi 的某些 POSIX 特性 → 改用 `dds://` / `intra://`

### 91.16.3 Linux / POSIX

- `SysSharemem: shm_open is not supported on this platform.` — 裁剪内核
- `SysSharemem: ftruncate failed.` — `/dev/shm` 不足或 tmpfs 配额
- `SysSharemem: mmap failed.` — VA 耗尽 / 32-bit
- `SysSemaphore: sem_open failed.` / `sem_close` / `sem_unlink` — 检查 `/dev/shm/sem.*`

### 91.16.4 Android

- `shm://` (Iceoryx v1) **不可用**（Android 不支持 RouDi daemon）；当前 `shm2://` 模块在 Android 构建中也会跳过。优先改用 `intra://`、`dds://`、`fdbus://` 等可用后端。
- 日志后端默认 `native`（logcat）
- NDK >= r25

### 91.16.5 QNX

- 有专用 `qnx://` transport（见 [§91.13.11](#911311-qnx)）
- `shm://` 可用但配置细节不同；需自己维护 RouDi
- 日志后端默认 `native`（slog2）

---

## 91.17 QoS + 安全兼容性

### 91.17.1 CDR + Security 全面不兼容

**出处**：12 处源码显式打印 `"Cdr type does not support security."`：

- `modules/ddst/ddst_publisher_impl.cc:60`
- `modules/ddst/ddst_subscriber_impl.cc:104`
- `modules/ddst/ddst_client_impl.cc:141`
- `modules/ddst/ddst_server_impl.cc:107`
- `modules/ddst/ddst_setter_impl.cc:36`
- `modules/ddst/ddst_getter_impl.cc:104`
- `modules/dds/dds_publisher_impl.cc:60`
- `modules/dds/dds_subscriber_impl.cc:151`
- `modules/dds/dds_client_impl.cc:182`
- `modules/dds/dds_server_impl.cc:159`
- `modules/dds/dds_setter_impl.cc:36`
- `modules/dds/dds_getter_impl.cc:151`

**原因**：CDR 编码路径直接交 DDS 管理，绕过 VLink 的加解密流程。

**修复**：换 Protobuf / FlatBuffers / Bytes；或启用 DDS-Security（RTPS 层加密）。

### 91.17.2 QoS 预设 vs 自定义 Qos

- 传入 URL 的 `?qos=xxx` 要小写（`kSensor` → `sensor`）
- 自定义 Qos 必须 `valid = true` 才生效
- `KeepAll` 的 depth 参数是**忽略的**（只对 KeepLast 有效）

### 91.17.3 `DdscFactory: ssl.* properties are set but CycloneDDS was built without DDS_HAS_SSL support.`

**源码**：`modules/ddsc/ddsc_factory.cc:638`。

**修复**：
- 改用支持 TLS 的 CycloneDDS 构建
- 或 unset `VLINK_SSL_*` 跑明文

---

## 91.18 容器 / K8s / VPN 部署

### 91.18.1 Docker 网络模式

| 模式 | UDP 多播 | 适用 |
|---|:-:|---|
| `--net=host` | ✅ | **推荐**，与宿主机共享 stack |
| `--net=bridge`（默认） | ❌（除非额外配置） | 不推荐用 dds:// / intra：跨容器用 host 或 overlay |
| `--net=container:other` | 取决于 other | — |

**修复**：启动容器时加 `--net=host`；或改用 unicast（`VLINK_DDS_PEER=IP`）。

### 91.18.2 Kubernetes / Service Mesh

- Pod-to-pod 多播默认禁。方案：
  - 用 hostNetwork: true（生产不推荐）
  - 改 unicast peer 列表（`VLINK_DDS_PEER=10.0.0.1,10.0.0.2`）
  - 用 `zenoh://` + Zenoh router pod 做中继
- env 注入：ConfigMap / Secret（`VLINK_*` 变量从 env 读）

### 91.18.3 VPN / 跨子网

- DDS / VLink 多播不路由到另一个子网；用 `ddsr://`、`zenoh://`、或自己搭 multicast relay
- `vlink-proxy` 可以跨子网桥接：一端起 `vlink-proxy -b SUBNET_A`，另一端起 `vlink-proxy -p SUBNET_B`

### 91.18.4 Docker `/dev/shm` 太小

容器默认 `/dev/shm` 只有 64 MB；iceoryx 用完就失败。

**修复**：`docker run --shm-size=2g ...` 或挂载 tmpfs。

---

## 91.19 插件加载失败

**源码**：`src/base/plugin.cc:268-493`。

| 错误 | 行 | 说明 |
|---|---|---|
| `Plugin: Plugin id is empty.` | 274, 400 | `VLINK_PLUGIN_REGISTER_BY_ID` 没带 id，或卸载时传入空 id |
| `Plugin: Lib name is empty.` | 282 | `Plugin::load` 第一个参数空 |
| `Plugin: Already loaded (...)` | 295 | 同一 lib 加载两次 |
| `Plugin: Cannot find plugin (...)` | 328 | `dlopen`/`LoadLibrary` 失败；`ldd` 看依赖 |
| `Plugin: Cannot find symbol function to create (...)` | 349 | 插件没导出 `create_*` 入口 |
| `Plugin: Failed to create handle (...)` | 359 | 插件的 create 返回 nullptr |
| `Plugin: Failed to load plugin (...): ...` | 390 | create 过程中抛异常 |
| `Plugin: Not loaded (...)` | 408 | `destroy` 之前没 load 成功 |
| `Plugin: Cannot find symbol function to destroy (...)` | 437 | 插件没导出 destroy |
| `Plugin: Failed to destroy handle (...)` | 445, 452, 458 | destroy 返回 false 或抛异常 |
| `Plugin: Plugin id mismatch: expected '...', got '...'` | 477 | lib 内部的 PLUGIN_ID 不对 |
| `Plugin: Version mismatch: local X.Y, required X.Z.` | 485 | ABI 版本不兼容 |

**通用排查**：
```bash
ldd plugin.so                          # Linux 依赖检查
nm -D plugin.so | grep create          # 看导出符号
export VLINK_LOG_LEVEL=0 && ./app      # 看 plugin.cc 的详细日志
```

---

## 91.20 Bag 损坏 / 恢复

### 91.20.1 流程

```
vlink-bag check broken.vdb            # 快速检测
  ↓ FAILED
vlink-bag reindex broken.vdb          # 重建索引（结构损坏但数据完好时）
  ↓ still bad
vlink-bag fix broken.vdb              # 原地修复（跳过损坏段落）
vlink-bag fix broken.vdb -y           # 重建模式（-y/--rebuild，破坏性更大）
```

### 91.20.2 常见错误消息

| 错误 | 源 | 可做的 |
|---|---|---|
| `VDBReader: Database version is incompatible.` | `vdb_reader.cc:1747` | 换回录制端相同版本 |
| `VDBReader: Incomplete data detected.` | `vdb_reader.cc:375` | `vlink-bag fix` 修 |
| `VDBReader: Invalid start_timestamp.` | `vdb_reader.cc:1777` | `-b` 超出 bag 时间段 |
| `VDBReader: Database accuracy is not supported.` | `vdb_reader.cc:1790` | VDB 精度版本差太多 |
| `VCAPReader: Mcap version is incompatible.` | `vcap_reader.cc:848` | 升级 VLink |
| `VCAPReader: DB list is empty.` | `vcap_reader.cc:1357` | MCAP 头损坏 |
| `VCAPReader: JSON parse error, ...` | `vcap_reader.cc:1482` | metadata JSON 坏 |

### 91.20.3 Kill -9 防护

录制时尽量不要直接 `kill -9` —— 这会让 SQLite 的 WAL 文件处于不一致状态。优雅退出用 `SIGINT` 或 `SIGTERM`（VLink 会 graceful drain）。

---

## 91.21 日志调优

### 91.21.1 级别

| env | 默认 | 值 |
|---|---|---|
| `VLINK_LOG_LEVEL` | `kDebug(1)` | 0=Trace / 1=Debug / 2=Info / 3=Warn / 4=Error / 5=Fatal / 6=Off |
| `VLINK_LOG_CONSOLE_LEVEL` | 继承 | 只控制 stdout/stderr |
| `VLINK_LOG_FILE_LEVEL` | 继承 | 只控制文件 sink |

### 91.21.2 日志文件轮转

```bash
export VLINK_LOG_DIR=/var/log/vlink
export VLINK_LOG_MAX_SIZE=$((100*1024*1024))   # 100 MB
export VLINK_LOG_MAX_COUNT=10                  # 保留 10 份
export VLINK_LOG_FLUSH_DELAY=1000              # 1 秒刷一次
```

### 91.21.3 线上不要 TRACE/DEBUG

`VLOG_T` / `VLOG_D` 在 Release 构建下被编译消除（零指令），**前提是** `VLINK_LOG_LEVEL` 宏在编译时就设了。运行时设置只影响不被消除的那部分。

### 91.21.4 backtrace 辅助调试

```cpp
vlink::Logger::enable_backtrace(50);
vlink::Logger::dump_backtrace();
```

### 91.21.5 自定义日志 sink（插件）

`VLINK_LOG_PLUGIN=/path/to/libmylogger.so`；实现 `LoggerPluginInterface`。见 [19-extensions.md](19-extensions.md)。

---

## 91.22 版本 / ABI 不匹配

### 91.22.1 Plugin 版本

- `Plugin: Version mismatch: local X.Y, required X.Z.` → plugin 要求的主/次版本不是当前 VLink 的版本
- Major 必须严格相同；Minor 向下兼容（`plugin.cc:483-490`，条件 `target_version_major != local_version_major || target_version_minor > local_version_minor`）
- 修复：重新编译 plugin 对齐 VLink 版本

### 91.22.2 Bag 版本

- 录制端 / 读取端 VLink 版本差距过大，可能出现 `VDBReader: Database version is incompatible.` 或 `VCAPReader: Mcap version is incompatible.`
- 修复：升级 reader；或在录制端省略 `-p/--compress` 录无压缩版本以减少版本敏感性

### 91.22.3 Python 绑定 ABI

`_vlink_nanobind.so` 对 Python 次版本敏感（3.10 != 3.11 的 nanobind ABI）；对 VLink C++ 侧 ABI 也敏感。

**修复**：重新 `pip install` 或重新构建 Python API。

### 91.22.4 `VLINK_VERSION` 检查

运行时：
```bash
vlink-info                  # 版本 / Git tag / commit id
vlink-info -l | grep ENABLE # 当前构建开启了哪些特性
```

代码里：
```cpp
static_assert(VLINK_VERSION_CHECK(2, 0, 0), "需要 VLink ≥ 2.0.0");
```

---

## 91.23 性能反模式 & 调试技巧

### 91.23.1 热路径里的代码气味

- 在 `Subscriber::listen` 回调里：文件 IO / 上锁 / new/delete / `std::regex` / `stringstream` / JSON 序列化 → **所有这些都是坑**。把重活转到 `ThreadPool`
- 每个消息都 `std::shared_ptr` 分配 → 用 `ObjectPool`
- 对 POD struct 用 `kStreamType` 强行文本化 → 用 `kStandardType`（memcpy）
- `History::kKeepAll` + 突发高频 → reliable 重传会堆积

### 91.23.2 常见"静默"警告（运行时打 WARN 但不崩）

- `WheelTimer: Timer is already running.` — 重复 start
- `BagProcessor: Cache size is full, waiting to consume.` — 回放太慢（源：`src/extension/bag_processor.cc`）
- `VCAPWriter: Compress is not supported without cache_size > 0.` — MCAP 压缩要 buffer
- `DiscoveryReporter: Url is too long` — URL > 300 字符被截断

### 91.23.3 性能调试黄金三步

```bash
# 1. 基线
vlink-bench run --preset quick --report terminal

# 2. 订阅端统计
sub.set_latency_and_lost_enabled(true);

# 3. CPU 剖析（本进程每个 endpoint）
VLINK_PROFILER_ENABLE=1 ./my_app
vlink-monitor             # 在另一个终端看 cpu_usage 列
```

### 91.23.4 SHM 状态速查

```bash
# Linux
ls -la /dev/shm/iceoryx_*            # 段名
ipcs -m                              # SysV shm
lsof /dev/shm/* | head              # 谁在用

# Windows
# 看 ProgramData\iceoryx*，或用 Windows Resource Monitor
```

### 91.23.5 DDS 状态速查

- Fast-DDS：`fastdds discovery`（需要 Fast-DDS tools）
- CycloneDDS：`cyclonedds inspect`
- RTI Connext：`rtiddspingpub`

---

## 91.24 按错误字符串速查（Reverse Lookup）

| 遇到的字符串 | 去哪看 | 一句话 |
|---|---|---|
| `The URL transport prefix cannot be empty.` | [§91.3.1](#9131-构造-publisher-直接抛异常vlinkexceptionruntimeerror) | URL 不含 `xxx://` |
| `Bad key in the query string!` | [§91.3.1](#9131-构造-publisher-直接抛异常vlinkexceptionruntimeerror) | URL `?` 后面的 key 打错 |
| `Unsupported plugin module, libname: ...` | [§91.3.3](#9133-failed-to-load-plugin--unsupported-plugin-module) / [§91.19](#9119-插件加载失败) | transport 没编进或 plugin 不存在 |
| `Cdr type does not support security.` | [§91.17.1](#91171-cdr--security-全面不兼容) | CDR + kWithSecurity 不兼容 |
| `Topic ... has no registered typesupport.` | [§91.13.3](#91133-dds-fast-dds) | DDS schema 没注册 |
| `Failed to loan buffer, size: ...` | [§91.6.2](#shm-loan-failed) | SHM mempool 不够大 |
| `Shm roudi is not supported.` | [§91.6.3](#9163-shm-roudi-is-not-supported-错误) | RouDi 模式错 |
| `Database version is incompatible.` | [§91.8.1](#9181-vdb-打不开--损坏database-version-is-incompatible) | bag 版本错位 |
| `Mcap version is incompatible.` | [§91.8.2](#9182-mcap-文件打不开mcap-version-is-incompatible) | MCAP 版本错位 |
| `Must set proto dir [-d], ...` | [§91.10.2](#91102-vlink-eproto-sub-报-must-set-proto-dir) | 动态 proto 没 schema 目录 |
| `Plugin: Version mismatch: local X.Y, required X.Z.` | [§91.22.1](#91221-plugin-版本) | plugin ABI 不对 |
| `Cache size is full, waiting to consume.` | [§91.8.6](#9186-cache-size-is-full-waiting-to-consume-刷屏) | 回放消费跟不上 |
| `DiscoveryReporter: Failed to set multicast ...` | [§91.15.1](#91151-failed-to-set-multicast-interface-to-loopback-discovery_reportercc206) | 多播接口配置问题 |
| `Invalid domain id.` | [§91.12.6](#91126-vlink-proxyproxyproxycc) | `vlink-proxy -d` 参数超出 0–255 |
| `VLINK_RET_RUNTIME_ERROR` | [§91.9.1](#9191-vlink_ret_runtime_error-是什么意思) | C++ 构造时抛异常 |
| `VLINK_RET_MEMORY_ERROR` | [§91.9.3](#9193-vlink_ret_memory_error-是什么) | 调用方 buffer 太小 |
| `VLINK_RET_TRANSFER_ERROR` | [§91.9.4](#9194-vlink_ret_transfer_error) | send/recv/invoke 失败 |
| `write_data() type mismatch, expected raw bytes but received ptr type.` | [§91.14.1](#91141-publisher-热路径) | 用错类型发 DDS |
| `BagWriter force to quit.` | [§91.12.1](#cli-bag-errors) | writer 被强制终止 |
| `The number of messages has reached the upper limit...` | [§91.8.7](#9187-the-number-of-messages-has-reached-the-upper-limit) | bag 达到 max_row_count |
| `ShmConf: Input string length is too long.` | [§91.6.5](#9165-shmconf-input-string-length-is-too-long) | URL path > 128 |
| `Url is too long [...]` / `Ser type is too long [...]` | [§91.15.4](#discovery-url-ser-type-too-long) | Discovery buffer 300 上限 |

找不到？去 [§91.11 还没排出来？](#9111-还没排出来) 走最后一步。
