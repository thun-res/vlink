# object_pool — `vlink::ObjectPool<T>` 对象复用池

`vlink::ObjectPool<T>` 是一个固定容量的对象复用池：在工厂回调里构造、按需借出、归还时（可选）调 reset 回调清理状态。适合那些"构造开销大、复用频繁"的对象 —— 例如复杂的网络连接、缓冲区、消息对象。

读完本示例你能掌握：

- RAII / shared / 手动三种借出方式。
- 四种 ResetPolicy 的差异（None / Acquire / Release / Both）。
- 池容量耗尽的行为。
- 统计接口的用法。

## 背景与适用场景

适用：

- 业务对象构造慢（含大量字段、内部分配）。
- 高频借出+归还（典型周期 < 1ms）。
- 想限制并发使用数（最多 N 个并存）。

不适合：

- 对象构造成本 < 一次锁开销（直接 new 更便宜）。
- 对象需要在借出方之间安全共享（用 shared_ptr 模式）。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `ObjectPool(FactoryCallback factory, size_t initial_size, size_t max_size = 0, ResetCallback reset = nullptr, ResetPolicy = kPolicyRelease)` | 构造 | 工厂、初始尺寸、上限、reset 回调、reset 时机 |
| `ObjectPool::ResetPolicy` | enum | `kPolicyNone` / `kPolicyAcquire` / `kPolicyRelease` / `kPolicyBoth` |
| `ObjectPool::get` | `std::unique_ptr<T, PoolDeleter> get()` | RAII；析构归还 |
| `ObjectPool::get_shared` | `std::shared_ptr<T> get_shared()` | 共享所有权；最后 release 时归还 |
| `ObjectPool::borrow` | `T* borrow()` | 裸指针；调用方必须显式 give_back |
| `ObjectPool::give_back` | `void give_back(T*)` | 归还借出的对象 |
| `ObjectPool::size` / `borrowed` / `total_created` / `stats` | 同名 | 监控 |

## ResetPolicy

| Policy | 触发时机 |
|--------|---------|
| `kPolicyNone` | 不调 reset 回调 |
| `kPolicyAcquire` | 借出（get）时调 |
| `kPolicyRelease` | 归还时调（默认推荐） |
| `kPolicyBoth` | 借出和归还都调 |

## 代码导读

### 1. RAII get

```cpp
ObjectPool<Buffer> pool(
    /*factory=*/[]() { return std::make_unique<Buffer>(); },
    /*initial=*/4,
    /*max=*/8,
    /*reset=*/[](Buffer& b) { b.clear(); },
    ObjectPool<Buffer>::kPolicyRelease);

{
  auto ptr = pool.get();           // unique_ptr<Buffer, PoolDeleter>
  ptr->append("data");
}                                  // 离开作用域，自动归还 + reset
```

### 2. get_shared

```cpp
{
  std::shared_ptr<Buffer> a = pool.get_shared();
  std::shared_ptr<Buffer> b = a;   // 引用计数=2
}                                  // 最后 release 时归还
```

### 3. 手动 borrow / give_back

```cpp
Buffer* p = pool.borrow();
// 使用 p
pool.give_back(p);
```

适合 C 风格回调或不能用 RAII 的代码路径。

### 4. ResetPolicy 对比

```cpp
ObjectPool<Buffer> pool_release(factory, 4, 8, reset_cb, ObjectPool<Buffer>::kPolicyRelease);
ObjectPool<Buffer> pool_acquire(factory, 4, 8, reset_cb, ObjectPool<Buffer>::kPolicyAcquire);
ObjectPool<Buffer> pool_both(factory, 4, 8, reset_cb, ObjectPool<Buffer>::kPolicyBoth);
ObjectPool<Buffer> pool_none(factory, 4, 8, nullptr, ObjectPool<Buffer>::kPolicyNone);
```

业务通常用 `kPolicyRelease`：归还时清状态，下次 get 时对象已就绪。

### 5. 统计

```cpp
MLOG_I("  size={} borrowed={} total_created={}", pool.size(), pool.borrowed(), pool.total_created());
auto s = pool.stats();
// stats 字段：created / available / borrowed / max_size / acquire_count / release_count
```

### 6. 池耗尽

```cpp
ObjectPool<Buffer> small_pool(factory, 1, 1);
auto p1 = small_pool.get();
try {
  auto p2 = small_pool.get();  // 抛 std::runtime_error
} catch (const std::runtime_error& e) {
  VLOG_I("pool exhausted: ", e.what());
}
```

`max_size=0` 表示不限上限；非 0 时超出抛异常。

## 运行

```bash
./build/output/bin/example_object_pool
```

预期输出（节选）：

```
=== RAII get ===
  data
=== get_shared ===
  shared by 2
=== borrow + give_back ===
  manual ok
=== ResetPolicy ===
  Release: 4 resets
  Acquire: 4 resets
  Both:    8 resets
  None:    0 resets
=== Stats ===
  size=4 borrowed=2 total_created=4
=== Exhaustion ===
  pool exhausted: ObjectPool: max_size reached
```

## 常见陷阱

1. **borrow 之后忘记 give_back**：泄漏；建议优先用 RAII。
2. **kPolicyAcquire reset 慢**：每次 get 都跑；高频 get 时建议改 kPolicyRelease。
3. **factory 内 throw**：vlink 行为按实现可能让池处于不一致状态；避免。
4. **多线程 get 时 max_size 已满**：抛 runtime_error；高并发要么扩 max_size 要么 try-catch。
5. **shared_ptr 跨线程长期持有**：占用池容量；可能让其它线程拿不到。

## 设计要点

- ObjectPool 内部用 SpinLock + 队列管理空闲对象。
- 工厂函数延迟调用：实际 get 才创建；不预分配 initial_size 个对象（取决于实现细节）。
- PoolDeleter 通过 unique_ptr 的 deleter 接口实现自动归还。

## 配图

无专属配图。

## 参考

- `../memory_pool/` — 不同抽象（按字节）
- `../spin_lock/` — ObjectPool 内部用的同步原语
- `vlink/include/vlink/base/object_pool.h` — ObjectPool 接口
