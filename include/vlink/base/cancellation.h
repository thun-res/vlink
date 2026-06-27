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
 * @file cancellation.h
 * @brief Cooperative one-shot cancellation primitives shared by VLink async building blocks.
 *
 * @details
 * Cancellation is split into a writable @c CancellationSource owned by the work producer, one
 * or more read-only @c CancellationToken observers held by workers, and per-callback
 * @c CancellationRegistration handles used to unsubscribe pending callbacks.  All three share
 * a refcounted internal state allocated on the producer side.
 *
 * @par Role table
 *
 * | Type                          | Role                                  | Copy / Move         |
 * | ----------------------------- | ------------------------------------- | ------------------- |
 * | @c CancellationSource         | Mutator; signals cancellation         | Implicit (refcount) |
 * | @c CancellationToken          | Observer; poll / throw / subscribe    | Copyable            |
 * | @c CancellationRegistration   | RAII slot for one subscribed callback | Move-only           |
 *
 * @par State diagram
 *
 * @verbatim
 *   +------------+   request_cancel()    +---------------+   no-ops on further calls
 *   |  active    | --------------------> |  cancelled    | -------------------------+
 *   |  (default) |                       |  (terminal)   |                          |
 *   +------------+                       +---------------+                          |
 *      ^   ^                                ^                                       |
 *      |   |  is_cancellation_requested()=false                                     |
 *      |   |  throw_if_cancellation_requested()=no-op                               |
 *      |   |                                                                        v
 *      |   +-- new tokens via token() share the same state ------------------ read-only
 *      |
 *      +------ register_callback() inserts a slot; fires synchronously on transition
 * @endverbatim
 *
 * Cancellation is one-shot.  After the first successful @c request_cancel the state is
 * terminal; subsequent requests return @c false and never re-invoke callbacks.  The cancelled
 * type re-exported as @c vlink::Exception::OperationCancelled is the canonical thrown type at
 * structured cancellation points.
 *
 * @par Lock ordering
 * Internal mutexes are released before user callbacks fire, so callbacks may freely cancel
 * sibling sources or register more callbacks without self-deadlock.
 *
 * @par Example
 * @code
 *   vlink::CancellationSource source;
 *   auto token = source.token();
 *
 *   auto reg = token.register_callback([] {
 *     // Runs once on the cancelling thread, or inline if already cancelled.
 *   });
 *
 *   while (!token.is_cancellation_requested()) {
 *     token.throw_if_cancellation_requested();
 *     run_work_unit();
 *   }
 *
 *   source.request_cancel();
 * @endcode
 */

#pragma once

#include <cstddef>
#include <memory>

#include "./functional.h"
#include "./macros.h"

namespace vlink {

class CancellationToken;

/**
 * @class CancellationRegistration
 * @brief Move-only RAII handle that represents one subscribed cancellation callback.
 *
 * @details
 * Each registration owns exactly one slot inside the source's state.  Destroying or @c reset-ing
 * the handle detaches the callback if it has not yet fired; once the source has cancelled the
 * slot is consumed automatically and the handle becomes a benign no-op sink.  The type is
 * deliberately move-only to keep slot identity unique.
 */
class VLINK_EXPORT CancellationRegistration final {
 public:
  /**
   * @brief Constructs an empty registration that owns no callback slot.
   */
  CancellationRegistration() noexcept;

  /**
   * @brief Destructor; detaches the owned callback when cancellation has not yet fired.
   */
  ~CancellationRegistration();

  /**
   * @brief Move constructor; transfers slot ownership and leaves @p other empty.
   *
   * @param other  Source registration emptied by the move.
   */
  CancellationRegistration(CancellationRegistration&& other) noexcept;

  /**
   * @brief Move assignment; detaches any current slot then adopts @p other 's slot.
   *
   * @param other  Source registration emptied by the move.
   * @return Reference to @c *this.
   */
  CancellationRegistration& operator=(CancellationRegistration&& other) noexcept;

  /**
   * @brief Detaches the owned callback and resets to the empty state.
   *
   * @details
   * Idempotent.  Safe to call on an empty or already-reset registration.
   */
  void reset() noexcept;

  /**
   * @brief Reports whether the registration still owns an attached callback slot.
   *
   * @return @c true when the slot is live and the source has not yet cancelled.
   */
  [[nodiscard]] bool valid() const noexcept;

 private:
  struct State;

  CancellationRegistration(std::weak_ptr<State> state, size_t id) noexcept;

  std::weak_ptr<State> state_;
  size_t id_{0};

  friend class CancellationToken;
  friend class CancellationSource;

  VLINK_DISALLOW_COPY_AND_ASSIGN(CancellationRegistration)
};

/**
 * @class CancellationToken
 * @brief Lightweight observer of a @c CancellationSource shared across worker threads.
 *
 * @details
 * Tokens are cheap to copy and safe to hand to any thread; many tokens may observe the same
 * source concurrently.  A default-constructed token is invalid -- it never reports cancellation
 * and never accepts callbacks.  Workers typically poll @c is_cancellation_requested at safe
 * points, call @c throw_if_cancellation_requested at structured points, or install a one-shot
 * callback via @c register_callback.
 */
class VLINK_EXPORT CancellationToken final {
 public:
  /**
   * @brief Constructs an invalid token not bound to any source.
   */
  CancellationToken() noexcept;

  /**
   * @brief Reports whether the token observes a live cancellation source.
   *
   * @return @c true when the token was minted from a @c CancellationSource.
   */
  [[nodiscard]] bool valid() const noexcept;

  /**
   * @brief Returns @c true once the bound source has been cancelled.
   *
   * @return @c true after the source's @c request_cancel has succeeded.
   */
  [[nodiscard]] bool is_cancellation_requested() const noexcept;

  /**
   * @brief Subscribes a one-shot callback to fire when the source is cancelled.
   *
   * @details
   * When cancellation has not yet been requested the callback is stored in the source and fires
   * once on the thread that later calls @c CancellationSource::request_cancel.  When
   * cancellation already happened the callback runs synchronously inside this call and the
   * returned registration is empty.  Exceptions escaping the callback are caught and logged
   * via @c CLOG_E; they never propagate to the registering or cancelling thread.
   *
   * @param callback  Move-only nullary functor; an empty callback is silently ignored.
   * @return RAII registration for the subscription, or an empty handle when cancellation had
   *         already fired or the token / callback was invalid.
   */
  CancellationRegistration register_callback(MoveFunction<void()>&& callback) const;

  /**
   * @brief Throws @c vlink::Exception::OperationCancelled when cancellation has been requested.
   *
   * @throws vlink::Exception::OperationCancelled if @c is_cancellation_requested() is @c true.
   */
  void throw_if_cancellation_requested() const;

 private:
  using State = CancellationRegistration::State;

  explicit CancellationToken(std::shared_ptr<State> state) noexcept;

  std::shared_ptr<State> state_;

  friend class CancellationSource;
};

/**
 * @class CancellationSource
 * @brief Mutator that mints observer tokens and signals one-shot cancellation.
 *
 * @details
 * Holds a refcounted handle to the cancellation state; copies and moves share that state, so
 * any copy can mint tokens or request cancellation against the same one-shot transition.
 * @c token returns a cheap observer; @c request_cancel performs the transition exactly once and
 * fires every subscribed callback.
 */
class VLINK_EXPORT CancellationSource final {
 public:
  /**
   * @brief Constructs an uncancelled source with no subscribed callbacks.
   */
  CancellationSource();

  /**
   * @brief Returns a fresh observer token bound to this source.
   *
   * @return Valid @c CancellationToken sharing the source's state.
   */
  [[nodiscard]] CancellationToken token() const noexcept;

  /**
   * @brief Returns @c true once @c request_cancel has succeeded on this source.
   *
   * @return @c true after the one-shot transition has completed.
   */
  [[nodiscard]] bool is_cancellation_requested() const noexcept;

  /**
   * @brief Performs the one-shot active -> cancelled transition and fires registered callbacks.
   *
   * @details
   * Acquires the internal state mutex, swaps the cancellation flag, releases the mutex, then
   * runs every callback that was attached at the moment of the transition.  Exceptions thrown
   * by callbacks are caught and logged.
   *
   * @return @c true when this call performed the transition; @c false when a previous call had
   *         already cancelled the source.
   */
  bool request_cancel() const;

 private:
  using State = CancellationRegistration::State;

  std::shared_ptr<State> state_;
};

}  // namespace vlink
