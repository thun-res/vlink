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
 * @file publisher.h
 * @brief Write-side primitive of the VLink event communication model.
 *
 * @details
 * @c Publisher\<MsgT, SecT\> is the message emitter for VLink topics.  Each
 * call to @c publish() serialises an instance of @c MsgT according to the
 * compile-time-detected codec and hands the resulting payload to the active
 * transport back-end, which fans the bytes out to every connected
 * @c Subscriber that shares the same topic URL.
 *
 * The class is a thin, header-only template wrapper around @c PublisherImpl;
 * all runtime work is delegated to the impl.  Codec selection is fully
 * resolved at compile time so the runtime dispatch is zero-cost.
 *
 * @par Event-model Data Flow
 * @verbatim
 *   Publisher<MsgT>               Transport Back-end              Subscriber<MsgT>
 *   ----------------              ------------------              -----------------
 *      |                                  |                              |
 *      |-- publish(msg, force) ---------->|                              |
 *      |   Serializer::serialize          |                              |
 *      |   (loan-aware on shm/zenoh)      |                              |
 *      |                                  |--- frame on wire ----------->|
 *      |                                  |                              |
 *      |                                  |                              |--> callback
 *      |                                  |                              |    Serializer::
 *      |                                  |                              |    deserialize
 * @endverbatim
 *
 * @par Supported Message Types
 * | Category          | Example C++ Type                  | @c Serializer::Type | Notes                          |
 * | ----------------- | --------------------------------- | ------------------- | ------------------------------ |
 * | Raw bytes         | @c Bytes                          | @c kBytesType       | Pass-through, no codec call.   |
 * | Protobuf value    | @c MyProto                        | @c kProtoType       | Uses SerializeToArray.         |
 * | Protobuf pointer  | @c MyProto*                       | @c kProtoPtrType    | Caller owns lifetime.          |
 * | FlatBuffers obj   | @c MyTableT (NativeTable)         | @c kFlatTableType   | Object API.                    |
 * | FlatBuffers ptr   | @c MyTable* (Table pointer)       | @c kFlatPtrType     | Zero-copy read view.           |
 * | FlatBuffers build | @c MyBuilder (@c fbb_ + Finish)   | @c kFlatBuilderType | Finishes builder on publish.   |
 * | DDS CDR           | type with @c serialize(Cdr&)      | @c kCdrType         | DDS fast path.                 |
 * | POD struct        | trivial standard-layout type      | @c kStandardType    | @c sizeof(T) byte copy.        |
 * | UTF-8 text        | @c std::string                    | @c kStringType      | Length-prefixed.               |
 * | Custom            | type with @c operator>>/<<        | @c kCustomType      | User-supplied codec.           |
 *
 * @par Subscriber Detection
 * Use the detection API to gate writes on the presence of at least one
 * matching subscriber.  This is purely informational; pass @c force=true to
 * @c publish() if you wish to write regardless of detection state.
 *
 * @code
 * vlink::Publisher<MyMsg> pub("dds://sensor/imu");
 *
 * pub.detect_subscribers([&pub](bool present) {
 *   if (present) { pub.publish(MyMsg{}); }
 * });
 *
 * if (pub.wait_for_subscribers(std::chrono::milliseconds(500))) {
 *   pub.publish(MyMsg{});
 * }
 *
 * if (pub.has_subscribers()) { pub.publish(MyMsg{}); }
 * @endcode
 *
 * @par Zero-copy Loan on Loan-capable Transports
 * @code
 * vlink::Publisher<vlink::Bytes> pub("shm://image/raw");
 * if (pub.is_support_loan()) {
 *   vlink::Bytes buf = pub.loan(sizeof(FrameHeader) + payload_size);
 *   ::memcpy(buf.data(), &header, sizeof(header));
 *   pub.publish(buf);
 * }
 * @endcode
 *
 * @note Loans bypass an extra copy by writing directly into transport-managed
 *       memory.  Loans are not used when security is enabled because
 *       ciphertext length differs from plaintext length.
 *
 * @see subscriber.h, node.h, serializer.h, extension/security.h
 */

#pragma once

#include <memory>
#include <string>
#include <type_traits>

#include "./impl/publisher_impl.h"
#include "./node.h"

namespace vlink {

/**
 * @class Publisher
 * @brief Type-safe topic emitter for the VLink event communication model.
 *
 * @details
 * Inherits the full @c Node API (lifecycle, loans, properties, profiler) and
 * adds publish-specific operations: subscriber detection, blocking wait, and
 * the @c publish() entry point itself.  The transport implementation
 * (@c PublisherImpl) is selected by the URL scheme or by the typed
 * configuration object supplied at construction time.
 *
 * @tparam MsgT  C++ message type. Must satisfy @c Serializer::is_supported().
 * @tparam SecT  Security mode; defaults to @c SecurityType::kWithoutSecurity.
 */
template <typename MsgT, SecurityType SecT = SecurityType::kWithoutSecurity>
class Publisher : public Node<PublisherImpl, SecT> {
 public:
  using UniquePtr = std::unique_ptr<Publisher<MsgT, SecT>>;  ///< Owning unique-pointer alias.
  using SharedPtr = std::shared_ptr<Publisher<MsgT, SecT>>;  ///< Owning shared-pointer alias.
  using ConnectCallback = NodeImpl::ConnectCallback;         ///< Callback type for subscriber presence transitions.

  static constexpr ImplType kImplType = kPublisher;                              ///< Node role tag (@c kPublisher).
  static constexpr Serializer::Type kMsgType = Serializer::get_type_of<MsgT>();  ///< Codec resolved from @c MsgT.

  static_assert(Serializer::is_supported(kMsgType), "<MsgT> is not a supported Serializer type.");

  /**
   * @brief Heap-allocates a @c Publisher and wraps it in a @c std::unique_ptr.
   *
   * @param url_str  Topic URL string, e.g. @c "dds://vehicle/speed".
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   * @return         Owning @c UniquePtr to the newly constructed publisher.
   */
  [[nodiscard]] static UniquePtr create_unique(const std::string& url_str, InitType type = InitType::kWithInit);

  /**
   * @brief Heap-allocates a @c Publisher and wraps it in a @c std::shared_ptr.
   *
   * @param url_str  Topic URL string.
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   * @return         Owning @c SharedPtr to the newly constructed publisher.
   */
  [[nodiscard]] static SharedPtr create_shared(const std::string& url_str, InitType type = InitType::kWithInit);

  /**
   * @brief Constructs a publisher from a typed transport configuration object.
   *
   * @details
   * Accepts any @c Conf-derived configuration (@c DdsConf, @c ShmConf,
   * @c ZenohConf, etc.).  A compile-time check enforces that the chosen
   * configuration permits the publisher role.  Invalid configuration or a
   * @c nullptr factory result triggers a fatal log.
   *
   * @tparam ConfT  Concrete configuration type derived from @c Conf.
   * @param conf    Populated configuration aggregate.
   * @param type    Whether to call @c init() inline; default is @c InitType::kWithInit.
   */
  // NOLINTNEXTLINE(modernize-use-constraints)
  template <typename ConfT, typename = std::enable_if_t<std::is_base_of_v<Conf, ConfT>>>
  explicit Publisher(const ConfT& conf, InitType type = InitType::kWithInit);

  /**
   * @brief Constructs a publisher from a URL string.
   *
   * @details
   * The URL scheme selects the transport (e.g. @c "shm://" maps to the
   * Iceoryx back-end, @c "dds://" maps to FastDDS).  Internally the string is
   * parsed into a @c Url and dispatched to the matching @c ConfT.
   *
   * @param url_str  Topic URL such as @c "shm://vehicle/speed".
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   */
  explicit Publisher(const std::string& url_str, InitType type = InitType::kWithInit);

  /**
   * @brief Registers a callback invoked whenever subscriber presence transitions.
   *
   * @details
   * The callback fires synchronously immediately if a subscriber is already
   * present at registration time, then asynchronously each time the matching
   * subscriber set transitions between empty and non-empty.  The boolean is
   * @c true when at least one peer is present.
   *
   * @param callback  @c void(bool) callable -- @c true means at least one subscriber.
   */
  void detect_subscribers(ConnectCallback&& callback);

  /**
   * @brief Blocks until at least one subscriber is detected or @p timeout expires.
   *
   * @details
   * A @p timeout of @c 0 is interpreted as infinite (with a warning log).  A
   * negative value also waits indefinitely.  Calling @c interrupt() causes the
   * wait to abort and return @c false.
   *
   * @param timeout  Maximum wait duration.  Default: @c Timeout::kDefaultInterval.
   * @return         @c true if a subscriber appeared; @c false on timeout or interrupt.
   */
  bool wait_for_subscribers(std::chrono::milliseconds timeout = Timeout::kDefaultInterval);

  /**
   * @brief Non-blocking query of subscriber presence.
   *
   * @return @c true when the transport currently knows of at least one matching subscriber.
   */
  [[nodiscard]] bool has_subscribers() const;

  /**
   * @brief Serialises and emits @p msg to every connected subscriber.
   *
   * @details
   * Serialisation is dispatched to the codec selected by @c kMsgType.  On
   * loan-capable transports the output buffer is a loaned segment in
   * transport-managed memory; the loan is returned automatically once the
   * publish completes.  Loans are skipped when @c SecT == @c kWithSecurity
   * because the ciphertext size is unknown ahead of encryption.
   *
   * When @c force is @c false (the default) the call is skipped and returns
   * @c false if no subscribers are present.  Pass @c force=true to write
   * unconditionally, e.g. for bag recording or field-style semantics.
   *
   * Over @c intra:// with a message whose @c element_type derives from
   * @c IntraDataType (declared via @c VLINK_INTRA_DATA_DECLARE) the shared
   * pointer is forwarded zero-copy and no serialisation occurs.
   *
   * @param msg    Message to publish.
   * @param force  @c true to publish even when no subscribers are connected.
   * @return       @c true if the transport accepted the message.
   */
  bool publish(const MsgT& msg, bool force = false);

  /**
   * @brief Publishes the raw payload of a pre-finished @c flatbuffers::FlatBufferBuilder.
   *
   * @details
   * Useful in pipelines where the FlatBuffer is constructed externally and
   * the bytes can be shallow-borrowed without re-serialisation.  The pointer
   * must reference a builder on which @c Finish() has been called.
   *
   * @param fbb    Pointer to a finished @c flatbuffers::FlatBufferBuilder.
   * @param force  @c true to publish even when no subscribers are connected.
   * @return       @c true on success.
   */
  bool publish_fbb(const void* fbb, bool force = false);

  /**
   * @brief Promotes this publisher to behave as a @c Setter (field-writer) at the transport layer.
   *
   * @details
   * Switches the underlying @c impl_type from @c kPublisher to @c kSetter
   * so that field-mode semantics (late-joiner sync, latest-value retention)
   * are activated.  When called post-@c init(), the transport extension is
   * reinitialised automatically.  Used internally by @c Setter.
   */
  void mark_as_setter();

 private:
  bool write_bytes(const Bytes& data);

  bool write_intra(const IntraData& intra_data);
};

/**
 * @class SecurityPublisher
 * @brief Convenience alias of @c Publisher with per-message security enabled.
 *
 * @details
 * Equivalent to @c Publisher\<MsgT, SecurityType::kWithSecurity\>.  Every
 * outgoing payload is encrypted with the configured @c Security::Config
 * before transmission; an empty config falls back to the built-in default
 * symmetric slot.
 *
 * @note Security is not supported on @c intra:// or on @c dds:// CDR
 *       payloads -- using a @c SecurityPublisher in those configurations is
 *       a constructor-time fatal.
 *
 * @tparam MsgT  C++ message type to publish.
 */
template <typename MsgT>
class SecurityPublisher : public Publisher<MsgT, SecurityType::kWithSecurity> {
 public:
  using UniquePtr = std::unique_ptr<SecurityPublisher<MsgT>>;  ///< Owning unique-pointer alias.
  using SharedPtr = std::shared_ptr<SecurityPublisher<MsgT>>;  ///< Owning shared-pointer alias.

  /**
   * @brief Heap-allocates a @c SecurityPublisher and wraps it in a @c std::unique_ptr.
   *
   * @tparam SecurityConfigT  Forwardable @c Security::Config compatible type.
   * @param url_str  Topic URL string.
   * @param sec_cfg  Security configuration; empty uses the default symmetric slot.
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   * @return         Owning @c UniquePtr to the new secure publisher.
   */
  // NOLINTNEXTLINE(modernize-use-constraints)
  template <typename SecurityConfigT = Security::Config>
  [[nodiscard]] static UniquePtr create_unique(const std::string& url_str, SecurityConfigT&& sec_cfg = {},
                                               InitType type = InitType::kWithInit);

  /**
   * @brief Heap-allocates a @c SecurityPublisher and wraps it in a @c std::shared_ptr.
   *
   * @tparam SecurityConfigT  Forwardable @c Security::Config compatible type.
   * @param url_str  Topic URL string.
   * @param sec_cfg  Security configuration; empty uses the default symmetric slot.
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   * @return         Owning @c SharedPtr to the new secure publisher.
   */
  // NOLINTNEXTLINE(modernize-use-constraints)
  template <typename SecurityConfigT = Security::Config>
  [[nodiscard]] static SharedPtr create_shared(const std::string& url_str, SecurityConfigT&& sec_cfg = {},
                                               InitType type = InitType::kWithInit);

  /**
   * @brief Constructs a @c SecurityPublisher from a typed configuration object.
   *
   * @details
   * Constructs the base @c Publisher in @c kWithoutInit mode, installs
   * @p sec_cfg via @c enable_security(), then runs @c init() unless deferred.
   * If @c enable_security() fails to populate a usable security object the
   * subsequent @c init() will fail.
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
  explicit SecurityPublisher(const ConfT& conf, SecurityConfigT&& sec_cfg = {}, InitType type = InitType::kWithInit);

  /**
   * @brief Constructs a @c SecurityPublisher from a URL string.
   *
   * @tparam SecurityConfigT  Forwardable @c Security::Config compatible type.
   * @param url_str  Topic URL string.
   * @param sec_cfg  Security configuration; empty uses the default symmetric slot.
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   */
  // NOLINTNEXTLINE(modernize-use-constraints)
  template <typename SecurityConfigT = Security::Config>
  explicit SecurityPublisher(const std::string& url_str, SecurityConfigT&& sec_cfg = {},
                             InitType type = InitType::kWithInit);
};

}  // namespace vlink

#include "./internal/publisher-inl.h"
