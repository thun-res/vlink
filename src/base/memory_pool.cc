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
#include <exception>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "./base/logger.h"
#include "./base/spin_lock.h"
#include "./base/utils.h"

#define MEMORY_POOL_NERVER_DELETE 0

namespace vlink {

static constexpr int kMinMemoryLevel = 0;
static constexpr int kMaxMemoryLevel = 9;
static constexpr int kDefaultMemoryLevel = 3;
static constexpr size_t kMaxTierCount = 20U;
static constexpr size_t kMaxLevelCount = 10U;
static constexpr size_t kInitialBlocksPerChunk = 1U;
static constexpr size_t kInitialChunksReserve = 16U;
static constexpr size_t kInitialChunkBytesTarget = 64U * 1024U;

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

// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct alignas(64) MemoryTierState final {
  size_t max_size{0};
  size_t block_size{0};
  size_t blocks_per_chunk{0};
  size_t next_chunk_blocks{0};
  size_t initial_chunk_blocks{0};

  MemoryFreeNode* free_list_head{nullptr};
  std::vector<MemoryChunk> chunks;

  alignas(64) std::atomic<bool> growing{false};
  alignas(64) SpinLock mtx;
  alignas(64) std::atomic<uint64_t> hit_count{0};
  alignas(64) std::atomic<uint64_t> deallocate_count{0};

  alignas(64) std::atomic<uint64_t> chunk_count{0};
  alignas(64) std::atomic<uint64_t> upstream_alloc_count{0};
  std::atomic<uint64_t> upstream_alloc_bytes{0};
};

struct alignas(64) MemoryAllocCounters final {
  std::atomic<uint64_t> count{0};
  std::atomic<uint64_t> bytes{0};
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

static bool grow_tier_chunk(MemoryTierState& state) noexcept {
  size_t blocks = state.next_chunk_blocks;

  if VUNLIKELY (blocks > state.blocks_per_chunk) {
    blocks = state.blocks_per_chunk;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  const size_t block_size = state.block_size;
  const size_t chunk_bytes = block_size * blocks;

  if VUNLIKELY (chunk_bytes / block_size != blocks) {
    return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  state.mtx.unlock();

  void* ptr = ::operator new(chunk_bytes, std::align_val_t{MemoryPool::kBlockAlignment}, std::nothrow);

  if VUNLIKELY (ptr == nullptr) {
    state.mtx.lock();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return false;      // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  auto* base = static_cast<std::byte*>(ptr);
  auto* local_tail = ::new (base + (blocks - 1U) * block_size) MemoryFreeNode{nullptr};
  MemoryFreeNode* local_head = local_tail;

  for (size_t i = blocks - 1U; i > 0; --i) {
    local_head = ::new (base + (i - 1U) * block_size) MemoryFreeNode{local_head};
  }

  state.mtx.lock();

  try {
    state.chunks.push_back(MemoryChunk{ptr, chunk_bytes});
  } catch (std::exception&) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    state.mtx.unlock();
    ::operator delete(ptr, chunk_bytes, std::align_val_t{MemoryPool::kBlockAlignment});
    state.mtx.lock();

    return false;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  state.upstream_alloc_count.fetch_add(1, std::memory_order_relaxed);
  state.upstream_alloc_bytes.fetch_add(chunk_bytes, std::memory_order_relaxed);
  state.chunk_count.fetch_add(1, std::memory_order_relaxed);

  local_tail->next = state.free_list_head;
  state.free_list_head = local_head;

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
  for (;;) {
    {
      uint16_t grow_spins = 0;

      while (state.growing.load(std::memory_order_acquire)) {
        if (grow_spins < 128) {
          Utils::yield_cpu();
          ++grow_spins;
        } else {
          std::this_thread::yield();
        }
      }
    }

    state.mtx.lock();

    if VLIKELY (state.free_list_head != nullptr) {
      MemoryFreeNode* node = state.free_list_head;
      state.free_list_head = node->next;
      state.mtx.unlock();

      return node;
    }

    if (state.growing.load(std::memory_order_relaxed)) {
      state.mtx.unlock();
      continue;
    }

    state.growing.store(true, std::memory_order_release);
    const bool ok = grow_tier_chunk(state);
    state.growing.store(false, std::memory_order_release);

    if VUNLIKELY (!ok) {
      // LCOV_EXCL_START GCOVR_EXCL_START
      if (state.free_list_head != nullptr) {
        MemoryFreeNode* node = state.free_list_head;
        state.free_list_head = node->next;
        state.mtx.unlock();

        return node;
      }

      state.mtx.unlock();
      return nullptr;
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP
    }

    MemoryFreeNode* node = state.free_list_head;
    state.free_list_head = node->next;
    state.mtx.unlock();

    return node;
  }
}

static void tier_deallocate(MemoryTierState& state, void* p) noexcept {
  SpinLockGuard lock(state.mtx);

  auto* node = ::new (p) MemoryFreeNode{state.free_list_head};

  state.free_list_head = node;
}

static void prealloc_full_quota(MemoryTierState& state) noexcept {
  state.mtx.lock();

  state.growing.store(true, std::memory_order_release);
  state.next_chunk_blocks = state.blocks_per_chunk;
  const bool ok = grow_tier_chunk(state);

  if VUNLIKELY (!ok) {
    state.next_chunk_blocks = state.initial_chunk_blocks;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  state.growing.store(false, std::memory_order_release);

  state.mtx.unlock();

  if VUNLIKELY (!ok) {
    CLOG_W("MemoryPool: prealloc failed for tier (max_size=%zu, blocks_per_chunk=%zu); tier reverts to lazy growth.",
           state.max_size, state.blocks_per_chunk);
  }
}

static bool validate_tiers_log(const std::vector<MemoryPool::Tier>& tiers) noexcept {
  static constexpr size_t kMaxTierSize = SIZE_MAX - MemoryPool::kBlockAlignment + 1U;

  if VUNLIKELY (tiers.size() > kMaxTierCount) {
    CLOG_E("MemoryPool: tier count %zu exceeds max %zu; falling back to default pyramid.", tiers.size(), kMaxTierCount);
    return false;
  }

  for (size_t i = 0; i < tiers.size(); ++i) {
    if VUNLIKELY (tiers[i].max_size == 0) {
      CLOG_E("MemoryPool: tier %zu has max_size == 0; falling back to default pyramid.", i);
      return false;
    }

    if VUNLIKELY (tiers[i].max_size < sizeof(MemoryFreeNode)) {
      CLOG_E(
          "MemoryPool: tier %zu max_size (%zu) is below the minimum block size %zu; "
          "falling back to default pyramid.",
          i, tiers[i].max_size, sizeof(MemoryFreeNode));
      return false;
    }

    if VUNLIKELY (tiers[i].max_size > kMaxTierSize) {
      CLOG_E("MemoryPool: tier %zu max_size overflows after alignment rounding; falling back.", i);
      return false;
    }

    if VUNLIKELY (i > 0 && tiers[i].max_size <= tiers[i - 1].max_size) {
      CLOG_E("MemoryPool: tier %zu max_size is not strictly increasing; falling back to default pyramid.", i);
      return false;
    }
  }

  return true;
}

static MemoryPool::Config create_memory_config(int level, bool prealloc) {
  if VUNLIKELY (level < kMinMemoryLevel || level > kMaxMemoryLevel) {
    CLOG_W("MemoryPool: level %d out of range [%d, %d], clamped.", level, kMinMemoryLevel, kMaxMemoryLevel);
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

  alignas(64) size_t tier_max_sizes[kMaxTierCount]{};

  MemoryTierState* tier_states[kMaxTierCount]{};
  size_t tier_count{0};
  std::vector<std::unique_ptr<MemoryTierState>> owned_states;
  MemoryAllocCounters oversized_alloc;

  alignas(64) std::atomic<uint64_t> oversized_dealloc_count{0};
};

MemoryPool::MemoryPool() : MemoryPool(Config{}) {}

MemoryPool::MemoryPool(int level, bool prealloc) : MemoryPool(create_memory_config(level, prealloc)) {}

MemoryPool::MemoryPool(const Config& config) : impl_(std::make_unique<Impl>()) {
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

  impl_->owned_states.reserve(source.size());

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

    impl_->tier_max_sizes[live] = cfg.max_size;
    impl_->tier_states[live] = state.get();
    impl_->dispatch_states[dispatch] = state.get();
    impl_->owned_states.emplace_back(std::move(state));

    ++live;
    ++dispatch;
  }

  impl_->dispatch_count = dispatch;
  impl_->tier_count = live;

  if (config.prealloc) {
    for (auto& state : impl_->owned_states) {
      prealloc_full_quota(*state);
    }
  }
}

MemoryPool::~MemoryPool() {
  for (auto& state : impl_->owned_states) {
    // SpinLockGuard lock(state->mtx);

    for (const MemoryChunk& chunk : state->chunks) {
      ::operator delete(chunk.ptr, chunk.bytes, std::align_val_t{kBlockAlignment});
    }

    state->chunks.clear();
    state->free_list_head = nullptr;
  }
}

void* MemoryPool::allocate(size_t bytes, size_t alignment) noexcept {
  if VUNLIKELY (!is_power_of_two(alignment)) {
    CLOG_E("MemoryPool::allocate: alignment %zu is not a power of two; returning nullptr.", alignment);
    return nullptr;
  }

  const size_t idx = find_tier(bytes);

  if VUNLIKELY (idx == kMaxTierCount || alignment > kBlockAlignment || impl_->dispatch_states[idx] == nullptr) {
    void* p = ::operator new(bytes, std::align_val_t{alignment}, std::nothrow);

    if VUNLIKELY (p == nullptr) {
      return nullptr;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    impl_->oversized_alloc.count.fetch_add(1, std::memory_order_relaxed);
    impl_->oversized_alloc.bytes.fetch_add(bytes, std::memory_order_relaxed);

    return p;
  }

  MemoryTierState& state = *impl_->dispatch_states[idx];
  void* block = tier_allocate(state);

  if VUNLIKELY (block == nullptr) {
    return nullptr;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  state.hit_count.fetch_add(1, std::memory_order_relaxed);

  return block;
}

void MemoryPool::deallocate(void* p, size_t bytes, size_t alignment) noexcept {
  if VUNLIKELY (!is_power_of_two(alignment)) {
    CLOG_E("MemoryPool::deallocate: alignment %zu is not a power of two; leaking %p.", alignment, p);
    return;
  }

  if VUNLIKELY (p == nullptr) {
    return;
  }

  const size_t idx = find_tier(bytes);

  if VUNLIKELY (idx == kMaxTierCount || alignment > kBlockAlignment || impl_->dispatch_states[idx] == nullptr) {
    ::operator delete(p, bytes, std::align_val_t{alignment});
    impl_->oversized_dealloc_count.fetch_add(1, std::memory_order_relaxed);

    return;
  }

  MemoryTierState& state = *impl_->dispatch_states[idx];
  tier_deallocate(state, p);
  state.deallocate_count.fetch_add(1, std::memory_order_relaxed);
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
    const uint64_t hits = state.hit_count.load(std::memory_order_relaxed);
    const uint64_t deallocs = state.deallocate_count.load(std::memory_order_relaxed);

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

  result.alloc_count = impl_->oversized_alloc.count.load(std::memory_order_relaxed);
  result.alloc_bytes = impl_->oversized_alloc.bytes.load(std::memory_order_relaxed);
  result.dealloc_count = impl_->oversized_dealloc_count.load(std::memory_order_relaxed);

  return result;
}

void MemoryPool::reset_stats() noexcept {
  const size_t count = impl_->tier_count;

  for (size_t i = 0; i < count; ++i) {
    MemoryTierState& state = *impl_->tier_states[i];
    state.hit_count.store(0, std::memory_order_relaxed);
    state.deallocate_count.store(0, std::memory_order_relaxed);
  }

  impl_->oversized_alloc.count.store(0, std::memory_order_relaxed);
  impl_->oversized_alloc.bytes.store(0, std::memory_order_relaxed);
  impl_->oversized_dealloc_count.store(0, std::memory_order_relaxed);
}

void MemoryPool::clear() noexcept {
  static constexpr size_t kStackSlots = 64U;

  for (auto& state : impl_->owned_states) {
    size_t stack_free_counts[kStackSlots] = {};
    MemoryChunk stack_to_delete[kStackSlots];

    std::vector<size_t> heap_free_counts;
    std::vector<MemoryChunk> heap_to_delete;

    const size_t chunks_hint = state->chunk_count.load(std::memory_order_relaxed);

    if VUNLIKELY (chunks_hint > kStackSlots) {
      try {
        // LCOV_EXCL_START GCOVR_EXCL_START
        heap_free_counts.reserve(chunks_hint);
        heap_to_delete.reserve(chunks_hint);
      } catch (std::exception&) {
      }
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP
    }

    MemoryChunk* to_delete = nullptr;
    size_t to_delete_count = 0U;

    {
      SpinLockGuard lock(state->mtx);

      const size_t chunk_count = state->chunks.size();

      if VUNLIKELY (chunk_count == 0U) {
        continue;
      }

      size_t* free_counts = stack_free_counts;

      const bool spill_to_heap = (chunk_count > kStackSlots);

      if VUNLIKELY (spill_to_heap) {
        try {
          // LCOV_EXCL_START GCOVR_EXCL_START
          heap_free_counts.assign(chunk_count, 0U);
        } catch (std::exception&) {
          continue;
        }

        free_counts = heap_free_counts.data();
        // LCOV_EXCL_STOP GCOVR_EXCL_STOP
      }

      const size_t block_size = state->block_size;

      std::sort(state->chunks.begin(), state->chunks.end(), [](const MemoryChunk& a, const MemoryChunk& b) noexcept {
        return reinterpret_cast<std::uintptr_t>(a.ptr) < reinterpret_cast<std::uintptr_t>(b.ptr);
      });

      const auto find_chunk_idx = [&chunk_count, &state](std::uintptr_t addr) noexcept -> size_t {
        size_t lo = 0;
        size_t hi = chunk_count;

        while (lo < hi) {
          const size_t mid = lo + (hi - lo) / 2U;
          const auto cs = reinterpret_cast<std::uintptr_t>(state->chunks[mid].ptr);
          const auto ce = cs + state->chunks[mid].bytes;

          if (addr < cs) {
            hi = mid;
          } else if (addr >= ce) {
            lo = mid + 1U;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
          } else {
            return mid;
          }
        }

        return SIZE_MAX;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      };

      for (MemoryFreeNode* node = state->free_list_head; node != nullptr; node = node->next) {
        const size_t idx = find_chunk_idx(reinterpret_cast<std::uintptr_t>(node));

        if VLIKELY (idx != SIZE_MAX) {
          ++free_counts[idx];
        }
      }

      MemoryFreeNode* new_head = nullptr;
      MemoryFreeNode* current = state->free_list_head;

      while (current != nullptr) {
        MemoryFreeNode* next = current->next;
        const size_t idx = find_chunk_idx(reinterpret_cast<std::uintptr_t>(current));

        bool keep = false;

        if VLIKELY (idx != SIZE_MAX) {
          const size_t total_blocks = state->chunks[idx].bytes / block_size;
          keep = (free_counts[idx] != total_blocks);
        }

        if (keep) {
          current->next = new_head;
          new_head = current;
        }

        current = next;
      }

      state->free_list_head = new_head;

      size_t released = 0U;
      size_t write = 0U;

      for (size_t read = 0U; read < chunk_count; ++read) {
        const size_t total_blocks = state->chunks[read].bytes / block_size;

        if (free_counts[read] == total_blocks) {
          if VLIKELY (!spill_to_heap) {
            stack_to_delete[released] = state->chunks[read];
          } else {
            try {
              // LCOV_EXCL_START GCOVR_EXCL_START
              heap_to_delete.push_back(state->chunks[read]);
            } catch (std::exception&) {
              ::operator delete(state->chunks[read].ptr, state->chunks[read].bytes, std::align_val_t{kBlockAlignment});
            }
            // LCOV_EXCL_STOP GCOVR_EXCL_STOP
          }

          ++released;
        } else {
          if (read != write) {
            state->chunks[write] = state->chunks[read];  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
          }

          ++write;
        }
      }

      if VLIKELY (released > 0U) {
        state->chunks.resize(write);
        state->chunk_count.fetch_sub(released, std::memory_order_relaxed);
      }

      if VLIKELY (!spill_to_heap) {
        to_delete = stack_to_delete;
        to_delete_count = released;
      } else {
        to_delete = heap_to_delete.data();        // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        to_delete_count = heap_to_delete.size();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }
    }

    for (size_t i = 0; i < to_delete_count; ++i) {
      ::operator delete(to_delete[i].ptr, to_delete[i].bytes, std::align_val_t{kBlockAlignment});
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
      CLOG_W("MemoryPool: VLINK_MEMORY_LEVEL=\"%s\" is not a valid integer, fallback to %d.", env_value.c_str(),
             kDefaultMemoryLevel);

      return kDefaultMemoryLevel;
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP
    }

    if VUNLIKELY (parsed < kMinMemoryLevel || parsed > kMaxMemoryLevel) {
      // LCOV_EXCL_START GCOVR_EXCL_START
      CLOG_W("MemoryPool: VLINK_MEMORY_LEVEL=%d out of range [%d, %d], clamped.", parsed, kMinMemoryLevel,
             kMaxMemoryLevel);

      return parsed < kMinMemoryLevel ? kMinMemoryLevel : kMaxMemoryLevel;
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP
    }

    return parsed;
  }();

  static bool prealloc_env = (Utils::get_env("VLINK_MEMORY_PREALLOC") == "1");

  Config config = create_memory_config(level, prealloc_env);

  return config;
}

MemoryPool& MemoryPool::global_instance(bool use_env_level) {
#if MEMORY_POOL_NERVER_DELETE
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
