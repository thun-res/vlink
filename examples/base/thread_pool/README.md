# thread_pool — `vlink::ThreadPool` 多线程并行任务

`vlink::ThreadPool` 是固定线程数的工作池：N 个 worker 线程从共享队列取任务并行执行。与 `MessageLoop`（单线程串行）对比，ThreadPool 适合 CPU 密集型并行计算。

读完本示例你能掌握：

- `ThreadPool` 的构造、`post_task` / `invoke_task` 投递方式。
- 两种队列类型（normal vs lockfree）的取舍。
- `kBlockStrategy` vs `kPopStrategy` dispatch 策略。
- `is_in_work_thread()` 判断当前线程身份。
- `shutdown()` 的语义。

## 背景与适用场景

适用：

- CPU 密集型计算（图像处理、矩阵运算、规划算法）。
- 把 vlink Subscriber/Server 回调里的慢逻辑甩到工作线程。
- 并发处理 N 个独立任务并收集结果（fan-out / fan-in）。

不适合：

- 严格串行执行（用 MessageLoop）。
- 需要"绑定到特定线程"（vlink 不保证哪个 worker 处理任务）。
- IO bound 任务（用异步 IO 而非堆线程数）。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `ThreadPool(uint32_t threads, QueueType = kNormalType)` | 构造 | N 个 worker 线程 |
| `ThreadPool::kNormalType` / `kLockfreeType` | enum | 队列实现 |
| `post_task` | `void post_task(Function<void()>&&, const PostTaskOptions& = {})` | 不要结果 |
| `post_task_handle` | `TaskHandle post_task_handle(Function<void()>&&, const PostTaskOptions& = {})` | 带句柄 + cancellation |
| `invoke_task` | `template <typename Callable> std::future<R> invoke_task(Callable&&)` | 拿返回值 |
| `set_name` / `get_name` | const | 线程组名 |
| `set_strategy` / `get_strategy` | const | `kBlockStrategy` / `kPopStrategy` |
| `get_type` / `get_max_task_count` | const | 队列类型、容量 |
| `is_in_work_thread` | const | 当前线程是否是该 pool 的 worker |
| `shutdown` | `void shutdown()` | 阻塞等待所有任务完成；析构会自动调 |

## 代码导读

### 1. 基础并行执行

```cpp
ThreadPool pool(4);
pool.set_name("worker_pool");
parallel_tasks::demo_basic_parallel(pool);   // 10 个任务投递到 4 线程
```

`parallel_tasks.h` 的 helper 把任务分散到 pool；10 个任务在 4 worker 上以约 2.5 个/线程的速度跑完。

### 2. invoke_task + future

```cpp
ThreadPool pool(4);
parallel_tasks::demo_invoke_tasks(pool);     // 8 个 invoke 收集平方值
```

`invoke_task` 返回 `std::future<R>`，调用方 `.get()` 阻塞等结果；适合 fan-out + fan-in。

### 3. Lockfree 队列

```cpp
parallel_tasks::demo_lockfree_sum();
```

helper 用 `kLockfreeType` 构造 pool 并并行求和 1..100。lockfree 队列在高并发投递场景下延迟更稳定，但不支持优先级。

### 4. 策略 + 状态查询

```cpp
ThreadPool pool(2);
pool.set_name("query_pool");
MLOG_I("  name={} type={} strategy={} max_tasks={}", pool.get_name(),
       static_cast<int>(pool.get_type()), static_cast<int>(pool.get_strategy()),
       pool.get_max_task_count());

pool.set_strategy(ThreadPool::kBlockStrategy);
MLOG_I("  strategy after change={}", static_cast<int>(pool.get_strategy()));

pool.post_task([]() { VLOG_I("  task on block-strategy pool"); });
MLOG_I("  is_in_work_thread (main)={}", pool.is_in_work_thread());     // 0
pool.post_task([&pool]() { MLOG_I("  is_in_work_thread (worker)={}", pool.is_in_work_thread()); });   // 1

pool.shutdown();
```

`is_in_work_thread()` 用于"避免在 worker 里调阻塞 future.get() 导致死锁"等场景。

## 运行

```bash
./build/output/bin/example_thread_pool
```

预期输出（节选）：

```
=== Basic parallel ===
... 10 个任务在 4 worker 上散开 ...
=== invoke_task with future ===
... 8 个 future 收集 1^2..8^2 = 1, 4, 9, 16, ..., 64 ...
=== Lockfree queue ===
  sum(1..100) = 5050
=== Strategy + state ===
  name=query_pool type=0 strategy=0 max_tasks=...
  strategy after change=0
  task on block-strategy pool
  is_in_work_thread (main)=0
  is_in_work_thread (worker)=1
ThreadPool example finished.
```

## 常见陷阱

1. **future.get() 在 worker 线程里**：等的就是 worker 自己，死锁。先 `is_in_work_thread()` 判断。
2. **task 抛异常**：未捕获异常会让 worker 退出；vlink 不会重启 worker。回调里要 try/catch。
3. **shutdown 时间长**：等所有 inflight 任务跑完；想立即终止用 cancellation_token。
4. **lockfree + 大量小任务**：lockfree 队列有原子争抢，超高频投递下不一定比 mutex 快；先 benchmark。
5. **线程数过多**：超过 hardware_concurrency 后多线程争抢反而慢；默认 = N CPU。

## 设计要点

- ThreadPool 默认 `kBlockStrategy`：worker 空闲时 cv 阻塞；CPU 0%。
- `kPopStrategy` 自旋等待，亚微秒响应但 CPU 100%（每个 worker）。
- ThreadPool 不绑定 CPU 亲和性；要绑用 `Utils::set_thread_stick`。
- 析构会调 shutdown；不需要显式调，除非要在析构前等。

## 配图

无专属配图。

## 参考

- `../message_loop_basic/` — 单线程串行 vs ThreadPool 并行
- `../multi_loop/` — N 个 MessageLoop 的 facade
- `../task_handle/` — 带 cancellation 的任务句柄
- `../graph_task/` — DAG 依赖执行
- `vlink/include/vlink/base/thread_pool.h` — ThreadPool 完整接口
