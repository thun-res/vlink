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
 * @file mpmc_queue.h
 * @brief Bounded lock-free multi-producer multi-consumer ring buffer with optional cv blocking.
 *
 * @details
 * @c MpmcQueue is a fixed-capacity ring buffer based on a turn-counter algorithm.  Each slot
 * holds a per-slot atomic @c turn counter that encodes whether the slot is currently empty
 * (ready for a producer) or full (ready for a consumer).
 *
 * @par Algorithm summary
 *  - Producers atomically increment @c head_ to claim a slot and spin until
 *    @c chunk.turn @c == @c turn(head) @c * @c 2 (slot empty), then construct the value and
 *    publish @c turn @c = @c turn(head) @c * @c 2 @c + @c 1 (slot full).
 *  - Consumers atomically increment @c tail_ to claim a slot and spin until
 *    @c chunk.turn @c == @c turn(tail) @c * @c 2 @c + @c 1 (slot full), then move the value
 *    out and publish @c turn @c = @c turn(tail) @c * @c 2 @c + @c 2 (slot empty for next round).
 *  - Spinning runs for @c kFirstSpinTimes (@c 32) iterations before calling @c yield_cpu.
 *  - The struct is cache-line aligned: @c head_, @c tail_ and each @c Chunk live in dedicated
 *    64-byte regions to avoid false sharing.
 *
 * @par Producer / consumer guarantees
 *
 * | Behaviour             | Push contract                                | Pop contract                            |
 * | --------------------- | -------------------------------------------- | --------------------------------------- |
 * | @c kNoBehavior        | No notification on success or failure        | Spin-wait only                          |
 * | @c kConditionBehavior | Acquire @c cv_mtx_, notify @c not_empty      | Wake @c not_full waiter on pop          |
 * | Blocking variants     | @c emplace / @c push spin until success      | @c pop spins until a slot is full       |
 * | Non-blocking variants | @c try_emplace / @c try_push return on full  | @c try_pop returns false on empty       |
 * | Quit signal           | @c notify_to_quit drops further pushes       | Pop returns without value once quit set |
 *
 * @par Example
 * @code
 *   vlink::MpmcQueue<int> q(1024);
 *
 *   // Producer:
 *   q.push<vlink::MpmcQueue<int>::kConditionBehavior>(42);
 *
 *   // Consumer:
 *   q.wait_not_empty();
 *   int val = 0;
 *   q.pop<vlink::MpmcQueue<int>::kConditionBehavior>(val);
 * @endcode
 *
 * @note @c emplace / @c pop block by spinning; for bounded producers prefer the @c try_* forms.
 *       Capacity must be @c >= @c 1 (otherwise @c std::invalid_argument is thrown).
 *       @c notify_to_quit gracefully drains pending waiters and silently drops further pushes.
 */

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

#include "./condition_variable.h"
#include "./macros.h"
#include "./utils.h"

#if defined(__linux__)
#define VLINK_NO_INSTRUMENT __attribute__((no_instrument_function))
#else
#define VLINK_NO_INSTRUMENT
#endif

namespace vlink {

/**
 * @class MpmcQueueBase
 * @brief Non-template base class that owns every element-type-independent piece of state.
 *
 * @details
 * Hosts the cache-line aligned @c head_ / @c tail_ cursors, the optional condition variables,
 * the quit flag, the shared capacity and the @c void* slot for the @c Chunk array storage.
 * The element type @c T -- including the @c Chunk struct and the move / destroy plumbing --
 * lives in @c MpmcQueue<T>, which accesses slots via a typed cast of @c chunk_storage_.
 */
class VLINK_EXPORT MpmcQueueBase {
 public:
  /**
   * @brief Per-call notification behaviour selector.
   *
   * @details
   * Selected via the @c BehaviorT template argument to @c emplace / @c push / @c pop and the
   * @c try_* variants.  Use @c kConditionBehavior whenever consumers or producers may block on
   * @c wait_not_empty / @c wait_not_full; otherwise the cv notifications are pure overhead.
   */
  enum Behavior : uint8_t { kNoBehavior = 0, kConditionBehavior = 1 };

  /**
   * @brief Returns the fixed capacity chosen at construction.
   *
   * @return Slot count (always @c >= @c 1).
   */
  [[nodiscard]] size_t capacity() const noexcept VLINK_NO_INSTRUMENT;

  /**
   * @brief Returns an approximate count of currently enqueued elements.
   *
   * @details
   * Computes @c head_ @c - @c tail_ using acquire loads of both cursors; concurrent updates
   * may produce a slightly stale value.  @p real @c == @c true retries up to 50 times until
   * the @c tail_ reading is stable, at the cost of latency.  The result is clamped at @c 0
   * to suppress transient @c head_ @c < @c tail_ wrap-arounds.
   *
   * @param real  When @c true, retries for a stable snapshot.  Default: @c false.
   * @return Approximate number of enqueued elements.
   */
  [[nodiscard]] size_t size(bool real = false) const noexcept VLINK_NO_INSTRUMENT;

  /**
   * @brief Returns @c true when the queue appears empty.
   *
   * @param real  Pass @c true to use the retrying @c size variant.
   * @return @c true when no elements are currently enqueued (approximately).
   */
  [[nodiscard]] bool empty(bool real = false) const noexcept VLINK_NO_INSTRUMENT;

  /**
   * @brief Returns @c true when the queue appears full.
   *
   * @param real  Pass @c true to use the retrying @c size variant.
   * @return @c true when the queue is at or above capacity.
   */
  [[nodiscard]] bool is_full(bool real = false) const noexcept VLINK_NO_INSTRUMENT;

  /**
   * @brief Blocks until the queue is non-empty or @p timeout elapses.
   *
   * @details
   * Fast path: returns immediately when @c empty(true) is @c false.  Slow path: waits on
   * @c cv_not_empty until a producer publishes under @c kConditionBehavior or
   * @c notify_to_quit fires.
   *
   * @param timeout  Maximum wait; @c 0 means wait forever.  Default: @c 0.
   * @return @c true when the queue became non-empty; @c false on timeout or quit.
   */
  bool wait_not_empty(std::chrono::milliseconds timeout = std::chrono::milliseconds(0)) noexcept VLINK_NO_INSTRUMENT;

  /**
   * @brief Blocks until the queue has free space or @p timeout elapses.
   *
   * @details
   * Mirror of @c wait_not_empty for producers; waits on @c cv_not_full.
   *
   * @param timeout  Maximum wait; @c 0 means wait forever.  Default: @c 0.
   * @return @c true when space became available; @c false on timeout or quit.
   */
  bool wait_not_full(std::chrono::milliseconds timeout = std::chrono::milliseconds(0)) noexcept VLINK_NO_INSTRUMENT;

  /**
   * @brief Signals graceful shutdown to every waiter and rejects further pushes.
   *
   * @details
   * Sets the internal quit flag with release ordering and broadcasts both condition variables.
   * Existing @c wait_not_empty / @c wait_not_full calls return @c false; subsequent
   * @c emplace / @c push silently drop their payload; @c try_pop returns @c false without
   * touching the output.  Safe to call multiple times.
   */
  void notify_to_quit() noexcept VLINK_NO_INSTRUMENT;

 protected:
  static constexpr std::memory_order kMemoryOrderAcquire = std::memory_order_acquire;
  static constexpr std::memory_order kMemoryOrderRelease = std::memory_order_release;
  static constexpr std::memory_order kMemoryOrderRelaxed = std::memory_order_relaxed;

  static constexpr size_t kInterferenceSize = 64U;
  static constexpr size_t kFirstSpinTimes = 32U;

  explicit MpmcQueueBase(size_t capacity);

  ~MpmcQueueBase() noexcept = default;

  [[noreturn]] static void throw_mpmc_invalid_capacity();

  [[noreturn]] static void throw_mpmc_alignment_failure();

  [[nodiscard]] constexpr size_t idx(size_t i) const noexcept { return i % capacity_; }

  [[nodiscard]] constexpr size_t turn(size_t i) const noexcept { return i / capacity_; }

  alignas(kInterferenceSize) std::atomic<size_t> head_{0U};

  void* chunk_storage_{nullptr};
  size_t capacity_{0};

  mutable std::mutex cv_mtx_;

  alignas(kInterferenceSize) std::atomic<size_t> tail_{0U};

  ConditionVariable cv_not_empty_;
  ConditionVariable cv_not_full_;

  struct alignas(kInterferenceSize) QuitFlag {
    std::atomic_bool value{false};
    char padding[kInterferenceSize - sizeof(std::atomic_bool)];
  } quit_flag_;
};

/**
 * @class MpmcQueue
 * @brief Fixed-capacity lock-free MPMC ring buffer over @c T.
 *
 * @details
 * Allocates @c capacity @c + @c 1 cache-line-aligned slots, validates alignment, and provides
 * per-slot turn counters for non-blocking enqueue / dequeue.  Element type must be movable.
 *
 * @tparam T  Element type stored in each slot.
 */
template <typename T>
class MpmcQueue : public MpmcQueueBase {
 public:
  /**
   * @brief Constructs a queue with the given fixed capacity.
   *
   * @details
   * Allocates @c capacity @c + @c 1 slots via the aligned allocator (extra trailing guard slot
   * keeps index arithmetic simple).  Validates that the allocation is aligned to
   * @c kInterferenceSize bytes; misaligned allocations throw @c std::bad_alloc.  Each slot's
   * turn counter is default-initialised to zero.
   *
   * @param capacity  Maximum number of elements; must be @c >= @c 1.
   * @throws std::invalid_argument when @p capacity is below @c 1.
   * @throws std::bad_alloc       when allocation fails or returns a misaligned pointer.
   */
  explicit MpmcQueue(size_t capacity) VLINK_NO_INSTRUMENT;

  /**
   * @brief Destructor; destroys still-occupied slots and releases the chunk array.
   *
   * @details
   * Not thread-safe with concurrent producers or consumers; the caller must quiesce the queue
   * before destruction.
   */
  ~MpmcQueue() noexcept VLINK_NO_INSTRUMENT;

  /**
   * @brief In-place constructs an element and blocks until a slot is available.
   *
   * @details
   * Claims the next slot via @c head_.fetch_add and spins on the turn counter until the slot
   * is empty.  When @c BehaviorT is @c kConditionBehavior the producer first awaits free space
   * via @c wait_not_full and notifies one @c wait_not_empty waiter after publishing.  Returns
   * silently when @c notify_to_quit is observed.
   *
   * @tparam BehaviorT  Notification behaviour.  Default: @c kNoBehavior.
   * @tparam Args       Constructor argument types forwarded to @c T.
   * @param args        Arguments forwarded to @c T 's constructor.
   */
  template <Behavior BehaviorT = kNoBehavior, typename... Args>
  void emplace(Args&&... args) noexcept VLINK_NO_INSTRUMENT;

  /**
   * @brief Non-blocking in-place construction.
   *
   * @details
   * Reads @c head_, attempts a CAS to claim the slot and retries when the head moves; returns
   * @c false when the queue is full or @c notify_to_quit has been observed.  On success
   * publishes the slot and, when @c BehaviorT is @c kConditionBehavior, notifies a waiter.
   *
   * @tparam BehaviorT  Notification behaviour.  Default: @c kNoBehavior.
   * @tparam Args       Constructor argument types forwarded to @c T.
   * @param args        Arguments forwarded to @c T 's constructor.
   * @return @c true on successful enqueue; @c false on full queue or quit.
   */
  template <Behavior BehaviorT = kNoBehavior, typename... Args>
  [[nodiscard]] bool try_emplace(Args&&... args) noexcept VLINK_NO_INSTRUMENT;

  /**
   * @brief Pushes a perfect-forwarded value, blocking until a slot is available.
   *
   * @details
   * Wrapper around @c emplace<BehaviorT>(std::forward<P>(v)).
   *
   * @tparam BehaviorT  Notification behaviour.  Default: @c kNoBehavior.
   * @tparam P          Value type accepted by @c T 's constructor.
   * @param v  Value to push.
   */
  template <Behavior BehaviorT = kNoBehavior, typename P>
  void push(P&& v) noexcept VLINK_NO_INSTRUMENT;

  /**
   * @brief Non-blocking push wrapper around @c try_emplace.
   *
   * @tparam BehaviorT  Notification behaviour.  Default: @c kNoBehavior.
   * @tparam P          Value type accepted by @c T 's constructor.
   * @param v  Value to push.
   * @return @c true on successful enqueue; @c false on full queue or quit.
   */
  template <Behavior BehaviorT = kNoBehavior, typename P>
  [[nodiscard]] bool try_push(P&& v) noexcept VLINK_NO_INSTRUMENT;

  /**
   * @brief Pops a value by move; blocks until an element is available.
   *
   * @details
   * Claims the next slot via @c tail_.fetch_add, spins until the slot is full, moves the value
   * into @p v and republishes the slot as empty.  When @c BehaviorT is @c kConditionBehavior
   * a @c wait_not_full waiter is notified.  Returns without altering @p v when
   * @c notify_to_quit is observed mid-wait.
   *
   * @tparam BehaviorT  Notification behaviour.  Default: @c kNoBehavior.
   * @param v  Destination assigned via @c std::move.
   */
  template <Behavior BehaviorT = kNoBehavior>
  void pop(T& v) noexcept VLINK_NO_INSTRUMENT;

  /**
   * @brief Non-blocking pop; returns @c false when the queue is empty.
   *
   * @details
   * Uses CAS retry on @c tail_; @p v is untouched on failure.
   *
   * @tparam BehaviorT  Notification behaviour.  Default: @c kNoBehavior.
   * @param v  Destination assigned via @c std::move on success.
   * @return @c true on successful pop; @c false on empty or quit.
   */
  template <Behavior BehaviorT = kNoBehavior>
  [[nodiscard]] bool try_pop(T& v) noexcept VLINK_NO_INSTRUMENT;

 private:
#if defined(__cpp_aligned_new)
  template <typename ChunkT>
  using AlignedAllocator = std::allocator<ChunkT>;
#else
  template <typename ChunkT>
  struct AlignedAllocator {
    using value_type = ChunkT;

    ChunkT* allocate(size_t n) {
      if VUNLIKELY (n > std::numeric_limits<size_t>::max() / sizeof(ChunkT)) {
        throw std::bad_array_new_length();
      }

#ifdef _WIN32
      auto* p = static_cast<ChunkT*>(_aligned_malloc(sizeof(ChunkT) * n, alignof(ChunkT)));

      if VUNLIKELY (p == nullptr) {
        throw std::bad_alloc();
      }
#else
      ChunkT* p;

      if VUNLIKELY (posix_memalign(reinterpret_cast<void**>(&p), alignof(ChunkT), sizeof(ChunkT) * n) != 0) {
        throw std::bad_alloc();
      }
#endif

      return p;
    }

    void deallocate(ChunkT* p, size_t) {
#ifdef _WIN32
      _aligned_free(p);
#else
      free(p);
#endif
    }
  };
#endif

  struct Chunk {
    ~Chunk() noexcept {
      if ((turn.load(kMemoryOrderAcquire) & 1U) != 0U) {
        destroy();
      }
    }

    template <typename... Args>
    void construct(Args&&... args) noexcept {
      new (storage.data()) T(std::forward<Args>(args)...);
    }

    void destroy() noexcept { std::launder(reinterpret_cast<T*>(storage.data()))->~T(); }

    [[nodiscard]] T&& move() noexcept { return std::move(*std::launder(reinterpret_cast<T*>(storage.data()))); }

    alignas(kInterferenceSize) std::atomic<size_t> turn{0U};
    alignas(alignof(T)) std::array<uint8_t, sizeof(T)> storage;
  };

  [[nodiscard]] Chunk* chunks() noexcept { return static_cast<Chunk*>(chunk_storage_); }

  [[nodiscard]] const Chunk* chunks() const noexcept { return static_cast<const Chunk*>(chunk_storage_); }

  VLINK_DISALLOW_COPY_AND_ASSIGN(MpmcQueue)
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

template <typename T>
inline MpmcQueue<T>::MpmcQueue(size_t capacity) : MpmcQueueBase(capacity) {
  static_assert(alignof(Chunk) == kInterferenceSize,
                "Slot must be aligned to cache line boundary to prevent false sharing");
  static_assert(sizeof(Chunk) % kInterferenceSize == 0,
                "Slot size must be a multiple of cache line size to prevent "
                "false sharing between adjacent slots");
  static_assert(sizeof(MpmcQueue) % kInterferenceSize == 0,
                "Queue size must be a multiple of cache line size to "
                "prevent false sharing between adjacent queues");

  AlignedAllocator<Chunk> allocator;
  Chunk* raw = allocator.allocate(capacity_ + 1);

  if VUNLIKELY (reinterpret_cast<size_t>(raw) % alignof(Chunk) != 0U) {
    allocator.deallocate(raw, capacity_ + 1);
    throw_mpmc_alignment_failure();
  }

  chunk_storage_ = raw;

  for (size_t i = 0U; i < capacity_; ++i) {
    new (&raw[i]) Chunk();
  }
}

template <typename T>
inline MpmcQueue<T>::~MpmcQueue() noexcept {
  Chunk* raw = chunks();

  for (size_t i = 0U; i < capacity_; ++i) {
    raw[i].~Chunk();
  }

  AlignedAllocator<Chunk> allocator;
  allocator.deallocate(raw, capacity_ + 1);
}

template <typename T>
template <typename MpmcQueue<T>::Behavior BehaviorT, typename... Args>
inline void MpmcQueue<T>::emplace(Args&&... args) noexcept {
  if VUNLIKELY (quit_flag_.value.load(kMemoryOrderAcquire)) {
    return;
  }

  if constexpr (BehaviorT == kConditionBehavior) {
    wait_not_full(std::chrono::milliseconds(0));

    if VUNLIKELY (quit_flag_.value.load(kMemoryOrderAcquire)) {
      return;
    }
  }

  const auto head = head_.fetch_add(1U);
  auto& chunk = chunks()[idx(head)];

  size_t spin = 0;

  while (turn(head) * 2U != chunk.turn.load(kMemoryOrderAcquire)) {
    if VUNLIKELY (quit_flag_.value.load(kMemoryOrderAcquire)) {
      return;
    }

    if (++spin < kFirstSpinTimes) {
    } else {
      Utils::yield_cpu();
    }
  }

  chunk.construct(std::forward<Args>(args)...);
  chunk.turn.store((turn(head) * 2U) + 1U, kMemoryOrderRelease);

  if constexpr (BehaviorT == kConditionBehavior) {
    std::lock_guard lock(cv_mtx_);
    cv_not_empty_.notify_one();
  }
}

template <typename T>
template <typename MpmcQueue<T>::Behavior BehaviorT, typename... Args>
inline bool MpmcQueue<T>::try_emplace(Args&&... args) noexcept {
  if VUNLIKELY (quit_flag_.value.load(kMemoryOrderAcquire)) {
    return false;
  }

  auto head = head_.load(kMemoryOrderAcquire);

  for (;;) {
    auto& chunk = chunks()[idx(head)];

    if VLIKELY (turn(head) * 2U == chunk.turn.load(kMemoryOrderAcquire)) {
      if (head_.compare_exchange_strong(head, head + 1U)) {
        chunk.construct(std::forward<Args>(args)...);
        chunk.turn.store((turn(head) * 2U) + 1U, kMemoryOrderRelease);

        if constexpr (BehaviorT == kConditionBehavior) {
          std::lock_guard lock(cv_mtx_);
          cv_not_empty_.notify_one();
        }

        return true;
      }
    } else {
      const auto prev_head = head;
      head = head_.load(kMemoryOrderAcquire);

      if VUNLIKELY (head == prev_head) {
        return false;
      }
    }
  }
}

template <typename T>
template <typename MpmcQueue<T>::Behavior BehaviorT, typename P>
inline void MpmcQueue<T>::push(P&& v) noexcept {
  emplace<BehaviorT>(std::forward<P>(v));
}

template <typename T>
template <typename MpmcQueue<T>::Behavior BehaviorT, typename P>
inline bool MpmcQueue<T>::try_push(P&& v) noexcept {
  return try_emplace<BehaviorT>(std::forward<P>(v));
}

template <typename T>
template <typename MpmcQueue<T>::Behavior BehaviorT>
inline void MpmcQueue<T>::pop(T& v) noexcept {
  auto const tail = tail_.fetch_add(1U);
  auto& chunk = chunks()[idx(tail)];

  size_t spin = 0;

  while (turn(tail) * 2U + 1U != chunk.turn.load(kMemoryOrderAcquire)) {
    if VUNLIKELY (quit_flag_.value.load(kMemoryOrderAcquire)) {
      return;
    }

    if (++spin < kFirstSpinTimes) {
    } else {
      Utils::yield_cpu();
    }
  }

  v = chunk.move();
  chunk.destroy();
  chunk.turn.store((turn(tail) * 2U) + 2U, kMemoryOrderRelease);

  if constexpr (BehaviorT == kConditionBehavior) {
    std::lock_guard lock(cv_mtx_);
    cv_not_full_.notify_one();
  }
}

template <typename T>
template <typename MpmcQueue<T>::Behavior BehaviorT>
inline bool MpmcQueue<T>::try_pop(T& v) noexcept {
  if VUNLIKELY (quit_flag_.value.load(kMemoryOrderAcquire)) {
    return false;
  }

  auto tail = tail_.load(kMemoryOrderAcquire);

  // LCOV_EXCL_START
  // GCOVR_EXCL_START

  for (;;) {
    auto& chunk = chunks()[idx(tail)];

    if VLIKELY (turn(tail) * 2U + 1U == chunk.turn.load(kMemoryOrderAcquire)) {
      if (tail_.compare_exchange_strong(tail, tail + 1U)) {
        v = chunk.move();
        chunk.destroy();
        chunk.turn.store((turn(tail) * 2U) + 2U, kMemoryOrderRelease);

        if constexpr (BehaviorT == kConditionBehavior) {
          std::lock_guard lock(cv_mtx_);
          cv_not_full_.notify_one();
        }

        return true;
      }
    } else {
      const auto prev_tail = tail;
      tail = tail_.load(kMemoryOrderAcquire);

      if VUNLIKELY (tail == prev_tail) {
        return false;
      }
    }

    if VUNLIKELY (quit_flag_.value.load(kMemoryOrderAcquire)) {
      return false;
    }
  }

  // GCOVR_EXCL_STOP
  // LCOV_EXCL_STOP
}

}  // namespace vlink
