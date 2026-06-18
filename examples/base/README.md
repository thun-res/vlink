# base — vlink 基础库示例

`base/` 目录收录的是 `vlink/base/` 公共头文件下各组件的独立示例。它们不依赖任何通信后端（不出现 `intra://`、`shm://`、`dds://` 等 URL），可以单独编译运行，便于在阅读 `communication/` 等上层示例之前，先熟悉框架共用的基础设施。

与 `communication/` 等分类相比，`base/` 关注的是"在一个进程内"的能力：内存载体、日志、事件循环、定时器、并发原语、线程池、对象池、子进程管理等。绝大多数 vlink 上层 API 都建立在这些组件之上，因此即便不写消息收发代码，本目录的例子也值得通读一遍。

## 子示例索引

| 示例 | 主题 | 涉及类 / 头文件 |
|------|------|------------------|
| `bytes_basic/` | Bytes 创建、SBO/堆切换、offset、resize 族、字节序 | `vlink::Bytes` |
| `bytes_advanced/` | LZAV 压缩、Base64、CRC-32、十六进制、用户输入解析 | `vlink::Bytes` |
| `bytes_zerocopy/` | 五种所有权模式：`shallow_copy` / `deep_copy` / `loan_internal` / `shallow_copy_ptr` / `deep_copy_self` | `vlink::Bytes` |
| `uuid_basic/` | RFC 4122 UUID 构造、解析、版本/变体位、`random_bytes` / `random_hex` | `vlink::Uuid` |
| `logger_basic/` | 四种调用风格（`VLOG_*` / `MLOG_*` / `CLOG_*` / `SLOG_*`）+ 运行时级别 | `vlink::Logger` |
| `logger_advanced/` | 自定义 handler、backtrace 环形缓冲、Fatal 抛异常、`is_writable` 守卫 | `vlink::Logger` |
| `message_loop_basic/` | `async_run` / `post_task` / lifecycle handler / `spin_once` / 状态查询 | `vlink::MessageLoop` |
| `message_loop_advanced/` | 三种队列类型 + 调度策略 + `exec_task(Schedule::Config)` + `invoke_task` | `vlink::MessageLoop` |
| `timer/` | `attach` / `start` / `stop` / `loop_count` / `call_once` / 动态间隔切换 | `vlink::Timer` |
| `elapsed_timer/` | `kCpuTimestamp` vs `kCpuActiveTime`，毫秒/微秒/纳秒精度，静态时间戳工具 | `vlink::ElapsedTimer` |
| `deadline_timer/` | 基于绝对截止时间的原子超时检测（重试预算、流式 IO 超时） | `vlink::DeadlineTimer` |
| `thread_pool/` | 并行执行 + `invoke_task` future + 锁/无锁队列 + 调度策略查询 | `vlink::ThreadPool` |
| `multi_loop/` | 多线程 `MessageLoop` 派发，支持优先级与 Schedule | `vlink::MultiLoop` |
| `schedule/` | `Schedule::Config` 四元组与 `Status` / `RetStatus` 链式回调 | `vlink::Schedule` |
| `cancellation/` | `CancellationSource` / `Token` / `Registration` 协作取消模型 | `vlink::CancellationSource` |
| `task_handle/` | 可追踪任务：`TaskExecutionState` / `wait` / `cancel` / `kReject` / `kProtected` | `vlink::TaskHandle` |
| `graph_task/` | DAG 任务编排：线性 / 菱形 / 条件分支 / DOT 导出 | `vlink::GraphTask` |
| `spin_lock/` | 短临界区自旋锁与 RAII guard，对比 `std::mutex` 微基准 | `vlink::SpinLock` |
| `object_pool/` | 对象复用：RAII `get()` / `get_shared()` / 手动 `borrow` / 重置策略 | `vlink::ObjectPool` |
| `memory_pool/` | 分级 free-list 分配器，oversized 透传，与 `Bytes` 共享全局实例 | `vlink::MemoryPool` |
| `process/` | 跨平台子进程：同步 `execute` / 异步管道 / stdin/stdout/stderr / 信号 | `vlink::Process` |
| `utils/` | 系统信息、环境变量、网络枚举、单例锁、线程亲和、信号注册 | `vlink::Utils` |

## 推荐阅读顺序

**第一组：内存与日志（每个 vlink 程序都会用到）**

1. `bytes_basic/` → `bytes_zerocopy/` → `bytes_advanced/`
2. `logger_basic/` → `logger_advanced/`

先把 `Bytes` 的三种典型用法（拥有 / 别名 / 借用）和日志的四种风格搞清楚，后面所有示例都会用到 `VLOG_I` / `MLOG_I`，以及 `Bytes` 作为消息载体。

**第二组：事件循环与定时器**

3. `message_loop_basic/` → `timer/` → `message_loop_advanced/`
4. `elapsed_timer/` → `deadline_timer/`

事件循环是 vlink 调度的核心；`Timer` 必须绑定到 `MessageLoop` 才能触发回调；`ElapsedTimer` 用于性能测量，`DeadlineTimer` 用于超时预算。

**第三组：并发与任务调度**

5. `thread_pool/` → `multi_loop/`
6. `schedule/` → `task_handle/` → `cancellation/`
7. `graph_task/`

线程池与多 loop 解决"哪一个线程跑"的问题；`Schedule` / `TaskHandle` / `Cancellation` 解决"什么时候跑、能不能取消"的问题；`GraphTask` 把多个任务编排成 DAG。

**第四组：资源池与系统接口**

8. `spin_lock/` → `object_pool/` → `memory_pool/`
9. `process/` → `utils/`

资源池适合在热路径上避免分配；`Process` / `Utils` 是系统侧封装，跨平台一致。

## 配图

部分示例自带 `images/*.png` 图示，统一在自己的 README 中引用：

- `bytes_basic/images/bytes-sbo-vs-heap.png` —— `Bytes` 对象的内联 SBO 与堆切换边界
- `message_loop_basic/images/message-loop-architecture.png` —— 单事件循环的线程模型与任务流向
- `timer/images/timer-lifecycle.png` —— `Timer::attach/start/stop/restart` 的状态机
- `cancellation/images/cancellation-usage-modes.png` —— 轮询 / 结构化 / 回调三种取消用法
- `task_handle/images/task-handle-timeline.png` —— 一个任务从入队到终止的状态迁移
- `graph_task/images/graph-task-dag.png` —— 线性 / 菱形 / 条件分支三种 DAG 结构

## 参考

- 顶层 `doc/11-base-library.md` —— `vlink/base/` 整体设计文档
- 顶层 `doc/00-whitepaper.md` —— vlink 总览（含三种通信模型）
- `include/vlink/base/` —— 全部公共头文件
