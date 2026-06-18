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
 * @file object_pool.h
 * @brief Thread-safe generic object pool with RAII handles and a tunable reset policy.
 *
 * @details
 * @c ObjectPool<T> recycles previously allocated @c T instances so that hot paths avoid
 * repeated heap traffic.  Callers acquire an object through one of three entry points,
 * use it, and then either let RAII return it automatically or hand it back manually.
 *
 * Acquisition variants:
 *
 * | Method         | Returned handle                    | Returns to pool automatically       |
 * | -------------- | ---------------------------------- | ----------------------------------- |
 * | @c get()       | @c unique_ptr<T, PoolDeleter>      | Yes, on @c unique_ptr destruction   |
 * | @c get_shared  | @c shared_ptr<T>                   | Yes, on last @c shared_ptr release  |
 * | @c borrow()    | Raw @c T*                          | No, caller must invoke @c give_back |
 *
 * Reset-policy decision table for the optional @c ResetCallback:
 *
 * | Policy             | Reset on acquire | Reset on release | Typical use                       |
 * | ------------------ | ---------------- | ---------------- | --------------------------------- |
 * | @c kPolicyNone     | No               | No               | Pure / stateless objects          |
 * | @c kPolicyRelease  | No               | Yes              | Default; scrub on the return path |
 * | @c kPolicyAcquire  | Yes              | No               | Scrub right before use            |
 * | @c kPolicyBoth     | Yes              | Yes              | Defence-in-depth scrubbing        |
 *
 * Lifecycle of a pooled object:
 *
 * @verbatim
 *   factory ---> free list <----------+
 *                  |  acquire         |
 *                  v                  |
 *               in use ---> release --+   (reset on release if enabled)
 *                  ^                  |
 *                  +--reset-on-acquire+
 * @endverbatim
 *
 * @par Thread safety
 * Every public method acquires the internal mutex so concurrent callers may
 * interleave freely.  Factory and reset callbacks run outside the mutex.
 *
 * @par Example
 * @code
 * auto pool = std::make_shared<vlink::ObjectPool<Buffer>>(
 *     []{ return std::make_unique<Buffer>(4096); },
 *     4,
 *     16,
 *     [](Buffer& buf){ buf.clear(); },
 *     vlink::ObjectPool<Buffer>::kPolicyRelease);
 *
 * {
 *   auto buf = pool->get();
 *   buf->write(payload.data(), payload.size());
 * }
 *
 * Buffer* raw = pool->borrow();
 * raw->write(payload.data(), payload.size());
 * pool->give_back(raw);
 * @endcode
 */

#pragma once

#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "./functional.h"
#include "./macros.h"

namespace vlink {

/**
 * @class ObjectPoolBase
 * @brief Type-independent base of @c ObjectPool that owns counters, mutex and policy state.
 *
 * @details
 * Keeps the shared bookkeeping for @c ObjectPool<T> in a single translation unit so that
 * cold-path validation and error-throwing helpers do not need to be re-instantiated for
 * every element type.  Element storage and callbacks remain in the derived template.
 */
class VLINK_EXPORT ObjectPoolBase {
 public:
  /**
   * @brief Decides when (or whether) the user-supplied @c ResetCallback runs.
   *
   * @details
   * Reset failures on the release path are caught and the offending object is dropped from
   * the pool instead of being recycled.  Failures on the acquire path are rethrown after
   * the object has been put back on the free list.
   */
  enum Policy : uint8_t {
    kPolicyNone = 0,     ///< Never invoke the reset callback.
    kPolicyRelease = 1,  ///< Reset on the return-to-pool path.
    kPolicyAcquire = 2,  ///< Reset on the hand-out-to-caller path.
    kPolicyBoth = 3,     ///< Reset on both paths.
  };

  /**
   * @struct Stats
   * @brief Point-in-time snapshot of internal pool counters.
   */
  struct Stats final {
    size_t pool_size{0};      ///< Objects currently idle on the free list.
    size_t borrowed{0};       ///< Objects currently held by callers.
    size_t total_created{0};  ///< Cumulative objects ever produced by the factory.
    size_t max_size{0};       ///< Configured upper bound; @c 0 means unlimited.
  };

  /**
   * @brief Returns the number of objects currently checked out of the pool.
   *
   * @return Borrowed object count.
   */
  [[nodiscard]] size_t borrowed() const;

  /**
   * @brief Returns the cumulative number of objects ever produced by the factory.
   *
   * @return Total created count.
   */
  [[nodiscard]] size_t total_created() const;

  /**
   * @brief Returns the configured upper bound on live objects.
   *
   * @return Maximum size; @c 0 means unlimited.
   */
  [[nodiscard]] size_t max_size() const noexcept;

 protected:
  ObjectPoolBase(size_t max_size, size_t initial_size, Policy policy);

  ~ObjectPoolBase() noexcept = default;

  void safe_dec_borrowed_and_live() noexcept;

  [[nodiscard]] bool should_reset_on_acquire() const noexcept;

  [[nodiscard]] bool should_reset_on_release() const noexcept;

  [[noreturn]] static void throw_invalid_size();

  [[noreturn]] static void throw_factory_null_pre_fill();

  [[noreturn]] static void throw_factory_null();

  [[noreturn]] void throw_exhausted(size_t pool_size) const;

  Policy policy_{kPolicyNone};
  size_t max_size_{0};

  mutable std::mutex mutex_;

  size_t borrowed_{0};
  size_t live_count_{0};
  size_t total_created_{0};
};

/**
 * @class ObjectPool
 * @brief Thread-safe recycling pool for instances of @p T with RAII acquisition.
 *
 * @details
 * Must always live behind a @c std::shared_ptr because @c PoolDeleter holds a
 * @c std::weak_ptr back to the owning pool; constructing one on the stack leaks
 * outstanding RAII handles into a dangling pool.  Use @c std::make_shared<ObjectPool<T>>().
 *
 * @tparam T  Pooled element type.  Must be default-constructible unless a non-default
 *            factory callback is supplied.
 */
template <typename T>
class ObjectPool : public ObjectPoolBase, public std::enable_shared_from_this<ObjectPool<T>> {
 public:
  /**
   * @brief Signature of the factory used to grow the pool on demand.
   *
   * @details
   * A non-null @c std::unique_ptr<T> must be returned; @c nullptr or thrown exceptions
   * are propagated to the caller of @c get / @c get_shared / @c borrow.
   */
  using FactoryCallback = MoveFunction<std::unique_ptr<T>()>;

  /**
   * @brief Signature of the reset hook used to scrub objects on acquire and/or release.
   *
   * @details
   * Exceptions thrown on the release path are swallowed and the affected object is
   * discarded.  Exceptions on the acquire path return the object to the pool and
   * rethrow to the caller.
   */
  using ResetCallback = MoveFunction<void(T&)>;

  /**
   * @struct PoolDeleter
   * @brief Custom deleter installed on every RAII handle handed out by @c get / @c get_shared.
   *
   * @details
   * Holds a non-owning @c weak_ptr to the parent pool.  When the handle is destroyed,
   * the deleter either returns the object via @c release() (when the pool is alive)
   * or falls back to @c operator @c delete (when the pool has already been torn down).
   */
  struct PoolDeleter final {
    /**
     * @brief Returns @p ptr to the pool, or deletes it when the pool is gone.
     *
     * @param ptr  Raw pointer to the object being released; @c nullptr is silently ignored.
     */
    void operator()(T* ptr) const noexcept;
    std::weak_ptr<ObjectPool<T>> weak_pool;  ///< Weak reference to the parent pool.
  };

  /**
   * @brief Constructs the pool and optionally pre-warms the free list.
   *
   * @param factory_callback  Factory used to grow the pool.  Default: @c std::make_unique<T>().
   * @param initial_size      Objects to allocate eagerly at construction time.
   * @param max_size          Cap on the number of live objects.  @c 0 means unlimited.
   * @param reset_callback    Optional scrubbing hook driven by @p policy.
   * @param policy            When the reset hook fires.  Default: @c kPolicyRelease.
   *
   * @throws std::invalid_argument when @p initial_size exceeds a non-zero @p max_size.
   * @throws std::runtime_error    when the factory returns @c nullptr while pre-filling.
   *
   * @note Always construct via @c std::make_shared<ObjectPool<T>>() so that
   *       @c PoolDeleter can resolve its @c weak_ptr.
   */
  explicit ObjectPool(FactoryCallback factory_callback = get_default_factory(), size_t initial_size = 0,
                      size_t max_size = 0, ResetCallback reset_callback = nullptr, Policy policy = kPolicyRelease);

  /**
   * @brief Acquires an object wrapped in a @c unique_ptr with automatic return.
   *
   * @return RAII handle whose deleter releases the object back to the pool.
   *
   * @throws std::runtime_error when the pool is exhausted or the factory fails.
   */
  [[nodiscard]] std::unique_ptr<T, typename ObjectPool<T>::PoolDeleter> get();

  /**
   * @brief Acquires an object wrapped in a @c shared_ptr with automatic return.
   *
   * @return RAII handle whose deleter releases the object back to the pool.
   *
   * @throws std::runtime_error when the pool is exhausted or the factory fails.
   */
  [[nodiscard]] std::shared_ptr<T> get_shared();

  /**
   * @brief Acquires an object as a raw pointer; the caller owns the return path.
   *
   * @return Raw pointer that must later be returned via @c give_back().
   *
   * @throws std::runtime_error when the pool is exhausted or the factory fails.
   */
  [[nodiscard]] T* borrow();

  /**
   * @brief Returns an object obtained from @c borrow() back to the pool.
   *
   * @param ptr  Raw pointer previously returned by @c borrow(); @c nullptr is ignored.
   */
  void give_back(T* ptr);

  /**
   * @brief Captures a thread-safe snapshot of pool statistics.
   *
   * @return Filled @c Stats value.
   */
  [[nodiscard]] Stats stats() const;

  /**
   * @brief Returns the number of objects currently idle on the free list.
   *
   * @return Idle object count.
   */
  [[nodiscard]] size_t size() const;

 private:
  static FactoryCallback get_default_factory();

  std::unique_ptr<T> acquire();

  void release(std::unique_ptr<T> obj) noexcept;

  FactoryCallback factory_callback_;
  ResetCallback reset_callback_;
  std::vector<std::unique_ptr<T>> pool_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(ObjectPool)
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

template <typename T>
inline ObjectPool<T>::ObjectPool(FactoryCallback factory_callback, size_t initial_size, size_t max_size,
                                 ResetCallback reset_callback, Policy policy)
    : ObjectPoolBase(max_size, initial_size, policy),
      factory_callback_(std::move(factory_callback)),
      reset_callback_(std::move(reset_callback)) {
  pool_.reserve(initial_size);
  for (size_t i = 0; i < initial_size; ++i) {
    auto obj = factory_callback_();

    if VUNLIKELY (!obj) {
      throw_factory_null_pre_fill();
    }

    if (should_reset_on_release() && reset_callback_) {
      reset_callback_(*obj);
    }

    pool_.emplace_back(std::move(obj));
  }

  total_created_ = initial_size;
  live_count_ = initial_size;
}

template <typename T>
inline std::unique_ptr<T, typename ObjectPool<T>::PoolDeleter> ObjectPool<T>::get() {
  std::unique_ptr<T> obj = acquire();

  try {
    if (should_reset_on_acquire() && reset_callback_) {
      reset_callback_(*obj);
    }
  } catch (...) {
    release(std::move(obj));
    throw;
  }

  return {obj.release(), PoolDeleter{this->weak_from_this()}};
}

template <typename T>
inline std::shared_ptr<T> ObjectPool<T>::get_shared() {
  std::unique_ptr<T> obj = acquire();

  try {
    if (should_reset_on_acquire() && reset_callback_) {
      reset_callback_(*obj);
    }
  } catch (...) {
    release(std::move(obj));
    throw;
  }

  return {obj.release(), PoolDeleter{this->weak_from_this()}};
}

template <typename T>
inline T* ObjectPool<T>::borrow() {
  std::unique_ptr<T> obj = acquire();

  try {
    if (should_reset_on_acquire() && reset_callback_) {
      reset_callback_(*obj);
    }
  } catch (...) {
    release(std::move(obj));
    throw;
  }

  return obj.release();
}

template <typename T>
inline void ObjectPool<T>::give_back(T* ptr) {
  if VUNLIKELY (!ptr) {
    return;
  }

  std::unique_ptr<T> u(ptr);
  release(std::move(u));
}

template <typename T>
inline ObjectPoolBase::Stats ObjectPool<T>::stats() const {
  std::lock_guard lock(mutex_);
  return {pool_.size(), borrowed_, total_created_, max_size_};
}

template <typename T>
inline size_t ObjectPool<T>::size() const {
  std::lock_guard lock(mutex_);
  return pool_.size();
}

template <typename T>
inline typename ObjectPool<T>::FactoryCallback ObjectPool<T>::get_default_factory() {
  return [] { return std::make_unique<T>(); };
}

template <typename T>
inline std::unique_ptr<T> ObjectPool<T>::acquire() {
  std::unique_lock lock(mutex_);

  if VLIKELY (!pool_.empty()) {
    std::unique_ptr<T> obj = std::move(pool_.back());
    pool_.pop_back();
    ++borrowed_;
    return obj;
  }

  if VUNLIKELY (max_size_ > 0 && live_count_ >= max_size_) {
    throw_exhausted(pool_.size());
  }

  ++live_count_;
  ++total_created_;
  ++borrowed_;

  lock.unlock();

  std::unique_ptr<T> new_obj;
  try {
    new_obj = factory_callback_();

    if VUNLIKELY (!new_obj) {
      throw_factory_null();
    }
  } catch (...) {
    lock.lock();
    --borrowed_;
    --live_count_;
    --total_created_;
    throw;
  }

  return new_obj;
}

template <typename T>
inline void ObjectPool<T>::release(std::unique_ptr<T> obj) noexcept {
  if VUNLIKELY (!obj) {
    return;
  }

  if (should_reset_on_release() && reset_callback_) {
    try {
      reset_callback_(*obj);
    } catch (...) {
      safe_dec_borrowed_and_live();
      return;
    }
  }

  {
    std::lock_guard lock(mutex_);

    if VLIKELY (borrowed_ > 0) {
      --borrowed_;
    }

    try {
      pool_.emplace_back(std::move(obj));
    } catch (...) {
      if (live_count_ > 0) {
        --live_count_;
      }
    }
  }
}

template <typename T>
inline void ObjectPool<T>::PoolDeleter::operator()(T* ptr) const noexcept {
  if VUNLIKELY (!ptr) {
    return;
  }

  if (auto sp = weak_pool.lock()) {
    std::unique_ptr<T> u(ptr);
    sp->release(std::move(u));
  } else {
    delete ptr;
  }
}

}  // namespace vlink
