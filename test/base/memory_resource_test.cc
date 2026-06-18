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

#include "./base/memory_resource.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../common_test.h"
#include "./base/bytes.h"
#include "./base/memory_pool.h"

#ifdef VLINK_ENABLE_BASE_MEMORY_RESOURCE

#include <memory_resource>

TEST_SUITE("base-MemoryResource") {
  TEST_CASE("default construction owns a bypass pool") {
    MemoryResource res;

    MemoryPool& pool = res.get_memory_pool();
    CHECK_EQ(pool.get_tier_count(), 0u);

    void* p = res.allocate(64);
    REQUIRE(p != nullptr);

    auto over = pool.get_oversized_stats();
    CHECK_EQ(over.alloc_count, 1u);
    CHECK_EQ(over.alloc_bytes, 64u);

    res.deallocate(p, 64);
    CHECK_EQ(pool.get_oversized_stats().dealloc_count, 1u);
  }

  TEST_CASE("level 0 construction owns a bypass pool") {
    MemoryResource res(0);

    CHECK_EQ(res.get_memory_pool().get_tier_count(), 0u);

    void* p = res.allocate(64);
    REQUIRE(p != nullptr);
    CHECK_EQ(res.get_memory_pool().get_oversized_stats().alloc_count, 1u);

    res.deallocate(p, 64);
  }

  TEST_CASE("level 3 construction owns the level-3 pyramid pool") {
    MemoryResource res(3);

    MemoryPool& pool = res.get_memory_pool();
    REQUIRE(pool.get_tier_count() >= 4u);

    auto stats_before = pool.get_stats();
    CHECK_EQ(stats_before.front().max_size, 32u);
    CHECK_EQ(stats_before.at(1).max_size, 64u);

    void* p = res.allocate(64);
    REQUIRE(p != nullptr);

    auto stats = pool.get_stats();
    CHECK_EQ(stats[0].hit_count, 0u);
    CHECK_EQ(stats[1].hit_count, 1u);
    CHECK_EQ(pool.get_oversized_stats().alloc_count, 0u);

    res.deallocate(p, 64);
  }

  TEST_CASE("custom config construction routes allocations to the matching tier") {
    MemoryPool::Config cfg;
    cfg.tiers = {{64, 4}, {256, 4}, {1024, 2}};
    MemoryResource res(cfg);

    void* a = res.allocate(48);
    void* b = res.allocate(200);
    void* c = res.allocate(900);

    auto stats = res.get_memory_pool().get_stats();
    CHECK_EQ(stats[0].hit_count, 1u);
    CHECK_EQ(stats[1].hit_count, 1u);
    CHECK_EQ(stats[2].hit_count, 1u);

    res.deallocate(a, 48);
    res.deallocate(b, 200);
    res.deallocate(c, 900);
  }

  TEST_CASE("global_instance returns the same reference every time") {
    MemoryResource& r1 = MemoryResource::global_instance();
    MemoryResource& r2 = MemoryResource::global_instance();
    CHECK_EQ(&r1, &r2);
  }

  TEST_CASE("global_instance aliases the global MemoryPool") {
    MemoryResource& res = MemoryResource::global_instance();
    CHECK_EQ(&res.get_memory_pool(), &MemoryPool::global_instance());
  }

  TEST_CASE("allocations via global_instance appear in global pool stats") {
    MemoryResource& res = MemoryResource::global_instance();
    MemoryPool& pool = MemoryPool::global_instance();

    auto over_before = pool.get_oversized_stats();

    static constexpr size_t kHuge = 32u * 1024u * 1024u;
    void* p = res.allocate(kHuge);
    REQUIRE(p != nullptr);

    auto over_after = pool.get_oversized_stats();
    CHECK_EQ(over_after.alloc_count, over_before.alloc_count + 1u);
    CHECK(over_after.alloc_bytes >= over_before.alloc_bytes + kHuge);

    res.deallocate(p, kHuge);
  }

  TEST_CASE("global_instance shares the pool used by Bytes::bytes_malloc") {
    MemoryResource& res = MemoryResource::global_instance();

    auto over_before = MemoryPool::global_instance().get_oversized_stats();

    static constexpr size_t kHuge = 32u * 1024u * 1024u;
    uint8_t* via_bytes = vlink::Bytes::bytes_malloc(kHuge);
    REQUIRE(via_bytes != nullptr);

    auto over_after_bytes = MemoryPool::global_instance().get_oversized_stats();
    CHECK_EQ(over_after_bytes.alloc_count, over_before.alloc_count + 1u);

    vlink::Bytes::bytes_free(via_bytes, kHuge);

    void* via_res = res.allocate(kHuge);
    REQUIRE(via_res != nullptr);

    auto over_after_res = MemoryPool::global_instance().get_oversized_stats();
    CHECK_EQ(over_after_res.alloc_count, over_after_bytes.alloc_count + 1u);

    res.deallocate(via_res, kHuge);
  }

  TEST_CASE("std::pmr::vector routes through the resource and deallocates on scope exit") {
    MemoryResource res(3);
    MemoryPool& pool = res.get_memory_pool();

    {
      std::pmr::vector<int> v(&res);
      v.reserve(256);

      for (int i = 0; i < 200; ++i) {
        v.push_back(i);
      }

      uint64_t hits = 0;
      for (const auto& s : pool.get_stats()) {
        hits += s.hit_count;
      }

      CHECK(hits >= 1u);
      CHECK_EQ(v.front(), 0);
      CHECK_EQ(v.back(), 199);
    }

    uint64_t hits = 0;
    uint64_t deallocs = 0;

    for (const auto& s : pool.get_stats()) {
      hits += s.hit_count;
      deallocs += s.deallocate_count;
    }

    CHECK_EQ(hits, deallocs);
  }

  TEST_CASE("polymorphic_allocator works with the resource") {
    MemoryResource res(3);

    std::pmr::polymorphic_allocator<char> alloc(&res);
    char* p = alloc.allocate(128);
    REQUIRE(p != nullptr);

    for (size_t i = 0; i < 128; ++i) {
      p[i] = static_cast<char>(i);
    }

    CHECK_EQ(p[0], 0);
    CHECK_EQ(p[127], 127);

    alloc.deallocate(p, 128);
  }

  TEST_CASE("two distinct resources are not equal") {
    MemoryResource a(3);
    MemoryResource b(3);

    CHECK_FALSE(a.is_equal(b));
    CHECK_FALSE(b.is_equal(a));
  }

  TEST_CASE("a resource is equal to itself") {
    MemoryResource a(3);
    CHECK(a.is_equal(a));
  }

  TEST_CASE("global_instance is equal to itself") {
    MemoryResource& g1 = MemoryResource::global_instance();
    MemoryResource& g2 = MemoryResource::global_instance();
    CHECK(g1.is_equal(g2));
  }

  TEST_CASE("private resource is not equal to global_instance") {
    MemoryResource a(3);
    MemoryResource& g = MemoryResource::global_instance();
    CHECK_FALSE(a.is_equal(g));
  }

  TEST_CASE("not equal to a foreign pmr memory_resource") {
    MemoryResource a;
    auto* sys = std::pmr::new_delete_resource();
    CHECK_FALSE(a.is_equal(*sys));
  }

  TEST_CASE("trim does not crash") {
    MemoryResource res(3);

    void* p = res.allocate(64);
    res.deallocate(p, 64);
    res.trim();
  }
}

#endif  // VLINK_ENABLE_BASE_MEMORY_RESOURCE

// NOLINTEND
