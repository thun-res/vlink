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

/**
 * @file memory_pool.h
 * @brief Size-class tiered memory pool with per-tier free lists and runtime statistics.
 *
 * @details
 * @c MemoryPool dispatches each allocation request to one of a fixed pyramid of size classes.
 * Every tier owns a singly-linked free list of fixed-size blocks plus a vector of upstream
 * chunks; the chunk capacity starts small and doubles geometrically until it reaches the
 * configured @c blocks_per_chunk.  Requests larger than the biggest tier (or with an alignment
 * stricter than @c alignof(std::max_align_t)) bypass the pool and route directly to
 * @c ::operator @c new / @c ::operator @c delete.
 *
 * @par Tier / bucket source
 *
 * | Source                                | When used                                              |
 * | ------------------------------------- | ------------------------------------------------------ |
 * | Caller-supplied @c Config             | @c MemoryPool(const @c Config&) with non-empty tiers   |
 * | @c get_default_config() (level 0..9)  | @c global_instance(true)                               |
 * | Built-in level-3 balanced pyramid     | Malformed input fallback or @c global_instance(false)  |
 * | Built-in level @c N row               | @c MemoryPool(int @c level, @c bool @c prealloc) ctor  |
 *
 * @par Allocation flow
 *
 * @verbatim
 *  allocate(bytes, align)
 *      |
 *      v
 *  +-----------------+       align > kBlockAlignment?      +---------------------+
 *  |      guard      | ----------------------------------> |  oversized bypass   |
 *  +-----------------+                                     |  ::operator new     |
 *           |  no                                          +---------------------+
 *           v
 *  +-----------------+   no fit ----> oversized bypass
 *  |  find_tier(bs)  |
 *  +--------+--------+
 *           | tier hit
 *           v
 *  +-----------------+   free list non-empty -> pop block
 *  |  per-tier lock  |
 *  +--------+--------+
 *           | free list empty
 *           v
 *  +-----------------+   ::operator new for next chunk
 *  | upstream alloc  |   carve into N blocks of tier size
 *  +-----------------+
 * @endverbatim
 *
 * @par Cleanup primitives
 *
 * | Method             | What it releases                              | Concurrent traffic? |
 * | ------------------ | --------------------------------------------- | ------------------- |
 * | @c clear / @c trim | Only fully-free chunks; live blocks preserved | Safe                |
 * | @c ~MemoryPool     | Every chunk unconditionally                   | Caller must quiesce |
 *
 * @par Example
 * @code
 *   auto& pool = vlink::MemoryPool::global_instance();
 *   void* p = pool.allocate(512);
 *   pool.deallocate(p, 512);                       // sizes MUST match
 *
 *   pool.trim();                                   // periodic memory reclaim
 * @endcode
 *
 * @note Public methods are @c noexcept and safe for concurrent use.  Per-tier locking means
 *       traffic across different size classes does not contend.  @c deallocate requires the
 *       same @p bytes value passed to @c allocate.  @c allocate returns @c nullptr on upstream
 *       OOM and never throws.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "./macros.h"

namespace vlink {

/**
 * @class MemoryPool
 * @brief Per-tier free-list pool with runtime statistics and oversized passthrough.
 *
 * @details
 * Thread-safe.  Each tier owns its own spin lock so @c allocate, @c deallocate and @c clear on
 * different size classes never contend.  Bypass mode -- selected by an empty tier list -- routes
 * every request through the global allocator without any pooling.
 */
class VLINK_EXPORT MemoryPool final {
 public:
  /**
   * @brief Default block alignment for every pooled allocation.
   *
   * @details
   * Equal to @c alignof(std::max_align_t).  Stricter alignments bypass the pool and request
   * memory directly through @c ::operator @c new with @c std::align_val_t.
   */
  static constexpr size_t kBlockAlignment = alignof(std::max_align_t);

  /**
   * @brief Descriptor for one size class.
   */
  struct Tier final {
    size_t max_size{0};          ///< Inclusive upper bound of the tier in bytes.
    size_t blocks_per_chunk{0};  ///< Maximum blocks carved from a single upstream chunk.
  };

  /**
   * @brief Constructor configuration grouping the tier list and the preallocation toggle.
   *
   * @details
   * @c prealloc controls whether the constructor immediately fills every tier to its full
   * @c blocks_per_chunk quota.  Default @c false keeps the lazy growth path.  Preallocation is
   * best effort; any tier whose @c ::operator @c new fails stays in lazy state and the
   * constructor continues.
   */
  struct Config final {
    std::vector<Tier> tiers;  ///< Tier descriptors; empty or all-sentinel selects bypass mode.
    bool prealloc{false};     ///< When @c true, eagerly fill every managed tier to its quota.
  };

  /**
   * @brief Per-tier runtime statistics snapshot.
   *
   * @details
   * Counters use relaxed atomics; @c in_use_blocks and the lifetime upstream fields are best
   * effort under concurrent traffic, not globally atomic.
   */
  struct TierStats final {
    size_t max_size{0};                ///< Configured @c max_size for this tier.
    size_t blocks_per_chunk{0};        ///< Configured @c blocks_per_chunk for this tier.
    size_t block_size{0};              ///< Effective block size after alignment rounding.
    uint64_t hit_count{0};             ///< Allocations dispatched to this tier (resettable).
    uint64_t deallocate_count{0};      ///< Deallocations dispatched to this tier (resettable).
    uint64_t in_use_blocks{0};         ///< Best-effort @c hit_count @c - @c deallocate_count.
    uint64_t chunk_count{0};           ///< Currently owned chunks; @c clear decrements by released count.
    uint64_t upstream_alloc_count{0};  ///< Lifetime number of chunks fully installed in this tier.
    uint64_t upstream_alloc_bytes{0};  ///< Lifetime bytes of those installed chunks.
  };

  /**
   * @brief Statistics for allocations that bypass the tier free lists.
   *
   * @details
   * Captures requests whose size exceeds the largest tier or whose alignment exceeds
   * @c kBlockAlignment.
   */
  struct OversizedStats final {
    uint64_t alloc_count{0};    ///< Oversized allocations forwarded to the system allocator.
    uint64_t alloc_bytes{0};    ///< Total bytes of oversized allocations.
    uint64_t dealloc_count{0};  ///< Oversized deallocations observed.
  };

  /**
   * @brief Constructs an empty pool in bypass mode.
   *
   * @details
   * Equivalent to @c MemoryPool(Config{}): every request hits @c ::operator @c new / @c delete.
   */
  MemoryPool();

  /**
   * @brief Constructs a tiered pool using the built-in pyramid for @p level.
   *
   * @details
   * Out-of-range values are clamped to @c [0, @c 9] and a warning is logged.  Level @c 0 yields
   * bypass mode; the @p prealloc flag is ignored in that case.  Level @c 9 fully saturates to
   * roughly 656 MiB of resident memory.
   *
   * @param level     Built-in level in @c [0, @c 9].
   * @param prealloc  When @c true, fills every tier to its quota on construction (best effort).
   *                  Default: @c false.
   */
  explicit MemoryPool(int level, bool prealloc = false);

  /**
   * @brief Constructs a tiered pool with an explicit configuration.
   *
   * @details
   * Empty @c config.tiers selects bypass mode.  Sentinel entries with @c blocks_per_chunk
   * @c == @c 0 declare a size ceiling but are stripped at construction; an all-sentinel list is
   * therefore equivalent to bypass mode.  Malformed input (non-monotonic ordering, duplicate
   * @c max_size, zero @c max_size or @c max_size below the minimum block size) logs an error
   * and silently falls back to the level-3 default pyramid.  @c std::bad_alloc may propagate
   * from internal vector growth.
   *
   * @param config  Tier descriptors and preallocation flag.
   */
  explicit MemoryPool(const Config& config);

  /**
   * @brief Releases every owned chunk unconditionally.
   *
   * @warning The caller must guarantee no outstanding pooled block is in use and no other
   *          thread is calling @c allocate, @c deallocate, @c clear or @c trim on this instance.
   *          Use @c clear for a non-destructive trim.
   */
  ~MemoryPool();

  /**
   * @brief Allocates @p bytes of memory from the appropriate tier.
   *
   * @details
   * Routes to the first tier whose @c max_size is @c >= @p bytes.  Requests larger than the
   * biggest tier or with @p alignment @c > @c kBlockAlignment bypass the pool.  @p bytes @c ==
   * @c 0 routes to the smallest tier and still returns a unique non-null pointer that must be
   * passed back to @c deallocate with the same @c 0 size.  Never throws.
   *
   * @param bytes      Requested size.
   * @param alignment  Required alignment (power of two).  Default: @c kBlockAlignment.
   * @return Pointer to allocated memory, or @c nullptr on upstream OOM.
   */
  [[nodiscard]] void* allocate(size_t bytes, size_t alignment = kBlockAlignment) noexcept;

  /**
   * @brief Returns a block previously allocated through @c allocate to the pool.
   *
   * @param p          Pointer returned by @c allocate.  @c nullptr is a no-op.
   * @param bytes      Original size passed to @c allocate; MUST match.
   * @param alignment  Original alignment passed to @c allocate.
   */
  void deallocate(void* p, size_t bytes, size_t alignment = kBlockAlignment) noexcept;

  /**
   * @brief Returns the number of live managed tiers.
   *
   * @details
   * @c 0 indicates bypass mode.  Sentinel entries are stripped at construction so the count
   * reflects only tiers backed by a live free list.
   *
   * @return Tier count after sentinel stripping.
   */
  [[nodiscard]] size_t get_tier_count() const noexcept;

  /**
   * @brief Returns a snapshot of per-tier statistics.
   *
   * @return Vector with one entry per live tier.
   */
  [[nodiscard]] std::vector<TierStats> get_stats() const noexcept;

  /**
   * @brief Returns a snapshot of the oversized-passthrough statistics.
   *
   * @details
   * Counters are loaded with relaxed ordering and are not a globally atomic snapshot.
   *
   * @return Aggregated counters for the oversized path.
   */
  [[nodiscard]] OversizedStats get_oversized_stats() const noexcept;

  /**
   * @brief Resets per-call statistics counters to zero.
   *
   * @details
   * Clears @c hit_count and @c deallocate_count on every tier and the @c oversized_* counters.
   * Physical / lifetime state (@c chunk_count, @c upstream_alloc_count, @c upstream_alloc_bytes)
   * is preserved.
   */
  void reset_stats() noexcept;

  /**
   * @brief Releases only fully-free chunks; preserves chunks still backing live allocations.
   *
   * @details
   * For each tier the free list is grouped by owning chunk; chunks whose free-node count equals
   * their block capacity are released, others stay intact.  @c chunk_count is decremented by
   * the number of released chunks.  Safe to call concurrently with @c allocate and
   * @c deallocate.  Per-tier work is @c O(C @c log @c C @c + @c F @c log @c C) under the spin
   * lock.
   */
  void clear() noexcept;

  /**
   * @brief Alias of @c clear with a name suited to periodic-trim phrasing.
   *
   * @details
   * Behaviour, complexity and thread-safety are identical to @c clear.
   */
  void trim() noexcept;

  /**
   * @brief Returns the default tier pyramid using @c VLINK_MEMORY_LEVEL / @c VLINK_MEMORY_PREALLOC.
   *
   * @details
   * Each level @c 0..9 maps to a hand-coded row of @c {max_size, @c blocks_per_chunk} pairs.
   * Level @c 0 is bypass mode.  Levels @c 1..9 return 19 entries covering 32 B to 16 MiB; the
   * 1 MiB / 4 MiB / 8 MiB / 16 MiB ceilings activate at level @c 2 / @c 4 / @c 5 / @c 6 and are
   * sentinels below those thresholds.  Within each level the 32 B head tier carries twice the
   * @c blocks_per_chunk of the 64 B tier to absorb high-density tiny allocations.
   *
   * @c VLINK_MEMORY_PREALLOC controls the @c prealloc flag; only the literal value @c "1"
   * enables preallocation.
   *
   * @return @c Config ready to pass to the constructor.
   */
  [[nodiscard]] static Config get_default_config();

  /**
   * @brief Returns the process-wide shared @c MemoryPool instance.
   *
   * @details
   * Lazy Meyers singleton.  Only the first call decides the configuration; subsequent calls
   * return the same instance and ignore the argument.
   *
   * @warning Whichever value @p use_env_level takes on the first call is baked in for the
   *          rest of the program's lifetime.
   *
   * @param use_env_level  @c true (default): use @c get_default_config and honour the
   *                       environment.  @c false: use the built-in level-3 pyramid.
   * @return Reference to the global pool.
   */
  static MemoryPool& global_instance(bool use_env_level = true);

 private:
  size_t find_tier(size_t bytes) const noexcept;

  struct Impl;
  std::unique_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(MemoryPool)
};

}  // namespace vlink
