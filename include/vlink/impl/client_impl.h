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
 * @file client_impl.h
 * @brief Transport-neutral backbone shared by every method-model client implementation.
 *
 * @details
 * This is an internal implementation header used by the public @c Client template;
 * application code should depend on @c client.h instead.  @c ClientImpl extends
 * @c NodeImpl with the connectivity bookkeeping that lets a client wait for its
 * counterpart server and issue blocking calls.  Concrete transports such as
 * @c DdsClientImpl or @c ShmClientImpl derive from this class and implement the
 * pure virtual @c is_connected() and @c call() entry points.
 *
 * @par ImplType
 * The constructor stamps @c impl_type with @c kClient.  The base @c NodeImpl
 * surfaces this value to discovery, bag, and proxy layers so they can route
 * status callbacks correctly.
 *
 * @par Role table
 * | Concern               | Owner in this hierarchy                     |
 * | --------------------- | ------------------------------------------- |
 * | Wire send / receive   | Transport subclass (e.g. @c DdsClientImpl)  |
 * | Connection detection  | @c ClientImpl (helper condition variable)   |
 * | Blocking call timing  | @c ClientImpl + @c AckManager in subclass   |
 * | Connect callback fan  | @c ClientImpl::detect_connected()           |
 *
 * @par Internal API contract
 * | Method                       | Provided by         | Notes                                            |
 * | ---------------------------- | ------------------- | ------------------------------------------------ |
 * | @c is_connected() const      | Subclass override   | Queries the live transport handle.               |
 * | @c call(req, cb, timeout)    | Subclass override   | Performs the blocking round-trip.                |
 * | @c update_connected()        | This class          | Invoked from discovery threads on state change.  |
 * | @c detect_connected(cb)      | This class          | Stores the user callback and primes it.          |
 * | @c wait_for_connected(t)     | This class          | Sleeps on the helper condition variable.         |
 * | @c interrupt()               | This class          | Wakes waiters when the node is being shut down.  |
 *
 * @par Internal Notes
 * @c is_resp_type captures whether the public @c Client template expects a
 * response payload; backends that implement fire-and-forget RPC flavours may
 * read it to skip the response wait.
 */

#pragma once

#include <chrono>
#include <memory>

#include "./node_impl.h"

namespace vlink {

/**
 * @class ClientImpl
 * @brief Connection-aware base class for method-model client implementations.
 *
 * @details
 * Owns the helper condition variable used by @c wait_for_connected() and the
 * cached @c ConnectCallback delivered via @c detect_connected().  Backends are
 * expected to push presence updates by calling @c update_connected() whenever
 * the discovery layer reports a server appear / disappear event.
 */
class VLINK_EXPORT ClientImpl : public NodeImpl {
 public:
  /**
   * @brief Releases the helper state held by the base class.
   */
  ~ClientImpl() override;

  /**
   * @brief Wakes blocked waiters and forwards the interrupt to @c NodeImpl.
   *
   * @details
   * Marks the node interrupted, notifies the helper condition variable so any
   * @c wait_for_connected() call returns @c false promptly, and delegates the
   * rest of the shutdown handshake to @c NodeImpl::interrupt().  Transport
   * subclasses commonly override this to also cancel pending @c AckManager
   * requests.
   */
  void interrupt() override;

  /**
   * @brief Registers @p callback to fire whenever the server presence changes.
   *
   * @details
   * If the connection is already established at registration time, @p callback
   * is invoked with @c true before this method returns.  Subsequent transitions
   * are forwarded by @c update_connected().
   *
   * @param callback  Callable @c void(bool) to be notified on connect / disconnect.
   */
  virtual void detect_connected(ConnectCallback&& callback);

  /**
   * @brief Blocks until a server becomes reachable or @p timeout elapses.
   *
   * @details
   * Returns immediately when @c is_connected() already reports @c true.
   * Otherwise sleeps on the helper condition variable that is poked by
   * @c update_connected() and by @c interrupt().  A negative @p timeout, such
   * as @c Timeout::kInfinite, disables the deadline.
   *
   * @param timeout  Maximum sleep duration.
   * @return @c true when the server appeared in time; @c false on timeout or interruption.
   */
  virtual bool wait_for_connected(std::chrono::milliseconds timeout);

  /**
   * @brief Reports whether the transport currently holds a path to a server.
   *
   * @details
   * Pure virtual; subclasses query their underlying discovery handle.  Called
   * by both @c wait_for_connected() and @c update_connected() to compute state
   * transitions.
   *
   * @return @c true when a server is reachable; @c false otherwise.
   */
  [[nodiscard]] virtual bool is_connected() const = 0;

  /**
   * @brief Performs a synchronous round-trip with the remote server.
   *
   * @details
   * Pure virtual; subclasses send @p req_data, wait for the matching response
   * (typically through @c AckManager) and invoke @p callback with the received
   * bytes once a frame arrives.
   *
   * @param req_data  Serialised request payload.
   * @param callback  Callable @c void(const Bytes&) receiving the response bytes.
   * @param timeout   Maximum wait duration; negative for unlimited.
   * @return @c true on success; @c false on timeout, interruption or transport error.
   */
  virtual bool call(const Bytes& req_data, MsgCallback&& callback, std::chrono::milliseconds timeout) = 0;

  /**
   * @brief Notifies the base class that the underlying connection state may have changed.
   *
   * @details
   * Compares @c is_connected() against the helper cache; when the value flips
   * the condition variable is signalled and any registered @c ConnectCallback
   * is delivered.  Intended to run on the transport's discovery / listener
   * thread, not on the application thread.
   */
  void update_connected();

  bool is_resp_type{false};  ///< @c true when the public @c Client expects a response payload.

 protected:
  /**
   * @brief Stamps the node as @c kClient and prepares the helper state.
   */
  ClientImpl();

 private:
  std::unique_ptr<struct ClientImplHelper> helper_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(ClientImpl)
};

}  // namespace vlink
