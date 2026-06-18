# memory_pool — `vlink::MemoryPool` 分级金字塔字节内存池

`vlink::MemoryPool` 是 vlink 内部用于加速 `Bytes` 分配的分级 free-list 分配器。配置好之后，`Bytes::create(n)` 会按需路由到最小合适的 tier，避免每次 malloc/free 的系统调用开销。

读完本示例你能掌握：

- 分级内存池的工作原理。
- `VLINK_MEMORY_LEVEL` 环境变量的作用。
- `global_instance` 与 Bytes 集成的关系。
- 五种典型使用模式（默认、超大请求、自定义 tier、级别预设、bypass）。

## 背景与适用场景

适用：

- 高频小 Bytes 分配（每秒 ≥ 10万次 publish 的应用层）。
- 嵌入式系统希望避免 malloc 抖动。
- 自定义 tier 适配业务的固定消息大小。

不适合：

- 极少分配的场景（直接 malloc 更简单）。
- 分配 size 跨度极大且无 tier 命中模式（命中率低，反而比 malloc 慢）。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `MemoryPool::get_default_config` | `static Config get_default_config()` | 读 `VLINK_MEMORY_LEVEL` 生成 |
| `MemoryPool::global_instance` | `static MemoryPool& global_instance(bool prefer_default = true)` | 单例；与 Bytes 共享 |
| `MemoryPool(Config)` | 构造 | 显式 tier 列表 |
| `MemoryPool(int level)` | 构造 | 按 vlink 预设级别 0-9 |
| `MemoryPool()` | 默认 | bypass（每次 ::operator new） |
| `allocate` | `void* allocate(size_t bytes) noexcept` | 路由到 tier 或 fallback |
| `deallocate` | `void deallocate(void* ptr, size_t bytes) noexcept` | **必须传同样的 bytes** |
| `get_tier_count` | `size_t` | tier 数量 |
| `get_stats` | `std::vector<Stats>` | 每 tier 的统计 |
| `get_oversized_stats` | `Stats` | 超大请求统计 |
| `reset_stats` / `clear` | 同名 | 重置统计 / 释放完全空闲的 chunk |

## 代码导读

### 1. 默认配置 + 环境变量

```cpp
auto cfg = MemoryPool::get_default_config();
MLOG_I("  tiers: {}", cfg.tiers.size());
// VLINK_MEMORY_LEVEL=0..9 控制 tier 数量与上限
```

`VLINK_MEMORY_LEVEL=0` 表示 bypass；1-9 表示不同 tier 配置。

### 2. 超大请求 fallback

```cpp
MemoryPool tiny(MemoryPool::Config{/*small tiers*/});
void* p = tiny.allocate(16 * 1024 * 1024);   // 16 MiB
// 没有 tier 能装，自动 fallback ::operator new
tiny.deallocate(p, 16 * 1024 * 1024);
```

### 3. 自定义 tier

```cpp
MemoryPool::Config cfg;
cfg.tiers = {
  { /*max_size=*/4096, /*chunk_count=*/64, /*prealloc=*/true },
  { /*max_size=*/8192, /*chunk_count=*/32, /*prealloc=*/true },
};
MemoryPool pool(cfg);
```

为特定消息大小手工调 tier；`prealloc=true` 在构造时预分配 chunk_count 个对象。

### 4. 与 Bytes 集成

```cpp
auto& pool = MemoryPool::global_instance(/*prefer_default=*/true);
Bytes::init_memory_pool();
auto buf = Bytes::create(200);   // 走 global pool
```

`global_instance(true)` 在首次调用时读 `VLINK_MEMORY_LEVEL` 并构造单例；后续 `Bytes::create` 都走这个池。

### 5. 预设级别

```cpp
MemoryPool level_4(4);   // 用预设的 level 4 配置
```

### 6. bypass

```cpp
MemoryPool bypass;             // 等价 level=0
void* p = bypass.allocate(64); // 直接 ::operator new
bypass.deallocate(p, 64);
```

## 运行

```bash
./build/output/bin/example_memory_pool
```

预期输出（节选）：

```
=== Default config ===
  tiers: 5 (VLINK_MEMORY_LEVEL=...)
=== Oversized passthrough ===
  16 MiB allocated via fallback
=== Custom tier config ===
  tier(4096) prealloc=64
  tier(8192) prealloc=32
=== global_instance + Bytes ===
  pooled alloc size=200
=== Level preset ===
  level=4 tiers=...
=== Bypass mode ===
  allocated via system heap
```

## 常见陷阱

1. **deallocate 用错 size**：路由到错误 tier 损坏 free list；vlink 不会自动校验。一定要存原始 size。
2. **VLINK_MEMORY_LEVEL=0**：bypass，没有任何性能优化；生产建议 ≥ 1。
3. **频繁 clear()**：释放空闲 chunk，下次 allocate 又会创建；高频 clear 反而抖动。
4. **多个 MemoryPool 实例共存**：每个独立 tier list；不会跨实例共享。
5. **跨进程**：MemoryPool 是进程内的；shm 场景另用 Iceoryx 内部池。

## 设计要点

- 内部用 SpinLock 保护每 tier 的 free list。
- tier max_size 单调递增；分配按"最小够装"原则路由。
- prealloc=true 让构造期付分配代价、运行期 fast path 命中率高。
- `clear()` 只释放完全空闲的 chunk，不影响已借出对象。

## 配图

无专属配图。

## 参考

- `../bytes_basic/` — Bytes 是池的主要客户
- `../bytes_advanced/` — `Bytes::init_memory_pool()` 显式开启
- `../object_pool/` — 不同抽象（按对象）
- `vlink/include/vlink/base/memory_pool.h` — MemoryPool 接口
- 顶层 `doc/21-environment-vars.md` — VLINK_MEMORY_LEVEL
