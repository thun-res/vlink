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
 * @file subscriber_impl.h
 * @brief Transport-neutral base class for every event-model subscriber implementation.
 *
 * @details
 * This is an internal implementation header used by the public @c Subscriber
 * template; applications should depend on @c subscriber.h.  @c SubscriberImpl
 * extends @c NodeImpl with the receive-side semantics: two listen overloads
 * cover the serialised wire path (the default) and the zero-copy in-process
 * path used only by the intra backend, plus optional latency / loss tracking
 * that mirrors @c GetterImpl.
 *
 * @par ImplType
 * The constructor stamps @c impl_type with @c kSubscriber so subscriptions are
 * tagged correctly in discovery, recording and proxy paths.
 *
 * @par Lifecycle
 * - Constructed by the matching @c Conf::create_subscriber().
 * - The public @c Subscriber template calls @c listen() once the user installs
 *   a callback; @c is_listened becomes @c true on success.
 * - @c set_latency_and_lost_enabled() may be toggled before or after listen.
 * - @c init() / @c deinit() inherited from @c NodeImpl drive the transport.
 *
 * @par Role table
 * | Capability                      | Provider                                                         |
 * | ------------------------------- | ---------------------------------------------------------------- |
 * | Wire receive callback           | Subclass override of @c listen(MsgCallback&&)                    |
 * | Zero-copy in-process callback   | @c IntraSubscriberImpl override of @c listen(IntraMsgCallback&&) |
 * | Latency / loss reporting        | Subclass overrides of the optional getters                       |
 * | Listen state flag               | @c is_listened (set by @c Subscriber)                            |
 *
 * @par Internal API contract
 * | Method                                | Default                       | Subclass duty           |
 * | ------------------------------------- | ----------------------------- | ----------------------- |
 * | @c listen(MsgCallback&&)              | Pure virtual                  | Wire transport receiver |
 * | @c listen(IntraMsgCallback&&)         | Warns, returns @c false       | Only intra:// overrides |
 * | @c set_latency_and_lost_enabled(bool) | No-op                         | Toggle instrumentation  |
 * | @c is_latency_and_lost_enabled() const| Returns @c false              | Report tracking state   |
 * | @c get_latency() const                | Returns @c 0                  | Latest measured latency |
 * | @c get_lost() const                   | Returns zero @c SampleLostInfo| Cumulative loss stats   |
 */

#pragma once

#include "./node_impl.h"

namespace vlink {

/**
 * @class SubscriberImpl
 * @brief Event-model receive base shared by every transport-specific subscriber.
 *
 * @details
 * Provides safe defaults for the optional latency / loss instrumentation so
 * backends without those signals can be plugged in without empty overrides.
 * Concrete backends override @c listen() to bind the supplied callback to the
 * transport receive path.
 */
class VLINK_EXPORT SubscriberImpl : public NodeImpl {
 public:
  /**
   * @brief Releases backend resources.
   */
  ~SubscriberImpl() override;

  /**
   * @brief Installs the serialised-message receive callback.
   *
   * @details
   * Pure virtual.  Backends invoke @p callback with the raw payload bytes for
   * each frame received.  The public @c Subscriber sets @c is_listened to
   * @c true on success.
   *
   * @param callback  Callable @c void(const Bytes&) invoked on every received frame.
   * @return @c true when registration succeeded; @c false on error.
   */
  virtual bool listen(MsgCallback&& callback) = 0;

  /**
   * @brief Installs the zero-copy in-process receive callback.
   *
   * @details
   * Only the @c intra:// backend overrides this overload; the default logs a
   * warning and returns @c false.
   *
   * @param callback  Callable @c void(const IntraData&) invoked for each in-process delivery.
   * @return @c true when registration succeeded; @c false when the transport does not support
   *         @c IntraData.
   */
  virtual bool listen(IntraMsgCallback&& callback);

  /**
   * @brief Enables or disables per-message latency / loss tracking.
   *
   * @details
   * Default no-op.  Backends that maintain timestamps and sequence numbers
   * override the method.
   *
   * @param enable  @c true to enable; @c false to disable.
   */
  virtual void set_latency_and_lost_enabled(bool enable);

  /**
   * @brief Reports whether tracking is currently enabled.
   *
   * @return @c true when the backend is collecting latency / loss data.
   */
  [[nodiscard]] virtual bool is_latency_and_lost_enabled() const;

  /**
   * @brief Returns the most recently measured end-to-end latency in nanoseconds.
   *
   * @details
   * Only meaningful when @c is_latency_and_lost_enabled() returns @c true.
   *
   * @return Latency in nanoseconds; @c 0 when tracking is off or unsupported.
   */
  [[nodiscard]] virtual int64_t get_latency() const;

  /**
   * @brief Returns cumulative delivered / lost counters.
   *
   * @details
   * Only meaningful when tracking is enabled.  Default returns a zero-initialised
   * @c SampleLostInfo.
   *
   * @return @c SampleLostInfo with @c total and @c lost counts.
   */
  [[nodiscard]] virtual SampleLostInfo get_lost() const;

  bool is_listened{false};  ///< @c true once @c listen() has been registered successfully.

 protected:
  /**
   * @brief Stamps the node as @c kSubscriber.
   */
  SubscriberImpl();

 private:
  VLINK_DISALLOW_COPY_AND_ASSIGN(SubscriberImpl)
};

}  // namespace vlink
