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
 * @file publisher_impl.h
 * @brief Transport-neutral base class for every event-model publisher implementation.
 *
 * @details
 * This is an internal implementation header used by the public @c Publisher
 * template; applications should depend on @c publisher.h.  @c PublisherImpl
 * extends @c NodeImpl with the publish-side bookkeeping that lets a transport
 * detect subscriber presence and dispatch payloads either as serialised bytes
 * (the common case) or as zero-copy in-process @c IntraData (only the intra
 * backend overrides that path).
 *
 * @par ImplType
 * The constructor stamps @c impl_type with @c kPublisher, allowing discovery
 * and recording layers to label the produced frames correctly.
 *
 * @par Lifecycle
 * - Construction stamps the impl type and prepares the helper state.
 * - The public @c Publisher template calls @c init() once the conf is wired.
 * - @c detect_subscribers() may be installed at any time; if a subscriber is
 *   already known it fires once immediately.
 * - @c write() (overload chosen by the user) produces frames until @c interrupt()
 *   is called, after which @c reset_interrupted() can be used to resume.
 *
 * @par Role table
 * | Capability                  | Owner                                                 |
 * | --------------------------- | ----------------------------------------------------- |
 * | Serialised write path       | Subclass override of @c write(const Bytes&)           |
 * | Zero-copy intra write       | @c IntraPublisherImpl override of @c write(IntraData) |
 * | Subscriber presence query   | Subclass override of @c has_subscribers()             |
 * | Presence change publishing  | @c update_subscribers() in this class                 |
 * | Wait-for-subscriber gate    | @c wait_for_subscribers() / helper CV                 |
 *
 * @par Internal API contract
 * | Method                                | Default                 | Subclass duty              |
 * | ------------------------------------- | ----------------------- | -------------------------- |
 * | @c has_subscribers() const            | Pure virtual            | Query transport            |
 * | @c write(const Bytes&)                | Pure virtual            | Publish wire frame         |
 * | @c write(const IntraData&)            | Warns, returns @c false | Only intra:// overrides    |
 * | @c detect_subscribers(cb)             | Stores callback         | Usually inherited          |
 * | @c wait_for_subscribers(timeout)      | Helper CV wait          | Usually inherited          |
 * | @c update_subscribers()               | Implemented here        | Call from transport thread |
 */

#pragma once

#include <chrono>
#include <memory>

#include "./node_impl.h"

namespace vlink {

/**
 * @class PublisherImpl
 * @brief Publish-side base shared by every transport-specific publisher.
 *
 * @details
 * Owns the helper condition variable and connect-callback storage that back
 * @c Publisher<T>::wait_for_subscribers() and
 * @c Publisher<T>::detect_subscribers().  Transports invoke
 * @c update_subscribers() from their discovery threads whenever the subscriber
 * count flips between zero and non-zero.
 */
class VLINK_EXPORT PublisherImpl : public NodeImpl {
 public:
  /**
   * @brief Releases the helper state.
   */
  ~PublisherImpl() override;

  /**
   * @brief Wakes waiters and forwards the interrupt to @c NodeImpl.
   *
   * @details
   * Marks the node interrupted and notifies the helper condition variable so
   * pending @c wait_for_subscribers() calls return @c false promptly.
   */
  void interrupt() override;

  /**
   * @brief Registers @p callback to fire when subscriber presence changes.
   *
   * @details
   * The callback is invoked with @c true when the first subscriber appears and
   * with @c false after the last one drops.  When subscribers are already
   * known at registration time the callback is primed with @c true before
   * this function returns.
   *
   * @param callback  Callable @c void(bool) describing the new presence.
   */
  virtual void detect_subscribers(ConnectCallback&& callback);

  /**
   * @brief Blocks until at least one subscriber is reachable or @p timeout elapses.
   *
   * @details
   * Returns immediately when @c has_subscribers() already reports @c true.
   * Negative timeouts (e.g. @c Timeout::kInfinite) wait indefinitely.
   *
   * @param timeout  Wait budget; negative disables the deadline.
   * @return @c true when a subscriber arrived in time; @c false otherwise.
   */
  virtual bool wait_for_subscribers(std::chrono::milliseconds timeout);

  /**
   * @brief Reports whether at least one subscriber is currently connected.
   *
   * @details
   * Pure virtual; subclasses query their discovery handle.  Used by both
   * @c wait_for_subscribers() and @c update_subscribers() to detect state
   * transitions.
   *
   * @return @c true when a subscriber is reachable.
   */
  [[nodiscard]] virtual bool has_subscribers() const = 0;

  /**
   * @brief Publishes a serialised payload to every connected subscriber.
   *
   * @details
   * Pure virtual.  @p msg_data is produced by @c Serializer::serialize() at
   * the public layer.
   *
   * @param msg_data  Serialised payload bytes.
   * @return @c true when the frame was successfully delivered or queued.
   */
  virtual bool write(const Bytes& msg_data) = 0;

  /**
   * @brief Publishes an in-process payload without serialisation.
   *
   * @details
   * Default implementation warns and returns @c false; only
   * @c IntraPublisherImpl forwards the shared payload directly to co-located
   * subscribers.
   *
   * @param intra_data  Shared payload pointer.
   * @return @c true when the message was dispatched; @c false otherwise.
   */
  virtual bool write(const IntraData& intra_data);

  /**
   * @brief Notifies the base class that subscriber presence may have changed.
   *
   * @details
   * Compares @c has_subscribers() against the helper cache; on a transition
   * the condition variable is signalled and the stored @c ConnectCallback is
   * invoked.  Intended for use from transport discovery threads.
   */
  void update_subscribers();

 protected:
  /**
   * @brief Stamps the node as @c kPublisher and primes the helper state.
   */
  PublisherImpl();

 private:
  std::unique_ptr<struct PublisherImplHelper> helper_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(PublisherImpl)
};

}  // namespace vlink
