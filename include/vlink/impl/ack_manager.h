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
 * @file ack_manager.h
 * @brief Pending-request bookkeeping used to implement blocking RPC calls.
 *
 * @details
 * This is an internal implementation header used by the public @c Client template
 * and the method-model transport backends; it is not part of the user API.
 * @c AckManager links the thread that initiates a request with the transport
 * thread that delivers its acknowledgement: callers obtain a @c RequestPtr, hand
 * it to @c process() (which both publishes the wire frame through a supplied send
 * callback and blocks on a per-request condition variable), and the matching
 * @c notify() / @c remove() call resolves the wait once the response is in flight.
 *
 * @par Request state diagram
 * @code
 *                create_request()                process()
 *      [none]  --------------------> [pending] -----------> [waiting]
 *                                       ^                       |
 *                                       | notify() / remove()   |
 *                                       +-----------------------+
 *                                                | timeout                | clear()
 *                                                v                        v
 *                                            [resolved]              [interrupted]
 * @endcode
 *
 * @par API summary
 * | Method                  | Caller                | Effect                                                  |
 * | ----------------------- | --------------------- | ------------------------------------------------------- |
 * | @c create_request()     | RPC caller            | Allocates a unique sequence number and pending entry.   |
 * | @c process()            | RPC caller            | Sends, blocks, returns @c true on @c notify().          |
 * | @c notify()             | Transport callback    | Removes pending entry and wakes the waiting caller.     |
 * | @c remove()             | RPC caller / cleanup  | Cancels an entry without waking a waiter.               |
 * | @c clear()              | Node shutdown         | Aborts all waits and refuses new @c process() calls.    |
 * | @c reset_interrupted()  | Node resume           | Re-enables new @c process() calls after @c clear().     |
 *
 * @par Example
 * @code
 * vlink::AckManager mgr;
 * auto req = mgr.create_request();
 *
 * bool ok = mgr.process(req, 200, [&]() {
 *   return transport.send(req_bytes);  // returning false aborts the wait
 * });
 *
 * // Transport thread, upon receiving a matching reply:
 * mgr.notify(req, [&]() {
 *   response_bytes = std::move(reply_payload);
 * });
 * @endcode
 *
 * @par Thread safety
 * All public methods are safe to call from any thread.  Multiple concurrent
 * @c process() calls may be in flight at the same time; each request is tracked
 * independently and is keyed by a monotonically increasing sequence number.
 */

#pragma once

#include <memory>
#include <mutex>
#include <set>

#include "../base/condition_variable.h"
#include "../base/functional.h"
#include "../base/macros.h"

namespace vlink {

/**
 * @class AckManager
 * @brief Thread-safe coordinator that pairs RPC requests with their acknowledgements.
 *
 * @details
 * Stores a sorted set of in-flight @c RequestPtr handles indexed by their
 * sequence number and uses one condition variable per request to wake the
 * blocked caller when @c notify() arrives.  Used internally by every
 * @c ClientImpl subclass to provide the blocking @c call() semantics exposed at
 * the public @c Client API.
 */
class VLINK_EXPORT AckManager final {
 private:
  struct Request;

 public:
  /**
   * @brief Send callback supplied to @c process(); returning @c false aborts the wait.
   *
   * @details
   * Invoked while the request is already part of the pending set, so any
   * concurrent @c notify() that arrives before the callback returns is safe.
   */
  using ProcessCallback = MoveFunction<bool()>;

  /**
   * @brief Optional fill-in callback invoked from inside @c notify() under the lock.
   *
   * @details
   * Lets the transport copy the response into the caller's buffer before the
   * waiting thread is resumed.  May be @c nullptr.
   */
  using NotifyCallback = MoveFunction<void()>;

  /**
   * @brief Shared handle representing a single in-flight RPC request.
   *
   * @details
   * Returned by @c create_request() and consumed by @c process(), @c notify()
   * and @c remove().  Equality is implied by the monotonic sequence number.
   */
  using RequestPtr = std::shared_ptr<Request>;

  /**
   * @brief Constructs an empty manager.
   */
  AckManager() noexcept;

  /**
   * @brief Destroys the manager and releases any remaining pending records.
   */
  ~AckManager() noexcept;

  /**
   * @brief Allocates a new request token with the next monotonic sequence number.
   *
   * @return Shared handle ready to be passed to @c process().
   */
  [[nodiscard]] RequestPtr create_request() noexcept;

  /**
   * @brief Publishes @p request via @p process_callback and blocks until it is acknowledged.
   *
   * @details
   * The method performs four steps:
   * -# Registers @p request in the pending set; returns @c false straight away
   *    if @c clear() has been called and not yet reset.
   * -# Invokes @p process_callback to dispatch the request.  When the callback
   *    returns @c false the entry is removed and @c process() also returns @c false.
   * -# Sleeps on a per-request condition variable until @c notify() / @c remove()
   *    fires or @p ms expires.  A negative @p ms blocks forever.
   * -# Returns @c true on a successful @c notify(); @c false on timeout, abort or
   *    @c clear() interruption.
   *
   * @param request           Token obtained from @c create_request().
   * @param ms                Wait budget in milliseconds; negative for unlimited.
   * @param process_callback  Send callback; returning @c false aborts the wait.
   * @return @c true on acknowledgement, @c false otherwise.
   */
  [[nodiscard]] bool process(RequestPtr request, int ms, ProcessCallback&& process_callback) noexcept;

  /**
   * @brief Resolves @p request and, if supplied, runs @p notify_callback before waking the caller.
   *
   * @details
   * Erases the entry from the pending set, executes @p notify_callback while
   * holding the request lock so the caller observes the side effects before
   * resuming, and signals the condition variable.
   *
   * @param request          Token to acknowledge.
   * @param notify_callback  Optional callable executed before notification.
   * @return @c true if the request was still pending and was resolved; @c false otherwise.
   */
  bool notify(RequestPtr request, NotifyCallback&& notify_callback = nullptr) noexcept;

  /**
   * @brief Cancels @p request without waking any waiter.
   *
   * @param request  Token to drop from the pending set.
   * @return @c true if the entry existed and was removed; @c false otherwise.
   */
  bool remove(RequestPtr request) noexcept;

  /**
   * @brief Interrupts every pending wait and refuses new ones until @c reset_interrupted().
   *
   * @details
   * Bumps the generation counter, drains the pending set into a local copy and
   * notifies every condition variable so that the affected @c process() calls
   * return @c false.  Used during node shutdown to avoid blocking destructors.
   */
  void clear() noexcept;

  /**
   * @brief Permits new @c process() calls again after a previous @c clear().
   *
   * @details
   * Does not unblock requests interrupted by the earlier @c clear(); the
   * generation tag they were created in remains cancelled.
   */
  void reset_interrupted() noexcept;

 private:
  struct Request final {
    int64_t seq{0};
    int64_t generation{0};
    std::mutex mtx;
    ConditionVariable cv;

    struct Compare final {
      bool operator()(const RequestPtr& left, const RequestPtr& right) const noexcept {
        if VUNLIKELY (!left || !right) {
          return left < right;
        }

        return left->seq < right->seq;
      }
    };
  };

  bool is_interrupted_{false};
  int64_t request_seq_{0};
  int64_t generation_{0};
  mutable std::mutex mtx_;
  std::set<RequestPtr, Request::Compare> request_set_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(AckManager)
};

}  // namespace vlink
