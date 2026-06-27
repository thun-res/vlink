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
 * @file getter_impl.h
 * @brief Transport-neutral base for field-model getter (latest-value reader) implementations.
 *
 * @details
 * This is an internal implementation header used by the public @c Getter template;
 * applications should depend on @c getter.h instead.  @c GetterImpl extends
 * @c NodeImpl with the field-model receive semantics: a single value is tracked
 * per topic, and the getter is notified whenever the matching @c SetterImpl
 * writes a new value.  Optional latency / loss tracking mirrors the interface
 * found on @c SubscriberImpl so that both receivers expose the same diagnostics.
 *
 * @par ImplType
 * The constructor stamps @c impl_type with @c kGetter, which the discovery and
 * recording layers use to label outputs originating from this node.
 *
 * @par Lifecycle
 * - Constructed by the matching @c Conf::create_getter().
 * - @c listen() is called by the public @c Getter once the user installs a
 *   callback; @c is_listened flips to @c true on success.
 * - @c init() / @c deinit() inherited from @c NodeImpl bring the underlying
 *   transport up and tear it down.
 * - @c set_latency_and_lost_enabled() may be toggled before or after listening.
 *
 * @par Role table
 * | Capability                  | Provider                                  |
 * | --------------------------- | ----------------------------------------- |
 * | Wire receive callback       | Subclass override of @c listen()          |
 * | Latency / loss reporting    | Subclass override of the optional getters |
 * | Listen state flag           | @c is_listened (set by @c Getter)         |
 *
 * @par Internal API contract
 * | Method                                | Default                       | Subclass duty           |
 * | ------------------------------------- | ----------------------------- | ----------------------- |
 * | @c listen(MsgCallback&&)              | Pure virtual                  | Bind transport callback |
 * | @c set_latency_and_lost_enabled(bool) | No-op                         | Toggle tracking         |
 * | @c is_latency_and_lost_enabled() const| Returns @c false              | Report tracking state   |
 * | @c get_latency() const                | Returns @c 0                  | Latest measured latency |
 * | @c get_lost() const                   | Returns zero @c SampleLostInfo| Cumulative loss stats   |
 */

#pragma once

#include "./node_impl.h"

namespace vlink {

/**
 * @class GetterImpl
 * @brief Field-model receive base class shared by every transport-specific getter.
 *
 * @details
 * Provides safe defaults for the optional latency / loss instrumentation so
 * that backends without those signals can be plugged in without supplying
 * empty overrides.  Concrete backends override @c listen() to wire the
 * transport receive path to the supplied @c MsgCallback.
 */
class VLINK_EXPORT GetterImpl : public NodeImpl {
 public:
  /**
   * @brief Releases backend resources.
   */
  ~GetterImpl() override;

  /**
   * @brief Installs the latest-value notification callback.
   *
   * @details
   * Pure virtual.  Backends bind @p callback to the transport receive path so
   * the public @c Getter sees every value written by the matching @c Setter.
   * The owning @c Getter sets @c is_listened to @c true once this method
   * succeeds.
   *
   * @param callback  Callable @c void(const Bytes&) invoked on each update.
   * @return @c true on successful registration; @c false on error.
   */
  virtual bool listen(MsgCallback&& callback) = 0;

  /**
   * @brief Enables or disables per-update latency and loss tracking.
   *
   * @details
   * Default no-op.  Backends that maintain timestamps and sequence numbers
   * override the method to activate the instrumentation.
   *
   * @param enable  @c true to start tracking; @c false to stop.
   */
  virtual void set_latency_and_lost_enabled(bool enable);

  /**
   * @brief Reports whether tracking is currently enabled.
   *
   * @return @c true when the backend is collecting latency / loss data.
   */
  [[nodiscard]] virtual bool is_latency_and_lost_enabled() const;

  /**
   * @brief Returns the most recent end-to-end latency measurement in nanoseconds.
   *
   * @details
   * Only meaningful when @c is_latency_and_lost_enabled() returns @c true.
   *
   * @return Latency in nanoseconds; @c 0 if tracking is disabled or unsupported.
   */
  [[nodiscard]] virtual int64_t get_latency() const;

  /**
   * @brief Returns cumulative delivered / lost counts for field updates.
   *
   * @return @c SampleLostInfo with @c total and @c lost counters.
   */
  [[nodiscard]] virtual SampleLostInfo get_lost() const;

  bool is_listened{false};  ///< @c true once @c listen() has been registered successfully.

 protected:
  /**
   * @brief Stamps the node as @c kGetter.
   */
  GetterImpl();

 private:
  VLINK_DISALLOW_COPY_AND_ASSIGN(GetterImpl)
};

}  // namespace vlink
