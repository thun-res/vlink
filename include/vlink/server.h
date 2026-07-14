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
 * @file server.h
 * @brief Handler-side primitive of the VLink method (RPC) communication model.
 *
 * @details
 * @c Server\<ReqT, RespT, SecT\> registers a handler for inbound RPC requests
 * issued by matching @c Client instances.  Three handler styles are
 * supported: fire-and-forget (no response), synchronous request/response,
 * and deferred asynchronous reply.  Codec selection for both the request
 * and the response payload is resolved at compile time.
 *
 * The class is a thin, header-only template wrapper around @c ServerImpl.
 *
 * @par Request / Response Sequence
 * @verbatim
 *   Client<Req,Resp>           Transport Back-end           Server<Req,Resp>
 *   -----------------          ------------------           -----------------
 *      | invoke(req)                  |                            |
 *      |----------------------------->|  serialised request        |
 *      |                              |--------------------------->|
 *      |                              |                            |  handler
 *      |                              |                            |  fills resp
 *      |                              |                            |   (sync mode)
 *      |                              |                            |     -- or --
 *      |                              |                            |  store req_id
 *      |                              |                            |  reply(req_id, resp)
 *      |                              |                            |   (async mode)
 *      |                              |  serialised response       |
 *      |                              |<---------------------------|
 *      | deserialised response        |                            |
 *      |<-----------------------------|                            |
 * @endverbatim
 *
 * @par Three Listen-Handler Variants
 * | Method                                    | Handler signature              | Semantics                     |
 * | ----------------------------------------- | ------------------------------ | ----------------------------- |
 * | @c listen(ReqCallback)                    | @c void(const ReqT&)           | Fire-and-forget; no response. |
 * | @c listen(ReqRespCallback)                | @c void(const ReqT&, RespT&)   | Synchronous fill of @c resp.  |
 * | @c listen_for_reply(ReqAsyncRespCallback) | @c void(uint64_t, const Req&)  | Deferred reply via @c reply.  |
 *
 * @par Synchronous Reply Example
 * @code
 * vlink::Server<Req, Resp> svr("dds://compute/sum");
 * svr.listen([](const Req& q, Resp& r) {
 *   r.value = q.a + q.b;
 * });
 * @endcode
 *
 * @par Deferred Asynchronous Reply Example
 * @code
 * vlink::Server<Req, Resp> svr("dds://compute/sum");
 * uint64_t pending = 0;
 * svr.listen_for_reply([&](uint64_t id, const Req& q) {
 *   pending = id;
 *   schedule_async_work(q);
 * });
 *
 * // ...some time later, from any thread:
 * svr.reply(pending, Resp{result});
 * @endcode
 *
 * @par Fire-and-Forget Example
 * @code
 * vlink::Server<Req> svr("dds://logger/push");   // RespT defaults to EmptyType
 * svr.listen([](const Req& q) { write_log(q); });
 * @endcode
 *
 * @note Calling @c listen() or @c listen_for_reply() more than once is a
 *       fatal error.  @c reply() must only be used following
 *       @c listen_for_reply(); calling it after a synchronous @c listen()
 *       triggers a fatal log.
 *
 * @see client.h, node.h, serializer.h, base/functional.h
 */

#pragma once

#include <memory>
#include <string>
#include <type_traits>

#include "./base/functional.h"
#include "./impl/server_impl.h"
#include "./node.h"

namespace vlink {

/**
 * @class Server
 * @brief Type-safe RPC handler for the VLink method communication model.
 *
 * @details
 * Inherits the full @c Node API and adds handler-side operations:
 * three @c listen() variants and the deferred @c reply() entry point.  The
 * transport implementation (@c ServerImpl) is selected by the URL scheme or
 * by the typed configuration object supplied at construction time.
 *
 * @tparam ReqT   Request message type. Must satisfy @c Serializer::is_supported().
 * @tparam RespT  Response message type.  Defaults to @c Traits::EmptyType (no response).
 * @tparam SecT   Security mode; defaults to @c SecurityType::kWithoutSecurity.
 */
template <typename ReqT, typename RespT = Traits::EmptyType, SecurityType SecT = SecurityType::kWithoutSecurity>
class Server : public Node<ServerImpl, SecT> {
 public:
  using UniquePtr = std::unique_ptr<Server<ReqT, RespT, SecT>>;  ///< Owning unique-pointer alias.
  using SharedPtr = std::shared_ptr<Server<ReqT, RespT, SecT>>;  ///< Owning shared-pointer alias.
  using ReqCallback = Function<void(const ReqT&)>;               ///< Fire-and-forget handler signature.
  using ReqRespCallback = Function<void(const ReqT&, RespT&)>;   ///< Synchronous fill-response handler.

  /**
   * @brief Handler signature for deferred asynchronous replies.
   *
   * @details
   * Receives the opaque @c req_id assigned by the framework alongside the
   * incoming request.  The handler must eventually call @c reply(req_id, resp)
   * from any thread to deliver the response.
   */
  using ReqAsyncRespCallback = Function<void(uint64_t, const ReqT&)>;

  static constexpr ImplType kImplType = kServer;                                   ///< Node role tag (@c kServer).
  static constexpr bool kHasResp = !std::is_same_v<RespT, Traits::EmptyType>;      ///< @c true when response produced.
  static constexpr Serializer::Type kReqType = Serializer::get_type_of<ReqT>();    ///< Codec for the request.
  static constexpr Serializer::Type kRespType = Serializer::get_type_of<RespT>();  ///< Codec for the response.

  static_assert(Serializer::is_supported(kReqType), "<ReqT> is not a supported Serializer type.");
  static_assert(!kHasResp || Serializer::is_supported(kRespType), "<RespT> is not a supported Serializer type.");

  /**
   * @brief Heap-allocates a @c Server and wraps it in a @c std::unique_ptr.
   *
   * @param url_str  Service URL such as @c "dds://my_service".
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   * @return         Owning @c UniquePtr to the new server.
   */
  [[nodiscard]] static UniquePtr create_unique(const std::string& url_str, InitType type = InitType::kWithInit);

  /**
   * @brief Heap-allocates a @c Server and wraps it in a @c std::shared_ptr.
   *
   * @param url_str  Service URL string.
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   * @return         Owning @c SharedPtr to the new server.
   */
  [[nodiscard]] static SharedPtr create_shared(const std::string& url_str, InitType type = InitType::kWithInit);

  /**
   * @brief Constructs a server from a typed transport configuration object.
   *
   * @details
   * Accepts any @c Conf-derived configuration.  A compile-time check
   * enforces that the configuration permits the server role.
   *
   * @tparam ConfT  Concrete configuration type derived from @c Conf.
   * @param conf    Populated configuration aggregate.
   * @param type    Whether to call @c init() inline; default is @c InitType::kWithInit.
   */
  // NOLINTNEXTLINE(modernize-use-constraints)
  template <typename ConfT, typename = std::enable_if_t<std::is_base_of_v<Conf, ConfT>>>
  explicit Server(const ConfT& conf, InitType type = InitType::kWithInit);

  /**
   * @brief Constructs a server from a URL string.
   *
   * @param url_str  Service URL such as @c "someip://30490/0x1/my_method".
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   */
  explicit Server(const std::string& url_str, InitType type = InitType::kWithInit);

  /**
   * @brief Installs a fire-and-forget request handler.
   *
   * @details
   * Only available when @c RespT == @c EmptyType (enforced by
   * @c static_assert).  The handler is invoked for every received request
   * and no response is produced.
   *
   * @param callback  @c void(const ReqT&) handler.
   * @return          @c true if registration succeeded.
   */
  bool listen(ReqCallback&& callback);

  /**
   * @brief Installs a synchronous request/response handler.
   *
   * @details
   * Only available when @c kHasResp is @c true.  The handler must populate
   * @c resp before returning; the framework serialises @c resp immediately
   * and emits the reply on the underlying transport.
   *
   * @param callback  @c void(const ReqT&, RespT&) handler that fills @c resp.
   * @return          @c true if registration succeeded.
   */
  bool listen(ReqRespCallback&& callback);

  /**
   * @brief Installs a handler that defers the reply via @c reply().
   *
   * @details
   * Only available when @c kHasResp is @c true.  The handler receives an
   * opaque @c req_id and the deserialised request.  The handler must
   * eventually invoke @c reply(req_id, resp) -- from any thread -- to send
   * the response back to the waiting client.
   *
   * @param callback  @c void(uint64_t, const ReqT&) handler.
   * @return          @c true if registration succeeded.
   */
  bool listen_for_reply(ReqAsyncRespCallback&& callback);

  /**
   * @brief Emits the asynchronous response for a previously received request.
   *
   * @details
   * Must be paired with @c listen_for_reply(); calling @c reply() after a
   * synchronous @c listen() triggers a fatal log.  The @c req_id must match
   * the value passed to the async handler.  Unknown IDs may be silently
   * rejected by the active transport.
   *
   * @param req_id  Opaque identifier received in the async handler.
   * @param resp    Response value to serialise and emit.
   * @return        @c true if the transport accepted the response.
   */
  bool reply(uint64_t req_id, const RespT& resp);

 private:
  [[nodiscard]] bool has_clients() const;

  bool listen_bytes(NodeImpl::ReqRespCallback&& callback);

  template <bool HasPtrT>
  bool reply_bytes(uint64_t req_id, const Bytes& resp_data, bool is_sync, Bytes* resp_data_ptr = nullptr);
};

/**
 * @class SecurityServer
 * @brief Convenience alias of @c Server with per-message encryption enabled.
 *
 * @details
 * Equivalent to @c Server\<ReqT, RespT, SecurityType::kWithSecurity\>.  Each
 * incoming request is decrypted before dispatch to the handler, and each
 * outgoing response is encrypted before transmission.
 *
 * @tparam ReqT   Request message type.
 * @tparam RespT  Response message type. Defaults to @c Traits::EmptyType.
 */
template <typename ReqT, typename RespT = Traits::EmptyType>
class SecurityServer : public Server<ReqT, RespT, SecurityType::kWithSecurity> {
 public:
  using UniquePtr = std::unique_ptr<SecurityServer<ReqT, RespT>>;  ///< Owning unique-pointer alias.
  using SharedPtr = std::shared_ptr<SecurityServer<ReqT, RespT>>;  ///< Owning shared-pointer alias.

  /**
   * @brief Heap-allocates a @c SecurityServer and wraps it in a @c std::unique_ptr.
   *
   * @tparam SecurityConfigT  Forwardable @c Security::Config compatible type.
   * @param url_str  Service URL string.
   * @param sec_cfg  Security configuration; empty uses the default symmetric slot.
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   * @return         Owning @c UniquePtr to the new secure server.
   */
  // NOLINTNEXTLINE(modernize-use-constraints)
  template <typename SecurityConfigT = Security::Config>
  [[nodiscard]] static UniquePtr create_unique(const std::string& url_str, SecurityConfigT&& sec_cfg = {},
                                               InitType type = InitType::kWithInit);

  /**
   * @brief Heap-allocates a @c SecurityServer and wraps it in a @c std::shared_ptr.
   *
   * @tparam SecurityConfigT  Forwardable @c Security::Config compatible type.
   * @param url_str  Service URL string.
   * @param sec_cfg  Security configuration; empty uses the default symmetric slot.
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   * @return         Owning @c SharedPtr to the new secure server.
   */
  // NOLINTNEXTLINE(modernize-use-constraints)
  template <typename SecurityConfigT = Security::Config>
  [[nodiscard]] static SharedPtr create_shared(const std::string& url_str, SecurityConfigT&& sec_cfg = {},
                                               InitType type = InitType::kWithInit);

  /**
   * @brief Constructs a @c SecurityServer from a typed configuration object.
   *
   * @tparam ConfT           Configuration type derived from @c Conf.
   * @tparam SecurityConfigT Forwardable @c Security::Config compatible type.
   * @param conf     Populated configuration aggregate.
   * @param sec_cfg  Security configuration; empty uses the default symmetric slot.
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   */
  // NOLINTNEXTLINE(modernize-use-constraints)
  template <typename ConfT, typename SecurityConfigT = Security::Config,
            typename = std::enable_if_t<std::is_base_of_v<Conf, ConfT>>>  // NOLINT(modernize-use-constraints)
  explicit SecurityServer(const ConfT& conf, SecurityConfigT&& sec_cfg = {}, InitType type = InitType::kWithInit);

  /**
   * @brief Constructs a @c SecurityServer from a URL string and installs the security configuration.
   *
   * @details
   * Builds the base @c Server in @c kWithoutInit mode, installs @p sec_cfg
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
  explicit SecurityServer(const std::string& url_str, SecurityConfigT&& sec_cfg = {},
                          InitType type = InitType::kWithInit);
};

}  // namespace vlink

#include "./internal/server-inl.h"
