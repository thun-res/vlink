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

#include "./base/sys_sharemem.h"

#ifdef __linux__

#include <doctest/doctest.h>
#include <unistd.h>

#include <cstring>
#include <string>

#include "../common_test.h"

namespace {

std::string make_shm_name(const char* tag) {
  return std::string("/vlink_test_") + tag + "_" + std::to_string(::getpid());
}

struct ShmGuard {
  SysSharemem& shm;
  bool force;

  explicit ShmGuard(SysSharemem& s, bool f = true) : shm(s), force(f) {}

  ~ShmGuard() {
    if (shm.is_attached()) {
      shm.detach(force);
    }
  }
};

}  // namespace

TEST_SUITE("base-SysSharemem") {
  TEST_CASE("default constructed is not attached with null data and zero size") {
    SysSharemem shm;

    CHECK_FALSE(shm.is_attached());
    CHECK(shm.data() == nullptr);
    CHECK(static_cast<const SysSharemem&>(shm).data() == nullptr);
    CHECK_EQ(shm.size(), 0u);
  }

  TEST_CASE("mode enum values match documented integers") {
    CHECK_EQ(static_cast<int>(SysSharemem::kReadOnly), 0);
    CHECK_EQ(static_cast<int>(SysSharemem::kReadWrite), 1);
  }

  TEST_CASE("create returns true and produces mapped region with requested size") {
    const std::string name = make_shm_name("create");
    SysSharemem shm;
    ShmGuard guard{shm};

    static constexpr size_t kSize = 4096;
    bool ok = shm.create(name, kSize);

    CHECK(ok);
    CHECK(shm.is_attached());
    CHECK_EQ(shm.size(), kSize);
    CHECK(shm.data() != nullptr);
  }

  TEST_CASE("create then write then read back via same object") {
    const std::string name = make_shm_name("write_read");
    SysSharemem shm;
    ShmGuard guard{shm};

    static constexpr size_t kSize = 256;
    REQUIRE(shm.create(name, kSize));

    auto* ptr = static_cast<uint8_t*>(shm.data());
    REQUIRE(ptr != nullptr);

    for (size_t i = 0; i < kSize; ++i) {
      ptr[i] = static_cast<uint8_t>(i & 0xFFu);
    }

    const auto* cptr = static_cast<const uint8_t*>(static_cast<const SysSharemem&>(shm).data());
    REQUIRE(cptr != nullptr);

    for (size_t i = 0; i < kSize; ++i) {
      CHECK_EQ(cptr[i], static_cast<uint8_t>(i & 0xFFu));
    }
  }

  TEST_CASE("create then attach from second object sees same data") {
    const std::string name = make_shm_name("cre_att");
    SysSharemem creator;

    static constexpr size_t kSize = 1024;
    REQUIRE(creator.create(name, kSize));

    auto* wptr = static_cast<uint32_t*>(creator.data());
    REQUIRE(wptr != nullptr);
    *wptr = 0xDEADBEEFu;

    SysSharemem reader;
    bool att = reader.attach(name);

    CHECK(att);

    if (att) {
      CHECK(reader.is_attached());
      CHECK_EQ(reader.size(), kSize);

      const auto* rptr = static_cast<const uint32_t*>(reader.data());
      REQUIRE(rptr != nullptr);
      CHECK_EQ(*rptr, 0xDEADBEEFu);

      reader.detach(false);
      CHECK_FALSE(reader.is_attached());
    }

    creator.detach(true);
    CHECK_FALSE(creator.is_attached());
  }

  TEST_CASE("read-only attach exposes valid const data pointer") {
    const std::string name = make_shm_name("ro_att");
    SysSharemem creator;

    static constexpr size_t kSize = 512;
    REQUIRE(creator.create(name, kSize));

    auto* wptr = static_cast<uint8_t*>(creator.data());
    REQUIRE(wptr != nullptr);
    std::memset(wptr, 0xABu, kSize);

    SysSharemem reader;
    bool att = reader.attach(name, SysSharemem::kReadOnly);

    CHECK(att);

    if (att) {
      CHECK(reader.is_attached());
      CHECK_EQ(reader.size(), kSize);

      const auto* rptr = static_cast<const uint8_t*>(static_cast<const SysSharemem&>(reader).data());
      REQUIRE(rptr != nullptr);
      CHECK_EQ(rptr[0], uint8_t{0xABu});
      CHECK_EQ(rptr[kSize - 1], uint8_t{0xABu});

      reader.detach(false);
    }

    creator.detach(true);
  }

  TEST_CASE("detach when not attached returns false") {
    SysSharemem shm;

    CHECK_FALSE(shm.detach(true));
    CHECK_FALSE(shm.detach(false));
  }

  TEST_CASE("attach to non-existent name fails gracefully") {
    SysSharemem shm;
    const std::string name = make_shm_name("noexist_zzz");

    bool ok = shm.attach(name);

    CHECK_FALSE(ok);
    CHECK_FALSE(shm.is_attached());
    CHECK(shm.data() == nullptr);
    CHECK_EQ(shm.size(), 0u);
  }

  TEST_CASE("create fails when name already exists in namespace") {
    const std::string name = make_shm_name("replace");
    SysSharemem first;

    REQUIRE(first.create(name, 256));
    first.detach(false);

    SysSharemem second;
    bool ok = second.create(name, 512);

    CHECK_FALSE(ok);

    SysSharemem cleanup;

    if (cleanup.attach(name)) {
      cleanup.detach(true);
    }
  }

  TEST_CASE("data returns nullptr and size returns zero after detach") {
    const std::string name = make_shm_name("data_det");
    SysSharemem shm;

    REQUIRE(shm.create(name, 128));
    CHECK(shm.data() != nullptr);
    CHECK_EQ(shm.size(), 128u);

    shm.detach(true);

    CHECK(shm.data() == nullptr);
    CHECK_EQ(shm.size(), 0u);
  }

  TEST_CASE("detach force=false leaves object accessible in namespace") {
    const std::string name = make_shm_name("det_nof");
    SysSharemem creator;

    REQUIRE(creator.create(name, 256));
    CHECK(creator.detach(false));
    CHECK_FALSE(creator.is_attached());

    SysSharemem probe;
    bool ok = probe.attach(name);

    CHECK(ok);
    CHECK_EQ(probe.size(), 256u);

    probe.detach(true);
  }

  TEST_CASE("destructor auto-detaches without crash") {
    const std::string name = make_shm_name("dtor");

    {
      SysSharemem shm;
      REQUIRE(shm.create(name, 128));
      CHECK(shm.is_attached());
    }

    SysSharemem probe;
    bool ok = probe.attach(name);

    CHECK(ok);

    if (ok) {
      probe.detach(true);
    }
  }

  TEST_CASE("large region can be created written and read") {
    const std::string name = make_shm_name("large");
    SysSharemem shm;
    ShmGuard guard{shm};

    static constexpr size_t kOneMiB = 1024UL * 1024UL;
    bool ok = shm.create(name, kOneMiB);

    CHECK(ok);

    if (ok) {
      CHECK_EQ(shm.size(), kOneMiB);

      auto* ptr = static_cast<uint8_t*>(shm.data());
      REQUIRE(ptr != nullptr);
      ptr[0] = 0xAAu;
      ptr[kOneMiB - 1] = 0xBBu;

      const auto* cptr = static_cast<const uint8_t*>(static_cast<const SysSharemem&>(shm).data());
      CHECK_EQ(cptr[0], uint8_t{0xAAu});
      CHECK_EQ(cptr[kOneMiB - 1], uint8_t{0xBBu});
    }
  }

  TEST_CASE("kReadWrite mode allows mutation through data pointer") {
    const std::string name = make_shm_name("rw_mode");
    SysSharemem shm;
    ShmGuard guard{shm};

    REQUIRE(shm.create(name, 64, SysSharemem::kReadWrite));

    auto* ptr = static_cast<uint8_t*>(shm.data());
    REQUIRE(ptr != nullptr);
    ptr[0] = 0xFFu;

    CHECK_EQ(ptr[0], uint8_t{0xFFu});
  }

  TEST_CASE("multiple sequential create detach cycles succeed") {
    for (int i = 0; i < 3; ++i) {
      const std::string name = make_shm_name(("seq_" + std::to_string(i)).c_str());
      SysSharemem shm;

      bool ok = shm.create(name, 128);

      CHECK(ok);

      if (ok) {
        auto* ptr = static_cast<uint8_t*>(shm.data());
        REQUIRE(ptr != nullptr);
        ptr[0] = static_cast<uint8_t>(i);

        const auto* cptr = static_cast<const uint8_t*>(static_cast<const SysSharemem&>(shm).data());
        CHECK_EQ(cptr[0], static_cast<uint8_t>(i));

        shm.detach(true);
        CHECK_FALSE(shm.is_attached());
      }
    }
  }
}

#endif  // __linux__

// NOLINTEND
