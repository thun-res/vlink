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

#include "./base/sys_semaphore.h"

#ifdef __linux__

#include <doctest/doctest.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <thread>

#include "../common_test.h"

namespace {

std::string make_sem_name(const char* tag) {
  return std::string("/vlink_test_") + tag + "_" + std::to_string(::getpid());
}

struct SemGuard {
  SysSemaphore& sem;

  ~SemGuard() {
    if (sem.is_attached()) {
      sem.detach(true);
    }
  }
};

}  // namespace

TEST_SUITE("base-SysSemaphore") {
  TEST_CASE("default constructed is not attached and count is zero") {
    SysSemaphore sem;

    CHECK_FALSE(sem.is_attached());
    CHECK_EQ(sem.get_count(), 0u);
  }

  TEST_CASE("kInfinite sentinel equals -1") { CHECK_EQ(SysSemaphore::kInfinite, -1); }

  TEST_CASE("attach creates semaphore and is_attached becomes true") {
    const std::string name = make_sem_name("attach");
    SysSemaphore sem(0);
    SemGuard guard{sem};

    CHECK(sem.attach(name));
    CHECK(sem.is_attached());
  }

  TEST_CASE("detach after attach succeeds and is_attached becomes false") {
    const std::string name = make_sem_name("detach");
    SysSemaphore sem(0);

    REQUIRE(sem.attach(name));

    bool ok = sem.detach(true);

    CHECK(ok);
    CHECK_FALSE(sem.is_attached());
  }

  TEST_CASE("detach when not attached returns false") {
    SysSemaphore sem;

    CHECK_FALSE(sem.detach(true));
    CHECK_FALSE(sem.detach(false));
  }

  TEST_CASE("get_count returns zero before attach") {
    SysSemaphore sem(5);

    CHECK_EQ(sem.get_count(), 0u);
  }

  TEST_CASE("initial count is reflected after attach") {
    const std::string name = make_sem_name("init_cnt");
    SysSemaphore sem(2);
    SemGuard guard{sem};

    REQUIRE(sem.attach(name));

    CHECK_EQ(sem.get_count(), 2u);
  }

  TEST_CASE("release increments count and acquire with zero timeout succeeds") {
    const std::string name = make_sem_name("rel_acq");
    SysSemaphore sem(0);
    SemGuard guard{sem};

    REQUIRE(sem.attach(name));

    CHECK_FALSE(sem.acquire(1, 0));

    sem.release(1);

    CHECK(sem.acquire(1, 0));
  }

  TEST_CASE("release multiple permits at once and acquire individually") {
    const std::string name = make_sem_name("rel_multi");
    SysSemaphore sem(0);
    SemGuard guard{sem};

    REQUIRE(sem.attach(name));

    sem.release(3);
    CHECK_EQ(sem.get_count(), 3u);

    CHECK(sem.acquire(1, 0));
    CHECK(sem.acquire(1, 0));
    CHECK(sem.acquire(1, 0));
    CHECK_FALSE(sem.acquire(1, 0));
  }

  TEST_CASE("acquire times out when count stays zero") {
    const std::string name = make_sem_name("timeout");
    SysSemaphore sem(0);
    SemGuard guard{sem};

    REQUIRE(sem.attach(name));

    auto t0 = std::chrono::steady_clock::now();
    bool ok = sem.acquire(1, 100);
    auto elapsed = std::chrono::steady_clock::now() - t0;

    CHECK_FALSE(ok);
    CHECK(elapsed >= 50ms);
    CHECK(elapsed < 2000ms);
  }

  TEST_CASE("cross-thread release unblocks acquire") {
    const std::string name = make_sem_name("xthread");
    SysSemaphore sem(0);
    SemGuard guard{sem};

    REQUIRE(sem.attach(name));

    std::atomic<bool> produced{false};

    std::thread producer([&sem, &produced]() {
      std::this_thread::sleep_for(50ms);
      produced.store(true, std::memory_order_release);
      sem.release(1);
    });

    bool ok = sem.acquire(1, 3000);

    producer.join();

    CHECK(ok);
    CHECK(produced.load(std::memory_order_acquire));
  }

  TEST_CASE("acquire with n greater than available times out and leaves count unchanged") {
    const std::string name = make_sem_name("multi_rollback");
    SysSemaphore sem(0);
    SemGuard guard{sem};

    REQUIRE(sem.attach(name));
    sem.release(1);
    CHECK_EQ(sem.get_count(), 1u);

    CHECK_FALSE(sem.acquire(2, 50));
    CHECK_EQ(sem.get_count(), 1u);
  }

  TEST_CASE("acquire n permits at once succeeds when count is sufficient") {
    const std::string name = make_sem_name("multi_acq");
    SysSemaphore sem(0);
    SemGuard guard{sem};

    REQUIRE(sem.attach(name));
    sem.release(3);

    bool ok = sem.acquire(3, 500);

    CHECK(ok);
    CHECK_EQ(sem.get_count(), 0u);
  }

  TEST_CASE("release zero does not change count") {
    const std::string name = make_sem_name("rel_zero");
    SysSemaphore sem(2);
    SemGuard guard{sem};

    REQUIRE(sem.attach(name));
    CHECK_EQ(sem.get_count(), 2u);

    sem.release(0);

    CHECK_EQ(sem.get_count(), 2u);
  }

  TEST_CASE("detach force=false closes handle but leaves semaphore in namespace") {
    const std::string name = make_sem_name("det_nof");
    SysSemaphore sem(1);

    REQUIRE(sem.attach(name));

    bool ok = sem.detach(false);

    CHECK(ok);
    CHECK_FALSE(sem.is_attached());

    SysSemaphore probe(0);
    bool reattached = probe.attach(name);

    CHECK(reattached);

    if (reattached) {
      CHECK_EQ(probe.get_count(), 1u);
      probe.detach(true);
    }
  }

  TEST_CASE("destructor auto-detaches without crash") {
    const std::string name = make_sem_name("dtor");

    {
      SysSemaphore sem(0);
      REQUIRE(sem.attach(name));
    }

    SysSemaphore probe(1);
    bool ok = probe.attach(name);

    CHECK(ok);

    if (ok) {
      probe.detach(true);
    }
  }
}

#endif  // __linux__

// NOLINTEND
