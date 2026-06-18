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
 * @file client.h
 * @brief Caller-side primitive of the VLink method (RPC) communication model.
 *
 * @details
 * @c Client\<ReqT, RespT, SecT\> issues remote calls to a matching @c Server.
 * The request is serialised with the codec selected at compile time, sent
 * via the active transport back-end, and (when @c RespT != @c EmptyType) the
 * response payload is decoded back into a @c RespT instance and handed to
 * the caller through one of several invocation styles.
 *
 * The class is a thin, header-only template wrapper around @c ClientImpl
 * with additional bookkeeping for @c std::future-based async invocations.
 *
 * @par Request / Response Sequence
 * @verbatim
 *   Client<Req,Resp>           Transport Back-end           Server<Req,Resp>
 *   -----------------          ------------------           -----------------
 *      | invoke(req)                  |                            |
 *      |----------------------------->|  serialised request        |
 *      |                              |--------------------------->|
 *      |                              |                            |  user handler
 *      |                              |                            |  fills resp
 *      |                              |                            |
 *      |                              |  serialised response       |
 *      |                              |<---------------------------|
 *      | deserialised response        |                            |
 *      |<-----------------------------|                            |
 * @endverbatim
 *
 * @par Five Invocation Modes
 * | Method                                          | Blocking | Result delivery       | When to use               |
 * | ----------------------------------------------- | -------- | --------------------- | ------------------------- |
 * | @c invoke(req, resp&, timeout)                  | Yes      | Out-param + bool      | Classic synchronous call  |
 * | @c invoke(req, timeout)                         | Yes      | @c std::optional      | Synchronous, no out-param |
 * | @c invoke(req, RespCallback)                    | No       | Callback              | Async with handler        |
 * | @c async_invoke(req)                            | No       | @c std::future        | Future / promise model    |
 * | @c send(req)                                    | No       | None                  | Fire-and-forget request   |
 *
 * @par Synchronous Invocation Example
 * @code
 * vlink::Client<Req, Resp> cli("dds://compute/sum");
 * cli.wait_for_connected();
 *
 * Resp resp;
 * if (cli.invoke(Req{1, 2}, resp)) { use(resp); }
 *
 * if (auto r = cli.invoke(Req{3, 4})) { use(*r); }
 * @endcode
 *
 * @par Asynchronous Invocation Example
 * @code
 * cli.invoke(Req{7, 8}, [](const Resp& r) { use(r); });
 *
 * std::future<Resp> fut = cli.async_invoke(Req{9, 10});
 * Resp value = fut.get();
 * @endcode
 *
 * @par Connection Detection
 * @code
 * cli.detect_connected([](bool connected) { ... });
 * cli.wait_for_connected(std::chrono::milliseconds(200));
 * if (cli.is_connected()) { ... }
 * @endcode
 *
 * @note A @p timeout of @c 0 is treated as infinite and logs a warning.
 *       @c send() is only available when @c RespT == @c EmptyType, and the
 *       response-bearing overloads are only available when @c kHasResp is
 *       @c true (enforced by @c static_assert inside each entry point).
 *
 * @see server.h, node.h, serializer.h, base/functional.h
 */

#pragma once

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>

#include "./base/functional.h"
#include "./impl/client_impl.h"
#include "./node.h"

namespace vlink {

/**
 * @class Client
 * @brief Type-safe RPC caller for the VLink method communication model.
 *
 * @details
 * Inherits the full @c Node API and adds method-specific operations:
 * connection detection, blocking and non-blocking invocation, future-based
 * async invocation, and fire-and-forget @c send().  The transport
 * implementation (@c ClientImpl) is selected by the URL scheme or by the
 * typed configuration object supplied at construction time.
 *
 * @tparam ReqT   Request message type. Must satisfy @c Serializer::is_supported().
 * @tparam RespT  Response message type.  Defaults to @c Traits::EmptyType for fire-and-forget.
 * @tparam SecT   Security mode; defaults to @c SecurityType::kWithoutSecurity.
 */
template <typename ReqT, typename RespT = Traits::EmptyType, SecurityType SecT = SecurityType::kWithoutSecurity>
class Client : public Node<ClientImpl, SecT> {
 public:
  using UniquePtr = std::unique_ptr<Client<ReqT, RespT, SecT>>;  ///< Owning unique-pointer alias.
  using SharedPtr = std::shared_ptr<Client<ReqT, RespT, SecT>>;  ///< Owning shared-pointer alias.
  using ConnectCallback = NodeImpl::ConnectCallback;             ///< Callback type for server presence transitions.
  using RespCallback = Function<void(const RespT&)>;             ///< Callback signature for asynchronous responses.

  static constexpr ImplType kImplType = kClient;                                   ///< Node role tag (@c kClient).
  static constexpr bool kHasResp = !std::is_same_v<RespT, Traits::EmptyType>;      ///< @c true when response expected.
  static constexpr Serializer::Type kReqType = Serializer::get_type_of<ReqT>();    ///< Codec for the request.
  static constexpr Serializer::Type kRespType = Serializer::get_type_of<RespT>();  ///< Codec for the response.

  static_assert(Serializer::is_supported(kReqType), "<ReqT> is not a supported Serializer type.");
  static_assert(!kHasResp || Serializer::is_supported(kRespType), "<RespT> is not a supported Serializer type.");

  /**
   * @brief Heap-allocates a @c Client and wraps it in a @c std::unique_ptr.
   *
   * @param url_str  Service URL string.
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   * @return         Owning @c UniquePtr to the new client.
   */
  [[nodiscard]] static UniquePtr create_unique(const std::string& url_str, InitType type = InitType::kWithInit);

  /**
   * @brief Heap-allocates a @c Client and wraps it in a @c std::shared_ptr.
   *
   * @param url_str  Service URL string.
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   * @return         Owning @c SharedPtr to the new client.
   */
  [[nodiscard]] static SharedPtr create_shared(const std::string& url_str, InitType type = InitType::kWithInit);

  /**
   * @brief Constructs a client from a typed transport configuration object.
   *
   * @details
   * Accepts any @c Conf-derived configuration.  A compile-time check
   * enforces that the configuration permits the client role.
   *
   * @tparam ConfT  Concrete configuration type derived from @c Conf.
   * @param conf    Populated configuration aggregate.
   * @param type    Whether to call @c init() inline; default is @c InitType::kWithInit.
   */
  // NOLINTNEXTLINE(modernize-use-constraints)
  template <typename ConfT, typename = std::enable_if_t<std::is_base_of_v<Conf, ConfT>>>
  explicit Client(const ConfT& conf, InitType type = InitType::kWithInit);

  /**
   * @brief Constructs a client from a URL string.
   *
   * @param url_str  Service URL such as @c "someip://30490/0x1/my_method".
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   */
  explicit Client(const std::string& url_str, InitType type = InitType::kWithInit);

  /**
   * @brief Destroys the client and tears down outstanding promises.
   *
   * @details
   * Required to drain any in-flight @c async_invoke() promises before this
   * object's locks and maps are destroyed.
   */
  ~Client() override;

  /**
   * @brief Registers a callback invoked when the server-presence state changes.
   *
   * @details
   * Fires synchronously immediately if a server is already discovered at
   * registration; otherwise fires asynchronously the first time a matching
   * server appears, and again on disconnection.
   *
   * @param callback  @c void(bool) callable -- @c true means connected.
   */
  void detect_connected(ConnectCallback&& callback);

  /**
   * @brief Blocks until a server is discovered or @p timeout expires.
   *
   * @details
   * A @p timeout of @c 0 is treated as infinite (with a warning).  A
   * negative value also waits indefinitely.  @c interrupt() causes the wait
   * to abort and return @c false.
   *
   * @param timeout  Maximum wait duration.  Default: @c Timeout::kDefaultInterval.
   * @return         @c true if a server appeared; @c false on timeout or interrupt.
   */
  bool wait_for_connected(std::chrono::milliseconds timeout = Timeout::kDefaultInterval);

  /**
   * @brief Non-blocking query of server presence.
   *
   * @return @c true when the transport currently has a matching server.
   */
  [[nodiscard]] bool is_connected() const;

  /**
   * @brief Synchronous request/response invocation with an output parameter.
   *
   * @details
   * Only available when @c kHasResp is @c true.  Serialises @p req, blocks for
   * up to @p timeout, and deserialises the reply into @p resp.  A @p timeout
   * of @c 0 is treated as infinite.
   *
   * @param req      Request value.
   * @param resp     Output parameter receiving the deserialised response.
   * @param timeout  Maximum wait for the response.
   * @return         @c true if the response was received in time.
   */
  [[nodiscard]] bool invoke(const ReqT& req, RespT& resp,
                            std::chrono::milliseconds timeout = Timeout::kDefaultInterval);

  /**
   * @brief Synchronous request/response invocation returning a @c std::optional.
   *
   * @details
   * Convenience wrapper that returns @c std::nullopt on timeout, transport
   * failure, or codec failure.  Only available when @c kHasResp is @c true.
   *
   * @param req      Request value.
   * @param timeout  Maximum wait for the response.
   * @return         @c std::optional\<RespT\>; empty on failure.
   */
  [[nodiscard]] std::optional<RespT> invoke(const ReqT& req,
                                            std::chrono::milliseconds timeout = Timeout::kDefaultInterval);

  /**
   * @brief Asynchronous callback-based invocation.
   *
   * @details
   * Only available when @c kHasResp is @c true.  The method returns
   * immediately; @p callback runs on the transport delivery thread or on the
   * attached @c MessageLoop thread when the response arrives.
   *
   * @param req       Request value.
   * @param callback  @c void(const RespT&) invoked once the response arrives.
   * @return          @c true if the transport accepted the request.
   */
  bool invoke(const ReqT& req, RespCallback&& callback);

  /**
   * @brief Asynchronous future-based invocation.
   *
   * @details
   * Returns a @c std::future that is set when the response arrives.  On
   * serialisation, transport, or deserialisation failure the future's
   * exception state is populated with @c Exception::RuntimeError.  Only
   * available when @c kHasResp is @c true.
   *
   * @param req  Request value.
   * @return     @c std::future\<RespT\> resolved when the response is delivered.
   */
  [[nodiscard]] std::future<RespT> async_invoke(const ReqT& req);

  /**
   * @brief Fire-and-forget request emission.
   *
   * @details
   * Only available when @c RespT == @c EmptyType.  Serialises @p req and
   * passes it to the transport without waiting for or expecting a reply.
   *
   * @param req  Request value.
   * @return     @c true if the transport accepted the request.
   */
  bool send(const ReqT& req);

 private:
  bool call_bytes(const Bytes& req_data, NodeImpl::MsgCallback&& callback = nullptr,
                  std::chrono::milliseconds timeout = std::chrono::milliseconds(0));

  uint64_t future_seq_{0};
  std::mutex future_mtx_;
  std::unordered_map<std::uint64_t, std::shared_ptr<std::promise<RespT>>> future_map_;
};

/**
 * @class SecurityClient
 * @brief Convenience alias of @c Client with per-message encryption enabled.
 *
 * @details
 * Equivalent to @c Client\<ReqT, RespT, SecurityType::kWithSecurity\>.  Each
 * outgoing request is encrypted before transmission and each incoming
 * response is decrypted before codec dispatch.
 *
 * @tparam ReqT   Request message type.
 * @tparam RespT  Response message type. Defaults to @c Traits::EmptyType.
 */
template <typename ReqT, typename RespT = Traits::EmptyType>
class SecurityClient : public Client<ReqT, RespT, SecurityType::kWithSecurity> {
 public:
  using UniquePtr = std::unique_ptr<SecurityClient<ReqT, RespT>>;  ///< Owning unique-pointer alias.
  using SharedPtr = std::shared_ptr<SecurityClient<ReqT, RespT>>;  ///< Owning shared-pointer alias.

  /**
   * @brief Heap-allocates a @c SecurityClient and wraps it in a @c std::unique_ptr.
   *
   * @tparam SecurityConfigT  Forwardable @c Security::Config compatible type.
   * @param url_str  Service URL string.
   * @param sec_cfg  Security configuration; empty uses the default symmetric slot.
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   * @return         Owning @c UniquePtr to the new secure client.
   */
  // NOLINTNEXTLINE(modernize-use-constraints)
  template <typename SecurityConfigT = Security::Config>
  [[nodiscard]] static UniquePtr create_unique(const std::string& url_str, SecurityConfigT&& sec_cfg = {},
                                               InitType type = InitType::kWithInit);

  /**
   * @brief Heap-allocates a @c SecurityClient and wraps it in a @c std::shared_ptr.
   *
   * @tparam SecurityConfigT  Forwardable @c Security::Config compatible type.
   * @param url_str  Service URL string.
   * @param sec_cfg  Security configuration; empty uses the default symmetric slot.
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   * @return         Owning @c SharedPtr to the new secure client.
   */
  // NOLINTNEXTLINE(modernize-use-constraints)
  template <typename SecurityConfigT = Security::Config>
  [[nodiscard]] static SharedPtr create_shared(const std::string& url_str, SecurityConfigT&& sec_cfg = {},
                                               InitType type = InitType::kWithInit);

  /**
   * @brief Constructs a @c SecurityClient from a typed configuration object.
   *
   * @tparam ConfT           Configuration type derived from @c Conf.
   * @tparam SecurityConfigT Forwardable @c Security::Config compatible type.
   * @param conf     Populated configuration aggregate.
   * @param sec_cfg  Security configuration; empty uses the default symmetric slot.
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   */
  // NOLINTNEXTLINE(modernize-use-constraints)
  template <typename ConfT, typename SecurityConfigT = Security::Config,
            typename = std::enable_if_t<std::is_base_of_v<Conf, ConfT>>>
  explicit SecurityClient(const ConfT& conf, SecurityConfigT&& sec_cfg = {}, InitType type = InitType::kWithInit);

  /**
   * @brief Constructs a @c SecurityClient from a URL string and installs the security configuration.
   *
   * @details
   * Builds the base @c Client in @c kWithoutInit mode, installs @p sec_cfg
   * via @c enable_security(), then calls @c init() unless deferred.  When
   * @c enable_security() fails to produce a usable @c NodeImpl::security the
   * subsequent @c init() will fail.
   *
   * @tparam SecurityConfigT  Forwardable @c Security::Config compatible type.
   * @param url_str  Service URL string.
   * @param sec_cfg  Security configuration; empty uses the default symmetric slot.
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   */
  // NOLINTNEXTLINE(modernize-use-constraints)
  template <typename SecurityConfigT = Security::Config>
  explicit SecurityClient(const std::string& url_str, SecurityConfigT&& sec_cfg = {},
                          InitType type = InitType::kWithInit);
};

}  // namespace vlink

#include "./internal/client-inl.h"
