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

#include "./base/functional.h"

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

#include "../common_test.h"

#ifdef VLINK_ENABLE_BASE_FUNCTIONAL

namespace {

struct MoveOnlyCallable {
  MoveOnlyCallable() = default;
  MoveOnlyCallable(const MoveOnlyCallable&) = delete;
  MoveOnlyCallable(MoveOnlyCallable&&) noexcept = default;
  MoveOnlyCallable& operator=(const MoveOnlyCallable&) = delete;
  MoveOnlyCallable& operator=(MoveOnlyCallable&&) noexcept = default;
  void operator()() const {}
};

static_assert(!std::is_constructible_v<Function<void()>, MoveOnlyCallable>, "Function must reject move-only targets");
static_assert(!std::is_assignable_v<Function<void()>&, MoveOnlyCallable>,
              "Function assignment must reject move-only targets");

struct UniquePtrCallable {
  std::unique_ptr<int> data;

  UniquePtrCallable() = default;
  explicit UniquePtrCallable(int v) : data(std::make_unique<int>(v)) {}

  UniquePtrCallable(const UniquePtrCallable&) = delete;
  UniquePtrCallable(UniquePtrCallable&&) noexcept = default;
  UniquePtrCallable& operator=(const UniquePtrCallable&) = delete;
  UniquePtrCallable& operator=(UniquePtrCallable&&) noexcept = default;

  int operator()() const { return data ? *data : -1; }
};

static_assert(!std::is_copy_constructible_v<MoveFunction<int()>>, "MoveFunction must be move-only");
static_assert(!std::is_copy_assignable_v<MoveFunction<int()>>, "MoveFunction must be move-only");
static_assert(std::is_nothrow_move_constructible_v<MoveFunction<int()>>);
static_assert(std::is_nothrow_move_assignable_v<MoveFunction<int()>>);

int free_fn_add_100(int x) { return x + 100; }

}  // namespace

TEST_SUITE("base-Function") {
  TEST_CASE("default construction yields empty wrapper") {
    Function<void()> cb;

    CHECK_FALSE(static_cast<bool>(cb));
    CHECK(cb == nullptr);
    CHECK(nullptr == cb);
  }

  TEST_CASE("nullptr construction yields empty wrapper") {
    Function<int(int)> cb(nullptr);

    CHECK_FALSE(static_cast<bool>(cb));
    CHECK(cb == nullptr);
    CHECK(nullptr == cb);
    CHECK_THROWS_AS(cb(0), std::bad_function_call);
  }

  TEST_CASE("construction from lambda produces callable non-empty wrapper") {
    Function<int(int)> cb = [](int x) { return x * 2; };

    CHECK(static_cast<bool>(cb));
    CHECK(cb != nullptr);
    CHECK_EQ(cb(21), 42);
  }

  TEST_CASE("kSboSize equals the SboSizeT template argument") {
    CHECK_EQ(Function<void()>::kSboSize, 64u);
    CHECK_EQ((Function<void(), 128>::kSboSize), 128u);
    CHECK_EQ((Function<void(), 256>::kSboSize), 256u);
  }

  TEST_CASE("small lambda fits inline storage") {
    int counter = 0;
    Function<void()> cb = [&counter]() { ++counter; };

    cb();
    cb();
    cb();

    CHECK_EQ(counter, 3);
  }

  TEST_CASE("large lambda exceeding 64 bytes uses heap path") {
    std::array<int, 32> payload{};
    for (size_t i = 0; i < payload.size(); ++i) {
      payload[i] = static_cast<int>(i);
    }
    Function<int()> cb = [payload]() {
      int sum = 0;
      for (int v : payload) {
        sum += v;
      }
      return sum;
    };

    CHECK_EQ(cb(), (31 * 32) / 2);
  }

  TEST_CASE("copy construction duplicates callable") {
    Function<int(int)> a = [](int x) { return x + 1; };
    Function<int(int)> b(a);

    CHECK_EQ(a(10), 11);
    CHECK_EQ(b(10), 11);
  }

  TEST_CASE("move construction transfers ownership and empties source") {
    Function<int(int)> a = [](int x) { return x + 5; };
    Function<int(int)> b(std::move(a));

    CHECK_EQ(b(7), 12);
    CHECK_FALSE(static_cast<bool>(a));
  }

  TEST_CASE("copy assignment replaces existing target") {
    Function<int()> a = []() { return 1; };
    Function<int()> b = []() { return 2; };

    a = b;

    CHECK_EQ(a(), 2);
    CHECK_EQ(b(), 2);
  }

  TEST_CASE("move assignment replaces target and empties source") {
    Function<int()> a = []() { return 1; };
    Function<int()> b = []() { return 2; };

    a = std::move(b);

    CHECK_EQ(a(), 2);
    CHECK_FALSE(static_cast<bool>(b));
  }

  TEST_CASE("nullptr assignment clears the stored callable") {
    Function<int()> cb = []() { return 42; };
    cb = nullptr;
    CHECK_FALSE(static_cast<bool>(cb));
  }

  TEST_CASE("self copy assignment is safe") {
    Function<int()> cb = []() { return 99; };
    Function<int()>& ref = cb;  // alias to avoid -Wself-assign-overloaded
    cb = ref;
    CHECK_EQ(cb(), 99);
  }

  TEST_CASE("self move assignment is safe") {
    Function<int()> cb = []() { return 99; };
    Function<int()>& ref = cb;  // alias to avoid -Wself-move
    cb = std::move(ref);
    CHECK_EQ(cb(), 99);
  }

  TEST_CASE("swap exchanges callable targets") {
    Function<int()> a = []() { return 1; };
    Function<int()> b = []() { return 2; };

    swap(a, b);

    CHECK_EQ(a(), 2);
    CHECK_EQ(b(), 1);
  }

  TEST_CASE("member swap with empty transfers ownership") {
    Function<int()> a = []() { return 7; };
    Function<int()> b;

    a.swap(b);

    CHECK_FALSE(static_cast<bool>(a));
    CHECK(static_cast<bool>(b));
    CHECK_EQ(b(), 7);
  }

  TEST_CASE("copy from empty yields empty") {
    Function<int()> a;
    Function<int()> b(a);
    CHECK_FALSE(static_cast<bool>(a));
    CHECK_FALSE(static_cast<bool>(b));
  }

  TEST_CASE("move from empty yields empty") {
    Function<int()> a;
    Function<int()> b(std::move(a));
    CHECK_FALSE(static_cast<bool>(a));
    CHECK_FALSE(static_cast<bool>(b));
  }

  TEST_CASE("wraps free function pointer") {
    Function<int(int)> cb = &free_fn_add_100;
    CHECK_EQ(cb(2), 102);
  }

  TEST_CASE("null function pointer construction yields empty wrapper") {
    using Fn = int (*)(int);
    Fn fn = nullptr;
    Function<int(int)> cb(fn);
    CHECK_FALSE(static_cast<bool>(cb));
  }

  TEST_CASE("wraps mutable stateful lambda and preserves state") {
    Function<int()> cb = [counter = 0]() mutable { return ++counter; };

    CHECK_EQ(cb(), 1);
    CHECK_EQ(cb(), 2);
    CHECK_EQ(cb(), 3);
  }

  TEST_CASE("operator() on const reference works") {
    const Function<int()> cb = []() { return 13; };
    CHECK_EQ(cb(), 13);
  }

  TEST_CASE("exception from target propagates") {
    Function<void()> cb = []() { throw std::runtime_error("boom"); };
    CHECK_THROWS_AS(cb(), std::runtime_error);
  }

  TEST_CASE("destroying function releases shared_ptr capture") {
    auto resource = std::make_shared<int>(123);
    CHECK_EQ(resource.use_count(), 1);

    {
      Function<int()> cb = [resource]() { return *resource; };
      CHECK_EQ(resource.use_count(), 2);
      CHECK_EQ(cb(), 123);
    }

    CHECK_EQ(resource.use_count(), 1);
  }

  TEST_CASE("copying function bumps shared_ptr use count") {
    auto resource = std::make_shared<int>(0);
    Function<void()> a = [resource]() { ++(*resource); };

    CHECK_EQ(resource.use_count(), 2);

    Function<void()> b(a);
    CHECK_EQ(resource.use_count(), 3);

    a();
    b();
    CHECK_EQ(*resource, 2);
  }

  TEST_CASE("moving function does not bump shared_ptr use count") {
    auto resource = std::make_shared<int>(0);
    Function<void()> a = [resource]() { ++(*resource); };

    CHECK_EQ(resource.use_count(), 2);

    Function<void()> b(std::move(a));

    CHECK_EQ(resource.use_count(), 2);
  }

  TEST_CASE("forwards rvalue-reference argument") {
    Function<int(std::unique_ptr<int>)> cb = [](std::unique_ptr<int> p) { return *p; };
    CHECK_EQ(cb(std::make_unique<int>(42)), 42);
  }

  TEST_CASE("returns std::unique_ptr from wrapped lambda") {
    Function<std::unique_ptr<int>()> cb = []() { return std::make_unique<int>(42); };
    auto p = cb();
    REQUIRE(p != nullptr);
    CHECK_EQ(*p, 42);
  }

  TEST_CASE("wraps std::function and wraps it back") {
    std::function<int(int)> stdfn = [](int x) { return x * 3; };
    Function<int(int)> cb = stdfn;
    CHECK_EQ(cb(4), 12);
  }

  TEST_CASE("empty std::function constructs empty Function") {
    std::function<int(int)> stdfn;
    Function<int(int)> cb = stdfn;

    CHECK_FALSE(static_cast<bool>(cb));
    CHECK_THROWS_AS(cb(1), std::bad_function_call);
  }

  TEST_CASE("assigning empty std::function clears Function") {
    Function<int()> cb = []() { return 7; };
    std::function<int()> stdfn;
    cb = stdfn;
    CHECK_FALSE(static_cast<bool>(cb));
  }

  TEST_CASE("round trip Function to std::function and back") {
    Function<int(int)> a = [](int x) { return x * x; };
    std::function<int(int)> mid = a;
    Function<int(int)> b = mid;
    CHECK_EQ(b(11), 121);
  }

  TEST_CASE("different SboSizeT yields distinct types that remain interchangeable") {
    using Small = Function<void(), 64>;
    using Big = Function<void(), 256>;
    CHECK_FALSE((std::is_same_v<Small, Big>));

    CHECK((std::is_constructible_v<Big, const Small&>));
    CHECK((std::is_constructible_v<Small, const Big&>));

    int counter = 0;
    Small small_fn = [&counter]() { ++counter; };

    Big big_from_small(small_fn);
    big_from_small();
    CHECK_EQ(counter, 1);
  }

  TEST_CASE("heap target is destroyed on function destruction") {
    auto count = std::make_shared<std::atomic<int>>(0);

    struct LargePayload {
      std::array<char, 512> bytes{};
      std::shared_ptr<std::atomic<int>> count;

      explicit LargePayload(std::shared_ptr<std::atomic<int>> c) : count(std::move(c)) {}
      LargePayload(const LargePayload&) = default;
      LargePayload(LargePayload&&) noexcept = default;
      LargePayload& operator=(const LargePayload&) = default;
      LargePayload& operator=(LargePayload&&) noexcept = default;
      ~LargePayload() {
        if (count) {
          count->fetch_add(1, std::memory_order_relaxed);
        }
      }
      void operator()() const {}
    };

    {
      Function<void()> cb = LargePayload(count);
      cb();
    }

    CHECK(count->load() >= 1);
  }

  TEST_CASE("copy-assign with throwing copy ctor leaves destination empty") {
    struct ThrowOnCopy {
      bool* flag{nullptr};
      explicit ThrowOnCopy(bool* f) : flag(f) {}
      ThrowOnCopy(const ThrowOnCopy& o) : flag(o.flag) {
        if (flag && *flag) {
          throw std::runtime_error("copy threw");
        }
      }
      ThrowOnCopy(ThrowOnCopy&&) noexcept = default;
      ThrowOnCopy& operator=(const ThrowOnCopy&) = default;
      ThrowOnCopy& operator=(ThrowOnCopy&&) noexcept = default;
      void operator()() const {}
    };

    bool throw_flag = false;
    Function<void()> a = ThrowOnCopy{&throw_flag};

    throw_flag = true;
    Function<void()> b;
    CHECK_THROWS_AS(b = a, std::runtime_error);
    CHECK_FALSE(static_cast<bool>(b));
    CHECK(static_cast<bool>(a));
  }

#if defined(__cpp_rtti)
  TEST_CASE("target_type returns typeid(void) when empty") {
    Function<int()> cb;
    CHECK(cb.target_type() == typeid(void));
  }

  TEST_CASE("target_type and target return stored callable") {
    auto fn = [base = 4](int x) { return base + x; };
    using Fn = decltype(fn);

    Function<int(int)> cb = fn;
    CHECK(cb.target_type() == typeid(Fn));
    REQUIRE(cb.target<Fn>() != nullptr);
    CHECK_EQ((*cb.target<Fn>())(5), 9);
  }

  TEST_CASE("target returns null for wrong type") {
    Function<int(int)> cb = [](int x) { return x; };
    CHECK(cb.target<int (*)(int)>() == nullptr);
  }
#endif
}

TEST_SUITE("base-MoveFunction") {
  TEST_CASE("default construction yields empty wrapper") {
    MoveFunction<void()> cb;

    CHECK_FALSE(static_cast<bool>(cb));
    CHECK(cb == nullptr);
    CHECK(nullptr == cb);
    CHECK_THROWS_AS(cb(), std::bad_function_call);
  }

  TEST_CASE("nullptr construction yields empty wrapper") {
    MoveFunction<int(int)> cb(nullptr);
    CHECK_FALSE(static_cast<bool>(cb));
    CHECK(cb == nullptr);
  }

  TEST_CASE("construction from copyable lambda produces callable wrapper") {
    MoveFunction<int(int)> cb = [](int x) { return x * 2; };
    CHECK(static_cast<bool>(cb));
    CHECK_EQ(cb(21), 42);
  }

  TEST_CASE("construction from move-only lambda with unique_ptr capture") {
    auto up = std::make_unique<int>(42);
    MoveFunction<int()> cb = [p = std::move(up)]() { return *p; };
    CHECK_EQ(cb(), 42);
  }

  TEST_CASE("construction from move-only callable struct") {
    MoveFunction<int()> cb = UniquePtrCallable{7};
    CHECK_EQ(cb(), 7);
  }

  TEST_CASE("wraps std::packaged_task without shared_ptr trampoline") {
    std::packaged_task<int()> task([] { return 100; });
    auto fut = task.get_future();

    MoveFunction<void()> cb = [t = std::move(task)]() mutable { t(); };
    cb();

    CHECK_EQ(fut.get(), 100);
  }

  TEST_CASE("move construction transfers ownership and empties source") {
    MoveFunction<int()> a = [] { return 5; };
    MoveFunction<int()> b = std::move(a);

    CHECK_FALSE(static_cast<bool>(a));
    CHECK(static_cast<bool>(b));
    CHECK_EQ(b(), 5);
  }

  TEST_CASE("move assignment replaces target and empties source") {
    MoveFunction<int()> a = [] { return 5; };
    MoveFunction<int()> b = [] { return 10; };

    b = std::move(a);

    CHECK_FALSE(static_cast<bool>(a));
    CHECK_EQ(b(), 5);
  }

  TEST_CASE("nullptr assignment clears callable") {
    MoveFunction<int()> a = [] { return 9; };
    a = nullptr;
    CHECK_FALSE(static_cast<bool>(a));
  }

  TEST_CASE("self move assignment is safe") {
    MoveFunction<int()> a = [] { return 7; };
    auto& ref = a;
    a = std::move(ref);
    CHECK(static_cast<bool>(a));
    CHECK_EQ(a(), 7);
  }

  TEST_CASE("member swap exchanges targets") {
    MoveFunction<int()> a = [] { return 1; };
    MoveFunction<int()> b = [] { return 2; };

    a.swap(b);

    CHECK_EQ(a(), 2);
    CHECK_EQ(b(), 1);
  }

  TEST_CASE("empty std::function becomes empty MoveFunction") {
    std::function<int()> sf;
    MoveFunction<int()> mf = sf;
    CHECK_FALSE(static_cast<bool>(mf));
    CHECK_THROWS_AS(mf(), std::bad_function_call);
  }

  TEST_CASE("null function pointer becomes empty MoveFunction") {
    using FnPtr = int (*)();
    FnPtr p = nullptr;
    MoveFunction<int()> mf = p;
    CHECK_FALSE(static_cast<bool>(mf));
  }

  TEST_CASE("Function rvalue converts to MoveFunction") {
    Function<int()> f = [] { return 42; };
    MoveFunction<int()> mf = std::move(f);
    CHECK_EQ(mf(), 42);
  }

  TEST_CASE("Function lvalue converts to MoveFunction via copy") {
    Function<int()> f = [] { return 21; };
    MoveFunction<int()> mf = f;
    CHECK_EQ(mf(), 21);
    CHECK(static_cast<bool>(f));
  }

  TEST_CASE("mutable lambda preserves state across calls") {
    MoveFunction<int()> cb = [n = 0]() mutable { return ++n; };
    CHECK_EQ(cb(), 1);
    CHECK_EQ(cb(), 2);
    CHECK_EQ(cb(), 3);
  }

  TEST_CASE("large move-only target uses heap path and survives move") {
    struct Heavy {
      std::unique_ptr<int> p;
      std::array<int, 32> pad{};
      Heavy() : p(std::make_unique<int>(99)) {}
      Heavy(const Heavy&) = delete;
      Heavy(Heavy&&) noexcept = default;
      Heavy& operator=(const Heavy&) = delete;
      Heavy& operator=(Heavy&&) noexcept = default;
      int operator()() const { return *p + static_cast<int>(pad.size()); }
    };

    MoveFunction<int()> cb = Heavy{};
    CHECK_EQ(cb(), 99 + 32);

    MoveFunction<int()> moved = std::move(cb);
    CHECK_FALSE(static_cast<bool>(cb));
    CHECK_EQ(moved(), 99 + 32);
  }

  TEST_CASE("kSboSize equals the SboSizeT template argument") {
    CHECK_EQ(MoveFunction<void()>::kSboSize, 64u);
    CHECK_EQ((MoveFunction<void(), 256>::kSboSize), 256u);
  }

#if defined(__cpp_rtti)
  TEST_CASE("target_type returns typeid(void) when empty") {
    MoveFunction<int()> cb;
    CHECK(cb.target_type() == typeid(void));
    CHECK(cb.target<int (*)()>() == nullptr);
  }

  TEST_CASE("target_type and target return stored callable") {
    auto fn = [base = 4](int x) { return base + x; };
    using Fn = decltype(fn);

    MoveFunction<int(int)> cb = fn;
    CHECK(cb.target_type() == typeid(Fn));
    REQUIRE(cb.target<Fn>() != nullptr);
    CHECK_EQ((*cb.target<Fn>())(5), 9);
  }

  TEST_CASE("moved-from MoveFunction reports empty target") {
    MoveFunction<int()> a = [] { return 5; };
    MoveFunction<int()> b = std::move(a);

    CHECK(a.target_type() == typeid(void));
    CHECK(a.target<int (*)()>() == nullptr);
  }
#endif

#if defined(__cpp_lib_move_only_function) && __cpp_lib_move_only_function >= 202110L
  TEST_CASE("empty std::move_only_function yields empty MoveFunction") {
    std::move_only_function<int()> sm;
    MoveFunction<int()> mf = std::move(sm);
    CHECK_FALSE(static_cast<bool>(mf));
  }

  TEST_CASE("non-empty std::move_only_function moves into MoveFunction") {
    std::move_only_function<int()> sm = [] { return 13; };
    MoveFunction<int()> mf = std::move(sm);
    CHECK_EQ(mf(), 13);
  }
#endif
}

TEST_SUITE("base-LargeFunction") {
  TEST_CASE("LargeFunction is exactly Function<Sig, 256>") {
    CHECK((std::is_same_v<vlink::LargeFunction<void()>, vlink::Function<void(), 256>>));
    CHECK_EQ(vlink::LargeFunction<void()>::kSboSize, 256u);
  }

  TEST_CASE("LargeMoveFunction is exactly MoveFunction<Sig, 256>") {
    CHECK((std::is_same_v<vlink::LargeMoveFunction<void()>, vlink::MoveFunction<void(), 256>>));
    CHECK_EQ(vlink::LargeMoveFunction<void()>::kSboSize, 256u);
  }

  TEST_CASE("LargeFunction holds a 200-byte functor inline") {
    struct HeavyFunctor {
      std::array<char, 200> payload{};
      int marker;

      explicit HeavyFunctor(int m) noexcept : marker(m) { payload.fill('L'); }

      int operator()() const noexcept { return marker + static_cast<int>(payload[0]); }
    };
    static_assert(sizeof(HeavyFunctor) > 64u);
    static_assert(sizeof(HeavyFunctor) <= 256u);

    vlink::LargeFunction<int()> cb = HeavyFunctor{1};
    CHECK_EQ(cb(), 1 + 'L');
  }

  TEST_CASE("LargeMoveFunction supports move-only state") {
    auto sentinel = std::make_unique<int>(42);
    vlink::LargeMoveFunction<int()> cb = [sp = std::move(sentinel)]() mutable { return *sp + 1; };
    CHECK_EQ(cb(), 43);

    vlink::LargeMoveFunction<int()> moved = std::move(cb);
    CHECK_EQ(moved(), 43);
    CHECK_FALSE(static_cast<bool>(cb));
  }
}

#endif

// NOLINTEND
