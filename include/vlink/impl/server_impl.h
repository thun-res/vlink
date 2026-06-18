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
 * @file server_impl.h
 * @brief Transport-neutral base class for every method-model server implementation.
 *
 * @details
 * This is an internal implementation header used by the public @c Server
 * template; applications should depend on @c server.h.  @c ServerImpl extends
 * @c NodeImpl with the responder-side bookkeeping: it tracks whether a
 * @c listen() callback has been installed, whether a response is expected and
 * whether the reply is delivered synchronously inside the request callback or
 * asynchronously via @c reply().
 *
 * @par ImplType
 * The constructor stamps @c impl_type with @c kServer, advertising the role to
 * discovery and recording layers.
 *
 * @par Lifecycle
 * - Construction stamps the impl type.
 * - The public @c Server template calls @c listen() once the user installs a
 *   request handler; @c is_listened becomes @c true on success.
 * - @c init() / @c deinit() inherited from @c NodeImpl bring the transport up.
 * - Synchronous mode writes the response into the buffer supplied to the
 *   handler; asynchronous mode requires a later @c reply() call.
 *
 * @par Role table
 * | Capability                       | Provider                                                |
 * | -------------------------------- | ------------------------------------------------------- |
 * | Request callback registration    | Subclass override of @c listen()                        |
 * | Client presence query            | Subclass override of @c has_clients() (optional)        |
 * | Deferred response delivery       | Subclass override of @c reply() for async transports    |
 * | Sync / async behaviour flags     | @c is_resp_type / @c is_sync_type (set by public layer) |
 *
 * @par Internal API contract
 * | Method                                 | Default                       | Subclass duty             |
 * | -------------------------------------- | ----------------------------- | ------------------------- |
 * | @c listen(ReqRespCallback&&)           | Pure virtual                  | Bind transport receiver   |
 * | @c has_clients() const                 | Returns @c false              | Report client presence    |
 * | @c reply(req_id, bytes, is_sync)       | Warns on async path           | Implement deferred reply  |
 */

#pragma once

#include "./node_impl.h"

namespace vlink {

/**
 * @class ServerImpl
 * @brief Method-model server base shared by every transport implementation.
 *
 * @details
 * Backends override @c listen() to bind the supplied callback to the transport
 * receive path and, where supported, override @c has_clients() and @c reply()
 * to expose client discovery and asynchronous responses respectively.
 */
class VLINK_EXPORT ServerImpl : public NodeImpl {
 public:
  /**
   * @brief Releases backend resources.
   */
  ~ServerImpl() override;

  /**
   * @brief Registers the request / response handler.
   *
   * @details
   * Pure virtual.  The callback is invoked for every incoming RPC with the
   * request bytes and a unique @c req_id; the handler writes the response into
   * @c *resp_data, or receives @c nullptr in fire-and-forget mode.  The owning
   * @c Server marks @c is_listened to @c true once this method succeeds.
   *
   * @param callback  Callable @c void(uint64_t req_id, const Bytes& req_data, Bytes* resp_data).
   * @return @c true on success; @c false on registration error.
   */
  virtual bool listen(ReqRespCallback&& callback) = 0;

  /**
   * @brief Reports whether at least one client is currently connected.
   *
   * @details
   * Default returns @c false.  Backends that expose matched-publication
   * discovery override the method.
   *
   * @return @c true when one or more clients are reachable.
   */
  [[nodiscard]] virtual bool has_clients() const;

  /**
   * @brief Sends a response for a previously received request.
   *
   * @details
   * Used in asynchronous server mode (@c is_sync_type == @c false) when the
   * response is produced after the request callback returns.  The base
   * implementation warns when @c is_sync is @c false and always returns
   * @c false; backends that support deferred replies override the method.
   *
   * @param req_id     Identifier supplied to the request callback.
   * @param resp_data  Serialised response payload.
   * @param is_sync    @c true when called synchronously inside the request callback.
   * @return @c true on success; @c false otherwise.
   */
  virtual bool reply(uint64_t req_id, const Bytes& resp_data, bool is_sync);

  bool is_listened{false};   ///< @c true once @c listen() has been registered successfully.
  bool is_resp_type{false};  ///< @c true when the server is expected to produce a response.
  bool is_sync_type{false};  ///< @c true when the reply is delivered inside the request callback.

 protected:
  /**
   * @brief Stamps the node as @c kServer.
   */
  ServerImpl();

 private:
  VLINK_DISALLOW_COPY_AND_ASSIGN(ServerImpl)
};

}  // namespace vlink
