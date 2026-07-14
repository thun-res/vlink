# VLink 与 eka-rt 热路径性能深度对比报告

> 分析日期：2026-07-14  
> VLink：`/work/vlink`，HEAD `7e4f32f`  
> eka-rt：`/work/eka/middleware/eka-rt`，HEAD `3de3c3e2`  
> 本报告先记录修改前基线与建议，随后追加了本轮克制的 MemoryPool、MessageLoop、ACK、MultiLoop、GraphTask、TaskHandle、FlatBuilder、Bag/extension、通讯日志与可视化快路径修复及回归结果；未形成充分证据的候选项仍只分析、不修改。

## 1. 结论摘要

本次检查得到的核心结论不是“VLink 整体都比 eka-rt 慢”，而是性能退化集中在少数基础设施，并通过复用关系放大到多个上层链路：

1. **修改前的全局 `MemoryPool` 是最明确的并发退化源，本轮已完成第一阶段修复。** 原实现让同尺寸对象争抢每个 tier 唯一的 `SpinLock` 和统计 cache line；现实现只在观察到真实争用后启用固定 free-list 分片，chunk/预分配额度不分片。最终固定核回归中 7 线程 128 B/1 KiB 已由修改前约 365.6/383.5 ms 降至约 3.95/4.33 ms，1 线程约 3.47/3.86 ms，没有单线程退化。收益已传递到 Bytes 和 Bag；ACK Request 根据同线程 tcache 模型单独切回 `std::make_shared`。
2. **普通 `MessageLoop` 修改前存在确定回退，本轮已修复两项根因。** normal/priority 入队现在直接在已有队列锁内合并唤醒状态，避免每个 wake episode 再获取一次相同互斥锁；normal worker 延迟构造并复用 staging deque，消除每个消费批次固定的 PMR map/block 分配。共享条件变量仍必须使用合并后的 `notify_all()`，因为同一 CV 还有 `wait_for_idle()` / `wait_for_quit()` 等等待者，机械照搬 eka 的 `notify_one()` 会有任务 worker 未被唤醒的风险。
3. **`ThreadPool`、`MultiLoop`、`GraphTask` 存在可复现的结构性开销，其中两项已克制收敛。** `MultiLoop` 修改前每任务除共享 callback 外还创建一个带自定义 deleter 的 `shared_ptr` 控制块；本轮只移除 completion 控制块，拒绝回退所需的共享 callback 保留。teardown 期间持 `pool_mtx` join 的既有锁环也已修复，ThreadPool 工作线程判断改为每线程一次登记的 O(1) TLS 比较。`GraphTask` 改为系统 `allocate_shared` 合并对象与控制块，独立 `Impl` 保留；复测 create/execute 均改善，拒绝把控制块改用 MemoryPool 或把 `Impl` 内联进公开对象。
4. **修改前 Intra 同步 RPC/ACK 是最明显的端到端退化点；ACK primitive 本轮已恢复。** MemoryPool 分片先把 7 线程 ACK 从约 251 ms 降到约 50 ms；allocator A/B 阶段确认 Request 属于同线程快速创建/销毁模型后，改回 `std::make_shared`。最终状态与锁序修复后，100k 次/线程的 1/2/4/7 线程中位约 12.19/12.36/12.29/13.01 ms，同轮 eka-rt 约 14.52/14.34/14.54/15.25 ms。Intra direct 4×50k 中位约 102 ms 对 eka-rt 约 202 ms；queue 中位约 540 ms 对约 565 ms，当前数据不再支持“queue 会中止”的旧结论，但仍不能据此宣称所有 RPC 组合全面领先。
5. **Bag 的磁盘吞吐没有确认退化，修改前退化集中在高并发生产者入队。** MemoryPool 修复后 7 生产者 VDB 的提交/最终耗时同轮已优于 eka-rt；这进一步支持“不修改 SQLite/VDB 落盘锁”的判断，但不同轮磁盘与调度噪声较大，只把修改前后和同轮中位数分别陈述。
6. **发现并修复一个继承自 eka-rt 的 FlatBuilder loan 生命周期缺陷。** 它不是 VLink 相对回退，但是真实的零拷贝性能/资源问题。现在先 `Finish()` 得到精确大小，再申请 transport 目标并直接复制一次；不再用 builder 指针覆盖 loan carrier。Publisher、Client、Server、Setter 的 8 个入口已统一，SHM/SHM2 连续发布和容量归还测试通过。
7. **VLink 新增模块中仍有绝对热点，但不能称为“相对退化”。** Perception 已按 URL 合并为最多一个 queued render，并保留渲染期间更新后的下一次调度；no-OSG 构建不再做无意义缓存/排队。SHM/SHM2 wait-mode 逐消息 INFO、Analyzer 每表达式 schema 查询和 Foxglove 两处只读独占锁也已删除。仍待权衡的是 MQTT client response 条件性深拷贝、WebViz 默认 queued 积压和 Proxy async payload copy；“提前构造 ProxyData”在 64 KiB 有利、在 1 MiB tier 边界反而有害，不能套用单一修复。
8. **没有证据支持把数据库锁、压缩锁、加密锁、SHM2 loan 表、用户回调锁或 GUI 大对象复制直接替换成自旋锁。** 当前 MemoryPool 已经证明“临界区很短”也不等于适合一个全局自旋锁；盲目扩大自旋锁使用会加重 CPU 消耗和尾延迟。
9. **补充专项测试后，功能基线是稳定的，但测试体系存在明确空洞。** Release CTest 212/212、C API 15/15、Python 21/27/43 三层均通过；最终源码的 ASan/LSan、clang-tidy 和编译矩阵结果见第 11 节。zerocopy 覆盖八种消息容器、公共 Header 与 MessageParser 共 10 个 suite；MQTT/FDBus 因本机缺外部服务而有 73 个受保护用例提前返回，不能写成数据面实测通过。Proxy、Viewer 和 CLI 没有对应的 CTest suite，因此本轮只能用专项微基准、GUI offscreen/Release 目标构建和 CLI 端到端转换/检查补证，仍需要仓库内可重复回归测试。

优先级建议如下：

| 优先级 | 项目 | 性质 | 建议 |
|---|---|---|---|
| P0 | 全局 MemoryPool 并发扩展性 | 第一阶段已修复并回归 | 自适应 tier 分片已落地；继续观察真实负载，不再叠加 TLS cache 或配置接口 |
| P0 | MessageLoop 普通队列唤醒与批次分配 | 已完成克制修复 | 入队锁内合并 pending，保留共享 CV 的 broadcast；normal worker 复用 staging deque |
| P0 | ACK request primitive | 已完成克制修复 | Request 使用 `std::make_shared`；保留 generation/CV，最小解析状态区分 ACK、取消和超时 |
| P0 | FlatBuilder loan | 继承缺陷，已修复 | Finish 后申请精确目标并 copy-to-loan；保留一次性 builder 语义，不再增加 prepare/复制封装 |
| P1 | MultiLoop/GraphTask/ThreadPool | MultiLoop token/teardown 与 GraphTask object/control 已修复，仍有差距 | 保留拒绝回退所需的共享 callback 和 GraphTask 独立 Impl；ThreadPool wrapper 无充分数据前不再改 |
| P1 | Bag 多生产者异步入队 | 已确认局部退化并修复 | MemoryPool/MessageLoop 收益叠加 Bag 的 untracked task/内存计费收敛；不改 SQLite/MCAP 锁 |
| P1 | Perception/Point3D/WebViz 队列 | Perception 已修复；其余为继承/条件性热点 | Perception 每 URL pending 已有界；Point3D 含逐帧统计、视频含帧间依赖，禁止机械合并；WebViz 先定义丢帧契约 |
| P1 | Proxy async payload 拷贝与尺寸边界 | 继承绝对热点/条件性 | 先处理 bounded queue 与 1 MiB tier 断点，再按 payload 分段选择 owning envelope；禁止无条件 prebuild |
| P2 | TaskHandle 状态设计 | 初始状态迁移已修复 | State 直接从 queued 开始，删除发布前锁/通知；lazy cancellation 会扩大并发语义，本轮明确拒绝 |
| P2 | MQTT/Zenoh 条件复制 | VLink-only 待谨慎优化 | 先明确回调生命周期和 Zenoh API 是否物化，再减少复制 |
| P2 | SHM wait INFO 日志、Analyzer 重复查询、WebViz 读锁 | 已完成局部清理 | 删除逐消息 INFO，schema 解析移出表达式循环，Foxglove 两处查询改 shared lock |

## 2. 范围、环境与防误判标准

### 2.1 覆盖范围

审计覆盖 VLink 第一方运行时代码和主要工具入口：

| 区域 | 规模（C/C++ 源/头文件） | 检查内容 |
|---|---:|---|
| `src/base` | 37 | 调度、内存、日志、同步原语、进程、插件、Timer、Schedule |
| `src/extension` | 17 | Bag/VDB/VCAP、播放、触发录制、Schema、Security、Discovery |
| `src/impl` + 公共模板头 | 17+ | Node、ACK、Publisher/Subscriber/Client/Server、序列化入口 |
| `src/zerocopy` + `include/vlink/zerocopy` | 10+ | POD wire format、深浅拷贝、解析、loan 配合 |
| 12 个通讯模块 | 222 | DDS/DDSC/DDSR/DDST/FDBus/Intra/MQTT/QNX/SHM/SHM2/SomeIP/Zenoh |
| `proxy` | 4 | Proxy API/Server，同步、异步、direct、转发 |
| `c_api` / `python_api` | 4 | 语言边界、GIL、buffer 生命周期、回调复制 |
| `viewer` | 112 | MainWindow、Point3D、Perception、OSG、FFmpeg、Player、Analyzer |
| `webviz` | 31 | ProxyBridge、Foxglove、Rerun、转换、WebSocket 发送 |
| `cli` | 42 | Bag/dump/monitor/bench/trigger 等入口与播放/输出路径 |

第三方库没有逐行重审其内部实现；检查的是 VLink 对 SQLite、MCAP、WebSocket++、Paho MQTT、Zenoh、Iceoryx、FFmpeg、OSG、spdlog/quill 等依赖的调用边界和生命周期。生成代码、协议库内部算法和未在本机启用的硬件/平台实现，不强行给出运行时结论。

### 2.2 当前工作树说明

VLink 工作树在分析开始前已经包含 Bag、MessageLoop、Python API、文档和测试等未提交改动。本报告比较的是**当前工作树实际代码**，并保留这些用户改动。本轮只处理由代码对照、专项测试或可复现基准支持的 MemoryPool、调度、ACK、Bag、FlatBuilder、日志与可视化热点，并同步对应单测、说明和 CHANGELOG；没有整理或覆盖第三方及无关未提交内容。后续复测应记录或提交该基线，否则数字无法严格复现。

### 2.3 测试环境

- CPU：Intel Core i7-12700，12 核/20 线程，单 NUMA；并发微基准主要固定到物理核 `0,2,4,6,8,10,12,14`。
- 内存：32 GiB，无 swap。
- 存储：NVMe（另有机械盘，但 Bag 基准使用当前工作目录所在文件系统）。
- 两边 Release 配置均为 `-O3 -DNDEBUG`；VLink 实际 compile commands 使用 `gnu++20`。eka-rt 主体为 C++17 配置。
- 数字采用多轮预热后的中位数；微基准源文件和二进制放在 `/tmp`，没有写入仓库。

### 2.4 判定门槛

只有同时满足以下条件才标为“已确认相对退化”：

1. 两边语义和负载足够一致；
2. 固定 CPU 后多轮可复现；
3. 差异超过正常抖动；
4. 能从代码结构或计数器解释差异。

以下情况单独分类，避免误判：

- eka-rt 本身已有的问题：标为“继承问题”；
- VLink 新增模块、eka-rt 不存在：标为“VLink-only 绝对热点”；
- 锁、分配或拷贝本身有合理生命周期要求：不直接判为问题；
- 平台/中间件无法在本机等价运行：标为“待目标环境验证”；
- 正确性修复带来的原子变量、弱引用和生命周期检查：若成本很小，不算回退。

## 3. 已确认的相对性能退化

### 3.1 全局 MemoryPool：P0，第一阶段已修复

以下代码位置与数字记录的是修改前基线：

- [`include/vlink/base/memory_resource.h`](include/vlink/base/memory_resource.h#L76) 在 Linux 有 `<memory_resource>` 时自动定义 `VLINK_ENABLE_BASE_MEMORY_RESOURCE`，即使构建系统没有显式开启。
- [`src/base/memory_pool.cc`](src/base/memory_pool.cc#L291) 每个尺寸 tier 只有一条 free list 和一个 `SpinLock`。
- [`src/base/memory_pool.cc`](src/base/memory_pool.cc#L417) allocate 必须持有 tier 锁弹出节点。
- [`src/base/memory_pool.cc`](src/base/memory_pool.cc#L474) deallocate 必须持有同一把锁压回节点。
- [`src/base/memory_pool.cc`](src/base/memory_pool.cc#L663) 每次操作还更新进程级原子统计。

同尺寸、短生命周期分配结果（128 B，150k 次/线程）：

| 线程数 | VLink | eka-rt | 结论 |
|---:|---:|---:|---|
| 1 | 3.59 ms | 6.67 ms | VLink 单线程池命中更快 |
| 2 | 47.79 ms | 40.57 ms | 开始出现竞争 |
| 4 | 144.18 ms | 72.18 ms | VLink 约 2.0 倍 |
| 7 | 364.66 ms | 141.52 ms | VLink 约 2.6 倍 |

1 KiB、7 线程约为 227 ms 对 84 ms。64 KiB 场景 VLink 有时更快，说明不能笼统说“VLink allocator 更慢”；问题集中在高频、同 tier、多核竞争。

额外交叉证据：

- 4 个独立 MessageLoop、每个提交 100k 任务：VLink 默认提交/总耗时约 16.46/29.38 ms，eka-rt 约 6.70/11.60 ms；`VLINK_MEMORY_LEVEL=0` 后 VLink 改善到约 11.89/17.16 ms。
- 4 线程 `Bytes` 128 B/1 KiB：默认池约 93.5/83 ms；level 0 约 14/14.7 ms；直接 aligned new 约 1.8/1.7 ms。level 0 仍有统计原子成本。
- ACK 与 Intra RPC 在 level 0 下大幅恢复，见 3.5。
- 7 生产者 Bag 入队在 level 0 下明显改善，见 3.6。
- 单独比较 `SpinLock` 本身时它并不比参考锁慢，因此根因是**共享层级设计**，不是简单把 `SpinLock` 换成另一个锁就能解决。

建议性改动：

1. 最克制的方向是为常用 tier 增加小容量 thread-local cache；批量从全局 tier 取/还节点，把每次分配的全局锁变为每 N 次一次。
2. 若不接受 TLS，至少按 CPU/线程哈希为每个 tier 分成 4～8 个 shard，分配优先本 shard，跨 shard 仅在空时发生。
3. hit/deallocate/bytes 统计改成 shard-local 或可关闭/批量汇总，避免 level 0 仍支付全局原子代价。
4. 在池扩展性修复并经过跨线程释放测试前，不要继续把新的高频对象迁入当前全局池。
5. 保留大块/跨线程释放场景的现有优势；不要用“一律关闭池”作为最终方案。

本轮采用建议 2/3 的保守实现，没有改变分配/释放接口；按后续要求仅增加一个可选批量参数：

- 每 tier 固定 8 个 free-list shard，但初始仍使用 primary shard；累计观察到 8 次 `try_lock()` 失败后才启用按线程分片，避免偶发单次碰撞永久改变模式，同时不在成功热路径增加计数原子。
- 分配优先本 shard，空时按 `MemoryPool::Config::batch_size` 批量窃取节点（默认 16）；`get_default_config()` 可由 `VLINK_MEMORY_BATCH_SIZE` 覆盖，显式 Config 不读取环境。只有扫描所有 shard 后才进入共享 chunk 增长。
- shard 只拆 free list、短临界区锁和高频 hit/deallocate 统计；chunk vector、增长上限、预分配 quota、trim/clear 和 oversized 路径仍按 tier 共享，没有形成 8 倍内存池数据。
- chunk 分配和 vector 变更使用低频 `std::mutex`，没有把可能包含系统分配的长临界区放进自旋锁。
- `VUNLIKELY` 只用于首次 TLS shard 初始化、溢出、OOM 等冷路径；free-list 为空属于负载相关分支，没有强行标冷。

同一固定 CPU、150k 次/线程、7 轮中位数的修改前后结果：

| 尺寸 | 线程 | 修改前 | 修改后 | 结论 |
|---:|---:|---:|---:|---|
| 128 B | 1 | 3.469 ms | 3.404 ms | 单线程无退化 |
| 128 B | 2 | 47.633 ms | 3.781 ms | 共享 tier 竞争消失 |
| 128 B | 4 | 181.389 ms | 3.904 ms | 随线程数基本平坦 |
| 128 B | 7 | 365.556 ms | 4.062 ms | 约 90 倍改善 |
| 1 KiB | 1 | 3.909 ms | 3.810 ms | 单线程无退化 |
| 1 KiB | 2 | 49.575 ms | 4.305 ms | 共享 tier 竞争消失 |
| 1 KiB | 4 | 182.965 ms | 4.220 ms | 随线程数基本平坦 |
| 1 KiB | 7 | 383.519 ms | 4.437 ms | 约 86 倍改善 |

交付前在空闲系统上再次复跑最终库：128 B 的 1/7 线程为 3.47/3.95 ms，1 KiB 为 3.86/4.33 ms，仍落在上述改善区间内，没有回归。

同轮 `Bytes::bytes_malloc/free` 对照中，VLink 128 B 的 4/7 线程中位数为 8.59/13.92 ms，eka-rt 为 86.22/141.24 ms；1 KiB 为 6.61/12.90 ms，eka-rt 为 70.86/131.82 ms。因此可以说 MemoryPool 直接相关路径当前优于 eka-rt，但不能外推为整个 VLink 全面优于 eka-rt。

内存占用也做了单独核验：字段布局收紧后，每个 shard 为 128 B，`MemoryTierState` 从 512 B 增到 1344 B；同时删除了只写不读的私有 `tier_max_sizes`，`Impl` 从 896 B 降到 704 B。默认 16 tier 的整池固定元数据净增 13,120 B。level 3 全量预分配仍精确为 16 tier、16 chunks、16 次 upstream allocation、16,777,216 B，`clear()` 后 chunk 为 0。也就是说付出了约 12.8 KiB 固定元数据，而不是把 16 MiB pool 数据扩大为 128 MiB。

#### 3.1.1 优化后与 eka/无池分配器的全尺寸对照

这里必须先纠正一个前提：当前 Linux 下 eka-rt 的 `Bytes` 并非“没有内存池”。其 `src/base/bytes.cc` 在 `__cpp_lib_memory_resource && __linux__` 时令 `ERT_BYTES_USE_MEMORY_POOL=1`，实际使用进程级 `std::pmr::synchronized_pool_resource`。因此本节同时比较三种实现：

1. VLink `Bytes` → 当前分片 `MemoryPool`；
2. eka `Bytes` → `std::pmr::synchronized_pool_resource`；
3. 真正无池的 glibc `malloc/free`，作为问题中“没有内存池方案”的代理。

以下覆盖 level 3 每个有效 tier 上界及 4 MiB bypass。每行在同一 payload/线程数下比较，采用 7 轮中位数；32 B～4 KiB 为 150k 次/线程，8～64 KiB 为 50k，128 KiB～1 MiB 为 10k，4 MiB 为 2k，因此不能跨行直接比较总毫秒数。

| size | VLink 1T | eka-pmr 1T | malloc 1T | VLink 7T | eka-pmr 7T | malloc 7T |
|---:|---:|---:|---:|---:|---:|---:|
| 32 B | 3.63 ms | 5.15 ms | 0.56 ms | 19.86 ms | 153.41 ms | 0.62 ms |
| 64 B | 3.62 | 5.15 | 0.60 | 4.24 | 129.46 | 0.69 |
| 128 B | 3.67 | 5.25 | 0.57 | 4.30 | 126.42 | 0.64 |
| 256 B | 3.69 | 5.15 | 0.57 | 4.33 | 132.20 | 0.66 |
| 512 B | 3.63 | 5.04 | 0.56 | 4.40 | 137.54 | 0.64 |
| 1 KiB | 3.69 | 5.02 | 0.57 | 4.28 | 136.60 | 0.67 |
| 2 KiB | 3.67 | 5.11 | 3.73 | 4.38 | 151.64 | 3.88 |
| 4 KiB | 3.75 | 5.04 | 3.73 | 4.68 | 148.61 | 3.86 |
| 8 KiB | 1.24 | 3.18 | 1.25 | 1.46 | 190.83 | 1.36 |
| 16 KiB | 1.27 | 3.35 | 1.28 | 1.47 | 187.61 | 1.33 |
| 32 KiB | 1.27 | 3.23 | 1.25 | 1.48 | 192.35 | 1.31 |
| 64 KiB | 1.40 | 3.46 | 1.27 | 1.54 | 205.05 | 1.33 |
| 128 KiB | 0.261 | 0.662 | 0.263 | 0.351 | 38.51 | 0.410 |
| 256 KiB | 0.260 | 0.655 | 0.262 | 0.410 | 39.12 | 0.362 |
| 512 KiB | 0.264 | 0.656 | 0.260 | 0.386 | 37.97 | 0.315 |
| 1 MiB | 0.271 | 0.652 | 0.261 | 0.359 | 37.94 | 0.348 |
| 4 MiB bypass | 0.100 | 0.134 | 0.055 | 0.834 | 7.90 | 0.097 |

可成立的结论有明确边界：

- **对当前 eka 实现：VLink 在本表 17/17 个尺寸的 1T 与 7T 吞吐中都领先。** 单线程通常约快 1.3～2.6 倍；7 线程有效 tier 多数快一个到两个数量级，主因是 eka 的进程级 synchronized pool 锁竞争。
- **对真正无池的 glibc malloc：不能说“大部分场景 VLink 领先”。** glibc tcache 对同线程立即 alloc/free 极强：≤1 KiB 时 VLink 单线程约慢 6 倍，7 线程也明显更慢；2 KiB～1 MiB 大多处于接近或约 ±20% 区间；4 MiB bypass 还要支付 VLink oversized 统计原子，明显慢于直接 malloc。
- 32 B 并非本轮新增，也不是相对旧版回退。把修改前源码直接嵌入同一基准后，32 B/7T 从约 365.5 ms 降到约 19.6 ms，改善约 18.6 倍，单线程 3.38/3.39 ms 持平。它只是没有达到 64/128 B 约 4 ms 的水平。尝试调整 steal 返回节点没有收益，已回退；删除 32 B tier 会让小对象落入 64 B 桶并增加内存占用，当前没有依据。

稳态逐次延迟（每个进程先预热，100k samples/线程，多轮范围）进一步说明同一结论：

| 场景 | VLink p50 / p99 | eka-pmr p50 / p99 | malloc p50 / p99 |
|---|---:|---:|---:|
| 1T，128 B～1 MiB | 36～56 / 38～99 ns | 48～97 / 64～247 ns | 15～48 / 17～86 ns |
| 7T，有效 tier | 40～180 / 53～317 ns | 0.73～3.29 / 2.4～17.5 μs | 15～70 / 17～106 ns |
| 7T，4 MiB bypass | 192～386 / 0.71～2.50 μs | 3.0～3.5 / 16～17 μs | 38～71 / 39～82 ns |

`max` 会受调度、缺页和采样线程影响，跨轮可到几十或数百微秒，故不把单次最大值当稳定 allocator 结论；p50/p99 与总吞吐方向一致。

VLink 的优势主要出现在跨线程归还，这是通信 pub/sub 更相关的模型。SPSC ring 中发布线程分配、消费线程释放，5 轮中位数如下；总耗时包含 ring 同步，因此只作端到端 allocator 模型对照：

| size / 次数 | VLink | eka-pmr | malloc |
|---|---:|---:|---:|
| 128 B / 1m | 104.6 ms | 3380 ms | 319.1 ms |
| 1 KiB / 1m | 99.0 | 3390 | 349.9 |
| 64 KiB / 300k | 44.4 | 103.4 | 141.3 |
| 1 MiB / 100k | 14.5 | 154.4 | 53.0 |
| 4 MiB bypass / 100k | 88.8 | 76.7（跨轮 71～701） | 90.3 |

因此对 **≤1 MiB 的跨线程 alloc/free**，当前 VLink 不但明显领先 eka，也在本机领先 malloc；对 bypass 大块则三者中位数接近，eka 尾部波动很大，不能宣称 VLink 全面领先。

资源占用使用 4 线程 burst、逐页触碰后读取当前 RSS；`retained` 为全部 free 后相对初始 RSS，VLink 还给出池内精确 upstream bytes：

| size × outstanding | VLink live / retained | eka-pmr live / retained | malloc live / retained | VLink `clear()` 后 |
|---|---:|---:|---:|---:|
| 128 B × 400k | +52.6 / +52.1 MiB（48.9 MiB upstream） | +52.4 / +4.5 MiB | +60.3 / +0.9 MiB | +2.4 MiB |
| 64 KiB × 1024 | +66.7 / +64.9 MiB（64.9 MiB upstream） | +64.4 / +9.0 MiB | +66.3 / +0.9 MiB | +0.7 MiB |
| 1 MiB × 64 | +64.7 / +64.8 MiB（64 MiB upstream） | +64.6 / +0.5 MiB | +66.5 / +0.4 MiB | +0.6 MiB |
| 4 MiB bypass × 16 | +64.5 / +0.5 MiB（0 pooled） | +64.4 / +0.5 MiB | +68.3 / +0.5 MiB | 不适用 |

VLink 的资源代价很明确：有效 tier 会把高水位 chunk 留在池内，空闲后 RSS 明显高于 eka/malloc；显式 `clear()` 可回到接近基线，但会带来暂停。level 3 `prealloc=true` 还会在启动时精确提交 16 MiB。故当前设计更适合重用稳定、跨线程交接频繁且重视延迟的常驻服务，不适合“偶发大 burst、随后长期空闲、又从不 trim”的进程。

#### 3.1.2 对外部审阅意见的逐条复核

- “一次 try_lock 失败永久分片”曾经成立，当前已改为累计 8 次真实失败后切换；成功热路径不更新计数。它仍是单向状态，8 次相隔很远的失败最终也会切换，报告不把它描述成可复位。增加时间窗/复位会给成功热路径增加原子写或时钟读取，当前证据不足。
- pub/sub 非对称释放是真实 workload，上表 handoff 已专项验证；默认 `batch_size=16` 显著优于 1，4/16/64/256 的最佳点随调度波动，不支持把默认值武断改成 64/256。保留 Config/env 调节是合理边界。
- “固定 offset 使所有线程集中撞低编号 victim”表述不准确：target `i` 首先看 `(i+1)%8`，绝对 victim 不相同。所有空 shard 最终追逐唯一有节点的 shard 时仍会争用，但那是非对称归还造成的单 victim，不是 offset 1 本身制造。
- `clear()` 确实在 `grow_mtx` 和全部 8 把 shard lock 下排序 chunks，并两遍扫描全部 free node；精确复杂度为 `O(C log C + F log C)`。并发测试中不调用 clear 时 p99/p999 约 50/52 ns；每 1 ms clear 时 p99/p999 约 51/53 ns，但单次最大分配延迟约 1.8 ms、clear p50 约 0.20 ms。旧单锁实现同样在 tier lock 下排序/扫描，实测尾延迟更差；且全仓搜索确认 VDB cache flush 不调用 `MemoryPool::trim()`。因此保留显式低频 trim，不重写为热路径计数或复杂增量回收。
- `tier_max_sizes` 只声明/写入而无读取，已删除；这是确定的冗余，不涉及公共 ABI。
- allocate/deallocate 信任调用方传回相同 `bytes` 的判断成立，但这是头文件已声明的 sized-deallocation 契约。增加指针归属校验需要块元数据或扫描 chunks，会伤害热路径和内存占用，本轮不加。
- `steal_free_nodes()` 先释放 source lock 再获取 target lock，任意时刻只持一把 shard lock；与 `clear()` 固定顺序持全锁不存在锁序环路。该正面判断成立。
- 进程级 TLS shard 编号会在超过 8 个热线程时发生复用，但每池 TLS map/随机状态会增加热路径和生命周期复杂度；当前 1/2/4/7 线程结果没有证据要求更换。
- `MEMORY_POOL_NEVER_DELETE=0` 的静态析构顺序风险和 never-delete/sanitizer 权衡是修改前已有设计，不由分片引入；宏拼写已规范，行为未改。

### 3.2 普通 MessageLoop：P0（本轮已修复锁与批次分配）

相关代码：

- [`include/vlink/base/message_loop.h`](include/vlink/base/message_loop.h#L128) callback 改为默认 64 B SBO 的 `MoveFunction`。
- [`src/base/message_loop.cc`](src/base/message_loop.cc#L80) NormalTaskTuple 因 callback 扩大到约 96 B；eka-rt 的任务 tuple 约 40～48 B。
- [`src/base/message_loop.cc`](src/base/message_loop.cc#L769) 修改前入队已经持有 `impl_->mtx`，成功后又经 `wakeup()` 获取同一个锁；现 normal/priority 在原临界区内设置 `wakeup_pending`，解锁后只在首次 pending 时通知。
- [`src/base/message_loop.cc`](src/base/message_loop.cc#L515) 公共 `wakeup()` 与 lock-free/Timer 路径仍保留 mutex 握手，防止 waiter 检查 predicate 到睡眠之间丢唤醒。
- [`src/base/message_loop.cc`](src/base/message_loop.cc#L1156) 修改前 normal 消费端每批构造/析构一个 PMR deque；现 worker 延迟创建并跨批次复用 staging deque，`spin_once()` 继续使用局部 deque 以保持重入隔离。

修改前固定物理核、普通队列结果：

| 场景 | VLink | eka-rt | 结论 |
|---|---:|---:|---|
| 单生产者，500k fixed-core | 159～308 ms | 65～79 ms | 稳定回退 |
| 单生产者，1m fixed-core | 546～658 ms | 140～147 ms | 约 3.7～4.5 倍 |
| 4 生产者，600k | 148～150 ms | 111～128 ms | 约 17～35% |
| ping-pong | 255～263 ms | 239～254 ms | 小幅回退 |

4m 任务 perf 采样中，VLink 约 2.17B instructions，eka-rt 约 1.44B，差异不是单纯调度噪声。某些 burst 场景 VLink 会胜出，因此修改前结论限定为普通队列的持续高频投递。

修复后的独立 A/B（固定 CPU、相同 benchmark 二进制，wakeup-only 与 staging 版本交替运行）：

| 场景 | 仅消除二次锁 | 再复用 staging | 结论 |
|---|---:|---:|---|
| 单生产者持续 1m，12 组中位 | 约 200.5 ms | 约 115.3 ms | 约 -42.5%；调度仍有波动 |
| 同场景最差样本 | 约 269.1 ms | 约 146.0 ms | 批次碎片导致的长尾收敛 |
| ping 100k，6 组中位 | 约 252.6 ms | 约 241.7 ms | 约 -4.3%，与 eka 同轮约 236～243 ms 接近 |
| blocked submit 1m | 约 31.77 ms | 约 31.41 ms | producer-only 路径基本中性 |
| 4 producer × 150k | 约 94.6 ms | 约 87.3 ms | 有正收益但噪声较大；与 eka 同轮约 ±10% |

分配计数闭合了因果关系：1m 任务时 64 B tier hit 从“约等于消费批次数 + 3”（中位约 189k）降为固定 4；512 B tier hit 从中位约 321k 降至约 200,005，剩余部分对应 96 B task tuple 的 deque block，不再包含每批 staging block。最新 lazy 版本只在 normal worker 首次运行时创建 staging：未运行 loop 不多占 64/512 B 池块，仅因 `Impl` 中的 optional 增加约 72～96 B/loop；已运行 normal loop 额外保留一份空 deque，固定成本约 0.6 KiB 级，restart 复用而不反复分配。

最终源码完整重建后的同轮 eka 对照（每次结果本身取 6 个计时样本的中位数，再交替跑 3 组）：

| 场景 | VLink 最终 | eka-rt | 判断 |
|---|---:|---:|---|
| normal 1 producer × 500k | 39.96～48.12 ms | 69.55～74.12 ms | 本轮 VLink 明显领先 |
| normal 4 producer × 150k | 107.12～111.35 ms | 103.87～107.99 ms | 基本持平，VLink 约 0～7% 慢 |
| normal ping 100k | 229.67～238.76 ms | 231.27～235.66 ms | 约 ±3%，视为持平 |

因此旧的“普通 MessageLoop 持续投递慢 2～4 倍”只适用于修改前基线，不能再描述当前实现；同样不能把单生产者领先外推成所有 producer 数和调度形态全面领先。多生产者过程中仍偶见 MemoryPool SpinLock 超限告警，说明 task node 跨线程归还仍有独立尾部争用，不能继续用 MessageLoop 状态机改动掩盖。

共享 `impl_->cv` 同时承载 worker、`wait_for_idle()` 和 `wait_for_quit()`。因此虽然 eka 在任务 wakeup 中使用 `notify_one()`，VLink 不能安全照搬：signal 可能唤醒 predicate 仍为 false 的 quit waiter，而唯一 worker 继续睡眠。最终实现保留合并后的 `notify_all()`，只删除确定冗余的第二次 mutex；没有引入第二套 CV、额外状态机或 locked atomic exchange。负载相关的 pending/staging 分支不标 `VLIKELY` / `VUNLIKELY`。

仍未处理的候选项只有 task wrapper 尺寸。为 MessageLoop/ThreadPool 单独改为 24/32 B SBO 会改变 ABI 和 inline capture 行为，现有数据不足以支持本轮修改；公共 `MoveFunction` 默认值保持不变。

### 3.3 ThreadPool、MultiLoop、GraphTask：P1

ThreadPool 空任务结果：

| 场景 | VLink | eka-rt |
|---|---:|---:|
| 1 worker/1 producer blocked submit 300k | 6.88 ms | 4.85 ms |
| 1 worker end-to-end | 37.2 ms | 30.2 ms |
| 4 workers/1 producer | 235.8 ms | 209.8 ms |
| 4 workers/4 producers | 145.1 ms | 113.6 ms |

MessageLoop 不应为了复用而改成内部持有 `ThreadPool(1)`：VLink 与 eka-rt 当前都由 MessageLoop 自己维护单消费者线程。MessageLoop 还必须支持同步 `run()`/`spin_once()`、timer、`on_begin()`/`on_end()`、固定线程亲和以及独立的 quit/drain 状态机；ThreadPool 构造即启动、没有这些生命周期语义。强行复用不仅不能删除 MessageLoop 自身的队列和唤醒状态，反而会叠加一层调度并改变公开行为。因此本轮复用的是 normal worker 的内部 staging 存储，不复用 ThreadPool 执行模型。

ThreadPool 的 TLS worker 身份优化也不应机械复制到 MessageLoop。MessageLoop 与 eka-rt 都只维护一个 `atomic<thread::id>`，`is_in_same_thread()` 已是 O(1) 原子读取和比较，不存在 ThreadPool 原先的 O(N) worker vector 遍历或 join/get_id 数据竞争。再增加 TLS 会复制 thread identity 状态，并要求额外处理同步 `run()`、`spin_once()`、嵌套 loop、restart 和退出恢复；现有调用点稀疏，没有证据证明收益能覆盖复杂度，因此保持不变。

MultiLoop 修改前在 [`src/base/multi_loop.cc`](src/base/multi_loop.cc#L183) 每个任务执行以下工作：

- `make_shared<Callback>`；
- 获取 `pool_mtx`；
- 更新 pending 原子；
- 构造一个指向 `Impl` 的 custom-deleter `shared_ptr` 及新控制块，在 deleter 中再获取 idle mutex、更新原子并通知；
- 再把包装 lambda 投进 ThreadPool。

早期一次性提交 100k 空任务的精确数字已撤回：两边外层 MessageLoop 与内部 ThreadPool 默认容量都是 10k，optimization 策略可在仍返回成功时丢弃旧任务，随后等待 `completed == 100k` 会非确定性永久自旋。它是基准设计错误，不是 MultiLoop 死锁，也不能作为性能证据。

本轮只删除 completion token 的控制块：用 captureless `noexcept` deleter 的 move-only `unique_ptr<Impl, Deleter>` 保持完全相同的析构时机和 pending decrement，token 本身为单指针大小，不拥有/删除 `Impl`。post 成功、拒绝、overflow drop 和 shutdown 销毁队列时都由 callback 生命周期触发 deleter；`Impl` 生命周期仍由 MultiLoop shutdown/join 保证。没有新增通用 ScopeGuard、Impl 字段或公开接口。

固定核独立 A/B（20k 空任务，batch=5000 并逐批 `wait_for_idle()`/校验完成数；每次结果取 5 个计时样本的中位数，再交替 5 组）：

| worker | custom-deleter `shared_ptr` token | move-only token | 改善 | eka-rt |
|---:|---:|---:|---:|---:|
| 1 | 10.63～11.29 ms | 8.29～8.85 ms | 约 20～25% | 5.25～5.46 ms |
| 4 | 20.56～21.10 ms | 17.74～18.76 ms | 约 9～16% | 14.57～15.76 ms |

最终源码另用 4096-task 滑窗、逐批校验完成数重跑 100k（7 轮中位）：VLink 1/2/4/7 worker 为 43.13/56.17/97.99/104.39 ms，eka-rt 为 30.14/38.19/89.03/98.79 ms。低并发仍有共享 callback 与 pending/idle 追踪成本，4/7 worker 差距收敛到约 10.1%/5.7%；该结果不会混入 queue drop。

独立的全局分配计数也与机制一致：10k/50k 任务分别从 20,015/100,055 次降到 10,015/50,055 次，即每个转发任务精确少一个控制块分配。64k 积压任务下，1/4 worker 的 RSS 中位增量约从 20,032/19,008 KiB 降到 16,964/17,032 KiB。该数据只证明 completion token 的收益，不代表剩余 callback 和 idle 追踪已经与 eka 等价。

VLink 仍慢于 eka，因为还保留 `make_shared<Callback>`、正确的 worker pending/idle 追踪和更大的 callback；这些功能差异不能为了追平数字直接删除。尤其 callback shared_ptr 负责在 ThreadPool 拒绝后让 dispatcher fallback 仍能执行原任务，不能无条件改成 direct move-capture。本轮同时补了 zero-worker fallback + `wait_for_idle()` 回归，覆盖 token 在拒绝路径的析构。

并发终审还确认旧版 `on_end()` 持 `pool_mtx` 调用 ThreadPool shutdown/join 时，尚在执行的 worker 若查询 `is_in_same_thread()` 会形成锁环。本轮只把 shutdown/join 移出 `pool_mtx`；为避免简单解锁后 join 与 `std::thread::get_id()` 并发访问，ThreadPool worker 在入口/退出各登记一次 TLS Impl 身份，`is_in_work_thread()` 改为 O(1) 指针比较，没有每任务写入。协调式 teardown 回归在旧版稳定超时，修复后通过。与仅完成 token 的保存版本做 20k 任务配对 A/B，1 worker 基本相同，4 worker 中位差约 2%，全局分配次数仍为每任务一次，未发现性能回退。

Linux shared-library 构建中该私有静态 TLS 仍可能生成 `__tls_get_addr`，且受外层导出类影响可见于动态符号表；但查询不在每任务执行路径，worker 只在入口/退出写一次。把它移回文件作用域与既定代码风格冲突，强制 TLS model 又会损害动态加载/跨平台兼容，本轮只统一私有成员命名，不为消一个低频符号扩大修改。

GraphTask 修改前在 [`src/base/graph_task.cc`](src/base/graph_task.cc#L90) 从池单独分配对象，`shared_ptr(raw, deleter)` 再分配控制块，构造函数还单独 `make_unique<Impl>`，即每节点包含三次逻辑分配。这里不能直接使用 `MemoryResource::make_shared`：4 线程独立 A/B 可重复退化，且会放大全局池高水位。

| 场景 | 修改前 VLink | eka-rt |
|---|---:|---:|
| 创建 100k，1 线程 | 46.27 ms | 29.01 ms |
| 创建 30k/线程，4 线程 | 37.89 ms | 14.78 ms |
| 4 线程创建并执行 | 27.1 ms | 9.0 ms |

本轮改用系统 `std::allocate_shared`，通过私有无状态 allocator 保留 protected 构造/析构和 `final`，只合并对象与控制块；`Impl` 继续独立对齐分配，公开布局和工厂签名不变。10k 节点计数从“对象池请求 + 控制块 + Impl”收敛为“combined object/control + Impl”，不再命中 32 B MemoryPool tier。当前工作树对修改前保存库的交替复测：1 线程 create 中位约改善 4.9%，4 线程 create 约 9.2%，1 线程 execute 约 6.7%；4 线程 execute 样本为改善但抖动较大，不把单轮约 33% 写成稳定收益。GraphTask 53 cases / 910 assertions 全通过。继续内联 `Impl` 会扩大 ABI、false sharing 和维护风险，当前没有必要。

### 3.4 MoveFunction 64 B 默认 SBO：P1 设计问题

[`include/vlink/base/functional.h`](include/vlink/base/functional.h#L130) 将 `Function` 和 `MoveFunction` 的默认 SBO 固定为 64 B。`MoveFunction<void()>` 实测对象约 80 B，任务 tuple 约 96 B。

构造/销毁微基准说明它不是普遍更快：

| capture | std::function | MoveFunction 64 | MoveFunction 32（有测时） |
|---:|---:|---:|---:|
| 8 B | 3.96 ms | 15.44 ms | - |
| 24 B | 14.7 ms | 16.75 ms | 7.45 ms |
| 48 B | 14.74 ms | 13.74 ms | spill |
| 80 B | 10.08 ms | 21.21 ms | - |

move-only callable 是合理功能，问题是把 64 B 作为所有高频任务容器的统一默认。建议保留通用 API，给 MessageLoop/ThreadPool 定义更小的专用 callback 类型，并先统计真实 capture 尺寸分布。

### 3.5 ACK 与 Intra RPC：P0 端到端放大

[`src/impl/ack_manager.cc`](src/impl/ack_manager.cc#L38) 修改前在 manager mutex 内通过全局 `MemoryResource` 创建每个 request，随后 request set、request mutex、condition variable 完成同步。互斥、generation 和等待语义本身合理，不应替换成自旋锁；退化来自同线程快速创建/销毁的 request 反而进入全局池，而 eka 的 `std::make_shared` 能直接利用线程本地 allocator cache。

ACK create/process/notify，100k 次/线程：

| 线程 | VLink | eka-rt | VLink level 0 |
|---:|---:|---:|---:|
| 1 | 16.33 ms | 14.18 ms | 16.33 ms |
| 2 | 35.34 ms | 14.46 ms | 18.74 ms |
| 4 | 94.34 ms | 14.86 ms | 19.57 ms |
| 7 | 251.30 ms | 32.27 ms | 38.90 ms |

4 client × 50k Intra RPC：VLink 约 214～239 ms，eka-rt 约 117～123 ms；VLink level 0 约 70.66 ms。

MemoryPool 修复后复测（100k 次/线程、5 轮中位数）：VLink 1/2/4/7 线程约 23.42/32.35/43.60/49.53 ms，eka-rt 同轮约 15.18/19.83/15.92/22.72 ms。VLink 的 4/7 线程相对修改前约 94.34/251.30 ms 已明显改善，但 ACK 仍慢于 eka-rt，说明剩余成本在 manager/request 状态、锁和条件变量等上层，不能宣称端到端已经全面反超。

独立 allocator A/B 进一步推翻了“剩余成本主要在状态/锁/CV”的猜测。只把 Request 从 `MemoryResource::make_shared` 改为 `std::make_shared`，不动任何状态机：

| 线程 | 全局 MemoryResource（5 轮范围） | `std::make_shared`（5 轮范围） | 同轮 eka-rt |
|---:|---:|---:|---:|
| 1 | 16.24～22.41 ms | 13.94～14.19 ms | 约 14.2～15.7 ms |
| 2 | 16.50～32.47 ms | 13.90～14.57 ms | 约 14.4～14.8 ms |
| 4 | 30.93～43.80 ms | 14.65～14.79 ms | 约 14.6～15.1 ms |
| 7 | 27.58～52.87 ms | 23.09～23.70 ms | 约 24.3～24.5 ms |

这与 MemoryPool 总结并不矛盾：Request 的最后一个引用通常仍由调用线程释放，是 glibc tcache 擅长的同线程 immediate reuse，不是 MemoryPool 在跨线程归还模型中占优的负载。allocator 优化本身只切换这一处，仍在 manager mutex 内完成“分配→seq→generation”，`clear/reset` 的线性化点、Request 布局、shared_ptr 类型和异常行为均不变；未引入 ACK 专用池、TLS cache 或新配置。

并发终审先修复了既有的早响应窗口，并在最终复核中补齐取消语义：Request 增加最小的 pending/acknowledged/cancelled 状态，`notify()` 和 `remove()` 均按 request→manager 锁序解析状态并唤醒；等待谓词不再反复获取 manager mutex。这样 process 不会在 fill callback 前返回，remove/clear/timeout 不会被误报为成功 ACK，无限等待也能被 remove 唤醒，timeout 后不会执行迟到 callback；`clear()` 仍先释放 manager mutex再逐 request 取消，不形成锁环。最终源码在固定 CPU 集上的 5 轮中位为 12.19/12.36/12.29/13.01 ms（1/2/4/7 线程），eka-rt 同轮为 14.52/14.34/14.54/15.25 ms；18 cases / 30 assertions 与全量回归同时通过。

早期 Intra queue 基准因 ACK 完成顺序窗口触发 `abort`；锁序修复后同一 4 client × 50k 基准可完整运行。最终配对结果：VLink direct 约 99.6～102.8 ms、中位约 102 ms，eka-rt 约 198.8～203.1 ms、中位约 202 ms；VLink queue 约 537～543 ms、中位约 540 ms，eka-rt 约 560～576 ms、中位约 565 ms。queue 只领先约 4%，且运行中仍有无法归因到具体锁的通用 SpinLock 超限日志，因此只判断“旧 abort 结论已失效、当前未见相对退化”，不扩大成所有 RPC 模式全面领先。

补充：Intra 普通 publish 并没有整体退化。`Publisher<int>` direct 500k 约 27.35 ms 对 69.26 ms；typed shared_ptr direct 1m 约 62.37 ms 对 122.64 ms。shared_ptr queue 1m 约慢 8.4%，且 level 0 无改善，属于任务生命周期/包装成本，而非 payload copy。

### 3.6 Bag 并发入队：P1，不能误判为落盘退化

VDB 7 生产者 × 20k、每条 1 KiB，多轮结果：

| 指标 | VLink | eka-rt |
|---|---:|---:|
| 提交耗时 | 127.8～130.2 ms | 65.5～76.0 ms |
| 最终总耗时 | 286.0～291.4 ms | 284.5～301.5 ms |

`VLINK_MEMORY_LEVEL=0` 后提交约 55.8～75.2 ms，但最终总耗时仍由 SQLite 主导。结论是：

- 并发生产者被 [`src/extension/vdb_writer.cc`](src/extension/vdb_writer.cc#L660) 的异步任务包装、Bytes owning copy、MessageLoop 和全局池拖慢；
- [`src/extension/vdb_writer.cc`](src/extension/vdb_writer.cc#L703) 的 `write_mtx` 覆盖 SQLite 写入是正确的长临界区，不能换自旋锁；
- VCAP 在 [`src/extension/vcap_writer.cc`](src/extension/vcap_writer.cc#L599) 使用相同异步结构，因此会继承入队成本，但本次没有单独量化 VCAP 多生产者端到端数字；
- 单生产者 VDB/VCAP 写读和 auto timestamp 基准没有确认当前工作树退化；Bag 相关测试通过 177 个 test case / 3401 assertions。

建议：先修 MemoryPool/MessageLoop，再复跑 Bag；不要为了提交延迟改 SQLite transaction、MCAP writer 或压缩锁。异步路径必须持有 payload，不能简单改成浅拷贝。

MemoryPool 修复后，7×20k×1 KiB 的 5 轮中位数为 VLink 提交约 101.48 ms、最终约 269.86 ms；同轮 eka-rt 约 116.26/308.94 ms。相对修改前 VLink 约 128～130 ms 的提交耗时有改善，且没有为了该结果修改 SQLite、VCAP/MCAP、payload 所有权或落盘锁。运行中仍可见其他 `SpinLock` 超限日志，因无法从该通用日志定位到 MemoryPool，报告不将其强行归因给本次池实现。

## 4. 继承问题与 VLink-only 绝对热点

### 4.1 FlatBuilder loan 生命周期：P0 继承缺陷

修改前 [`include/vlink/internal/serializer-inl.h`](include/vlink/internal/serializer-inl.h) 在 `Finish()` 前用 `fbb_.GetSize()` 返回 loan 大小，节点据此申请 transport loan；真正序列化时才 `Finish()`，并用指向 builder 的 shallow `Bytes` 覆盖 loan carrier。

后果：

1. 原 transport loan handle 从 `Bytes` 中丢失，SHM/SHM2 的 loan map 可能直到 publisher 销毁才释放，最终耗尽样本；
2. Finish 后大小可能大于预先 loan 的大小；
3. 实际 publish 收到的是非 loan builder 指针，transport 往往又重新 loan 并复制，所谓零拷贝失效。

该代码模式在 eka-rt 已存在，因此不是相对回退，但在修改前属于必须优先修复的真实问题。修改前的可观测 loan carrier 已直接验证：Finish 前 4100 B、Finish 后 4104 B，loan flag 从 1 变 0，且输出指针没有保留原 loan 地址。

本轮采用一次完成的克制修复，而不是“先禁用、以后再重构”：

- `get_serialized_size<kFlatBuilderType>()` 返回 0，不再暴露错误的 pre-Finish 大小；
- 新增公开 C++ 模板 `Serializer::serialize_to_transport<TypeT>(..., LoanCallbackT&&)`，FlatBuilder 分支先 `Finish()`，再按最终大小调用一次目标缓冲提供函数，并直接 `memcpy` 到真实 loan 或 owning fallback；
- Publisher 1、Client 4、Server 2、Setter 1 共 8 个序列化入口统一走该模板，非 FlatBuilder 分支仍是原有 size hint→loan→serialize 流程，lambda 可内联且没有额外分配；
- 公共 `Serializer::serialize<kFlatBuilderType>` 对预先传入的 loan 返回失败且不 Finish，非 loan 路径返回 owning copy，避免重新引入 loan handle 覆盖；
- FlatBuilder transport 路径一旦开始就会先 Finish，即使目标申请随后失败也已消费 builder。彻底隐藏该一次性语义需要复制 builder 或新增复杂 prepare 状态，本轮按异常路径低优先级只在 Serializer 契约中明确，不引入过度设计。

验证结果：Serializer `ser-flatbuffers` 8/8（38 assertions）、SHM2 连续发布/容量归还 1/1（44 assertions）、SHM v1 连续发布/手工 loan 3/3（74 assertions）全部通过。真实 SHM/SHM2 loan 路径只有一次 builder→loan payload copy；Zenoh 小消息可能返回 owning fallback，不能泛化宣称所有后端端到端只有一次 copy。Python API 不暴露 C++ FlatBuilder 模板类型，继续发布已完成字节缓冲，因此没有伪造一个不适用的 Python 对等接口。

### 4.2 Proxy async 多一次 payload 拷贝：P1 继承绝对热点

[`proxy/proxy_server/proxy_server.cc`](proxy/proxy_server/proxy_server.cc#L1195) async 路径按值 capture `Bytes`，这会先复制一份 raw；任务执行后 `ProxyData::create` 再复制 raw，随后 serializer/transport 再生成 wire 数据。同步路径少一次队列 capture copy。

纯构造微基准验证了第二次 copy 的成本：

| payload | 同步 create | 现 async 的 capture+create | 预构造后 move |
|---:|---:|---:|---:|
| 1 KiB × 200k | 8.89 ms | 15.17 ms | 10.36 ms |
| 64 KiB × 10k | 10.80 ms | 22.30 ms | 11.41 ms |
| 1 MiB × 800 | 27.46 ms | 58.02 ms | 27.93 ms |

但这不能直接推出“预构造后 move 就是修复”。在真实 MessageLoop 被阻塞、任务先积压再消费的测试中：

| payload / 数量 | 现 async submit/total | 预构造 submit/total | 解释 |
|---:|---:|---:|---|
| 1 KiB / 8000 | 0.74 / 1.46 ms | 1.18 / 1.56 ms | 差异很小，任务包装占主导 |
| 64 KiB / 2500 | 23.40 / 36.37 ms | 24.13 / 24.31 ms | prebuild 避免消费端第二次 copy，有利 |
| 1 MiB / 120 | 15.65 / 26.28 ms | 33.96 / 34.10 ms | prebuild 反而更慢 |

最后一行是默认池尺寸断点，不是测量噪声。默认 level 3 的 1 MiB tier 有效，而下一个 4 MiB entry 是 `blocks_per_chunk == 0` 的 bypass sentinel。现路径队列中持有恰好 1 MiB 的 pooled `Bytes`，消费端只串行创建一个略大于 1 MiB 的 ProxyData；prebuild 则让 120 个“1 MiB + URL/ser/hostname”的对象同时走上游 allocator。把 raw 改成 1,048,500/1,048,540 B、使 envelope 仍落在 1 MiB 内后，prebuild total 降至 18.04/18.26 ms，而现路径约 33.41/33.77 ms，确认了边界原因。

因此建议必须克制：

1. 先为 async forward 设定明确的有界队列/丢帧或 latest-only 策略，并记录 queue depth、drop、payload bytes；当前普通 MessageLoop 容量约束之外还会对每个拒绝任务打印 warning，大量溢出本身会制造日志风暴。
2. 只有在 envelope 总大小仍落入有效池 tier，或大块 allocator 已有稳定复用策略时，才采用 prebuild+move；不能无条件替换。
3. 将默认 1 MiB 边界及其他配置中“有效 tier→bypass”边界、队列空/满、消费者快/慢、RSS 高水位加入回归矩阵；这比只测单次 create 更能代表 Proxy。
4. 若最终要消除第二次 copy，更稳妥的长期方向是可 move 的共享 owning payload/envelope storage，或让 serializer 直接消费已拥有的 payload slice，而不是扩大当前全局池的大块常驻量。

ProxyServer async 源码与 eka-rt 基本相同，不能把上述问题写成 VLink 相对回退。ProxyData 自身 create→wire→parse 的同等基准中 VLink 更快，也不能把它扩展成“Proxy 整体退化”。direct send 中 [`proxy/proxy_api/proxy_api.cc`](proxy/proxy_api/proxy_api.cc#L364) 重复调用一次 `get_schema_type()`，只是很小的冗余 getter。

### 4.3 Perception/Point3D GUI 队列：P1

Perception 是 VLink 新增能力，没有 eka-rt 对照。修改前其数据 callback 在 [`viewer/perceptiondialog.cc`](viewer/perceptiondialog.cc) 每帧执行：

1. `proxy_data_cache_[url] = proxy_data` 深拷贝 raw；
2. 每条消息都向 Qt 队列投递一个 `render_url(url)`；
3. [`viewer/perceptiondialog.cc`](viewer/perceptiondialog.cc#L535) 渲染时再把 cache 复制到局部 `ProxyAPI::Data`，raw 再深拷贝一次；
4. GUI 落后时，多个排队的 URL 任务会反复渲染同一个“最新帧”。

用等价的 cache copy + render copy 模型测得：

| payload / 次数 | 当前两次 copy | 仅 cache 一次 | latest-only batch |
|---:|---:|---:|---:|
| 1 KiB / 100k | 6.55 ms | 1.64 ms | 1.62 ms |
| 64 KiB / 5k | 10.02 ms | 4.23 ms | 4.20 ms |
| 1 MiB / 400 | 21.46 ms | 6.07 ms | 6.22 ms |

该测试只隔离所有权复制，不代表完整 OSG 帧率；它足以证明第二次 raw copy 不是可忽略项。因为 Perception 是 VLink-only，这仍应称为绝对热点，而非相对回退。

本轮只实现有充分语义证据的 pending 合并，没有把 cache 改成 `shared_ptr`：callback 在原 cache mutex 下覆盖 latest 数据，并仅在 URL 首次进入 pending set 时排 Qt task；slot 在同一锁下先清 pending、复制最新数据并取得当前 URL context 指针，随后解锁解码/渲染。context 的创建、替换和 slot 消费都受 GUI 线程约束，数据 cache 则由 mutex 保护。因此同一 URL 最多一个 queued task 加一个 executing render，渲染期间到达的新帧仍会补排下一次；Camera child callback 仍在合并前逐帧转发。无 OSG 构建通过编译期开关跳过无意义的 Perception cache copy 和 Qt 排队。`vlink-viewer` Release 目标已编译通过，并经并发对抗审阅确认配置切换、旧 queued event、析构和 invoke 失败回滚没有新增锁序问题。

Point3D 在 [`viewer/point3ddialog.cc`](viewer/point3ddialog.cc#L871) 也有 cache copy + Qt queued copy，但 eka-rt 已有同样架构。它还逐帧更新频率、点数、跨帧极值/平均值、表达式状态和 Projection 通知；机械套用 latest-only 会改变可观察统计，因此本轮明确不改。Camera/FFmpeg 的 H264/H265 等格式还有帧间依赖，也不能做通用合并。

OSG 渲染实现没有发现“每帧重建整个 scene graph”的证据。大部分 `new osg::*` 位于 geode/layer 首次创建；更新路径清空并复用数组 capacity、填充后 dirty VBO。ObjectArray label 会复用已有 `osgText::Text`，仅对象数增长时新增。不要把初始化分配误判为每帧回退。

不过 [`viewer/perceptiondialog.cc`](viewer/perceptiondialog.cc#L1368) 的 `render_layer()` 在 12 类 render type 分支中每帧新建转换 vector；Lane/Prediction/StopLine/Freespace/HDMap/Trajectory 还为每条 polyline 新建嵌套 point vector。`reserve()` 只对本次临时对象有效，下一帧不会复用 capacity。这是确定的每帧 heap churn，但未跑真实场景 allocator profile，所以建议先把 scratch 挂到每 URL/render context 或让 OSG update 直接消费 perception span，再用对象数量分布验证，不应立即重写全部渲染器。

### 4.4 WebViz/Foxglove/Rerun：P1/P2

[`webviz/proxy_bridge.cc`](webviz/proxy_bridge.cc#L108) 默认 queued callback 会把 `ProxyAPI::Data` 深拷贝进 MessageLoop 任务；该 copy 对异步生命周期是必要的，但会继承 MessageLoop/MemoryPool 回退，且队列没有按 URL 合并。建议增加 latest-only/coalesce 模式或明确的 droppable 策略，direct 模式继续保留给允许阻塞 source callback 的场景。

局部冗余：

- `dispatch_data_callback()` 先获取 shared lock 检查 callback，direct 分支又获取一次同一把 shared lock；可合并为一次。
- [`webviz/foxglove/src/foxglove_server.cc`](webviz/foxglove/src/foxglove_server.cc#L2457) 的 channel 与 subscription 查询原先在只读路径使用 `unique_lock`；本轮已改为 `shared_lock`，写路径锁不变。独立 `ENABLE_WEBVIZ=ON` Release 构建已实际编译 `foxglove_server.cc` 并链接通过。
- 多订阅者发送只构建一次 payload，再修改 subscription id。已核对 WebSocket++ `send(void*, len)` 会把 payload 复制进自己的 message buffer，因此当前 builder/buffer 生命周期安全，不能误报悬空引用。
- Foxglove converter 的 thread-local FlatBufferBuilder 输出在同线程同步 send 前有效；不能因 shallow `Bytes` 就判定 UAF。
- 每种 converter 各自有 4 KiB～1 MiB lazy thread-local builder。它减少热路径分配，但若一个线程使用很多 converter 类型，会保留数 MiB 内存；属于内存占用设计权衡，不是已确认速度回退。

### 4.5 MQTT、Zenoh、SHM/SHM2：P1/P2

MQTT 外层 Paho callback 在 [`modules/mqtt/mqtt_factory.cc`](modules/mqtt/mqtt_factory.cc#L587) 必须在 `MQTTClient_freeMessage` 前深拷贝 payload，再投递到 MessageLoop；这一份 copy 是必要的。Client response 在 [`modules/mqtt/mqtt_factory.cc`](modules/mqtt/mqtt_factory.cc#L1163) 又把外层 owning packet 的 body 深拷贝一遍，而 callback 同步执行且外层 packet 仍存活，存在减少一份全 payload copy 的机会。

建议：先把 callback 生命周期明确写进内部契约，再把 response body 改为对 owning packet 的 shallow slice；若 callback 允许异步保留 `Bytes`，则不能直接改。

Zenoh 的 `ZenohPayloadView` 在 unstable API 可取得 contiguous view；stable API 调用 `z_bytes_to_slice`，随后 server 在 [`modules/zenoh/zenoh_factory.cc`](modules/zenoh/zenoh_factory.cc#L1507) 再 `Bytes::deep_copy`。`z_bytes_to_slice` 对当前 Zenoh 版本在连续/分片 payload 下是否物化必须先通过 allocator/trace 验证，因此这里只标为“可能双拷贝”，不直接下结论。若确认物化，建议用 reader 一次读入最终 owning buffer，避免 slice + Bytes 两次物化。

SHM v1 数据面 loan/copy 主体与 eka-rt 一致；SHM2 正常 subscriber path 使用 shallow view，manual-unloan 时才进入 loan map。以下不能换自旋锁：loan map 是 `unordered_map` 且临界区含 move/erase/handle 释放，client mutex 还保护中间件请求状态。

修改前确认的日志热点：

- [`modules/shm/shm_factory.cc`](modules/shm/shm_factory.cc#L1194) 在 `wait_ > 0` 时每次 publish 记录 sem_count 和 sub_count INFO；该问题继承自 eka-rt。
- [`modules/shm2/shm2_factory.cc`](modules/shm2/shm2_factory.cc#L1793) 在 `wait_ > 0` 时每次 publish 记录 sem_count INFO；这是 VLink-only。

本轮已直接删除这三条逐消息 INFO，没有增加新的级别、counter 或环境变量；wait/acquire 语义未变，SHM/SHM2 Release 模块编译和 transport 回归通过。SHM2 小消息使用 thread-local scratch 后调用 `send_slice_copy`，大消息走 middleware loan；默认阈值是否最优仍必须在真实 Iceoryx2、不同 payload 和订阅者数下测量，不能仅凭看到“两次 memcpy”就改阈值。

## 5. 日志、落盘、播放、API 与渲染的排除结论

### 5.1 Logger

Logger level/flag 从普通变量改为原子是正确性修复。10m disabled log checks：VLink 约 8.94 ms，eka-rt 约 8.15 ms，差异不到 0.1 ns/次；enabled noop 约 457.65 ms 对 460.02 ms，没有实质回退。CachedTimestamp 每 5m 调用约慢 3～5%，幅度小，且 Logger 已使用 thread-local 缓存。

未发现 VLink 新增的逐消息 `fsync`。spdlog/quill 文件 sink 使用异步/延迟 flush 配置；正常通讯热路径的日志大多位于失败分支。前述 SHM wait-mode INFO 已删除，没有重写 Logger 或修改全局日志策略。

### 5.2 VDB/VCAP/Bag 播放

- Writer 的 `write_mtx` 覆盖 SQLite/MCAP/压缩/I/O，属于长临界区，使用 mutex 正确。
- BagReader 每帧持有 playback state shared lock，参考实现也有同类状态保护。
- 当前 [`src/extension/bag_reader.cc`](src/extension/bag_reader.cc#L274) 会在 backend 未填 metadata 时做最多两个 URL map lookup；读基准没有显示整体回退，因此只列为固定小成本。
- CLI `bag2mcap`/`bag2rrd` 在 callback 内同步消费 frame view，没有发现为了持久化而多做一次无意义深拷贝。
- CLI/dump/monitor 中的频繁 `flush()` 是交互终端刷新，不是 Bag 文件落盘 fsync，不应混为一谈。
- TriggerRecorder 每条样本 deep-copy 到 shared payload 后进入 per-URL ring；snapshot 语义需要 owning 数据。它会受到全局池竞争，但不能改成借用 callback buffer。

### 5.3 DDS/DDSC/DDSR/DDST/FDBus/QNX/SomeIP

- DDS、DDSC、DDSR 核心 write/take 与 eka-rt 基本一致；新原子变量和 weak_ptr 修复竞态/生命周期，成本很小。
- callback 从配置 mutex 外执行、过期对象清理等变化偏向改善。
- DDST 是 VLink-only，但核心转发结构浅，没有发现新增的 payload 深拷贝链。
- ASan/LSan 曾定位到无 `DDS_HAS_SSL` 构建仍因 `ssl.*` 隐式启用 TCP，进而在 CycloneDDS TCP cache teardown 留下 100×200 B；本轮只把自动启用 TCP 限制到 SSL 支持分支。修正后原用例、`ddsc-qos` 12/12 及全量 sanitizer 212/212 均无泄漏；第三方 TCP cache 的释放实现没有被 VLink 补丁掩盖或修改。
- QNX 会继承 MultiLoop 的每任务开销；本机 Linux 无法给出 QNX 中间件端到端数字。
- FDBus/SomeIP 的主要变化集中在初始化、清理和生命周期，没有确认正常数据面回退。
- 不建议把 transport registry/status/user callback 锁换成自旋锁。

### 5.4 Serializer/Bytes/零拷贝

- Proto/CDR 主路径与参考接近；FlatTable 在已有精确 loan 时会 memcpy 到 loan，逻辑合理。
- StandardPtr `get_serialized_size()` 返回 0，因此 loan 分支不可达，属于错失零拷贝机会，但不是相对回退；必须先定义可计算 wire size 的约束再改。
- `Bytes` 单线程 heap path 多数比参考快；SBO 深拷贝约慢 1.8 倍但绝对值通常只有 5～10 ns。
- [`src/base/bytes.cc`](src/base/bytes.cc#L832) 的成员 `deep_copy(const Bytes&)` 在 offset > 0 时通过临时 Bytes 做两次分配/复制；当前已知调用多为 schema storage 且 offset 通常为 0，不属于热路径。可低优先级改为一次 allocation + overlap-safe copy。
- PointCloud vertical format 为 offsets/sizes 创建小 vector，参考实现也有类似行为，且字段数通常很小。
- ProxyData 完整 create→wire→parse 在 64 B/1 KiB/64 KiB 场景均未显示 VLink 回退。

RawData 专项把“对象构造”“复用已有目标 buffer 深拷贝”“serialize”“parse”拆开测量：

| 路径 | 代表结果 | 判断 |
|---|---|---|
| 1 KiB construct-copy，1/4/7 线程 | VLink 5.08/33.81/56.48 ms；eka 5.46/20.07/21.56 ms | 多线程回退来自全局池，单线程不慢 |
| 64 KiB construct-copy，1/4 线程 | 6.16/5.58 ms；eka 7.60/4.82 ms | 差异小，不能概括成全尺寸回退 |
| 1 KiB reused deep-copy，1/4/7 线程 | 1.92/1.43/1.31 ms；eka 1.83/0.60/0.44 ms | VLink Header/RawData 布局更大，语义和 ABI 已扩展；不单独定性 allocator 回退 |
| 1 KiB serialize，1/4/7 线程 | 2.18/0.71/0.50 ms；eka 2.07/0.69/0.46 ms | 基本相同 |
| 1 KiB parse，1/4/7 线程 | 1.60/0.56/0.38 ms；eka 1.70/0.56/0.40 ms | 基本相同 |
| 64 KiB serialize/parse，4 线程 | 4.02/0.058 ms；eka 4.01/0.066 ms | 无退化证据 |

因此零拷贝实现的主要相对问题仍是“构造 owning buffer 时命中争用池”，不是 wire parse/serialize 算法。VLink 有八种内置 zerocopy 消息容器（测试统计另含公共 Header 与 MessageParser，合计 10 个 suite），eka-rt 有五种；新增的 ObjectArray/OccupancyGrid/Tensor 等只能做绝对热点检查，不能伪造对照结论。

### 5.5 C API 与 Python API

- C API publish 对调用方 buffer 使用同步 shallow view，与参考一致；服务端 reply 的 owning copy 与参考的 new[]+memcpy 等价。
- Python publish/from-Python 必须在释放 GIL 前复制 Python buffer，避免 mutable buffer 和异步生命周期问题。
- Python callback 转成 `bytes` 必然需要 Python-owned copy。
- Python Bag `read_next` 的 deep-copy 是必要的，因为 reader frame 可能借用 SQLite/MCAP 的可复用 buffer。
- Python 暴露 MessageLoop/MultiLoop，因此会看到基础调度开销，但 wrapper 不是根因。

### 5.6 Viewer、Player、Analyzer、FFmpeg

- Qt queued `QVariant<ProxyAPI::Data>` 会复制 raw，但跨线程 queued connection 要求 owning 生命周期；eka-rt 的 MainWindow/Camera/Point3D 已有相同模式。
- FFmpeg 当前先复制到带 padding 的 vector，再异步 decode；FFmpeg padding 是必要的。最大任务深度从 100000 降到 3 有利于避免积压。
- FFmpeg 在 `cache_frame == false` 或无 parser 的分支中，每个 posted input 会 `av_packet_free()` 后立即 `av_packet_alloc()`；packet 本体通常可通过 `av_packet_unref()`/重置字段复用。它是明确的分配冗余候选，但 codec、parser 和 external-buffer 所有权复杂，必须先用真实 H264/H265/PNG/WEBP 样本验证后再改。
- decode 成功后对 `cost_total/cost_cnt` 使用默认 seq_cst 原子累加，而读取/清零只用于统计；若确认只有 decoder 线程写，可改 relaxed 或线程本地累计。绝对收益预计小，不应先于帧队列合并和 packet 复用。
- Player 输出 callback 使用 Frame 引用，没有发现额外全 payload copy。
- Analyzer 修改前在每表达式循环里调用 `player_->get_schema_type(unit.url)`；本轮改为在 unit/frame 外层用已经由 BagReader 补全的 `frame.schema_type` 解析一次，再进入 expression 循环。`vlink-analyzer` Release 目标已编译通过。
- Analyzer 的 protobuf object、FlatBuffers context 已缓存；`setData` 只在最终 create_plot，而不是每 sample 调用。
- `QT_QPA_PLATFORM=offscreen` 下分别运行 vlink-viewer、vlink-player、vlink-analyzer 4 秒，三者都稳定进入事件循环并由 timeout 结束（rc=124，无 stdout/stderr），未发生启动崩溃。该 smoke 不覆盖 GPU draw、OSG scene update、视频 decode 和交互式图表，因此不能用来排除真实帧率问题。

## 6. TaskHandle/Cancellation：VLink-only 可选路径

`TaskHandle::State` 在 [`src/base/task_handle.cc`](src/base/task_handle.cc) 内嵌 `CancellationSource`；`CancellationSource` 又创建独立 State，而 tracked callback 超过 `MoveFunction` SBO 后还会产生 wrapper spill。因此默认 tracked post 实际增加三次池分配，不是此前报告中的“两份 state”就能完整描述；修改前还会执行一次无观察者的 `kInvalid→kQueued` mutex transition 和 `notify_all()`。

同一个 VLink MessageLoop 的 plain `post_task` 与 `post_task_handle` A/B：

| producers × 50k | plain submit/total | tracked submit/total |
|---|---:|---:|
| 1 | 1.35 / 1.80 ms | 8.37 / 13.75 ms |
| 4 | 21.14 / 22.99 ms | 68.73 / 99.08 ms |
| 7 | 66.93 / 70.27 ms | 145.33 / 200.08 ms |

4/7 producer tracked 测试触发了 MemoryPool tier 的 “exceeded max spin count” 日志，进一步证明其会放大全局池竞争。该接口不是普通通讯默认路径，所以不能将数字表述为 VLink 全局回退。

本轮只实施低风险且有独立 A/B 的初始迁移消除：State 直接构造为 `kQueued`，删除 `mark_task_queued` 声明、定义和 ThreadPool/MessageLoop 三个调用点；非空 State 的 `kInvalid` 死分支也删除。handle 在工厂返回前不可被外部观察，parent 已取消、注册期间取消和入队后取消的时序仍由原 request/begin 检查覆盖。TaskHandle 29 cases / 152 assertions，以及 MessageLoop/ThreadPool/MultiLoop 141 cases / 350 assertions 全通过。20 万次单生产者 tracked post 对修改前保存库的 11 轮交替复测，submit/total 中位均约改善 21%。

lazy cancellation 被明确拒绝：`cancellation_token() noexcept` 当前保证有效 handle 立即有 token；改成首次访问时创建会引入额外同步、返回语义和 parent callback 竞态，代价超过已有证据。waiter-count/条件通知也没有稳定 A/B，本轮不改。

## 7. impl / extension 当前改动与遗漏复核

本轮把当前工作树尚未提交的性能改动也作为独立对象复核，避免只看 HEAD 对比而遗漏新引入的问题。

`src/extension` 当前改动方向与已测瓶颈一致：

- BagWriter persistent task 从完整 `TaskHandle` 改为 protected untracked post，避免每帧创建 TaskHandle State、Cancellation State、tracked wrapper spill、状态锁和通知；TaskHandle A/B 已证明该开销显著。
- VDB/VCAP 的 `MemoryCharge` 从 `unique_ptr` heap object 改成 move-only inline guard，去掉每帧一次小对象分配，同时保持 rejected/dropped/执行完成时扣减内存的 RAII 语义。
- 无 Bag plugin 时 `BagWriter::push()` 直接把计算后的 timestamp 传给 backend，不再为只修改 timestamp 构造/复制整份 Frame；`record(frame, timestamp)` 是 protected backend 接口，不是新增公共 API。
- BagReader 在无 URL rule/filter 时跳过转换/filter callback，在 backend 已填 meta 时跳过 map lookup；VCAP 不再无条件安装 topicFilter。单生产者读写、clone/check 和 21 个 extension suite 均通过，没有发现为了快路径破坏 remap/filter/plugin 行为的证据。
- NodeImpl 用 `data_recorder_enabled` 快速排除“绝大多数未录制消息”，再在 enabled 时获取 shared lock 并复制 recorder shared_ptr，避免每次 publish 都拿锁。无 recorder 50m 次固定核结果为 1.38～1.49 ns/call，eka-rt 为 1.054～1.056 ns/call；约 0.3～0.44 ns 是 richer/thread-safe state 的小固定成本，绝对值低，不建议为此牺牲并发配置正确性。

同步检查结果：CHANGELOG 已记录 Bag 快路径，C++ `push()` Doxygen、`doc/09-recording.md`、Python examples README、demo 注释和 binding docstring 均说明异步入队成功/拒绝语义以及已接受帧不会为新帧让位。`Serializer::serialize_to_transport()` 是本轮新增的公开 C++ 模板，声明、Doxygen、序列化文档和 CHANGELOG 已同步；Python 不暴露 C++ FlatBuilder 模板类型，无对应接口。`post_untracked_task()`、`record(frame,timestamp)`、`has_playback_url_rules()`、GraphTask allocator、TaskHandle 初始状态和 Perception pending 都是内部实现，不新增 Python API 或配置。

仍缺少的 extension 性能测试是 TriggerRecorder 持续采集。其每帧 `Bytes::deep_copy` 是 ring retention 必需所有权，`shared_ptr<const Bytes>` 也被 dump snapshot 跨 ring mutation 持有，不能直接删；但尚无多 URL × 大 payload × dump 并发下的 allocator、锁等待、drop 和 RSS 基准，应按第 11 节补齐。

## 8. 自旋锁与内存池的适用边界

### 8.1 可以考虑的方向

- 当前池已采用争用触发的分片 tier；经真实工作负载验证后，可继续承载 MessageLoop task node、callback spill、TriggerRecorder envelope 等短生命周期、尺寸集中的跨线程对象，但不因微基准改善就扩大到所有对象。ACK Request 已证明更符合同线程 tcache 模型，因此明确不走全局池。
- `CpuProfiler` 一类只保护少量计数器/时间戳、无分配、无系统调用的极短临界区可以继续使用 SpinLock。
- 某些只需替换一个不可变 callback/config snapshot 的位置，更适合 atomic shared_ptr/snapshot，而不是 SpinLock。

### 8.2 明确不建议使用 SpinLock 的位置

| 位置 | 原因 |
|---|---|
| MemoryPool 修改前的单 free-list | 已证实是实际瓶颈；本轮通过分片锁域解决，而不是增加单锁自旋 |
| ACK request set / condition wait | 含 `std::set`、通知和阻塞等待 |
| MessageLoop/ThreadPool 队列 | 生产者可能阻塞，消费者会 sleep/wakeup |
| Bag writer | 临界区含 SQLite/MCAP/压缩/文件 I/O |
| Security | 临界区含 allocation 和 OpenSSL crypto |
| SHM2 loan map | 含 unordered_map、handle move/drop 和中间件调用 |
| Proxy/transport user callback | 用户代码执行时间不受控 |
| GUI cache/render | 含大对象 copy、解析、Qt/OSG 操作 |
| CalculateSample/Discovery map | hash map 更新，长度不严格有界 |

## 9. 未确认、条件性或明确排除的候选

| 候选 | 结论 |
|---|---|
| MPMC queue | 1P1C VLink 更快，2P2C/4P3C 有 7～10% 抖动性回退；证据不足，不列修复项 |
| CachedTimestamp | 约 3～5% 小回退，绝对成本低，不应优先 |
| Schedule | 50k submit/total 约 10.77/18.95 ms 对 9.14/14.44 ms；当前修复 continuation race，语义不同，列条件性成本 |
| Timer/WheelTimer | 高密度 one-shot/add-only 更慢，正常低频 periodic 基本相同；WheelTimer 内部无主要生产调用 |
| Security | 当前 AES-GCM/replay/envelope 与参考 AES-CBC 语义不同，不能做直接性能回退结论 |
| SHM2 small-copy threshold | 需要目标 Iceoryx2 环境和 payload 分布，不凭静态代码调整 |
| Zenoh stable payload | 需验证 `z_bytes_to_slice` 是否物化，暂不认定双拷贝 |
| WebSocket builder shallow view | send 同步复制 payload，当前生命周期安全 |
| OSG `new` | 多数是首次 layer/geode 初始化，不是每帧分配 |
| Logger atomics | 微基准无实质回退 |
| Bag writer mutex | 正确的 I/O mutex，不能换自旋锁 |
| Python buffer copies | 跨 GIL/语言所有权必要 |

并发终审确认的两处既有正确性风险已作为独立小补丁修复：ACK 统一 request→manager 锁序并用 request 内最小状态区分确认、取消与超时；MultiLoop shutdown/join 移出 `pool_mtx`，ThreadPool 改用 TLS worker 身份。两项都补了定向契约/teardown 回归，不改变公开接口，也不把正确性修复误计为相对性能回退。

## 10. 不规范、过度设计和冗余代码

按性能相关性排序：

1. **MemoryResource 自动启用。** 头文件根据平台能力直接定义 feature macro，使构建选项无法真实表达是否启用 PMR，也让用户很难做 A/B。建议由 CMake 生成唯一配置宏。
2. **MemoryResource 相等比较已修复。** 修改前 NDEBUG 下把任意 `std::pmr::memory_resource&` `static_cast` 为 `MemoryResource*`，与其他 resource 比较属于未定义行为；现在使用资源对象 identity，self/global/distinct/foreign resource 测试 20 cases / 53 assertions 全通过。原拼错的宏名 `MEMORY_POOL_NERVER_DELETE` 也只做了拼写修正。
3. **MoveFunction 64 B 全局默认。** 为少量较大 capture 让所有队列元素膨胀，属于“一种尺寸覆盖所有场景”的过度设计。
4. **MultiLoop 仍有每任务共享 callback 与 idle 追踪成本。** 本轮已移除 completion custom-deleter `shared_ptr` 控制块；callback shared_ptr 因拒绝回退语义保留，idle mutex/atomic/cv 是当前正确性成本。
5. **GraphTask 手工池分配冗余已收敛。** 对象与控制块已由系统 `allocate_shared` 合并，独立 Impl 保留；没有新增对象池或内联公开布局。
6. **TaskHandle 仍无条件创建 cancellation registry。** 用户只想 wait/state 也承担完整 cancellation 设施；lazy 化会改变 token/parent 竞态契约，当前只删除了确定冗余的初始 queued transition。
7. **Bytes offset member deep-copy 使用中间对象。** 可用一次分配完成，低优先级。
8. **Proxy direct schema getter 调用两次。** 低风险小冗余。
9. **Analyzer inner loop 重复 schema lookup 已删除。** 每 unit/frame 只解析一次。
10. **WebViz read-only map 独占锁已收窄。** Foxglove 数据路径两处查询改为 shared lock，写路径不变。
11. **SHM wait-mode 每消息 INFO 已删除。** wait 行为不变。
12. **部分大型模块保留过多功能专用 thread-local builder。** 速度方向合理，但应有内存上限/统计，避免 converter 类型多时长期保留高水位。

补充的结构性审查结果：

| 类型 | 证据 | 判断与克制建议 |
|---|---|---|
| MemoryPool 配置过度设计 | 10 个 level、每 level 最多 19 个硬编码 tier；同一表同时用 `blocks_per_chunk=0` 表示 bypass，默认 1 MiB→4 MiB 产生已实测的行为断点 | 配置复杂度没有换来并发扩展性。先收敛到少数经过 workload 验证的尺寸组，再做 TLS/shard；不要继续增加 level/tier |
| MessageLoop policy 组合过重 | normal/lockfree/priority × wait/pop/optimization × tracked/untracked × protected/droppable；本轮已去掉普通 post 的二次锁和批次固定分配，但大 callback 与策略组合仍在 | 保留语义；没有 ABI/capture 数据前不缩小 callback，也不为 `notify_one()` 拆分两套 CV |
| Proxy async 缺少背压模型 | 可积压 payload task，过载时失败/移除路径可能逐任务日志；简单 prebuild 又在 tier 边界放大同时存活 allocation | 先定义 queue bytes/depth/drop 指标，再优化 copy；这是设计缺口，不是用一个 move 就能修好 |
| Viewer frame pipeline 重复表示 | ProxyAPI::Data cache → local Data → perception Layer → 12 类 OSG 临时 DTO vector → OSG arrays | latest-only pending 已完成，避免 backlog 重复 copy/decode/render；每个实际渲染帧的 cache→local copy 和临时 DTO allocation仍在，改 shared ownership/scratch 前需真实 OSG allocator/FPS A/B |
| 超大单文件 | 排除第三方后第一方约 256k 行，其中 56 个文件超过 1000 行；代表项：rerun_converter 8111、foxglove_converter 6481、mainwindow 5318、python binding 4088、CLI bench 4089、bag 3378、eproto 2994、monitor 2893 | 这是审查/测试隔离和重复逻辑风险，不等同于运行时回退。按 pipeline/format/parser/report 拆 internal translation unit，不改变公共 API |
| CLI 重复管线 | `cli/bag/bag.cc` 与 `cli/dump/dump_slice.cc` 分别维护 reader、schema、filter、writer、signal/quit 生命周期；dump 主文件又维护 live/bag 两套 output path | 建议抽内部 `BagTransformPipeline`/selection helper；只做共享内部实现，避免新增用户接口或模板框架 |
| 手工所有权代码 | raw `new/delete` 集中在 Qt/OSG/FFmpeg/FFI/custom deleter，placement new 集中在 MemoryPool | 逐处核对后大多有框架所有权理由，不能批量机械替换。GraphTask object/control 与旧 MemoryCharge 的确定冗余已分别收敛 |
| TriggerRecorder shared payload | deep-copy + shared_ptr 看起来重，但 snapshot 会跨 ring mutation/dump 持有 payload | 不是冗余；应通过 allocator/RSS benchmark决定是否换 arena/chunk，不可改 shallow view |
| 大量锁的“可换 SpinLock”假象 | DB、crypto、loan map、callback、GUI cache 临界区都含分配、I/O、hash 或用户代码 | 已明确排除。真正应优化的是锁域、快照和分片，而不是锁类型替换 |

规范性上还应补两条约束：热路径日志必须限频并带 counter，不能在 queue overflow/publish 循环逐条 INFO/WARN；新增 allocator/queue 策略必须暴露统计快照和 benchmark，而不是再增加不可观察的环境变量分支。

## 11. 已执行专项测试与仍需补齐的回归

### 11.1 本轮实际执行结果

| 范围 | 实际结果 | 能证明什么 | 不能证明什么 |
|---|---|---|---|
| Release 全量 CTest | manifest 212；串行 212/212，0 failed，188.23 s；最后 DDSc 条件修正后再跑 `ddsc-*` 8 suite 与 `modules-DdscConf`，9/9 通过，16.08 s | 当前工作树功能基线稳定，最后一处 DDSc 修正也完成定向回归 | MQTT/FDBus 服务缺失，73 个受保护用例提前返回；不覆盖目标平台 |
| ASan/LSan | 最终源码串行 212/212，0 sanitizer error，200.12 s；`detect_leaks=1` 保持开启 | MemoryPool、调度、ACK、Bag、Serializer、SHM/SHM2 与该构建全部 CTest 未检出越界、UAF 或泄漏 | 该独立构建设置 `SKIP_DDSR/DDST/QNX=ON`；未启用 UBSan；GCC 16 + ASan + LTO 的跨 DSO inline 变量 ODR 噪声使本轮仅关闭 `detect_odr_violation`，外部服务 guard 仍适用 |
| C API | 独立目标 15/15 顶层场景通过，0 skip，0.77 s | C ABI 的发布、订阅、方法、字段、安全与 SSL 场景可运行 | 不等价于多编译器 ABI 或目标平台验证 |
| clang-tidy | 本轮变更 TU、CLI、Viewer/Analyzer、WebViz 与 Python diff 行最终 RC=0 | 本轮修改没有遗留 actionable clang-tidy error | Python 全文件仍有 3 条未触碰既有告警（1 条 `modernize-use-auto`、2 条 `bugprone-unused-raii`），未夹带清理 |
| 编译矩阵 | GCC 16/C++17 core 与 9 个变更测试 TU、Clang 22/C++20 core、GCC 16/C++20 全测试目标、Python binding、Viewer/Analyzer、Foxglove 均编译通过 | 新增实现具备 C++17 语法兼容性，主要 Linux 构建组合闭环 | 本机无 QNX/macOS/Windows 完整 SDK，后三者仅做宏/ABI 静态审阅，不能称为真实平台编译通过 |
| Python API | Python 3.14.5 nanobind Release 目标重建通过；smoke 21/21（0.19 s）、coverage 27/27（0.34 s）、full 43/43（3.76 s），均 0 skip | MemoryPool `batch_size`、Bag、通信、Proxy、zerocopy 与公开绑定表面已同步并可运行 | Python 不暴露 C++ FlatBuilder 模板类型；不等价于跨版本 wheel/多 Python ABI 测试 |
| MemoryPool 并发/回收与配置 | 47 cases / 227 assertions 全通过；含 4 worker、并发 `clear()`、跨线程释放，以及子进程覆盖 batch env 有效/非法/零值和首次读取缓存 | 分片统计/回收、并发 clear、默认配置解析和显式零值 fallback 可运行 | 不替代 TSAN、长时间 soak 或异常参数滥用测试 |
| MemoryPool 性能/占用 | 128 B/1 KiB 的 1/2/4/7 固定核回归；level 3 预分配与结构尺寸核验 | 单线程未退化、并发争用已消除、chunk quota 未按 shard 放大 | 需要在不同 CPU/NUMA 与长稳服务负载继续观察 |
| extension / impl / zerocopy / serializer 定向 | 21 + 26 + 10 + 11 = 68 个 suite 全通过 | Bag、TriggerRecorder、Node/Ack、八种消息容器+Header+MessageParser、serializer 基本行为 | 大多数 suite 没有并发吞吐/RSS/尾延迟断言 |
| 8 类 transport zerocopy | DDS、Intra、SHM、SHM2、SomeIP、Zenoh 六类实际数据路径通过；MQTT 与 FDBus suite 因缺 broker/name_server 自跳过；新增 FlatBuilder SHM/SHM2 容量归还用例 | 本机可用 transport 的基本 zerocopy 行为；FlatBuilder 不再遗失 loan handle | 不能把 MQTT/FDBus skip 写成通过，也不等价于长时间 loan soak 或目标平台 RSS 测试 |
| Proxy 数据结构 | create→wire→parse 对照；async copy 纯构造；1 KiB/64 KiB/1 MiB blocked-queue；1 MiB 边界前后 | VLink ProxyData 本体无相对回退；async 第二 copy 和 tier 断点均可复现 | 无真实 DDS/SHM proxy 双进程、慢订阅者、丢包、RSS 长稳测试 |
| Viewer/WebViz | viewer/player/analyzer offscreen 启动 smoke；Perception ownership-copy 模型；本轮 `vlink-viewer`、`vlink-analyzer` 与独立 `ENABLE_WEBVIZ=ON` 的 `vlink-foxglove` Release 目标编译通过 | 启动与真实目标编译稳定；Perception pending 有界逻辑经锁序审阅 | 无 GPU/OSG draw、可注入 Qt burst 计数、FFmpeg 真码流、截图正确性和 FPS |
| CLI help | 构建出的 12 个 `vlink-*` 可执行文件（11 个用户 CLI 加 `vlink-test`）逐个 `--help`，均 rc=0 | 参数 parser 和动态链接基本可用 | 不覆盖每个子命令组合 |
| `vlink-check test` | 22 pass、4 warning、0 fail；Intra 两个 URL，SHM/SHM2/DDS/DDSC/Zenoh 的 EVENT/METHOD/FIELD，SomeIP EVENT 实测 | 表中本机可用链路可运行 | MQTT 一项及 FDBus 三项因缺 broker/name_server warning/skip；不能写成全后端实测 |
| CLI Bag check/info | 100k VDB check 31 ms/info 11 ms；100k VCAP 17/11 ms；140k VDB 75/12 ms | reader/index/metadata 工具链可完整走通 | 时间受 page cache 影响，不作为 VDB 对 VCAP 性能排名 |
| CLI 跨格式 clone | 149 MiB/140k VDB→VCAP 268 ms，输出 149,951,954 B；VCAP→VDB 261 ms，输出 155,303,936 B；两次 check 均 0 error | schema/frame/action filter 和双 backend writer 可闭环 | 默认 clone action filter 是 subscriber(6)，源数据是 publish(5)；必须显式 `-s 5`，先前 0-frame 文件不是代码 bug |
| CLI dump/slice/scan | bin dump 100 帧/100 文件/102,400 B；slice dry-run 识别 0.14 s segment；quality scan 生成 events.json；均 rc=0 | dump 二进制、分段规划、质量扫描基本路径 | 未对大图像/点云转换和长期终端 backpressure 做压力测试 |
| CLI bench | throughput+serialization 组合展开 28 cases，全部 OK；JSON report 52,083 B | bench runner、report、Bytes/RawData serialization 能运行 | 默认组合展开比命令直观更多；数字不作为 eka 对照或 CI 性能门槛 |
| Bag 多生产者 | VDB 7×20k×1 KiB，对照默认池/level0/eka；另做 100k 单生产者 VDB/VCAP 读写 | 区分了 submit CPU/latency 与最终持久化耗时 | VCAP 多生产者尚未单独量化；真实慢盘、压缩、多 split 未覆盖 |
| FlatBuilder loan | 修复前 carrier 证明 4100→4104 与 handle 丢失；修复后 Serializer 8/38、SHM2 1/44、SHM 3/74 全通过 | 精确 final size、分配失败的一次性语义、真实 loan pointer/归还容量与连续发布已闭环 | Zenoh owning fallback 不代表端到端单拷贝；未做多小时 soak |
| impl/base fast paths | ACK 1/2/4/7 线程、Intra direct/queue RPC、NodeImpl no-recorder、TaskHandle plain/tracked、GraphTask create/execute | 基础设施退化和非录制快路径成本有数字；GraphTask/TaskHandle 修改后定向回归通过 | 未覆盖所有 Publisher/Subscriber/Client/Server schema/transport 组合 |

### 11.2 仓库测试的真实空洞

`ctest -N` 中没有 Proxy、Viewer、Player、Analyzer 或 CLI 专属 test target。当前 extension/impl/zerocopy suite 数量不少，但更偏正确性；用户指出的覆盖缺口成立。建议按以下最小闭环补充，而不是建立一套巨大的通用测试框架：

1. **Proxy**：进程内 fake publisher 先覆盖 sync/async/direct、1 KiB/64 KiB/默认 1 MiB±128 B 及可配置 active/bypass tier 边界、queue depth、drop、RSS；再增加 DDS+SHM 双进程 nightly，慢 subscriber 下断言 bounded memory 和日志限频。
2. **Viewer**：补 Qt event-loop 定向测试，向现有 Perception callback 注入同 URL burst，断言只执行一次 queued render；再在 render 期间注入新帧，断言最终补执行第二次。不要为了测试再抽一层生产调度框架；真实 OSG/FFmpeg 样本放 nightly，记录 FPS、frame drop、allocation count、RSS。
3. **CLI**：把本轮 check/info/clone/dump/slice/scan 做成小型 fixture CTest，输入 bag 控制在数 MiB；明确 action filter；golden 校验 frame count、schema、timestamp、CRC，而不只检查 rc。
4. **zerocopy**：为八种消息容器各加 construct/reuse/serialize/parse 基准，并单测公共 Header 与 MessageParser；保留现有 FlatBuilder Serializer+SHM/SHM2 精确目标与容量归还回归，再补 Zenoh owning fallback、Client/Server/Setter 真实 transport 组合和长时间 loan soak。
5. **impl**：MessageLoop/ACK/Intra RPC/TaskHandle/GraphTask 加固定核 benchmark target，但 CI 只保存趋势或宽松阈值，避免虚拟机抖动误杀。现有功能测试已覆盖 queue full/drop/reject、parent cancel、callback exception、MultiLoop teardown/idle 和 Graph cycle/status；剩余缺口应写成具体长期竞态或平台场景，不能继续泛称这些路径未测。
6. **extension**：VDB 与 VCAP 都测 1/2/4/7 producers，分别记录 submit p50/p99、persist total、queue bytes；TriggerRecorder 测多 URL burst、同时 dump、overflow policy、snapshot 正确性和 RSS。
7. **外部环境**：MQTT broker、FDBus name_server、QNX、真实 Iceoryx2/Zenoh 分片 payload、GPU/codec 必须标为独立 nightly/目标机 job。本机缺服务时只能 skip，禁止把 skip 写成 pass。
8. **CycloneDDS TLS feature macro**：本机新版 CycloneDDS 定义 `DDS_HAS_TCP_TLS`，VLink 既有代码仍判断 `DDS_HAS_SSL`，会把实际具备 TLS 字段的版本当成不支持。后续应统一三个 guard 并用真实证书补通信测试；本轮只修复“判定为不支持后仍因 `ssl.*` 隐式开启明文 TCP”的确定问题，不夹带扩大兼容改造。

### 11.3 测试实现约束

- 性能 fixture 生成数据，不提交 100 MiB 级 bag；大样本放 artifact/nightly。
- 每项同时记录 correctness、吞吐、p99、allocation/RSS 和 drop，不只看平均耗时。
- 对照 eka-rt 时锁定 commit、编译器、Release flags、CPU affinity、payload 和语义；VLink-only 项明确写 absolute hotspot。
- 测试不得依赖逐条日志判断成功；日志需限频，统计走 counter/snapshot。
- `/tmp` 微基准用于本轮取证，不应原样变成庞大 benchmark framework；只把最小、稳定、能防止已确认回退的 case 收进仓库。

## 12. 建议实施顺序与验证要求

### 阶段 A：基础设施止血，保持改动克制

1. **已完成：** MemoryPool 采用争用触发的 8 shard free list；只公开跨 shard 批量大小，不叠加 TLS cache 或 shard 数量策略，先用真实工作负载观察。
2. **已完成：** MessageLoop 普通/优先级入队在原队列锁内合并 pending，保留共享 CV 所需的 `notify_all()`；normal worker 延迟创建并复用 staging deque。
3. **未实施：** 给 MessageLoop/ThreadPool 使用 32 B 专用 MoveFunction；当前 32 B/64 B capture 分布与 ABI 数据不足，保留公共 Function 默认值。
4. **已完成：** ACK Request 改用 `std::make_shared`；保留 generation/CV，增加最小解析状态，并统一 request→manager 锁序以区分 ACK、取消和超时。
5. **已完成：** FlatBuilder Finish 后申请精确目标并 copy-to-loan；公开 Serializer 模板、SHM/SHM2 连续发布与容量归还测试已补齐。

阶段 A 的 allocator 1/2/4/7、MessageLoop fixed-core/burst/ping、ThreadPool 定向回归、Intra direct/queue/RPC、VDB 单/7 producer、跨线程 allocate/free 和 Bytes 128 B/1 KiB/64 KiB 已复跑。仍缺 VDB/VCAP 2/4 producer 的同轮完整矩阵以及目标平台长稳数据。

### 阶段 B：去掉明确的每任务过度设计

1. **第一阶段已完成：** MultiLoop 去掉 completion custom-deleter token 的共享控制块，并修复 teardown join 锁环；shared callback 因拒绝 fallback 语义保留。
2. **已完成：** GraphTask 使用系统 `allocate_shared` 合并 object/control block；独立 Impl 保留，不再内联。
3. **部分完成并收口：** TaskHandle 初始 State 直接 queued，删除一次锁/通知；lazy cancellation 因语义和竞态复杂度拒绝实施。
4. Proxy 先增加 queue depth/bytes/drop 可观测性和 bounded policy；仅对不跨有效 pool tier 的 payload A/B 采用 owning ProxyData，1 MiB 边界禁止无条件 prebuild。

阶段 B 的任务 drop/reject/cancel、父 token、异常 callback、quit/teardown、MultiLoop idle、Graph cycle/status callback 已由定向与全量测试覆盖。仍未闭环的是 Proxy async 真实 queue full/quit、tier 边界、慢消费者与 RSS 高水位。

### 阶段 C：VLink-only 消费端削峰

1. **Perception 已完成：** 每 URL pending render 有界；Point3D 逐帧统计和视频帧间依赖禁止机械 coalesce，WebViz 需先定义丢帧契约。
2. **已完成：** 删除 SHM/SHM2 wait-mode 逐消息 INFO。
3. **已完成：** Analyzer schema lookup 外提，Foxglove 两处只读锁改 shared。
4. 在目标平台验证 SHM2 threshold、Zenoh materialization、QNX MultiLoop。

### 性能回归门槛建议

- 基础微基准固定物理核，至少 7 轮、丢弃前 2 轮，比较中位数和最大值；
- 1 线程不得因并发优化退化超过 5%；4/7 线程吞吐至少不低于 eka-rt；
- MessageLoop 普通持续投递至少把 1m fixed-core 差距压到 15% 内；
- Intra RPC 4 client 不得比 eka-rt 慢超过 15%；
- Bag 同时记录 submit latency 和 final persistence time，禁止只看最终磁盘时间掩盖生产者阻塞；
- 所有零拷贝测试必须同时校验 payload、loan map 数量、连续发布深度和归还行为。
- Proxy 需覆盖默认 1 MiB 和各配置 active/bypass tier 边界两侧，消费者阻塞时内存必须有界，优化后不得只改善 copy benchmark 却恶化 backlog/RSS；
- Viewer burst 应断言每 URL pending render 有界，并分别记录 input、rendered、coalesced、dropped 数量；
- TriggerRecorder 同时 dump 时不得让 capture submit p99 无界增长，snapshot frame/byte 统计必须与输出一致。

## 13. 最终结论

VLink 当前确实存在热路径性能退化，但不是所有通讯后端、序列化、日志或落盘实现都退化。最可靠的因果链是：

```text
修改前的全局单-tier MemoryPool 竞争（本轮已修复）
        + MessageLoop 二次锁/每批 PMR 分配（本轮已修复）
        + 仍偏大的任务 wrapper
                    |
                    +--> ThreadPool / MultiLoop（completion 控制块已移除）/ GraphTask（object/control 已合并）
                    +--> ACK / Intra RPC
                    +--> Bag 多生产者异步入队
                    +--> Proxy/WebViz/GUI queued 路径
```

修复应围绕这条因果链展开。allocator 扩展性的第一阶段、普通 MessageLoop 的两项确定热点、ACK Request allocator/响应顺序、MultiLoop completion token/teardown 锁环、GraphTask object/control、TaskHandle 初始状态和 FlatBuilder 精确 loan 已完成；下一步瓶颈更集中在 ThreadPool 的任务包装、MultiLoop 保留的共享 callback、Proxy/WebViz 的有界队列与可选 tracked-task 完整包装成本。不要因为池已经变快就把数据库、压缩、加密、中间件 loan map 或用户 callback 锁迁入池或改为自旋锁，这些位置风险仍大于收益。

就 MemoryPool 本身，最终判断不是“全面胜过无池方案”：它在 level 3 全部 tier 上界的同线程与 7 线程测试中均胜过 eka 当前的 `std::pmr::synchronized_pool_resource`，在 ≤1 MiB 跨线程归还模型中也同时胜过 eka 与 malloc；但 glibc tcache 在同线程立即 alloc/free 的小对象上更快，而且 VLink 会保留有效 tier 的 chunk 高水位直到显式 `clear()`。因此 VLink 当前更适合通信服务的稳定重用与跨线程所有权转移，不适合被描述为任何分配模式、任何资源目标下都占优。

本轮没有改变 MemoryPool 的分配/释放函数签名，但 `MemoryPool::Config` 新增 `batch_size`（默认 16），并增加 `VLINK_MEMORY_BATCH_SIZE` 对默认配置的可选覆盖。该公开聚合体的 size/layout 已改变：源码重编兼容，使用旧布局 Config 或 `MemoryResource(Config)` 的外部 C++ 二进制必须重建。C++ Doxygen、基础库与环境变量文档、Python binding/覆盖测试、CHANGELOG 均已同步；显式 Config 的字段值不会被环境变量暗改。`Serializer::serialize_to_transport()` 的公开声明、Doxygen、序列化文档和 CHANGELOG 已同步；Python 不暴露对应 C++ builder 模板，因此不新增伪对等接口。MessageLoop、GraphTask allocator、TaskHandle 初始状态、Perception pending、Analyzer/Foxglove 锁域都是内部实现，不新增配置或 Python API。当前 Bag 的 C++/Python/文档/CHANGELOG 公开说明也已对齐。

最终交付验证为：Release CTest 212/212，最终源码 ASan/LSan 212/212，C API 15/15，Python smoke/coverage/full 21/27/43 全通过；本轮变更范围 clang-tidy 为零错误。GCC 16/C++17、GCC 16/C++20、Clang 22/C++20、Python、Viewer/Analyzer 与 Foxglove 目标已编译；QNX、macOS、Windows 因本机缺完整 SDK 只完成条件分支与 ABI 静态审阅，不能写成真实平台编译通过。MQTT/FDBus 的 73 个外部服务依赖用例同样只可记录为提前返回，不能写成数据面通过。
