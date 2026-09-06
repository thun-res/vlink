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
 * @file getter.h
 * @brief Read-side primitive of the VLink field communication model.
 *
 * @details
 * @c Getter\<ValueT, SecT\> mirrors the latest value produced by a matching
 * @c Setter on the same URL.  Unlike @c Subscriber it retains only the most
 * recent payload rather than queuing every update, and it offers a blocking
 * @c wait_for_value() entry point in addition to the standard event callback.
 *
 * The class is a thin, header-only template wrapper around @c GetterImpl and
 * owns its own cached @c value_, change-tracking buffer, and condition
 * variable.
 *
 * @par Field-model Data Flow
 * @verbatim
 *   Setter<ValueT>             Transport Back-end             Getter<ValueT>
 *   --------------             ------------------             ---------------
 *      | set(v)                       |                              |
 *      |----------------------------->|  serialised latest value     |
 *      |                              |--- overwrites previous ----->|
 *      |                              |                              |
 *      |                              |                              |  Serializer::
 *      |                              |                              |  deserialize
 *      |                              |                              |--> listen cb (optional)
 *      |                              |                              |--> value_ cached
 *      |                              |                              |--> wait_for_value
 *      |                              |                              |    returns true
 *      |                              |                              |--> get() => value
 * @endverbatim
 *
 * @par Getter vs Subscriber
 * | Feature                  | @c Getter\<T\>                          | @c Subscriber\<T\>             |
 * | ------------------------ | --------------------------------------- | ------------------------------ |
 * | Value retention          | Latest value cached                     | None                           |
 * | Transport role tag       | @c kGetter                              | @c kSubscriber                 |
 * | Blocking read            | @c wait_for_value()                     | N/A                            |
 * | Duplicate suppression    | @c set_change_reporting(true)           | N/A                            |
 * | Cross-role bridge        | @c mark_as_subscriber()                 | @c mark_as_getter()            |
 *
 * @par Read-Mode Variants
 * | Method                                | Blocking | Result type             | Notes                        |
 * | ------------------------------------- | -------- | ----------------------- | ---------------------------- |
 * | @c get()                              | No       | @c std::optional        | Returns @c nullopt initially |
 * | @c wait_for_value(timeout)            | Yes      | @c bool (then @c get()) | Default infinite if @c 0     |
 * | @c listen(callback)                   | No       | @c void(const ValueT&)  | Fires on every update        |
 * | @c listen(cb) + @c change_reporting   | No       | @c void(const ValueT&)  | Suppress identical bytes     |
 *
 * @par Usage Patterns
 * @code
 * vlink::Getter<int> g("shm://vehicle/gear");
 *
 * if (auto v = g.get()) { use(*v); }
 *
 * if (g.wait_for_value(std::chrono::milliseconds(500))) {
 *   use(*g.get());
 * }
 *
 * g.listen([](const int& v) { on_update(v); });
 *
 * g.set_change_reporting(true);
 * g.listen([](const int& v) { on_changed(v); });
 * @endcode
 *
 * @note Until a @c Setter writes for the first time, @c get() returns
 *       @c std::nullopt and @c wait_for_value() blocks.
 *
 * @see setter.h, subscriber.h, node.h, serializer.h
 */

#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>

#include "./base/condition_variable.h"
#include "./base/functional.h"
#include "./impl/getter_impl.h"
#include "./node.h"

namespace vlink {

/**
 * @class Getter
 * @brief Type-safe latest-value reader for the VLink field communication model.
 *
 * @details
 * Inherits the full @c Node API and adds field-specific operations: cached
 * @c get(), blocking @c wait_for_value(), single-shot @c listen() callback,
 * change-reporting filter, and latency / sample-loss tracking.
 *
 * @tparam ValueT  C++ value type. Must satisfy @c Serializer::is_supported().
 * @tparam SecT    Security mode; defaults to @c SecurityType::kWithoutSecurity.
 */
template <typename ValueT, SecurityType SecT = SecurityType::kWithoutSecurity>
class Getter : public Node<GetterImpl, SecT> {
 public:
  using UniquePtr = std::unique_ptr<Getter<ValueT, SecT>>;  ///< Owning unique-pointer alias.
  using SharedPtr = std::shared_ptr<Getter<ValueT, SecT>>;  ///< Owning shared-pointer alias.
  using MsgCallback = Function<void(const ValueT&)>;        ///< User callback signature for value updates.

  static constexpr ImplType kImplType = kGetter;                                     ///< Node role tag (@c kGetter).
  static constexpr Serializer::Type kValueType = Serializer::get_type_of<ValueT>();  ///< Codec resolved from @c ValueT.

  static_assert(Serializer::is_supported(kValueType), "<ValueT> is not a supported Serializer type.");

  /**
   * @brief Heap-allocates a @c Getter and wraps it in a @c std::unique_ptr.
   *
   * @param url_str  Field URL such as @c "shm://vehicle/gear".
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   * @return         Owning @c UniquePtr to the new getter.
   */
  [[nodiscard]] static UniquePtr create_unique(const std::string& url_str, InitType type = InitType::kWithInit);

  /**
   * @brief Heap-allocates a @c Getter and wraps it in a @c std::shared_ptr.
   *
   * @param url_str  Field URL string.
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   * @return         Owning @c SharedPtr to the new getter.
   */
  [[nodiscard]] static SharedPtr create_shared(const std::string& url_str, InitType type = InitType::kWithInit);

  /**
   * @brief Constructs a getter from a typed transport configuration object.
   *
   * @tparam ConfT  Concrete configuration type derived from @c Conf.
   * @param conf    Populated configuration aggregate.
   * @param type    Whether to call @c init() inline; default is @c InitType::kWithInit.
   */
  // NOLINTNEXTLINE(modernize-use-constraints)
  template <typename ConfT, typename = std::enable_if_t<std::is_base_of_v<Conf, ConfT>>>
  explicit Getter(const ConfT& conf, InitType type = InitType::kWithInit);

  /**
   * @brief Constructs a getter from a URL string.
   *
   * @param url_str  Field URL such as @c "shm://vehicle/gear".
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   */
  explicit Getter(const std::string& url_str, InitType type = InitType::kWithInit);

  /**
   * @brief Destroys the getter and drains in-flight transport callbacks.
   *
   * @details
   * Required so the internal delivery callback installed by @c init() cannot
   * fire on the transport thread after this getter's mutex and value have
   * already been destroyed.
   */
  ~Getter() override;

  /**
   * @brief Returns the most recent cached value, if any has been received.
   *
   * @details
   * Thread-safe.  Returns @c std::nullopt before the first matching @c Setter
   * write reaches this getter.
   *
   * @return @c std::optional\<ValueT\> -- the latest value, or empty.
   */
  [[nodiscard]] std::optional<ValueT> get() const;

  /**
   * @brief Blocks until a value is received or @p timeout expires.
   *
   * @details
   * Returns immediately if a value is already cached.  A @p timeout of @c 0
   * is treated as infinite (with a warning).  @c interrupt() causes the wait
   * to abort and return @c false.  Use @c get() afterwards to retrieve the
   * value when this method returns @c true.
   *
   * @param timeout  Maximum wait duration.  Default: @c Timeout::kDefaultInterval.
   * @return         @c true if a value was received; @c false on timeout or interrupt.
   */
  bool wait_for_value(std::chrono::milliseconds timeout = Timeout::kDefaultInterval);

  /**
   * @brief Installs a callback invoked whenever a new value arrives.
   *
   * @details
   * The callback receives a deserialised @c ValueT reference.  It runs for
   * every setter write unless @c set_change_reporting(true) is active, in
   * which case duplicate consecutive payloads are filtered out.  Internally
   * the getter first fires the callback, then updates @c value_ and wakes
   * @c wait_for_value().
   *
   * @note Calling @c listen() more than once is fatal.
   *
   * @param callback  @c void(const ValueT&) invoked on each new value.
   * @return          @c true on successful installation; a second call is
   *                  fatal and never returns.
   */
  bool listen(MsgCallback&& callback);

  /**
   * @brief Toggles change-only reporting (duplicate suppression).
   *
   * @details
   * When enabled, incoming payloads whose raw serialised bytes match the
   * previous delivery are dropped before any caching or callback dispatch.
   * Useful when a @c Setter repeatedly writes the same value.  Toggling the
   * flag discards the previous comparison basis, so the first payload after
   * re-enabling is always delivered.
   *
   * @param enable  @c true to suppress duplicates; @c false (default) to deliver all.
   */
  void set_change_reporting(bool enable);

  /**
   * @brief Toggles per-value latency and sample-loss measurement.
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
   * @return Latency in nanoseconds; @c 0 if tracking is disabled.
   */
  [[nodiscard]] int64_t get_latency() const;

  /**
   * @brief Returns cumulative sample-delivery statistics.
   *
   * @return @c SampleLostInfo with total expected and total lost counts.
   */
  [[nodiscard]] SampleLostInfo get_lost() const;

  /**
   * @brief Reports whether change-only reporting is currently active.
   *
   * @return @c true if duplicate suppression is enabled.
   */
  [[nodiscard]] bool get_change_reporting() const;

  /**
   * @brief Initialises the getter and installs the internal delivery hook.
   *
   * @details
   * Overrides @c Node::init() to additionally register a bytes-level callback
   * that deserialises each delivery, invokes the user @c listen() callback
   * (if installed), then updates @c value_ and wakes the condition variable
   * used by @c wait_for_value().
   *
   * @return @c true on first successful initialisation; @c false otherwise.
   */
  bool init() override;

  /**
   * @brief Aborts any blocking @c wait_for_value() call.
   *
   * @details
   * Overrides @c Node::interrupt() to additionally notify the local
   * condition variable so that the blocking wait returns @c false promptly.
   */
  void interrupt() override;

  /**
   * @brief Reports this getter as a @c Subscriber in discovery metadata.
   *
   * @details
   * Updates the role label and refreshes discovery when already initialised.
   * The getter's value cache and existing transport endpoint remain active.
   */
  void mark_as_subscriber();

 private:
  void listen_bytes(NodeImpl::MsgCallback&& callback);

  std::optional<ValueT> value_;
  mutable std::mutex mtx_;
  ConditionVariable cv_;
  MsgCallback callback_;
  std::optional<Bytes> last_cache_;
  bool has_value_notification_{false};
  bool change_reporting_{false};
};

/**
 * @class SecurityGetter
 * @brief Convenience alias of @c Getter with per-message decryption enabled.
 *
 * @details
 * Equivalent to @c Getter\<ValueT, SecurityType::kWithSecurity\>.  Each
 * incoming payload is decrypted before codec dispatch and caching.
 *
 * @tparam ValueT  Value type to read.
 */
template <typename ValueT>
class SecurityGetter : public Getter<ValueT, SecurityType::kWithSecurity> {
 public:
  using UniquePtr = std::unique_ptr<SecurityGetter<ValueT>>;  ///< Owning unique-pointer alias.
  using SharedPtr = std::shared_ptr<SecurityGetter<ValueT>>;  ///< Owning shared-pointer alias.

  /**
   * @brief Heap-allocates a @c SecurityGetter and wraps it in a @c std::unique_ptr.
   *
   * @tparam SecurityConfigT  Forwardable @c Security::Config compatible type.
   * @param url_str  Field URL string.
   * @param sec_cfg  Security configuration; empty uses the default symmetric slot.
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   * @return         Owning @c UniquePtr to the new secure getter.
   */
  // NOLINTNEXTLINE(modernize-use-constraints)
  template <typename SecurityConfigT = Security::Config>
  [[nodiscard]] static UniquePtr create_unique(const std::string& url_str, SecurityConfigT&& sec_cfg = {},
                                               InitType type = InitType::kWithInit);

  /**
   * @brief Heap-allocates a @c SecurityGetter and wraps it in a @c std::shared_ptr.
   *
   * @tparam SecurityConfigT  Forwardable @c Security::Config compatible type.
   * @param url_str  Field URL string.
   * @param sec_cfg  Security configuration; empty uses the default symmetric slot.
   * @param type     Whether to call @c init() inline; default is @c InitType::kWithInit.
   * @return         Owning @c SharedPtr to the new secure getter.
   */
  // NOLINTNEXTLINE(modernize-use-constraints)
  template <typename SecurityConfigT = Security::Config>
  [[nodiscard]] static SharedPtr create_shared(const std::string& url_str, SecurityConfigT&& sec_cfg = {},
                                               InitType type = InitType::kWithInit);

  /**
   * @brief Constructs a @c SecurityGetter from a typed configuration object.
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
  explicit SecurityGetter(const ConfT& conf, SecurityConfigT&& sec_cfg = {}, InitType type = InitType::kWithInit);

  /**
   * @brief Constructs a @c SecurityGetter from a URL string and installs the security configuration.
   *
   * @details
   * Builds the base @c Getter in @c kWithoutInit mode, installs @p sec_cfg
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
  explicit SecurityGetter(const std::string& url_str, SecurityConfigT&& sec_cfg = {},
                          InitType type = InitType::kWithInit);
};

}  // namespace vlink

#include "./internal/getter-inl.h"
