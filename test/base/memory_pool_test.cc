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

// NOLINTBEGIN

#include "./base/memory_pool.h"

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <thread>
#include <unordered_set>
#include <vector>

#include "../common_test.h"

namespace {

MemoryPool::Config simple_tiers() {
  MemoryPool::Config cfg;
  cfg.tiers = {{64, 4}, {256, 4}, {1024, 2}};
  return cfg;
}

MemoryPool::Config make_config(std::vector<MemoryPool::Tier> tiers, bool prealloc = false) {
  MemoryPool::Config cfg;
  cfg.tiers = std::move(tiers);
  cfg.prealloc = prealloc;
  return cfg;
}

uint64_t total_hits(const std::vector<MemoryPool::TierStats>& stats) {
  uint64_t sum = 0;
  for (const auto& s : stats) {
    sum += s.hit_count;
  }
  return sum;
}

}  // namespace

TEST_SUITE("base-MemoryPool") {
  TEST_CASE("default construction selects bypass mode") {
    MemoryPool pool;
    CHECK_EQ(pool.get_tier_count(), 0u);
    CHECK(pool.get_stats().empty());
  }

  TEST_CASE("level 0 construction selects bypass mode") {
    MemoryPool pool(0);
    CHECK_EQ(pool.get_tier_count(), 0u);
  }

  TEST_CASE("level 3 construction yields the level-3 pyramid") {
    MemoryPool pool(3);

    auto stats = pool.get_stats();
    CHECK(stats.size() >= 4u);
    CHECK_EQ(stats.front().max_size, 32u);
    CHECK_EQ(stats.back().max_size, 1u * 1024u * 1024u);
  }

  TEST_CASE("out-of-range level is clamped to a valid pyramid") {
    MemoryPool pool(99);
    CHECK(pool.get_tier_count() >= 4u);
  }

  TEST_CASE("config construction with simple tiers") {
    MemoryPool pool(simple_tiers());

    CHECK_EQ(pool.get_tier_count(), 3u);
    CHECK_EQ(pool.get_stats().size(), 3u);
    CHECK_EQ(pool.get_oversized_stats().alloc_count, 0u);
  }

  TEST_CASE("get_default_config is well-formed and monotone") {
    auto config = MemoryPool::get_default_config();
    const auto& tiers = config.tiers;

    REQUIRE(tiers.size() >= 4u);
    CHECK_EQ(tiers.front().max_size, 32u);

    for (size_t i = 1; i < tiers.size(); ++i) {
      CHECK(tiers[i].max_size > tiers[i - 1].max_size);
    }
  }

  TEST_CASE("empty tier list selects bypass mode") {
    MemoryPool pool(make_config({}));
    CHECK_EQ(pool.get_tier_count(), 0u);
    CHECK(pool.get_stats().empty());
  }

  TEST_CASE("zero max_size triggers default-pyramid fallback") {
    MemoryPool pool(make_config({{0, 4}}));
    CHECK(pool.get_tier_count() >= 4u);
  }

  TEST_CASE("zero blocks_per_chunk is stripped and causes bypass mode") {
    MemoryPool pool(make_config({{64, 0}}));
    CHECK_EQ(pool.get_tier_count(), 0u);
  }

  TEST_CASE("non-monotonic ordering triggers default-pyramid fallback") {
    MemoryPool pool(make_config({{256, 4}, {64, 4}}));
    CHECK(pool.get_tier_count() >= 4u);
  }

  TEST_CASE("duplicate max_size triggers default-pyramid fallback") {
    MemoryPool pool(make_config({{64, 4}, {64, 4}}));
    CHECK(pool.get_tier_count() >= 4u);
  }

  TEST_CASE("allocate from bypass pool routes to oversized path") {
    MemoryPool pool;

    void* p = pool.allocate(128);
    REQUIRE(p != nullptr);
    CHECK_EQ(pool.get_oversized_stats().alloc_count, 1u);
    CHECK_EQ(pool.get_oversized_stats().alloc_bytes, 128u);

    pool.deallocate(p, 128);
    CHECK_EQ(pool.get_oversized_stats().dealloc_count, 1u);
  }

  TEST_CASE("nullptr deallocate is a no-op") {
    MemoryPool pool(simple_tiers());
    pool.deallocate(nullptr, 64);
    pool.deallocate(nullptr, 100000);
    CHECK_EQ(pool.get_oversized_stats().dealloc_count, 0u);
  }

  TEST_CASE("allocate dispatches to the smallest fitting tier") {
    MemoryPool pool(simple_tiers());

    void* a = pool.allocate(1);
    void* b = pool.allocate(64);
    void* c = pool.allocate(65);
    void* d = pool.allocate(256);
    void* e = pool.allocate(1024);

    auto stats = pool.get_stats();
    CHECK_EQ(stats[0].hit_count, 2u);
    CHECK_EQ(stats[1].hit_count, 2u);
    CHECK_EQ(stats[2].hit_count, 1u);

    pool.deallocate(a, 1);
    pool.deallocate(b, 64);
    pool.deallocate(c, 65);
    pool.deallocate(d, 256);
    pool.deallocate(e, 1024);
  }

  TEST_CASE("oversized allocation bypasses all tiers") {
    MemoryPool pool(simple_tiers());

    static constexpr size_t kBig = 4096;
    void* p = pool.allocate(kBig);

    REQUIRE(p != nullptr);
    CHECK_EQ(total_hits(pool.get_stats()), 0u);
    CHECK_EQ(pool.get_oversized_stats().alloc_count, 1u);
    CHECK_EQ(pool.get_oversized_stats().alloc_bytes, kBig);

    pool.deallocate(p, kBig);
    CHECK_EQ(pool.get_oversized_stats().dealloc_count, 1u);
  }

  TEST_CASE("alignment larger than max_align_t routes to oversized path") {
    MemoryPool pool(simple_tiers());

    static constexpr size_t kBigAlign = 64;
    static_assert(kBigAlign > alignof(std::max_align_t), "test premise");

    void* p = pool.allocate(64, kBigAlign);
    REQUIRE(p != nullptr);
    CHECK_EQ(reinterpret_cast<uintptr_t>(p) % kBigAlign, 0u);
    CHECK_EQ(pool.get_oversized_stats().alloc_count, 1u);

    pool.deallocate(p, 64, kBigAlign);
  }

  TEST_CASE("deallocated block is reused on the next allocation") {
    MemoryPool pool(simple_tiers());

    void* p = pool.allocate(64);
    pool.deallocate(p, 64);

    void* q = pool.allocate(64);
    CHECK_EQ(p, q);
    pool.deallocate(q, 64);
  }

  TEST_CASE("all allocated blocks within a tier are unique") {
    MemoryPool pool(make_config({{128, 64}}));

    std::unordered_set<void*> seen;
    std::vector<void*> blocks;
    static constexpr int kCount = 64;

    for (int i = 0; i < kCount; ++i) {
      void* p = pool.allocate(64);
      CHECK(seen.insert(p).second);
      blocks.push_back(p);
    }

    for (void* p : blocks) {
      pool.deallocate(p, 64);
    }
  }

  TEST_CASE("hit count and in_use_blocks track allocations correctly") {
    MemoryPool pool(simple_tiers());

    void* p = pool.allocate(64);
    void* q = pool.allocate(64);

    auto stats = pool.get_stats();
    CHECK_EQ(stats[0].hit_count, 2u);
    CHECK_EQ(stats[0].deallocate_count, 0u);
    CHECK_EQ(stats[0].in_use_blocks, 2u);

    pool.deallocate(p, 64);
    stats = pool.get_stats();
    CHECK_EQ(stats[0].deallocate_count, 1u);
    CHECK_EQ(stats[0].in_use_blocks, 1u);

    pool.deallocate(q, 64);
    stats = pool.get_stats();
    CHECK_EQ(stats[0].in_use_blocks, 0u);
  }

  TEST_CASE("block_size is aligned to alignof(max_align_t)") {
    MemoryPool pool(make_config({{8, 4}, {24, 4}, {64, 4}}));

    auto stats = pool.get_stats();
    CHECK(stats[0].block_size >= sizeof(void*));
    CHECK_EQ(stats[0].block_size % alignof(std::max_align_t), 0u);
    CHECK_EQ(stats[1].block_size % alignof(std::max_align_t), 0u);
    CHECK_EQ(stats[2].block_size, 64u);
  }

  TEST_CASE("upstream_alloc_bytes accumulates across chunk allocations") {
    MemoryPool pool(make_config({{128, 4}}));

    void* a = pool.allocate(128);
    void* b = pool.allocate(128);
    void* c = pool.allocate(128);
    void* d = pool.allocate(128);

    auto stats = pool.get_stats();
    CHECK(stats[0].upstream_alloc_count >= 1u);
    CHECK(stats[0].upstream_alloc_bytes >= stats[0].block_size * 4u);

    pool.deallocate(a, 128);
    pool.deallocate(b, 128);
    pool.deallocate(c, 128);
    pool.deallocate(d, 128);
  }

  TEST_CASE("reset_stats clears hit counters but preserves upstream allocation history") {
    MemoryPool pool(make_config({{64, 4}}));

    void* p = pool.allocate(64);
    pool.deallocate(p, 64);

    auto before = pool.get_stats();
    REQUIRE(before[0].upstream_alloc_count > 0u);
    REQUIRE(before[0].hit_count > 0u);

    pool.reset_stats();

    auto after = pool.get_stats();
    CHECK_EQ(after[0].hit_count, 0u);
    CHECK_EQ(after[0].deallocate_count, 0u);
    CHECK_EQ(after[0].upstream_alloc_count, before[0].upstream_alloc_count);
    CHECK_EQ(after[0].upstream_alloc_bytes, before[0].upstream_alloc_bytes);
    CHECK_EQ(after[0].chunk_count, before[0].chunk_count);
  }

  TEST_CASE("prealloc false leaves tiers lazy before first allocation") {
    MemoryPool pool(make_config({{64, 8}, {256, 4}}, false));

    auto stats = pool.get_stats();
    REQUIRE(stats.size() == 2u);
    CHECK_EQ(stats[0].chunk_count, 0u);
    CHECK_EQ(stats[0].upstream_alloc_count, 0u);
    CHECK_EQ(stats[1].chunk_count, 0u);
    CHECK_EQ(stats[1].upstream_alloc_count, 0u);
  }

  TEST_CASE("prealloc true fills every tier to blocks_per_chunk in one upstream allocation") {
    static constexpr size_t kBpc0 = 8;
    static constexpr size_t kBpc1 = 4;
    MemoryPool pool(make_config({{64, kBpc0}, {256, kBpc1}}, true));

    auto stats = pool.get_stats();
    REQUIRE(stats.size() == 2u);

    CHECK_EQ(stats[0].chunk_count, 1u);
    CHECK_EQ(stats[0].upstream_alloc_count, 1u);
    CHECK_EQ(stats[0].upstream_alloc_bytes, stats[0].block_size * kBpc0);

    CHECK_EQ(stats[1].chunk_count, 1u);
    CHECK_EQ(stats[1].upstream_alloc_count, 1u);
    CHECK_EQ(stats[1].upstream_alloc_bytes, stats[1].block_size * kBpc1);
  }

  TEST_CASE("prealloc serves all blocks_per_chunk slots without further upstream growth") {
    static constexpr size_t kBpc = 6;
    MemoryPool pool(make_config({{64, kBpc}}, true));

    std::vector<void*> blocks;
    blocks.reserve(kBpc);

    for (size_t i = 0; i < kBpc; ++i) {
      void* p = pool.allocate(64);
      REQUIRE(p != nullptr);
      blocks.push_back(p);
    }

    auto stats = pool.get_stats();
    REQUIRE(stats.size() == 1u);
    CHECK_EQ(stats[0].chunk_count, 1u);
    CHECK_EQ(stats[0].upstream_alloc_count, 1u);
    CHECK_EQ(stats[0].hit_count, kBpc);

    for (void* p : blocks) {
      pool.deallocate(p, 64);
    }
  }

  TEST_CASE("prealloc on empty tier list is a no-op") {
    MemoryPool pool(make_config({}, true));
    CHECK_EQ(pool.get_tier_count(), 0u);
    CHECK_EQ(pool.get_oversized_stats().alloc_count, 0u);
  }

  TEST_CASE("clear releases all chunks when no blocks are live") {
    MemoryPool pool(make_config({{64, 4}}));

    std::vector<void*> blocks;
    for (int i = 0; i < 7; ++i) {
      blocks.push_back(pool.allocate(64));
    }
    for (void* p : blocks) {
      pool.deallocate(p, 64);
    }

    auto before = pool.get_stats();
    REQUIRE(before[0].chunk_count > 0u);

    pool.clear();

    auto after = pool.get_stats();
    CHECK_EQ(after[0].chunk_count, 0u);

    void* q = pool.allocate(64);
    REQUIRE(q != nullptr);
    pool.deallocate(q, 64);
  }

  TEST_CASE("clear keeps chunks that still back a live allocation") {
    MemoryPool pool(make_config({{64, 4}}));

    std::vector<void*> blocks;
    for (int i = 0; i < 7; ++i) {
      blocks.push_back(pool.allocate(64));
    }

    void* live = blocks.back();
    blocks.pop_back();
    for (void* p : blocks) {
      pool.deallocate(p, 64);
    }

    auto before = pool.get_stats();
    REQUIRE(before[0].in_use_blocks == 1u);
    const uint64_t chunks_before = before[0].chunk_count;

    pool.clear();

    auto after = pool.get_stats();
    CHECK(after[0].chunk_count >= 1u);
    CHECK(after[0].chunk_count < chunks_before);
    CHECK_EQ(after[0].in_use_blocks, 1u);

    std::memset(live, 0xEE, 64);
    pool.deallocate(live, 64);
  }

  TEST_CASE("clear on empty pool is a no-op") {
    MemoryPool pool(make_config({{64, 4}}));
    pool.clear();
    pool.clear();

    auto stats = pool.get_stats();
    CHECK_EQ(stats[0].chunk_count, 0u);
    CHECK_EQ(stats[0].upstream_alloc_count, 0u);
  }

  TEST_CASE("clear leaves oversized stats untouched") {
    MemoryPool pool(make_config({{64, 4}}));

    static constexpr size_t kBig = 4096;
    void* p = pool.allocate(kBig);
    pool.deallocate(p, kBig);

    auto before = pool.get_oversized_stats();
    REQUIRE(before.alloc_count == 1u);
    REQUIRE(before.dealloc_count == 1u);

    pool.clear();

    auto after = pool.get_oversized_stats();
    CHECK_EQ(after.alloc_count, before.alloc_count);
    CHECK_EQ(after.alloc_bytes, before.alloc_bytes);
    CHECK_EQ(after.dealloc_count, before.dealloc_count);
  }

  TEST_CASE("trim does not crash") {
    MemoryPool pool(simple_tiers());

    void* p = pool.allocate(64);
    pool.deallocate(p, 64);
    pool.trim();
  }

  TEST_CASE("global_instance returns the same reference every time") {
    MemoryPool& a = MemoryPool::global_instance();
    MemoryPool& b = MemoryPool::global_instance();
    CHECK_EQ(&a, &b);
  }

  TEST_CASE("global_instance allocate and deallocate do not crash") {
    MemoryPool& pool = MemoryPool::global_instance();

    void* p = pool.allocate(128);
    REQUIRE(p != nullptr);
    pool.deallocate(p, 128);
  }

  TEST_CASE("upstream chunk growth is sub-linear relative to block count") {
    MemoryPool pool(make_config({{64, 32}}));

    std::vector<void*> blocks;
    for (int i = 0; i < 63; ++i) {
      blocks.push_back(pool.allocate(64));
    }

    auto stats = pool.get_stats();
    CHECK(stats[0].upstream_alloc_count <= 6u);
    CHECK_EQ(stats[0].chunk_count, stats[0].upstream_alloc_count);

    for (void* p : blocks) {
      pool.deallocate(p, 64);
    }
  }

  TEST_CASE("blocks_per_chunk acts as a hard cap on chunk size") {
    MemoryPool pool(make_config({{64, 4}}));

    std::vector<void*> blocks;
    for (int i = 0; i < 32; ++i) {
      blocks.push_back(pool.allocate(64));
    }

    auto stats = pool.get_stats();
    CHECK(stats[0].upstream_alloc_count >= 8u);

    for (void* p : blocks) {
      pool.deallocate(p, 64);
    }
  }

  TEST_CASE("large allocation within the top live tier at level 3") {
    MemoryPool pool(3);

    static constexpr size_t kSize = 1u * 1024u * 1024u;
    void* p = pool.allocate(kSize);
    REQUIRE(p != nullptr);

    std::memset(p, 0xCD, kSize);
    CHECK_EQ(static_cast<uint8_t*>(p)[kSize - 1], 0xCDu);
    CHECK_EQ(pool.get_oversized_stats().alloc_count, 0u);

    pool.deallocate(p, kSize);
  }

  TEST_CASE("allocation exceeding the largest tier routes to oversized path") {
    MemoryPool pool(3);

    static constexpr size_t kHuge = 8'000'000u * 3u / 2u;
    void* p = pool.allocate(kHuge);
    REQUIRE(p != nullptr);
    CHECK_EQ(pool.get_oversized_stats().alloc_count, 1u);

    pool.deallocate(p, kHuge);
  }

  TEST_CASE("concurrent allocations all return non-null unique pointers") {
    MemoryPool pool(make_config({{64, 64}}));

    static constexpr int kThreads = 16;
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::vector<void*> blocks(static_cast<size_t>(kThreads), nullptr);

    auto worker = [&](int index) {
      ready.fetch_add(1, std::memory_order_release);

      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }

      blocks[static_cast<size_t>(index)] = pool.allocate(64);
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int i = 0; i < kThreads; ++i) {
      threads.emplace_back(worker, i);
    }

    while (ready.load(std::memory_order_acquire) != kThreads) {
      std::this_thread::yield();
    }

    start.store(true, std::memory_order_release);

    for (auto& t : threads) {
      t.join();
    }

    for (void* p : blocks) {
      REQUIRE(p != nullptr);
      pool.deallocate(p, 64);
    }
  }

  TEST_CASE("many threads alloc and dealloc without races or corruption") {
    MemoryPool pool(MemoryPool::get_default_config());

    static constexpr int kThreads = 8;
    static constexpr int kIterations = 2000;
    std::atomic<bool> error{false};

    auto worker = [&](int seed) {
      std::mt19937 rng(static_cast<uint32_t>(seed));
      std::uniform_int_distribution<size_t> size_dist(1, 8 * 1024);
      std::vector<std::pair<void*, size_t>> live;
      live.reserve(32);

      for (int i = 0; i < kIterations; ++i) {
        const size_t bytes = size_dist(rng);
        void* p = pool.allocate(bytes);

        if (p == nullptr) {
          error.store(true);
          return;
        }

        std::memset(p, static_cast<int>(seed & 0xFF), bytes);
        live.emplace_back(p, bytes);

        if (live.size() > 16 || (rng() & 1u)) {
          auto [ptr, sz] = live.back();
          live.pop_back();
          pool.deallocate(ptr, sz);
        }
      }

      for (auto& [ptr, sz] : live) {
        pool.deallocate(ptr, sz);
      }
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int i = 0; i < kThreads; ++i) {
      threads.emplace_back(worker, i + 1);
    }

    for (auto& t : threads) {
      t.join();
    }

    CHECK_FALSE(error.load());

    auto stats = pool.get_stats();
    uint64_t hits = 0;
    uint64_t deallocs = 0;

    for (const auto& s : stats) {
      hits += s.hit_count;
      deallocs += s.deallocate_count;
    }

    auto over = pool.get_oversized_stats();
    CHECK_EQ(hits, deallocs);
    CHECK_EQ(over.alloc_count, over.dealloc_count);
  }
}

// NOLINTEND
