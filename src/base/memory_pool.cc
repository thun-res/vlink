/*
 * Copyright (C) 2026 by Thun Lu. All rights reserved.
 * Author: Thun Lu <thun.lu@zohomail.cn>
 * Repo:   https://github.com/thun-res/vlink
 *  _    __   __      _           __
 * | |  / /  / /     (_) ____    / /__
 * | | / /  / /     / / / __ \  / //_/
 * | |/ /  / /___  / / / / / / / ,<
 * |___/  /_____/ /_/ /_/ /_/ /_/|_|
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "./base/memory_pool.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "./base/spin_lock.h"
#include "./base/utils.h"

#define MEMORY_POOL_NEVER_DELETE 0

#define MEMORY_POOL_LOG(format, ...)                \
  do {                                              \
    std::fprintf(stderr, format "\n", __VA_ARGS__); \
    std::fflush(stderr);                            \
  } while (false)

namespace vlink {

static constexpr int kMinMemoryLevel = 0;
static constexpr int kMaxMemoryLevel = 9;
static constexpr int kDefaultMemoryLevel = 3;
static constexpr size_t kMaxTierCount = 20U;
static constexpr size_t kMaxLevelCount = 10U;
static constexpr size_t kInitialBlocksPerChunk = 1U;
static constexpr size_t kInitialChunksReserve = 16U;
static constexpr size_t kInitialChunkBytesTarget = 64U * 1024U;
static constexpr size_t kMaxLazyChunkBytes = 64U * 1024U;
static constexpr size_t kMinLazyChunkBytes = 32U * 1024U;
static constexpr size_t kLazyChunkQuotaDivisor = 16U;
static constexpr size_t kMinTierShardCount = 8U;
static constexpr size_t kMaxTierShardCount = 64U;
static constexpr size_t kDefaultBatchSize = 16U;
static constexpr uint32_t kShardingContentionThreshold = 8U;

static_assert(__STDCPP_DEFAULT_NEW_ALIGNMENT__ >= MemoryPool::kBlockAlignment,
              "MemoryPool: plain operator new must satisfy kBlockAlignment");

// clang-format off
static constexpr MemoryPool::Tier kDefaultTierTable[kMaxLevelCount][kMaxTierCount] = {
    // L0 ~ 0 MiB. (bypass; all entries are sentinels)
    {
        {32U, 0U},
        {64U, 0U},
        {128U, 0U},
        {256U, 0U},
        {512U, 0U},
        {1U * 1024U, 0U},
        {2U * 1024U, 0U},
        {4U * 1024U, 0U},
        {8U * 1024U, 0U},
        {16U * 1024U, 0U},
        {32U * 1024U, 0U},
        {64U * 1024U, 0U},
        {128U * 1024U, 0U},
        {256U * 1024U, 0U},
        {512U * 1024U, 0U},
        {1U * 1024U * 1024U, 0U},
        {4U * 1024U * 1024U, 0U},
        {8U * 1024U * 1024U, 0U},
        {16U * 1024U * 1024U, 0U},
    },
    // L1 ~ 4 MiB.
    {
        {32U, 8U * 1024U},
        {64U, 4U * 1024U},
        {128U, 2U * 1024U},
        {256U, 1U * 1024U},
        {512U, 512U},
        {1U * 1024U, 256U},
        {2U * 1024U, 128U},
        {4U * 1024U, 64U},
        {8U * 1024U, 32U},
        {16U * 1024U, 16U},
        {32U * 1024U, 8U},
        {64U * 1024U, 4U},
        {128U * 1024U, 2U},
        {256U * 1024U, 1U},
        {512U * 1024U, 1U},
        {1U * 1024U * 1024U, 0U},
        {4U * 1024U * 1024U, 0U},
        {8U * 1024U * 1024U, 0U},
        {16U * 1024U * 1024U, 0U},
    },
    // L2 ~ 8.5 MiB.
    {
        {32U, 16U * 1024U},
        {64U, 8U * 1024U},
        {128U, 4U * 1024U},
        {256U, 2U * 1024U},
        {512U, 1U * 1024U},
        {1U * 1024U, 512U},
        {2U * 1024U, 256U},
        {4U * 1024U, 128U},
        {8U * 1024U, 64U},
        {16U * 1024U, 32U},
        {32U * 1024U, 16U},
        {64U * 1024U, 8U},
        {128U * 1024U, 4U},
        {256U * 1024U, 2U},
        {512U * 1024U, 1U},
        {1U * 1024U * 1024U, 1U},
        {4U * 1024U * 1024U, 0U},
        {8U * 1024U * 1024U, 0U},
        {16U * 1024U * 1024U, 0U},
    },
    // L3 ~ 16 MiB. (Default)
    {
        {32U, 32U * 1024U},
        {64U, 16U * 1024U},
        {128U, 8U * 1024U},
        {256U, 4U * 1024U},
        {512U, 2U * 1024U},
        {1U * 1024U, 1U * 1024U},
        {2U * 1024U, 512U},
        {4U * 1024U, 256U},
        {8U * 1024U, 128U},
        {16U * 1024U, 64U},
        {32U * 1024U, 32U},
        {64U * 1024U, 16U},
        {128U * 1024U, 8U},
        {256U * 1024U, 4U},
        {512U * 1024U, 2U},
        {1U * 1024U * 1024U, 1U},
        {4U * 1024U * 1024U, 0U},
        {8U * 1024U * 1024U, 0U},
        {16U * 1024U * 1024U, 0U},
    },
    // L4 ~ 42 MiB.
    {
        {32U, 64U * 1024U},
        {64U, 32U * 1024U},
        {128U, 16U * 1024U},
        {256U, 8U * 1024U},
        {512U, 4U * 1024U},
        {1U * 1024U, 2U * 1024U},
        {2U * 1024U, 1U * 1024U},
        {4U * 1024U, 512U},
        {8U * 1024U, 256U},
        {16U * 1024U, 128U},
        {32U * 1024U, 64U},
        {64U * 1024U, 32U},
        {128U * 1024U, 16U},
        {256U * 1024U, 8U},
        {512U * 1024U, 4U},
        {1U * 1024U * 1024U, 4U},
        {4U * 1024U * 1024U, 2U},
        {8U * 1024U * 1024U, 0U},
        {16U * 1024U * 1024U, 0U},
    },
    // L5 ~ 92 MiB.
    {
        {32U, 128U * 1024U},
        {64U, 64U * 1024U},
        {128U, 32U * 1024U},
        {256U, 16U * 1024U},
        {512U, 8U * 1024U},
        {1U * 1024U, 4U * 1024U},
        {2U * 1024U, 2U * 1024U},
        {4U * 1024U, 1U * 1024U},
        {8U * 1024U, 512U},
        {16U * 1024U, 256U},
        {32U * 1024U, 128U},
        {64U * 1024U, 64U},
        {128U * 1024U, 32U},
        {256U * 1024U, 16U},
        {512U * 1024U, 8U},
        {1U * 1024U * 1024U, 8U},
        {4U * 1024U * 1024U, 4U},
        {8U * 1024U * 1024U, 1U},
        {16U * 1024U * 1024U, 0U},
    },
    // L6 ~ 200 MiB.
    {
        {32U, 256U * 1024U},
        {64U, 128U * 1024U},
        {128U, 64U * 1024U},
        {256U, 32U * 1024U},
        {512U, 16U * 1024U},
        {1U * 1024U, 8U * 1024U},
        {2U * 1024U, 4U * 1024U},
        {4U * 1024U, 2U * 1024U},
        {8U * 1024U, 1U * 1024U},
        {16U * 1024U, 512U},
        {32U * 1024U, 256U},
        {64U * 1024U, 128U},
        {128U * 1024U, 64U},
        {256U * 1024U, 32U},
        {512U * 1024U, 16U},
        {1U * 1024U * 1024U, 16U},
        {4U * 1024U * 1024U, 8U},
        {8U * 1024U * 1024U, 2U},
        {16U * 1024U * 1024U, 1U},
    },
    // L7 ~ 264 MiB.
    {
        {32U, 256U * 1024U},
        {64U, 128U * 1024U},
        {128U, 64U * 1024U},
        {256U, 32U * 1024U},
        {512U, 16U * 1024U},
        {1U * 1024U, 8U * 1024U},
        {2U * 1024U, 4U * 1024U},
        {4U * 1024U, 2U * 1024U},
        {8U * 1024U, 1U * 1024U},
        {16U * 1024U, 512U},
        {32U * 1024U, 256U},
        {64U * 1024U, 128U},
        {128U * 1024U, 64U},
        {256U * 1024U, 32U},
        {512U * 1024U, 16U},
        {1U * 1024U * 1024U, 16U},
        {4U * 1024U * 1024U, 16U},
        {8U * 1024U * 1024U, 4U},
        {16U * 1024U * 1024U, 2U},
    },
    // L8 ~ 528 MiB.
    {
        {32U, 512U * 1024U},
        {64U, 256U * 1024U},
        {128U, 128U * 1024U},
        {256U, 64U * 1024U},
        {512U, 32U * 1024U},
        {1U * 1024U, 16U * 1024U},
        {2U * 1024U, 8U * 1024U},
        {4U * 1024U, 4U * 1024U},
        {8U * 1024U, 2U * 1024U},
        {16U * 1024U, 1U * 1024U},
        {32U * 1024U, 512U},
        {64U * 1024U, 256U},
        {128U * 1024U, 128U},
        {256U * 1024U, 64U},
        {512U * 1024U, 32U},
        {1U * 1024U * 1024U, 32U},
        {4U * 1024U * 1024U, 32U},
        {8U * 1024U * 1024U, 8U},
        {16U * 1024U * 1024U, 4U},
    },
    // L9 ~ 656 MiB.
    {
        {32U, 512U * 1024U},
        {64U, 256U * 1024U},
        {128U, 128U * 1024U},
        {256U, 64U * 1024U},
        {512U, 32U * 1024U},
        {1U * 1024U, 16U * 1024U},
        {2U * 1024U, 8U * 1024U},
        {4U * 1024U, 4U * 1024U},
        {8U * 1024U, 2U * 1024U},
        {16U * 1024U, 1U * 1024U},
        {32U * 1024U, 512U},
        {64U * 1024U, 256U},
        {128U * 1024U, 128U},
        {256U * 1024U, 64U},
        {512U * 1024U, 32U},
        {1U * 1024U * 1024U, 32U},
        {4U * 1024U * 1024U, 32U},
        {8U * 1024U * 1024U, 16U},
        {16U * 1024U * 1024U, 8U},
    }
};
// clang-format on

struct MemoryFreeNode final {
  MemoryFreeNode* next{nullptr};
};

struct MemoryChunk final {
  void* ptr{nullptr};
  size_t bytes{0};
};

struct alignas(64) MemoryTierShard final {
  SpinLock mtx;
  std::atomic<MemoryFreeNode*> free_list_head{nullptr};
  std::atomic<uint64_t> hit_count{0};
  std::atomic<uint64_t> deallocate_count{0};
};

struct MemoryChunkTally final {
  MemoryChunk chunk;
  size_t free_nodes{0};
};

// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct alignas(64) MemoryTierState final {
  size_t max_size{0};
  size_t block_size{0};
  size_t blocks_per_chunk{0};
  size_t next_chunk_blocks{0};
  size_t initial_chunk_blocks{0};
  size_t lazy_chunk_blocks{0};
  size_t batch_size{kDefaultBatchSize};

  MemoryTierShard* shards{nullptr};
  size_t shard_count{0};
  std::vector<MemoryChunk> chunks;
  std::mutex grow_mtx;
  std::mutex clear_mtx;
  std::atomic<bool> sharded{false};
  std::atomic<uint32_t> contention_count{0U};

  std::atomic<uint64_t> chunk_count{0};
  std::atomic<uint64_t> upstream_alloc_count{0};
  std::atomic<uint64_t> upstream_alloc_bytes{0};
};

struct alignas(64) MemoryOversizedCounters final {
  std::atomic<uint64_t> alloc_count{0};
  std::atomic<uint64_t> alloc_bytes{0};
  std::atomic<uint64_t> dealloc_count{0};
};

static constexpr bool is_power_of_two(size_t x) noexcept { return x != 0 && ((x & (x - 1U)) == 0U); }

static constexpr size_t round_up(size_t value, size_t alignment) noexcept {
  return (value + alignment - 1U) & ~(alignment - 1U);
}

static constexpr bool default_tier_table_well_formed() noexcept {
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (size_t level = 0; level < kMaxLevelCount; ++level) {
    size_t prev_max_size = 0U;

    for (size_t t = 0; t < kMaxTierCount; ++t) {
      const size_t max_size = kDefaultTierTable[level][t].max_size;

      if (max_size == 0U) {
        break;
      }

      if (max_size < sizeof(MemoryFreeNode)) {
        return false;
      }

      if (t > 0U && max_size <= prev_max_size) {
        return false;
      }

      prev_max_size = max_size;
    }
  }

  return true;
}

static_assert(default_tier_table_well_formed(),
              "MemoryPool: kDefaultTierTable contains a malformed row "
              "(undersized tier or non-monotonic max_size)");

static std::atomic<size_t> next_thread_slot{0U};
static thread_local size_t current_thread_slot_plus_one = 0U;

static size_t current_thread_slot() noexcept {
  if VUNLIKELY (current_thread_slot_plus_one == 0U) {
    current_thread_slot_plus_one = next_thread_slot.fetch_add(1U, std::memory_order_relaxed) + 1U;
  }

  return current_thread_slot_plus_one - 1U;
}

static size_t current_tier_shard(const MemoryTierState& state) noexcept {
  return current_thread_slot() & (state.shard_count - 1U);
}

static size_t default_tier_shard_count() noexcept {
  const size_t threads = std::thread::hardware_concurrency();
  size_t count = kMinTierShardCount;

  while (count < threads && count < kMaxTierShardCount) {
    count *= 2U;
  }

  return count;
}

static MemoryFreeNode* head_of(const MemoryTierShard& shard) noexcept {
  return shard.free_list_head.load(std::memory_order_relaxed);
}

static void set_head(MemoryTierShard& shard, MemoryFreeNode* head) noexcept {
  shard.free_list_head.store(head, std::memory_order_relaxed);
}

static void bump_counter(std::atomic<uint64_t>& counter) noexcept {
  counter.store(counter.load(std::memory_order_relaxed) + 1U, std::memory_order_relaxed);
}

static MemoryFreeNode* pop_free_node(MemoryTierShard& shard) noexcept {
  SpinLockGuard lock(shard.mtx);
  MemoryFreeNode* node = head_of(shard);

  if (node == nullptr) {
    return nullptr;
  }

  set_head(shard, node->next);
  bump_counter(shard.hit_count);

  return node;
}

static MemoryFreeNode* steal_free_nodes(MemoryTierState& state, size_t target_index) noexcept {
  MemoryTierShard& target = state.shards[target_index];

  for (size_t offset = 1U; offset < state.shard_count; ++offset) {
    MemoryTierShard& source = state.shards[(target_index + offset) & (state.shard_count - 1U)];
    MemoryFreeNode* first = nullptr;
    MemoryFreeNode* last = nullptr;

    if (head_of(source) == nullptr) {
      continue;
    }

    {
      SpinLockGuard source_lock(source.mtx);
      first = head_of(source);

      if (first == nullptr) {
        continue;
      }

      last = first;

      size_t count = 1U;

      while (count < state.batch_size && last->next != nullptr) {
        last = last->next;
        ++count;
      }

      set_head(source, last->next);
      last->next = nullptr;
      bump_counter(source.hit_count);
    }

    MemoryFreeNode* cached = first->next;
    first->next = nullptr;

    if (cached != nullptr) {
      SpinLockGuard target_lock(target.mtx);
      last->next = head_of(target);
      set_head(target, cached);
    }

    return first;
  }

  return nullptr;
}

static MemoryFreeNode* try_allocate_from_shards(MemoryTierState& state, size_t shard_index) noexcept {
  MemoryFreeNode* node = pop_free_node(state.shards[shard_index]);

  if VLIKELY (node != nullptr) {
    return node;
  }

  if (!state.sharded.load(std::memory_order_relaxed)) {
    return nullptr;
  }

  return steal_free_nodes(state, shard_index);
}

// state.grow_mtx must be held.  When allocated is non-null, one node is removed from the new chunk.
static bool grow_tier_chunk(MemoryTierState& state, size_t shard_index, MemoryFreeNode** allocated) noexcept {
  size_t blocks = state.next_chunk_blocks;

  if VUNLIKELY (blocks > state.blocks_per_chunk) {
    blocks = state.blocks_per_chunk;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if (allocated != nullptr && blocks > state.lazy_chunk_blocks) {
    blocks = state.lazy_chunk_blocks;
  }

  const size_t block_size = state.block_size;
  const size_t chunk_bytes = block_size * blocks;

  if VUNLIKELY (chunk_bytes / block_size != blocks) {
    return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  void* ptr = ::operator new(chunk_bytes, std::align_val_t{MemoryPool::kBlockAlignment}, std::nothrow);

  if VUNLIKELY (ptr == nullptr) {
    return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  auto* base = static_cast<std::byte*>(ptr);
  auto* local_tail = ::new (base + (blocks - 1U) * block_size) MemoryFreeNode{nullptr};
  MemoryFreeNode* local_head = local_tail;

  for (size_t i = blocks - 1U; i > 0; --i) {
    local_head = ::new (base + (i - 1U) * block_size) MemoryFreeNode{local_head};
  }

  try {
    state.chunks.push_back(MemoryChunk{ptr, chunk_bytes});
  } catch (std::exception&) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    ::operator delete(ptr, chunk_bytes, std::align_val_t{MemoryPool::kBlockAlignment});
    return false;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  state.upstream_alloc_count.fetch_add(1, std::memory_order_relaxed);
  state.upstream_alloc_bytes.fetch_add(chunk_bytes, std::memory_order_relaxed);
  state.chunk_count.fetch_add(1, std::memory_order_relaxed);

  {
    MemoryTierShard& shard = state.shards[shard_index];
    SpinLockGuard lock(shard.mtx);

    local_tail->next = head_of(shard);

    if (allocated != nullptr) {
      *allocated = local_head;
      local_head = local_head->next;
      (*allocated)->next = nullptr;
      bump_counter(shard.hit_count);
    }

    set_head(shard, local_head);
  }

  const size_t doubled = blocks * 2U;
  const size_t target = (doubled < blocks || doubled > state.blocks_per_chunk)
                            ? state.blocks_per_chunk
                            : doubled;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

  if (target > state.next_chunk_blocks) {
    state.next_chunk_blocks = target;
  }

  return true;
}

static void* tier_allocate(MemoryTierState& state) noexcept {
  size_t shard_index = 0U;
  bool sharded = state.sharded.load(std::memory_order_relaxed);

  if (!sharded) {
    MemoryTierShard& primary = state.shards[0];

    if (primary.mtx.try_lock()) {
      MemoryFreeNode* node = head_of(primary);

      if VLIKELY (node != nullptr) {
        set_head(primary, node->next);
        bump_counter(primary.hit_count);
      }

      primary.mtx.unlock();

      if VLIKELY (node != nullptr) {
        return node;
      }
    } else {
      const uint32_t contentions = state.contention_count.fetch_add(1U, std::memory_order_relaxed) + 1U;

      if (contentions >= kShardingContentionThreshold) {
        state.sharded.store(true, std::memory_order_relaxed);
        sharded = true;
      }
    }
  }

  if (sharded) {
    shard_index = current_tier_shard(state);
  }

  MemoryFreeNode* node = try_allocate_from_shards(state, shard_index);

  if VLIKELY (node != nullptr) {
    return node;
  }

  {
    std::lock_guard grow_lock(state.grow_mtx);

    node = try_allocate_from_shards(state, shard_index);

    if VLIKELY (node != nullptr) {
      return node;
    }

    if VLIKELY (grow_tier_chunk(state, shard_index, &node)) {
      return node;
    }
  }

  std::lock_guard clear_lock(state.clear_mtx);

  node = try_allocate_from_shards(state, shard_index);

  if (node != nullptr) {
    return node;
  }

  std::lock_guard grow_lock(state.grow_mtx);

  return grow_tier_chunk(state, shard_index, &node) ? node : nullptr;
}

static void tier_deallocate(MemoryTierState& state, void* p) noexcept {
  if (!state.sharded.load(std::memory_order_relaxed)) {
    MemoryTierShard& primary = state.shards[0];

    if (primary.mtx.try_lock()) {
      set_head(primary, ::new (p) MemoryFreeNode{head_of(primary)});
      bump_counter(primary.deallocate_count);
      primary.mtx.unlock();

      return;
    }

    const uint32_t contentions = state.contention_count.fetch_add(1U, std::memory_order_relaxed) + 1U;

    if (contentions < kShardingContentionThreshold) {
      SpinLockGuard lock(primary.mtx);
      set_head(primary, ::new (p) MemoryFreeNode{head_of(primary)});
      bump_counter(primary.deallocate_count);

      return;
    }

    state.sharded.store(true, std::memory_order_relaxed);
  }

  MemoryTierShard& shard = state.shards[current_tier_shard(state)];
  SpinLockGuard lock(shard.mtx);

  set_head(shard, ::new (p) MemoryFreeNode{head_of(shard)});
  bump_counter(shard.deallocate_count);
}

static void prealloc_full_quota(MemoryTierState& state) noexcept {
  std::lock_guard grow_lock(state.grow_mtx);

  state.next_chunk_blocks = state.blocks_per_chunk;
  const bool ok = grow_tier_chunk(state, 0U, nullptr);

  if VUNLIKELY (!ok) {
    state.next_chunk_blocks = state.initial_chunk_blocks;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if VUNLIKELY (!ok) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    MEMORY_POOL_LOG(
        "MemoryPool: prealloc failed for tier (max_size=%zu, blocks_per_chunk=%zu); "
        "tier reverts to lazy growth.",
        state.max_size, state.blocks_per_chunk);
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }
}

static bool validate_tiers_log(const std::vector<MemoryPool::Tier>& tiers) noexcept {
  static constexpr size_t kMaxTierSize = SIZE_MAX - MemoryPool::kBlockAlignment + 1U;

  if VUNLIKELY (tiers.size() > kMaxTierCount) {
    MEMORY_POOL_LOG("MemoryPool: tier count %zu exceeds max %zu; falling back to default pyramid.", tiers.size(),
                    kMaxTierCount);

    return false;
  }

  for (size_t i = 0; i < tiers.size(); ++i) {
    if VUNLIKELY (tiers[i].max_size == 0) {
      MEMORY_POOL_LOG("MemoryPool: tier %zu has max_size == 0; falling back to default pyramid.", i);

      return false;
    }

    if VUNLIKELY (tiers[i].max_size < sizeof(MemoryFreeNode)) {
      MEMORY_POOL_LOG(
          "MemoryPool: tier %zu max_size (%zu) is below the minimum block size %zu; "
          "falling back to default pyramid.",
          i, tiers[i].max_size, sizeof(MemoryFreeNode));

      return false;
    }

    if VUNLIKELY (tiers[i].max_size > kMaxTierSize) {
      MEMORY_POOL_LOG("MemoryPool: tier %zu max_size overflows after alignment rounding; falling back.", i);

      return false;
    }

    if VUNLIKELY (i > 0 && tiers[i].max_size <= tiers[i - 1].max_size) {
      MEMORY_POOL_LOG("MemoryPool: tier %zu max_size is not strictly increasing; falling back to default pyramid.", i);

      return false;
    }
  }

  return true;
}

static MemoryPool::Config create_memory_config(int level, bool prealloc) {
  if VUNLIKELY (level < kMinMemoryLevel || level > kMaxMemoryLevel) {
    MEMORY_POOL_LOG("MemoryPool: level %d out of range [%d, %d], clamped.", level, kMinMemoryLevel, kMaxMemoryLevel);

    level = (level < kMinMemoryLevel) ? kMinMemoryLevel : kMaxMemoryLevel;
  }

  const auto row_index = static_cast<size_t>(level - kMinMemoryLevel);
  const auto& row = kDefaultTierTable[row_index];

  MemoryPool::Config config;
  config.prealloc = prealloc;
  config.tiers.reserve(kMaxTierCount);

  for (size_t i = 0; i < kMaxTierCount && row[i].max_size != 0; ++i) {
    config.tiers.emplace_back(row[i]);
  }

  return config;
}  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

struct MemoryPool::Impl final {  // NOLINT(clang-analyzer-optin.performance.Padding)
  alignas(64) size_t dispatch_max_sizes[kMaxTierCount]{};
  MemoryTierState* dispatch_states[kMaxTierCount]{};
  size_t dispatch_count{0};

  MemoryTierState* tier_states[kMaxTierCount]{};
  size_t tier_count{0};
  std::unique_ptr<MemoryTierShard[]> shards;
  std::vector<std::unique_ptr<MemoryTierState>> owned_states;

  std::unique_ptr<MemoryOversizedCounters[]> oversized;
  size_t oversized_count{0};
};

MemoryPool::MemoryPool() : MemoryPool(Config{}) {}

MemoryPool::MemoryPool(int level, bool prealloc) : MemoryPool(create_memory_config(level, prealloc)) {}

MemoryPool::MemoryPool(const Config& config) : impl_(std::make_unique<Impl>()) {
  const size_t shard_count = default_tier_shard_count();

  impl_->oversized = std::make_unique<MemoryOversizedCounters[]>(shard_count);
  impl_->oversized_count = shard_count;

  if (config.tiers.empty()) {
    impl_->tier_count = 0;
    return;
  }

  std::vector<Tier> fallback;
  const bool use_caller = validate_tiers_log(config.tiers);

  if VUNLIKELY (!use_caller) {
    const auto& row = kDefaultTierTable[kDefaultMemoryLevel - kMinMemoryLevel];
    fallback.assign(row, row + kMaxTierCount);
  }

  const std::vector<Tier>& source = use_caller ? config.tiers : fallback;
  const size_t batch_size = config.batch_size == 0U ? kDefaultBatchSize : config.batch_size;

  if VUNLIKELY (config.batch_size == 0U) {
    MEMORY_POOL_LOG("MemoryPool: batch_size is 0; fallback to %zu.", kDefaultBatchSize);
  }

  impl_->owned_states.reserve(source.size());

  size_t managed = 0;

  for (const auto& cfg : source) {
    if (cfg.max_size != 0U && cfg.blocks_per_chunk != 0U) {
      ++managed;
    }
  }

  impl_->shards = std::make_unique<MemoryTierShard[]>(managed * shard_count);

  size_t live = 0;
  size_t dispatch = 0;

  for (const auto& cfg : source) {
    if VUNLIKELY (cfg.max_size == 0U) {
      continue;
    }

    impl_->dispatch_max_sizes[dispatch] = cfg.max_size;

    if VUNLIKELY (cfg.blocks_per_chunk == 0U) {
      impl_->dispatch_states[dispatch] = nullptr;
      ++dispatch;
      continue;
    }

    auto state = std::make_unique<MemoryTierState>();
    state->max_size = cfg.max_size;
    state->blocks_per_chunk = cfg.blocks_per_chunk;
    state->batch_size = batch_size;
    state->shards = impl_->shards.get() + live * shard_count;
    state->shard_count = shard_count;
    state->chunks.reserve(kInitialChunksReserve);
    state->block_size = round_up(cfg.max_size, kBlockAlignment);

    size_t initial = (state->block_size > 0U) ? (kInitialChunkBytesTarget / state->block_size) : kInitialBlocksPerChunk;

    if (initial < kInitialBlocksPerChunk) {
      initial = kInitialBlocksPerChunk;
    }

    if (initial > state->blocks_per_chunk) {
      initial = state->blocks_per_chunk;
    }

    state->initial_chunk_blocks = initial;
    state->next_chunk_blocks = initial;

    const size_t lazy_blocks = config.lazy_scale ? std::max(state->blocks_per_chunk / kLazyChunkQuotaDivisor,
                                                            kMinLazyChunkBytes / state->block_size)
                                                 : kMaxLazyChunkBytes / state->block_size;

    state->lazy_chunk_blocks = std::clamp(lazy_blocks, size_t{1}, state->blocks_per_chunk);

    impl_->tier_states[live] = state.get();
    impl_->dispatch_states[dispatch] = state.get();
    impl_->owned_states.emplace_back(std::move(state));

    ++live;
    ++dispatch;
  }

  impl_->dispatch_count = (live == 0U) ? 0U : dispatch;
  impl_->tier_count = live;

  if (config.prealloc) {
    for (auto& state : impl_->owned_states) {
      prealloc_full_quota(*state);
    }
  }
}

MemoryPool::~MemoryPool() {
#ifdef _WIN32
  if (Utils::is_terminating()) {
    (void)impl_.release();
    return;
  }
#endif

  for (auto& state : impl_->owned_states) {
    for (const MemoryChunk& chunk : state->chunks) {
      ::operator delete(chunk.ptr, chunk.bytes, std::align_val_t{kBlockAlignment});
    }

    state->chunks.clear();

    for (size_t index = 0U; index < state->shard_count; ++index) {
      set_head(state->shards[index], nullptr);
    }
  }
}

void* MemoryPool::allocate(size_t bytes, size_t alignment) noexcept {
  if VUNLIKELY (!is_power_of_two(alignment)) {
    MEMORY_POOL_LOG("MemoryPool::allocate: alignment %zu is not a power of two; returning nullptr.", alignment);

    return nullptr;
  }

  const size_t idx = find_tier(bytes);

  if VUNLIKELY (idx == kMaxTierCount || alignment > kBlockAlignment || impl_->dispatch_states[idx] == nullptr) {
    void* p = (alignment > kBlockAlignment) ? ::operator new(bytes, std::align_val_t{alignment}, std::nothrow)
                                            : ::operator new(bytes, std::nothrow);

    if VUNLIKELY (p == nullptr) {
      return nullptr;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    MemoryOversizedCounters& counters = impl_->oversized[current_thread_slot() & (impl_->oversized_count - 1U)];
    counters.alloc_count.fetch_add(1, std::memory_order_relaxed);
    counters.alloc_bytes.fetch_add(bytes, std::memory_order_relaxed);

    return p;
  }

  return tier_allocate(*impl_->dispatch_states[idx]);
}

void MemoryPool::deallocate(void* p, size_t bytes, size_t alignment) noexcept {
  if VUNLIKELY (!is_power_of_two(alignment)) {
    MEMORY_POOL_LOG("MemoryPool::deallocate: alignment %zu is not a power of two; leaking %p.", alignment, p);

    return;
  }

  if VUNLIKELY (p == nullptr) {
    return;
  }

  const size_t idx = find_tier(bytes);

  if VUNLIKELY (idx == kMaxTierCount || alignment > kBlockAlignment || impl_->dispatch_states[idx] == nullptr) {
    if (alignment > kBlockAlignment) {
      ::operator delete(p, bytes, std::align_val_t{alignment});
    } else {
      ::operator delete(p, bytes);
    }

    impl_->oversized[current_thread_slot() & (impl_->oversized_count - 1U)].dealloc_count.fetch_add(
        1, std::memory_order_relaxed);

    return;
  }

  tier_deallocate(*impl_->dispatch_states[idx], p);
}

size_t MemoryPool::get_tier_count() const noexcept { return impl_->tier_count; }

std::vector<MemoryPool::TierStats> MemoryPool::get_stats() const noexcept {
  const size_t count = impl_->tier_count;

  std::vector<TierStats> result;

  try {
    result.reserve(count);
  } catch (...) {  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return {};     // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

  for (size_t i = 0; i < count; ++i) {
    const MemoryTierState& state = *impl_->tier_states[i];
    uint64_t hits = 0U;
    uint64_t deallocs = 0U;

    for (size_t index = 0U; index < state.shard_count; ++index) {
      hits += state.shards[index].hit_count.load(std::memory_order_relaxed);
      deallocs += state.shards[index].deallocate_count.load(std::memory_order_relaxed);
    }

    TierStats item;
    item.max_size = state.max_size;
    item.blocks_per_chunk = state.blocks_per_chunk;
    item.block_size = state.block_size;
    item.hit_count = hits;
    item.deallocate_count = deallocs;
    item.in_use_blocks = (hits >= deallocs) ? (hits - deallocs) : 0U;
    item.upstream_alloc_count = state.upstream_alloc_count.load(std::memory_order_relaxed);
    item.upstream_alloc_bytes = state.upstream_alloc_bytes.load(std::memory_order_relaxed);
    item.chunk_count = state.chunk_count.load(std::memory_order_relaxed);

    result.emplace_back(item);
  }

  return result;
}

MemoryPool::OversizedStats MemoryPool::get_oversized_stats() const noexcept {
  OversizedStats result;

  for (size_t i = 0; i < impl_->oversized_count; ++i) {
    const MemoryOversizedCounters& counters = impl_->oversized[i];

    result.alloc_count += counters.alloc_count.load(std::memory_order_relaxed);
    result.alloc_bytes += counters.alloc_bytes.load(std::memory_order_relaxed);
    result.dealloc_count += counters.dealloc_count.load(std::memory_order_relaxed);
  }

  return result;
}

void MemoryPool::reset_stats() noexcept {
  const size_t count = impl_->tier_count;

  for (size_t i = 0; i < count; ++i) {
    MemoryTierState& state = *impl_->tier_states[i];

    for (size_t index = 0U; index < state.shard_count; ++index) {
      MemoryTierShard& shard = state.shards[index];
      SpinLockGuard lock(shard.mtx);

      shard.hit_count.store(0, std::memory_order_relaxed);
      shard.deallocate_count.store(0, std::memory_order_relaxed);
    }
  }

  for (size_t i = 0; i < impl_->oversized_count; ++i) {
    MemoryOversizedCounters& counters = impl_->oversized[i];

    counters.alloc_count.store(0, std::memory_order_relaxed);
    counters.alloc_bytes.store(0, std::memory_order_relaxed);
    counters.dealloc_count.store(0, std::memory_order_relaxed);
  }
}

static MemoryFreeNode* detach_free_list(MemoryTierShard& shard) noexcept {
  SpinLockGuard lock(shard.mtx);

  MemoryFreeNode* head = head_of(shard);
  set_head(shard, nullptr);

  return head;
}

static void attach_free_list(MemoryTierShard& shard, MemoryFreeNode* head, MemoryFreeNode* tail) noexcept {
  if (head == nullptr) {
    return;
  }

  SpinLockGuard lock(shard.mtx);

  tail->next = head_of(shard);
  set_head(shard, head);
}

void MemoryPool::clear() noexcept {
  for (auto& state : impl_->owned_states) {
    std::lock_guard clear_lock(state->clear_mtx);
    std::vector<MemoryChunkTally> tally;

    try {
      std::lock_guard grow_lock(state->grow_mtx);

      std::sort(state->chunks.begin(), state->chunks.end(), [](const MemoryChunk& a, const MemoryChunk& b) noexcept {
        return reinterpret_cast<std::uintptr_t>(a.ptr) < reinterpret_cast<std::uintptr_t>(b.ptr);
      });

      tally.reserve(state->chunks.size());

      for (const MemoryChunk& chunk : state->chunks) {
        tally.push_back(MemoryChunkTally{chunk, 0U});
      }
    } catch (std::exception&) {
      continue;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if (tally.empty()) {
      continue;
    }

    MemoryFreeNode* heads[kMaxTierShardCount] = {};

    for (size_t s = 0U; s < state->shard_count; ++s) {
      heads[s] = detach_free_list(state->shards[s]);
    }

    const size_t block_size = state->block_size;

    const auto find_tally = [&tally](const void* p) noexcept -> MemoryChunkTally* {
      const auto addr = reinterpret_cast<std::uintptr_t>(p);
      size_t lo = 0;
      size_t hi = tally.size();

      while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2U;
        const auto cs = reinterpret_cast<std::uintptr_t>(tally[mid].chunk.ptr);
        const auto ce = cs + tally[mid].chunk.bytes;

        if (addr < cs) {
          hi = mid;
        } else if (addr >= ce) {
          lo = mid + 1U;
        } else {
          return &tally[mid];
        }
      }

      return nullptr;
    };

    const auto is_free = [block_size](const MemoryChunkTally* t) noexcept {
      return t != nullptr && t->free_nodes == t->chunk.bytes / block_size;
    };

    for (size_t s = 0U; s < state->shard_count; ++s) {
      for (MemoryFreeNode* node = heads[s]; node != nullptr; node = node->next) {
        MemoryChunkTally* t = find_tally(node);

        if VLIKELY (t != nullptr) {
          ++t->free_nodes;
        }
      }
    }

    for (size_t s = 0U; s < state->shard_count; ++s) {
      MemoryFreeNode* kept_head = nullptr;
      MemoryFreeNode* kept_tail = nullptr;
      MemoryFreeNode* current = heads[s];

      while (current != nullptr) {
        MemoryFreeNode* next = current->next;

        if (!is_free(find_tally(current))) {
          current->next = kept_head;
          kept_head = current;

          if (kept_tail == nullptr) {
            kept_tail = current;
          }
        }

        current = next;
      }

      attach_free_list(state->shards[s], kept_head, kept_tail);
    }

    size_t released = 0U;

    {
      std::lock_guard grow_lock(state->grow_mtx);
      std::vector<MemoryChunk>& live = state->chunks;
      size_t write = 0U;

      for (size_t read = 0U; read < live.size(); ++read) {
        if (read < tally.size() && is_free(&tally[read])) {
          ++released;
          continue;
        }

        live[write] = live[read];
        ++write;
      }

      live.resize(write);
      state->chunk_count.fetch_sub(released, std::memory_order_relaxed);
    }

    for (const MemoryChunkTally& t : tally) {
      if (is_free(&t)) {
        ::operator delete(t.chunk.ptr, t.chunk.bytes, std::align_val_t{kBlockAlignment});
      }
    }
  }
}

void MemoryPool::trim() noexcept { clear(); }

MemoryPool::Config MemoryPool::get_default_config() {
  static int level = []() noexcept {
    const std::string env_value = Utils::get_env("VLINK_MEMORY_LEVEL", "3");

    int parsed = kDefaultMemoryLevel;

    const char* first = env_value.data();
    const char* last = first + env_value.size();

    auto [ptr, ec] = std::from_chars(first, last, parsed);

    if VUNLIKELY (ec != std::errc() || ptr != last) {
      // LCOV_EXCL_START GCOVR_EXCL_START
      MEMORY_POOL_LOG("MemoryPool: VLINK_MEMORY_LEVEL=\"%s\" is not a valid integer, fallback to %d.",
                      env_value.c_str(), kDefaultMemoryLevel);

      return kDefaultMemoryLevel;
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP
    }

    if VUNLIKELY (parsed < kMinMemoryLevel || parsed > kMaxMemoryLevel) {
      // LCOV_EXCL_START GCOVR_EXCL_START
      MEMORY_POOL_LOG("MemoryPool: VLINK_MEMORY_LEVEL=%d out of range [%d, %d], clamped.", parsed, kMinMemoryLevel,
                      kMaxMemoryLevel);

      return parsed < kMinMemoryLevel ? kMinMemoryLevel : kMaxMemoryLevel;
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP
    }

    return parsed;
  }();

  static bool prealloc_env = (Utils::get_env("VLINK_MEMORY_PREALLOC") == "1");
  static bool lazy_scale_env = (Utils::get_env("VLINK_MEMORY_LAZY_SCALE") == "1");

  static size_t batch_size = []() noexcept {
    const std::string env_value = Utils::get_env("VLINK_MEMORY_BATCH_SIZE", "16");
    size_t parsed = kDefaultBatchSize;

    const char* first = env_value.data();
    const char* last = first + env_value.size();
    auto [ptr, ec] = std::from_chars(first, last, parsed);

    if VUNLIKELY (ec != std::errc() || ptr != last || parsed == 0U) {
      // LCOV_EXCL_START GCOVR_EXCL_START
      MEMORY_POOL_LOG("MemoryPool: VLINK_MEMORY_BATCH_SIZE=\"%s\" is not a positive integer, fallback to %zu.",
                      env_value.c_str(), kDefaultBatchSize);

      return kDefaultBatchSize;
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP
    }

    return parsed;
  }();

  Config config = create_memory_config(level, prealloc_env);
  config.batch_size = batch_size;
  config.lazy_scale = lazy_scale_env;

  return config;
}

MemoryPool& MemoryPool::global_instance(bool use_env_level) {
#if MEMORY_POOL_NEVER_DELETE
  alignas(MemoryPool) static char buf[sizeof(MemoryPool)];

  static auto* instance =
      new (buf) MemoryPool(use_env_level ? get_default_config() : create_memory_config(kDefaultMemoryLevel, false));

  return *instance;
#else
  static MemoryPool instance(use_env_level ? get_default_config() : create_memory_config(kDefaultMemoryLevel, false));

  return instance;
#endif
}

size_t MemoryPool::find_tier(size_t bytes) const noexcept {
  const size_t count = impl_->dispatch_count;
  const size_t* const sizes = impl_->dispatch_max_sizes;

  for (size_t i = 0; i < count; ++i) {
    if (bytes <= sizes[i]) {
      return i;
    }
  }

  return kMaxTierCount;
}

}  // namespace vlink
