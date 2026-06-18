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

#include <vlink/base/bytes.h>
#include <vlink/base/logger.h>
#include <vlink/base/memory_pool.h>

#include <array>

// Pretty-prints the per-tier statistics so each scenario below can attribute
// allocations to the right size class. Includes the oversized (>= largest
// tier) passthrough counter at the end.
static void print_tier_stats(const vlink::MemoryPool& pool) {
  auto stats = pool.get_stats();
  MLOG_I("  tiers={}", stats.size());
  for (size_t i = 0; i < stats.size(); ++i) {
    const auto& s = stats[i];
    MLOG_I(
        "    tier[{}]: max_size={} block_size={} per_chunk={} hits={} deallocs={} in_use={} chunks={} "
        "up_allocs={} up_bytes={}",
        i, s.max_size, s.block_size, s.blocks_per_chunk, s.hit_count, s.deallocate_count, s.in_use_blocks,
        s.chunk_count, s.upstream_alloc_count, s.upstream_alloc_bytes);
  }

  auto over = pool.get_oversized_stats();
  MLOG_I("  oversized: allocs={} bytes={} deallocs={}", over.alloc_count, over.alloc_bytes, over.dealloc_count);
}
// -----------------------------------------------------------------------------
// MemoryPool example
//
// Module:   vlink/base/memory_pool.h
// Scenario: MemoryPool is the slab/tier allocator backing Bytes::create and
//           reusable across user-owned subsystems. Each tier owns a free-list
//           of fixed-size blocks; allocate(n) picks the smallest tier whose
//           block_size >= n. Requests larger than every tier fall through to
//           the oversized passthrough (malloc/free).
// CRITICAL: deallocate(ptr, n) MUST be called with the SAME size that was
//           passed to allocate(). The pool uses that size to route the
//           pointer back to the correct tier; a mismatched size corrupts
//           the free-list and produces silent UB. Bytes handles this for
//           you internally -- only manual users need to remember it.
// -----------------------------------------------------------------------------
int main() {
  // Default config from VLINK_MEMORY_LEVEL: a sane default ladder of tiers
  // for general-purpose middleware traffic. Construction validates and
  // strips the unused sentinels, so get_tier_count() is the live count.
  {
    VLOG_I("=== get_default_config ===");
    auto config = vlink::MemoryPool::get_default_config();
    MLOG_I("  config tiers={} prealloc={}", config.tiers.size(), config.prealloc);

    vlink::MemoryPool pool(config);
    MLOG_I("  live tiers (after sentinel strip)={}", pool.get_tier_count());

    void* a = pool.allocate(24);
    void* b = pool.allocate(96);
    void* c = pool.allocate(1024);
    void* d = pool.allocate(1U * 1024U * 1024U);

    if (a == nullptr || b == nullptr || c == nullptr || d == nullptr) {
      VLOG_W("  upstream OOM");
    } else {
      print_tier_stats(pool);
      // Remember: free with the SAME size, otherwise the pool cannot route
      // the pointer back to its origin tier.
      pool.deallocate(a, 24);
      pool.deallocate(b, 96);
      pool.deallocate(c, 1024);
      pool.deallocate(d, 1U * 1024U * 1024U);
      VLOG_I("  after deallocate:");
      print_tier_stats(pool);
    }
  }

  // Oversized passthrough: a 16 MiB request blows past every tier, so the
  // pool routes it directly to the system allocator. The oversized
  // counters track these passthrough requests independently.
  {
    VLOG_I("=== Oversized passthrough ===");
    vlink::MemoryPool::Config cfg;
    cfg.tiers = {{64U, 128U}, {256U, 64U}, {1024U, 16U}};
    vlink::MemoryPool tiny(cfg);

    static constexpr size_t kHuge = 16U * 1024U * 1024U;
    void* p = tiny.allocate(kHuge);

    if (p == nullptr) {
      VLOG_W("  16 MiB alloc failed");
    } else {
      VLOG_I("  16 MiB allocation routed to oversized passthrough");
      tiny.deallocate(p, kHuge);
      print_tier_stats(tiny);
    }
  }

  // Custom tier config tuned for a 4 KiB page workload: 32 page-sized
  // allocations all land in the 4096-byte tier, exercising the hit path
  // and the in_use_blocks counter. reset_stats clears counters but keeps
  // the allocated chunks; clear() releases fully-free chunks.
  {
    VLOG_I("=== Custom tier config ===");
    vlink::MemoryPool::Config cfg;
    cfg.tiers = {{64U, 256U}, {4U * 1024U, 64U}, {64U * 1024U, 8U}};
    cfg.prealloc = true;
    vlink::MemoryPool pool(cfg);

    static constexpr size_t kPageBytes = 4096U;
    static constexpr size_t kPageCount = 32U;

    std::array<void*, kPageCount> pages{};
    for (size_t i = 0; i < kPageCount; ++i) {
      pages[i] = pool.allocate(kPageBytes);
    }

    VLOG_I("  allocated 32 x 4 KiB pages");
    print_tier_stats(pool);

    for (size_t i = 0; i < kPageCount; ++i) {
      pool.deallocate(pages[i], kPageBytes);
    }

    pool.reset_stats();
    VLOG_I("  after reset_stats:");
    print_tier_stats(pool);

    pool.clear();
    VLOG_I("  after clear (fully-free chunks released):");
    print_tier_stats(pool);
  }

  // global_instance(true) returns the process-wide pool shared with Bytes.
  // Allocations made through Bytes::create show up in the same stats here,
  // letting the user see which size classes their messages exercise.
  {
    VLOG_I("=== global_instance shared with Bytes ===");
    auto& global = vlink::MemoryPool::global_instance(true);
    MLOG_I("  global tiers={}", global.get_tier_count());

    auto buf_a = vlink::Bytes::create(2048);
    auto buf_b = vlink::Bytes::create(64U * 1024U);
    MLOG_I("  Bytes::create sizes {} and {}", buf_a.size(), buf_b.size());
    print_tier_stats(global);
  }

  // Construct by VLINK_MEMORY_LEVEL value (5 = the medium preset). Useful
  // when the caller wants a non-default tier ladder without spelling out
  // every {max_size, blocks_per_chunk} pair.
  {
    VLOG_I("=== MemoryPool(int level) ===");
    vlink::MemoryPool level5(5);
    MLOG_I("  level5 tiers={}", level5.get_tier_count());

    void* p = level5.allocate(96);
    if (p != nullptr) {
      level5.deallocate(p, 96);
      print_tier_stats(level5);
    }
  }

  // level=0 / default-ctor: bypass mode. The pool exists but has zero
  // tiers; every allocate() goes through the oversized passthrough. Acts
  // as a transparent pass-through to system malloc/free.
  {
    VLOG_I("=== Bypass mode (level=0) ===");
    vlink::MemoryPool bypass;
    MLOG_I("  bypass tiers={} (expect 0)", bypass.get_tier_count());

    void* a = bypass.allocate(48);
    void* b = bypass.allocate(64U * 1024U);

    if (a != nullptr) {
      bypass.deallocate(a, 48);
    }

    if (b != nullptr) {
      bypass.deallocate(b, 64U * 1024U);
    }

    auto over = bypass.get_oversized_stats();
    MLOG_I("  bypass oversized: allocs={} bytes={} deallocs={}", over.alloc_count, over.alloc_bytes,
           over.dealloc_count);
  }

  VLOG_I("MemoryPool example finished.");
  return 0;
}
