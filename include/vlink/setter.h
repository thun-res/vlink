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
 * @file setter.h
 * @brief Write-side primitive of the VLink field communication model.
 *
 * @details
 * @c Setter\<ValueT, SecT\> publishes the latest value for a field topic.
 * Every call to @c set() updates the locally cached value and emits the new
 * serialised payload over the transport.  When a late @c Getter joins after
 * the setter has already written, an internally registered @c sync()
 * callback resends the cached value so the late peer receives the current
 * state immediately.
 *
 * The class is a thin, header-only template wrapper around @c SetterImpl.
 *
 * @par Field-model Data Flow
 * @verbatim
 *   Setter<ValueT>             Transport Back-end             Getter<ValueT>
 *   --------------             ------------------             ---------------
 *      | set(v)                       |                              |
 *      |  cache value                 |                              |
 *      |  Serializer::serialize       |                              |
 *      |----------------------------->|--- latest-value frame ------>|
 *      |                              |  (overwrites any previous)   |
 *      |                              |                              |--> callback(v)
 *      |                              |                              |--> get() => v
 *      | <-- late joiner sync ------- | <-- sync() fired by impl --- |
 *      | re-emit cached value         |                              |
 *      |----------------------------->|----------------------------->|
 * @endverbatim
 *
 * @par Setter vs Publisher
 * | Feature                  | @c Setter\<T\>                          | @c Publisher\<T\>              |
 * | ------------------------ | --------------------------------------- | ------------------------------ |
 * | Transport role tag       | @c kSetter                              | @c kPublisher                  |
 * | Latest-value retention   | Re-sent to late-joining getters         | No re-send to late subscribers |
 * | Sync-on-connect callback | Yes (registered inside @c init())       | No                             |
 * | Cross-role bridge        | @c mark_as_publisher()                  | @c mark_as_setter()            |
 *
 * @par Durability and Persistence Modes
 * @c Setter inherits durability behaviour from the transport's effective
 * @c Qos profile.  Common configurations:
 *
 * | Durability mode      | Behaviour                                                  |
 * | -------------------- | ---------------------------------------------------------- |
 * | @c kVolatile         | Cached in-process only; re-sent on local @c sync() hook.   |
 * | @c kTransientLocal   | Cached inside the transport writer for late subscribers.   |
 * | @c kTransient        | Persisted in an external durability service (DDS only).    |
 * | @c kPersistent       | Persisted to stable storage (DDS only).                    |
 *
 * @par Usage Example
 * @code
 * vlink::Setter<MyStruct> setter("dds://vehicle/gear");
 * setter.set(MyStruct{3, "Drive"});
 *
 * // A Getter that connects afterwards still receives the cached value:
 * vlink::Getter<MyStruct> getter("dds://vehicle/gear");
 * if (auto v = getter.get()) { use(*v); }
 * @endcode
 *
 * @see getter.h, publisher.h, node.h, extension/qos.h
 */

#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>

#include "./impl/setter_impl.h"
#include "./node.h"

namespace vlink {

/**
 * @class Setter
 * @brief Type-safe latest-value writer for the VLink field communication model.
 *
 * @details
 * Inherits the full @c Node API and adds field-specific operations: cached
 * @c set(), automatic late-joiner sync via an internally registered
 * @c sync() callback, and bridging hooks to behave as a plain publisher
 * on transports that do not natively differentiate the two roles.
 *
 * @tparam ValueT  C++ value type. Must satisfy @c Serializer::is_supported().
 * @tparam SecT    Security mode; defaults to @c SecurityType::kWithoutSecurity.
 */
template <typename ValueT, SecurityType SecT = SecurityType::kWithoutSecurity>
class Setter : public Node<SetterImpl, SecT> {
 public:
  using UniquePtr = std::unique_ptr<Setter<ValueT, SecT>>;  ///< Owning unique-pointer alias.
  using SharedPtr = std::shared_ptr<Setter<ValueT, SecT>>;  ///< Owning shared-pointer alias.

  static constexpr ImplType kImplType = kSetter;                                     ///< Node role tag (@c kSetter).
  static constexpr Serializer::Type kValueType = Serializer::get_type_of<ValueT>();  ///< Codec resolved from @c ValueT.

  static_assert(Serializer::is_supported(kValueType), "<ValueT> is not a supported Serializer type.");

  /**
   * @brief Heap-allocates a @c Setter and wraps it in a @c std::unique_ptr.
   *
   * @param url_str  Field URL such as @c "shm://vehicle/gear".
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   * @return         Owning @c UniquePtr to the new setter.
   */
  [[nodiscard]] static UniquePtr create_unique(const std::string& url_str, InitType type = InitType::kWithInit);

  /**
   * @brief Heap-allocates a @c Setter and wraps it in a @c std::shared_ptr.
   *
   * @param url_str  Field URL string.
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   * @return         Owning @c SharedPtr to the new setter.
   */
  [[nodiscard]] static SharedPtr create_shared(const std::string& url_str, InitType type = InitType::kWithInit);

  /**
   * @brief Constructs a setter from a typed transport configuration object.
   *
   * @details
   * Accepts any @c Conf-derived configuration.  Following @c init() the
   * setter registers a transport-level @c sync() callback that re-emits the
   * cached value whenever a late @c Getter connects.
   *
   * @tparam ConfT  Concrete configuration type derived from @c Conf.
   * @param conf    Populated configuration aggregate.
   * @param type    Whether to call @c init() inline; default is @c InitType::kWithInit.
   */
  // NOLINTNEXTLINE(modernize-use-constraints)
  template <typename ConfT, typename = std::enable_if_t<std::is_base_of_v<Conf, ConfT>>>
  explicit Setter(const ConfT& conf, InitType type = InitType::kWithInit);

  /**
   * @brief Constructs a setter from a URL string.
   *
   * @param url_str  Field URL such as @c "dds://vehicle/gear".
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   */
  explicit Setter(const std::string& url_str, InitType type = InitType::kWithInit);

  /**
   * @brief Destroys the setter and drains in-flight transport callbacks.
   *
   * @details
   * Required so the @c sync() callback installed by @c init() cannot fire on
   * the transport thread after this setter's mutex and cached value have
   * already been destroyed.  Mirrors the lifecycle contract of @c Client and
   * @c Getter.
   */
  ~Setter() override;

  /**
   * @brief Initialises the setter and registers the late-getter @c sync() hook.
   *
   * @details
   * Calls the base @c Node initialisation, then installs a transport-level
   * callback that resends the cached value whenever a new @c Getter joins
   * after this setter has already been written.
   *
   * @return @c true on first successful initialisation; @c false otherwise.
   */
  bool init() override;

  /**
   * @brief Deinitialises the setter and releases the underlying transport resources.
   *
   * @details
   * Delegates to the base @c Node teardown path.  After deinitialisation the
   * setter no longer participates in @c sync() or writes until @c init() is
   * called again.
   *
   * @return @c true on first successful deinitialisation; @c false if not initialised.
   */
  bool deinit() override;

  /**
   * @brief Writes a new field value and emits it to every connected @c Getter.
   *
   * @details
   * Caches @p value under @c mtx_, then serialises and writes it via the
   * transport.  When security is enabled the serialised payload is encrypted
   * before transmission.  The cached value is automatically re-emitted when
   * a late @c Getter joins (see @c init()).
   *
   * @param value  New field value to publish.
   */
  void set(const ValueT& value);

  /**
   * @brief Promotes this setter to behave as a @c Publisher at the transport layer.
   *
   * @details
   * Switches @c impl_type from @c kSetter to @c kPublisher so that
   * event-mode semantics are applied (no latest-value retention).
   * Reinitialises the transport extension if called post-@c init().  Useful
   * on transports that do not natively distinguish the two roles.
   */
  void mark_as_publisher();

 private:
  void write(const ValueT& value);

  void write_bytes(const Bytes& data);

  std::optional<ValueT> value_;
  std::mutex mtx_;
};

/**
 * @class SecuritySetter
 * @brief Convenience alias of @c Setter with per-message encryption enabled.
 *
 * @details
 * Equivalent to @c Setter\<ValueT, SecurityType::kWithSecurity\>.  Each
 * outgoing payload is encrypted before transmission.
 *
 * @tparam ValueT  Value type to write.
 */
template <typename ValueT>
class SecuritySetter : public Setter<ValueT, SecurityType::kWithSecurity> {
 public:
  using UniquePtr = std::unique_ptr<SecuritySetter<ValueT>>;  ///< Owning unique-pointer alias.
  using SharedPtr = std::shared_ptr<SecuritySetter<ValueT>>;  ///< Owning shared-pointer alias.

  /**
   * @brief Heap-allocates a @c SecuritySetter and wraps it in a @c std::unique_ptr.
   *
   * @tparam SecurityConfigT  Forwardable @c Security::Config compatible type.
   * @param url_str  Field URL string.
   * @param sec_cfg  Security configuration; empty uses the default symmetric slot.
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   * @return         Owning @c UniquePtr to the new secure setter.
   */
  // NOLINTNEXTLINE(modernize-use-constraints)
  template <typename SecurityConfigT = Security::Config>
  [[nodiscard]] static UniquePtr create_unique(const std::string& url_str, SecurityConfigT&& sec_cfg = {},
                                               InitType type = InitType::kWithInit);

  /**
   * @brief Heap-allocates a @c SecuritySetter and wraps it in a @c std::shared_ptr.
   *
   * @tparam SecurityConfigT  Forwardable @c Security::Config compatible type.
   * @param url_str  Field URL string.
   * @param sec_cfg  Security configuration; empty uses the default symmetric slot.
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   * @return         Owning @c SharedPtr to the new secure setter.
   */
  // NOLINTNEXTLINE(modernize-use-constraints)
  template <typename SecurityConfigT = Security::Config>
  [[nodiscard]] static SharedPtr create_shared(const std::string& url_str, SecurityConfigT&& sec_cfg = {},
                                               InitType type = InitType::kWithInit);

  /**
   * @brief Constructs a @c SecuritySetter from a typed configuration object.
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
  explicit SecuritySetter(const ConfT& conf, SecurityConfigT&& sec_cfg = {}, InitType type = InitType::kWithInit);

  /**
   * @brief Constructs a @c SecuritySetter from a URL string and installs the security configuration.
   *
   * @details
   * Builds the base @c Setter in @c kWithoutInit mode, installs @p sec_cfg
   * via @c enable_security(), then calls @c init() unless deferred.  When
   * @c enable_security() fails to produce a usable @c NodeImpl::security the
   * subsequent @c init() will fail.
   *
   * @tparam SecurityConfigT  Forwardable @c Security::Config compatible type.
   * @param url_str  Field URL string.
   * @param sec_cfg  Security configuration; empty uses the default symmetric slot.
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   */
  // NOLINTNEXTLINE(modernize-use-constraints)
  template <typename SecurityConfigT = Security::Config>
  explicit SecuritySetter(const std::string& url_str, SecurityConfigT&& sec_cfg = {},
                          InitType type = InitType::kWithInit);
};

}  // namespace vlink

#include "./internal/setter-inl.h"
