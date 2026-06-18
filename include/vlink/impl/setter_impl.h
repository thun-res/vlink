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
 * @file setter_impl.h
 * @brief Transport-neutral base class for every field-model setter (latest-value writer).
 *
 * @details
 * This is an internal implementation header used by the public @c Setter
 * template; applications should depend on @c setter.h.  @c SetterImpl extends
 * @c NodeImpl with the field-model write semantics: writing overwrites the
 * topic's single tracked value and the backend may notify the setter via a
 * sync callback when a late getter needs the cached value resent.
 *
 * @par ImplType
 * The constructor stamps @c impl_type with @c kSetter so the discovery and
 * recording layers correctly identify outgoing field updates.
 *
 * @par Lifecycle
 * - Constructed by the matching @c Conf::create_setter().
 * - @c init() / @c deinit() inherited from @c NodeImpl bring the transport up.
 * - @c sync() is invoked by the public @c Setter once the user wires a
 *   late-getter sync callback.
 * - @c write() pushes the serialised value to all matched getters.
 *
 * @par Role table
 * | Capability                  | Provider                                  |
 * | --------------------------- | ----------------------------------------- |
 * | Wire write                  | Subclass override of @c write()           |
 * | Late-getter resend          | Subclass override of @c sync()            |
 *
 * @par Internal API contract
 * | Method                          | Default        | Subclass duty            |
 * | ------------------------------- | -------------- | ------------------------ |
 * | @c write(const Bytes&)          | Pure virtual   | Push frame to transport  |
 * | @c sync(SyncCallback&&)         | Pure virtual   | Bind late-join callback  |
 */

#pragma once

#include "./node_impl.h"

namespace vlink {

/**
 * @class SetterImpl
 * @brief Field-model writer base shared by every transport-specific setter.
 *
 * @details
 * Backends override @c write() to dispatch the serialised value onto the
 * transport and @c sync() to receive the transport-specific late-join
 * notification so the public @c Setter can resend its cached value.
 */
class VLINK_EXPORT SetterImpl : public NodeImpl {
 public:
  /**
   * @brief Releases backend resources.
   */
  ~SetterImpl() override;

  /**
   * @brief Writes a new value to every reachable getter.
   *
   * @details
   * Pure virtual.  @p msg_data is produced by @c Serializer::serialize() in the
   * public layer; the call overwrites any previously stored value on the topic.
   *
   * @param msg_data  Serialised value bytes.
   */
  virtual void write(const Bytes& msg_data) = 0;

  /**
   * @brief Installs the transport-specific late-getter sync callback.
   *
   * @details
   * Pure virtual.  Backends that detect a freshly attached getter invoke
   * @p callback so the public @c Setter can resend the cached latest value;
   * transports without such a signal may simply discard the callback.
   *
   * @param callback  Callable invoked when a late-getter sync is requested.
   */
  virtual void sync(SyncCallback&& callback) = 0;

 protected:
  /**
   * @brief Stamps the node as @c kSetter.
   */
  SetterImpl();

 private:
  VLINK_DISALLOW_COPY_AND_ASSIGN(SetterImpl)
};

}  // namespace vlink
