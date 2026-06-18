# spin_lock — `vlink::SpinLock` 极短临界区自旋锁

`vlink::SpinLock` 是用户态自旋锁，用于**极短**临界区（几条指令、几十纳秒）。任何会阻塞 / 涉及系统调用 / 涉及内存分配的代码都不应该用 SpinLock —— 用 `std::mutex`。

读完本示例你能掌握：

- SpinLock 的基本 API（`lock` / `unlock` / `try_lock`）。
- 配合 `SpinLockGuard` 或 `std::lock_guard` 的 RAII 用法。
- 与 `std::mutex` 的性能权衡。
- 适用 / 不适用场景的判断。

## 背景与适用场景

适用：

- 极短临界区（cache-line 写入、原子计数无法表达的复合操作）。
- 锁持有时间确定 < 几十纳秒。
- 高频锁定 + 几乎无争抢。

不适合：

- 临界区里有任何 syscall（read/write/socket）、malloc、log 输出。
- 持有时间长的场景。
- 高争抢 —— 自旋会浪费 CPU。

注意：SpinLock 在 vlink 内部仅在 hot path 的小数据结构上用；业务代码 99% 应该用 `std::mutex`。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `vlink::SpinLock` | 默认构造 | unlocked |
| `lock` | `void lock()` | 自旋直到拿到锁 |
| `unlock` | `void unlock()` | 释放 |
| `try_lock` | `bool try_lock()` | 非阻塞，立刻返回 |
| `vlink::SpinLockGuard(SpinLock&)` | RAII | 构造时 lock，析构时 unlock |
| `std::lock_guard<SpinLock>` | RAII | SpinLock 满足 Lockable 概念 |

## 代码导读

### 1. 手动 lock/unlock

```cpp
SpinLock lock;
lock.lock();
// critical section
lock.unlock();
```

### 2. SpinLockGuard / std::lock_guard

```cpp
SpinLock lock;
{
  SpinLockGuard guard(lock);
  // critical section
}

// C++17 CTAD：
{
  std::lock_guard guard(lock);     // std::lock_guard<SpinLock> 自动推导
  // critical section
}
```

### 3. try_lock

```cpp
if (lock.try_lock()) {
  // 拿到锁
  lock.unlock();
} else {
  // 别人持有
}
```

### 4. 高并发计数器

```cpp
SpinLock lock;
int64_t counter = 0;

std::vector<std::thread> threads;
for (int t = 0; t < 4; ++t) {
  threads.emplace_back([&]() {
    for (int i = 0; i < 100000; ++i) {
      SpinLockGuard guard(lock);
      counter++;
    }
  });
}
for (auto& th : threads) th.join();
```

4 线程各 10 万次递增；最终 counter=400000。

注意：这种场景 `std::atomic<int64_t>` + `fetch_add` 是更优解，根本不需要锁。SpinLock 只在"无法用 atomic 表达的复合操作"（如同时改两个字段）时才合理。

### 5. 与 std::mutex 微基准

示例代码里跑 1M 次 lock/unlock，对比 SpinLock 与 std::mutex 的无争抢 fast-path 延迟。典型差距约 2-5 倍（SpinLock 更快），但争抢下 SpinLock 急剧恶化。

## 运行

```bash
./build/output/bin/example_spin_lock
```

预期输出（节选）：

```
=== Manual lock/unlock ===
  ok
=== RAII guard ===
  guarded section done
=== try_lock ===
  free=1 held=0 after_release=1
=== Counter (4 threads x 100000) ===
  counter=400000
=== std::lock_guard CTAD ===
  ok
=== Microbenchmark (1M cycles) ===
  spinlock: ... ns
  mutex:    ... ns
```

## 常见陷阱

1. **忘记 unlock**：死锁；用 SpinLockGuard / std::lock_guard 防御。
2. **临界区里阻塞**：极短临界区前提被破坏；所有其它线程都卡在自旋。
3. **多 SpinLock 嵌套**：死锁概率剧增；避免嵌套或固定 lock order。
4. **uncontended 微基准误导**：SpinLock 在无争抢下飞快，争抢下灾难；按真实负载测。
5. **跨进程同步**：SpinLock 是进程内的；进程间用 `vlink::SysSemaphore` 或 shm。

## 设计要点

- vlink SpinLock 基于 `std::atomic_flag` + `pause` 指令。
- 没有公平性保证（饥饿可能）。
- 满足 C++ Lockable 概念，可与 `std::lock_guard` / `std::unique_lock` 联动。

## 配图

无专属配图。

## 参考

- `../message_loop_basic/` — 单线程串行天然无锁
- `../object_pool/` / `../memory_pool/` — vlink 内部使用 SpinLock 的典型场景
- `vlink/include/vlink/base/spin_lock.h` — SpinLock 接口
