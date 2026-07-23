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
 * @file subscriber.h
 * @brief Read-side primitive of the VLink event communication model.
 *
 * @details
 * @c Subscriber\<MsgT, SecT\> attaches a callback to a VLink topic.  Each
 * frame delivered by the transport back-end is deserialised into a @c MsgT
 * instance and forwarded to the user callback registered via @c listen().
 * Unlike @c Getter, the subscriber retains no value history -- it simply
 * forwards every event in delivery order.
 *
 * The class is a thin, header-only template wrapper around @c SubscriberImpl.
 * Codec dispatch is fully resolved at compile time using the type detected
 * by @c Serializer::get_type_of\<MsgT\>().
 *
 * @par Event-model Delivery Path
 * @verbatim
 *   Transport Back-end                    Subscriber<MsgT>
 *   ------------------                    -----------------
 *      | inbound frame                          |
 *      |--------------------------------------> |
 *      |                                        |  Serializer::deserialize
 *      |                                        |  (per-delivery local value)
 *      |                                        |
 *      |                                        |  optional MessageLoop hop
 *      |                                        |
 *      |                                        v
 *      |                                  user callback(const MsgT&)
 * @endverbatim
 *
 * @par Supported Message Types
 * | Category          | Example C++ Type                 | @c Serializer::Type | Notes                          |
 * | ----------------- | -------------------------------- | ------------------- | ------------------------------ |
 * | Raw bytes         | @c Bytes                         | @c kBytesType       | Pass-through to callback.      |
 * | Protobuf value    | @c MyProto                       | @c kProtoType       | ParseFromArray path.           |
 * | Protobuf pointer  | @c MyProto*                      | @c kProtoPtrType    | Needs @c bind_proto_arena.     |
 * | FlatBuffers obj   | @c MyTableT (NativeTable)        | @c kFlatTableType   | Object API.                    |
 * | FlatBuffers ptr   | @c MyTable*                      | @c kFlatPtrType     | Zero-copy view of buffer.      |
 * | DDS CDR           | FastDDS IDL or ROS2 message type | @c kCdrType         | Encapsulated CDR bytes.        |
 * | POD struct        | trivial standard-layout type     | @c kStandardType    | @c sizeof(T) byte copy.        |
 * | String            | @c std::string                   | @c kStringType      | Payload-sized byte string.     |
 * | Custom            | type with @c operator>>/<<       | @c kCustomType      | User-supplied codec.           |
 *
 * Raw Protobuf pointer receivers allocate a distinct message in the bound
 * Arena for each delivery; that storage remains until the Arena is reset or
 * destroyed.
 *
 * @par Basic Listen Example
 * @code
 * vlink::Subscriber<MyMsg> sub("dds://vehicle/speed");
 * sub.listen([](const MyMsg& msg) { handle(msg); });
 * @endcode
 *
 * @par Callback Dispatch via MessageLoop
 * @code
 * vlink::MessageLoop loop;
 * vlink::Subscriber<MyMsg> sub("dds://vehicle/speed", vlink::InitType::kWithoutInit);
 * sub.attach(&loop);
 * sub.init();
 * sub.listen([](const MyMsg& m) { handle(m); });
 * loop.run();
 * @endcode
 *
 * @par Zero-copy Intra Transport
 * When @c MsgT is a shared pointer whose @c element_type derives from
 * @c IntraDataType (generated via @c VLINK_INTRA_DATA_DECLARE) and the URL
 * scheme is @c intra://, the shared pointer is forwarded zero-copy to the
 * callback without serialisation:
 * @code
 * VLINK_INTRA_DATA_DECLARE(MyProtoMsg, MyIntra)
 * vlink::Subscriber<MyIntra> sub("intra://my_topic");
 * sub.listen([](const MyIntra& data) { use(data); });
 * @endcode
 *
 * @par Latency and Sample-loss Tracking
 * @code
 * vlink::Subscriber<MyMsg> sub("dds://my_topic");
 * sub.set_latency_and_lost_enabled(true);
 * auto latency_ns = sub.get_latency();
 * auto stats = sub.get_lost();
 * @endcode
 *
 * @warning For value message types, the deserialised callback argument is a
 *          per-delivery local object whose reference is valid only for the
 *          callback duration.  Pointer-view types may instead refer to an
 *          external buffer or arena and follow that storage's lifetime.
 *          Copy retained values into explicitly owned storage.  Copying
 *          @c Bytes creates owned storage, and copying an @c IntraData shared
 *          pointer extends that object's lifetime.
 *
 * @note Calling @c listen() more than once is a fatal error.  The subscriber
 *       must be initialised (either by @c InitType::kWithInit or by explicit
 *       @c init()) before @c listen() is called.
 *
 * @see publisher.h, node.h, serializer.h, base/message_loop.h
 */

#pragma once

#include <memory>
#include <string>
#include <type_traits>

#include "./base/functional.h"
#include "./impl/subscriber_impl.h"
#include "./node.h"

namespace vlink {

/**
 * @class Subscriber
 * @brief Type-safe topic listener for the VLink event communication model.
 *
 * @details
 * Inherits the full @c Node API and adds receive-specific operations:
 * @c listen() to register the user callback and latency / sample-loss
 * tracking.  The transport
 * implementation (@c SubscriberImpl) is selected by the URL scheme or by
 * the typed configuration object supplied at construction time.
 *
 * @tparam MsgT  C++ message type. Must satisfy @c Serializer::is_supported().
 * @tparam SecT  Security mode; defaults to @c SecurityType::kWithoutSecurity.
 */
template <typename MsgT, SecurityType SecT = SecurityType::kWithoutSecurity>
class Subscriber : public Node<SubscriberImpl, SecT> {
 public:
  using UniquePtr = std::unique_ptr<Subscriber<MsgT, SecT>>;  ///< Owning unique-pointer alias.
  using SharedPtr = std::shared_ptr<Subscriber<MsgT, SecT>>;  ///< Owning shared-pointer alias.
  using MsgCallback = Function<void(const MsgT&)>;            ///< User callback signature for received messages.

  static constexpr ImplType kImplType = kSubscriber;                             ///< Node role tag (@c kSubscriber).
  static constexpr Serializer::Type kMsgType = Serializer::get_type_of<MsgT>();  ///< Codec resolved from @c MsgT.

  static_assert(Serializer::is_supported(kMsgType), "<MsgT> is not a supported Serializer type.");

  /**
   * @brief Heap-allocates a @c Subscriber and wraps it in a @c std::unique_ptr.
   *
   * @param url_str  Topic URL string.
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   * @return         Owning @c UniquePtr to the new subscriber.
   */
  [[nodiscard]] static UniquePtr create_unique(const std::string& url_str, InitType type = InitType::kWithInit);

  /**
   * @brief Heap-allocates a @c Subscriber and wraps it in a @c std::shared_ptr.
   *
   * @param url_str  Topic URL string.
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   * @return         Owning @c SharedPtr to the new subscriber.
   */
  [[nodiscard]] static SharedPtr create_shared(const std::string& url_str, InitType type = InitType::kWithInit);

  /**
   * @brief Constructs a subscriber from a typed transport configuration object.
   *
   * @details
   * Accepts any @c Conf-derived configuration.  A compile-time check enforces
   * that the configuration permits the subscriber role.
   *
   * @tparam ConfT  Concrete configuration type derived from @c Conf.
   * @param conf    Populated configuration aggregate.
   * @param type    Whether to call @c init() inline; default is @c InitType::kWithInit.
   */
  // NOLINTNEXTLINE(modernize-use-constraints)
  template <typename ConfT, typename = std::enable_if_t<std::is_base_of_v<Conf, ConfT>>>
  explicit Subscriber(const ConfT& conf, InitType type = InitType::kWithInit);

  /**
   * @brief Constructs a subscriber from a URL string.
   *
   * @param url_str  Topic URL such as @c "shm://vehicle/speed".
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   */
  explicit Subscriber(const std::string& url_str, InitType type = InitType::kWithInit);

  /**
   * @brief Installs the receive callback that runs for every inbound message.
   *
   * @details
   * The callback is invoked on the transport delivery thread by default; if
   * the subscriber is @c attach()ed to a @c MessageLoop the callback is
   * posted onto that loop instead.  For @c intra:// transports carrying an
   * @c IntraDataType shared pointer the value is forwarded zero-copy and
   * no deserialisation occurs.
   *
   * @warning The argument reference is valid only for the duration of the
   *          callback.  Value messages use per-delivery local storage, while
   *          pointer-view messages and @c Bytes may refer to external storage.
   *          Copy retained values into explicitly owned storage; copying
   *          @c Bytes creates owned storage, and copying an @c IntraData shared
   *          pointer extends that object's lifetime.
   *
   * @note Calling @c listen() more than once is fatal.  The subscriber must
   *       be initialised before the first call to @c listen().
   *
   * @param callback  @c void(const MsgT&) invoked for each received message.
   * @return          @c true if the callback was installed successfully.
   */
  bool listen(MsgCallback&& callback);

  /**
   * @brief Toggles per-message latency and sample-loss measurement.
   *
   * @param enable  @c true to begin tracking; @c false to stop.
   */
  void set_latency_and_lost_enabled(bool enable);

  /**
   * @brief Reports whether latency and sample-loss tracking is currently active.
   *
   * @return @c true if @c set_latency_and_lost_enabled(true) was invoked.
   */
  [[nodiscard]] bool is_latency_and_lost_enabled() const;

  /**
   * @brief Returns the most recent end-to-end latency measurement.
   *
   * @details
   * Computed as receive-timestamp minus source-timestamp on the last
   * delivered message.  Returns @c 0 when tracking is not enabled.
   *
   * @return Latency in nanoseconds; @c 0 if disabled.
   */
  [[nodiscard]] int64_t get_latency() const;

  /**
   * @brief Returns cumulative sample-delivery statistics.
   *
   * @return @c SampleLostInfo with total expected and total lost counts.
   */
  [[nodiscard]] SampleLostInfo get_lost() const;

  /**
   * @brief Promotes this subscriber to behave as a @c Getter (field-reader) at the transport layer.
   *
   * @details
   * Switches @c impl_type from @c kSubscriber to @c kGetter so that
   * latest-value delivery semantics are activated.  Reinitialises the
   * transport extension if called post-@c init().  Used internally by
   * @c Getter.
   */
  void mark_as_getter();

 private:
  bool listen_bytes(NodeImpl::MsgCallback&& callback);

  bool listen_intra(MsgCallback&& callback);
};

/**
 * @class SecuritySubscriber
 * @brief Convenience alias of @c Subscriber with per-message decryption enabled.
 *
 * @details
 * Equivalent to @c Subscriber\<MsgT, SecurityType::kWithSecurity\>.  Every
 * inbound payload is decrypted with the configured @c Security::Config
 * before the codec dispatcher is invoked.
 *
 * @note Security is not supported on @c intra:// or on @c dds:// CDR
 *       payloads.
 *
 * @tparam MsgT  C++ message type to subscribe.
 */
template <typename MsgT>
class SecuritySubscriber : public Subscriber<MsgT, SecurityType::kWithSecurity> {
 public:
  using UniquePtr = std::unique_ptr<SecuritySubscriber<MsgT>>;  ///< Owning unique-pointer alias.
  using SharedPtr = std::shared_ptr<SecuritySubscriber<MsgT>>;  ///< Owning shared-pointer alias.

  /**
   * @brief Heap-allocates a @c SecuritySubscriber and wraps it in a @c std::unique_ptr.
   *
   * @tparam SecurityConfigT  Forwardable @c Security::Config compatible type.
   * @param url_str  Topic URL string.
   * @param sec_cfg  Security configuration; empty uses the default symmetric slot.
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   * @return         Owning @c UniquePtr to the new secure subscriber.
   */
  // NOLINTNEXTLINE(modernize-use-constraints)
  template <typename SecurityConfigT = Security::Config>
  [[nodiscard]] static UniquePtr create_unique(const std::string& url_str, SecurityConfigT&& sec_cfg = {},
                                               InitType type = InitType::kWithInit);

  /**
   * @brief Heap-allocates a @c SecuritySubscriber and wraps it in a @c std::shared_ptr.
   *
   * @tparam SecurityConfigT  Forwardable @c Security::Config compatible type.
   * @param url_str  Topic URL string.
   * @param sec_cfg  Security configuration; empty uses the default symmetric slot.
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   * @return         Owning @c SharedPtr to the new secure subscriber.
   */
  // NOLINTNEXTLINE(modernize-use-constraints)
  template <typename SecurityConfigT = Security::Config>
  [[nodiscard]] static SharedPtr create_shared(const std::string& url_str, SecurityConfigT&& sec_cfg = {},
                                               InitType type = InitType::kWithInit);

  /**
   * @brief Constructs a @c SecuritySubscriber from a typed configuration object.
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
  explicit SecuritySubscriber(const ConfT& conf, SecurityConfigT&& sec_cfg = {}, InitType type = InitType::kWithInit);

  /**
   * @brief Constructs a @c SecuritySubscriber from a URL string and installs the security configuration.
   *
   * @details
   * Builds the base @c Subscriber in @c kWithoutInit mode, installs @p sec_cfg
   * via @c enable_security(), then calls @c init() unless deferred.  When
   * @c enable_security() fails to produce a usable @c NodeImpl::security the
   * subsequent @c init() will fail.
   *
   * @tparam SecurityConfigT  Forwardable @c Security::Config compatible type.
   * @param url_str  Topic URL string.
   * @param sec_cfg  Security configuration; empty uses the default symmetric slot.
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   */
  // NOLINTNEXTLINE(modernize-use-constraints)
  template <typename SecurityConfigT = Security::Config>
  explicit SecuritySubscriber(const std::string& url_str, SecurityConfigT&& sec_cfg = {},
                              InitType type = InitType::kWithInit);
};

}  // namespace vlink

#include "./internal/subscriber-inl.h"
