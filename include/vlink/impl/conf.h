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
 * @file conf.h
 * @brief Transport-configuration base contract and the supporting boilerplate macros.
 *
 * @details
 * This is an internal implementation header used by the public node templates and
 * by every transport-specific @c *Conf class; user code should never include it
 * directly.  The @c Conf base struct is the bridge between the URL parsing layer
 * and the concrete factories that produce @c NodeImpl instances.  A typical node
 * construction follows the chain
 * @c Url -> concrete @c Conf -> @c Conf::create_xxx() -> @c NodeImpl subclass.
 *
 * @par Inheritance hierarchy
 * @code
 *                              +--------+
 *                              |  Conf  |
 *                              +---+----+
 *                                  |
 *      +---------+--------+--------+---------+----------+--------------+-----------+
 *      |         |        |        |         |          |              |           |
 *   IntraConf ShmConf  Shm2Conf ZenohConf  DdsConf  DdscConf        MqttConf   ...etc
 *                                                      (DdsrConf,   DdstConf,  SomeipConf,
 *                                                       FdbusConf,  QnxConf,   plugin Conf)
 * @endcode
 *
 * @par Virtual interface contract
 * | Method                       | Default behaviour                             | Subclass responsibility            |
 * | ---------------------------- | --------------------------------------------- | ---------------------------------- |
 * | @c parse(impl_type)          | Caches @p impl_type; rejects @c kUnknown.     | Validate transport-specific data.  |
 * | @c is_valid()                | Returns @c false.                             | Report readiness for factories.    |
 * | @c get_impl_type()           | Returns the cached value from @c parse().     | Usually inherited unchanged.       |
 * | @c get_transport_type()      | Returns @c TransportType::kUnknown.           | Return the backend identifier.     |
 * | @c parse_protocol(protocol)  | Returns @c false.                             | Pull URL fields into the conf.     |
 * | @c create_publisher() / etc. | Returns @c nullptr.                           | Allocate the matching @c NodeImpl. |
 *
 * @par Macro reference
 * | Macro                            | Purpose                                                              |
 * | -------------------------------- | -------------------------------------------------------------------- |
 * | @c VLINK_DECLARE_CONF_FRIEND     | Grants friend access to all six public Node<> templates.             |
 * | @c VLINK_CONF_IMPL(classname)    | Bundles friend grant + standard override declarations + ostream op.  |
 * | @c VLINK_ALLOW_IMPL_TYPE(type)   | Records which @c ImplType bits a conf may serve, for compile checks. |
 * | @c VLINK_DECLARE_GLOBAL_PROPERTY | Declares static thread-count and global-property storage in a conf.  |
 * | @c VLINK_DEFINE_GLOBAL_PROPERTY  | Provides the storage definitions for the declaration above.          |
 *
 * @par Example
 * @code
 * // include/myapp/my_conf.h
 * struct MyConf final : public vlink::Conf {
 *   VLINK_CONF_IMPL(MyConf)
 *   VLINK_ALLOW_IMPL_TYPE(vlink::kPublisher | vlink::kSubscriber)
 *   VLINK_DECLARE_GLOBAL_PROPERTY()
 *
 *   std::string host;
 *   uint16_t port{0};
 * };
 *
 * // src/myapp/my_conf.cc
 * VLINK_DEFINE_GLOBAL_PROPERTY(MyConf)
 *
 * void MyConf::global_init() { setup_shared_transport_state(); }
 * bool MyConf::is_valid() const { return !host.empty() && port != 0; }
 * @endcode
 */

#pragma once

#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <utility>

#include "../base/macros.h"
#include "./types.h"

namespace vlink {

/**
 * @struct Conf
 * @brief Abstract base for every transport-specific configuration aggregate.
 *
 * @details
 * Holds the cached @c ImplType selected by @c parse() and declares the protected
 * factory hooks that the public node templates use to instantiate @c NodeImpl
 * peers.  Default implementations of the factory hooks return @c nullptr so
 * subclasses only need to override the roles they actually support; combine
 * with @c VLINK_ALLOW_IMPL_TYPE to make the compile-time guard explicit.
 *
 * @note Instances are never owned by application code; they are produced by
 *       @c Url and live as long as the node that references them.
 */
struct VLINK_EXPORT Conf {
  /**
   * @brief Key/value property map shared between confs and node implementations.
   *
   * @details
   * Stores transport tuning entries (e.g. @c "dds.ip" = @c "127.0.0.1") that
   * are read by backends during @c init() and by helpers such as @c SslOptions.
   */
  using PropertiesMap = std::map<std::string, std::string>;

  /**
   * @brief Virtual destructor.
   */
  virtual ~Conf();

  /**
   * @brief Validates the conf for @p impl_type and caches it for subsequent factories.
   *
   * @details
   * The base implementation rejects @c kUnknownImplType (the underlying logger
   * call is configured to abort the process) and stores any other value into
   * @c impl_type_ so that follow-up @c create_*() calls know the requested role.
   * Subclasses typically chain @c Conf::parse() and then run their own checks.
   *
   * @param impl_type  Role the caller intends to instantiate.
   * @return @c true on success; the unknown-type fatal path never returns.
   */
  [[nodiscard]] virtual bool parse(ImplType impl_type) const;

  /**
   * @brief Indicates whether the conf currently holds usable data.
   *
   * @details
   * The base implementation returns @c false; concrete confs override it to
   * verify that mandatory fields have been populated.
   *
   * @return @c true once the conf is ready to drive @c create_*() factories.
   */
  [[nodiscard]] virtual bool is_valid() const;

  /**
   * @brief Returns the @c ImplType cached by the most recent @c parse() call.
   *
   * @return Cached @c ImplType, or @c kUnknownImplType before @c parse() runs.
   */
  [[nodiscard]] virtual ImplType get_impl_type() const;

  /**
   * @brief Returns the transport backend this conf wraps.
   *
   * @details
   * Default implementation returns @c TransportType::kUnknown; concrete confs
   * (and dynamic plugins) override it to advertise their backend.
   *
   * @return Matching @c TransportType identifier.
   */
  [[nodiscard]] virtual TransportType get_transport_type() const;

  uint32_t hash_code{0};  ///< Channel / topic hash assigned by concrete backends.

 protected:
  Conf();

  [[nodiscard]] virtual bool parse_protocol(struct Protocol* protocol);

  [[nodiscard]] virtual std::unique_ptr<class ServerImpl> create_server() const;

  [[nodiscard]] virtual std::unique_ptr<class ClientImpl> create_client() const;

  [[nodiscard]] virtual std::unique_ptr<class PublisherImpl> create_publisher() const;

  [[nodiscard]] virtual std::unique_ptr<class SubscriberImpl> create_subscriber() const;

  [[nodiscard]] virtual std::unique_ptr<class SetterImpl> create_setter() const;

  [[nodiscard]] virtual std::unique_ptr<class GetterImpl> create_getter() const;

 private:
  friend struct Url;
  template <typename, typename, SecurityType>
  friend class Server;
  template <typename, typename, SecurityType>
  friend class Client;
  template <typename, SecurityType>
  friend class Publisher;
  template <typename, SecurityType>
  friend class Subscriber;
  template <typename, SecurityType>
  friend class Setter;
  template <typename, SecurityType>
  friend class Getter;

  mutable ImplType impl_type_{kUnknownImplType};
};

}  // namespace vlink

////////////////////////////////////////////////////////////////
/// Macro Definitions
////////////////////////////////////////////////////////////////

/**
 * @def VLINK_DECLARE_CONF_FRIEND
 * @brief Grants the six public Node<> templates friend access to the conf.
 *
 * @details
 * Inject this macro into a concrete @c Conf subclass to expose the protected
 * factory methods to @c Server, @c Client, @c Publisher, @c Subscriber,
 * @c Setter and @c Getter.  @c VLINK_CONF_IMPL already expands it; use this
 * macro on its own only when @c VLINK_CONF_IMPL is not suitable.
 */
#define VLINK_DECLARE_CONF_FRIEND()           \
  template <typename, typename, SecurityType> \
  friend class Server;                        \
  template <typename, typename, SecurityType> \
  friend class Client;                        \
  template <typename, SecurityType>           \
  friend class Publisher;                     \
  template <typename, SecurityType>           \
  friend class Subscriber;                    \
  template <typename, SecurityType>           \
  friend class Setter;                        \
  template <typename, SecurityType>           \
  friend class Getter;

/**
 * @def VLINK_CONF_IMPL
 * @brief Convenience macro that emits the standard concrete conf boilerplate.
 *
 * @details
 * Expands to the friend grant, the six factory overrides, an ostream insertion
 * operator declaration, the default constructor / destructor and an
 * @c is_valid() override declaration whose body the subclass must provide.
 *
 * @param classname  Subclass name being declared.
 */
#define VLINK_CONF_IMPL(classname)                                                                     \
 private:                                                                                              \
  VLINK_DECLARE_CONF_FRIEND()                                                                          \
                                                                                                       \
  [[nodiscard]] bool parse_protocol(struct Protocol* protocol) override;                               \
                                                                                                       \
  [[nodiscard]] std::unique_ptr<class ServerImpl> create_server() const override;                      \
                                                                                                       \
  [[nodiscard]] std::unique_ptr<class ClientImpl> create_client() const override;                      \
                                                                                                       \
  [[nodiscard]] std::unique_ptr<class PublisherImpl> create_publisher() const override;                \
                                                                                                       \
  [[nodiscard]] std::unique_ptr<class SubscriberImpl> create_subscriber() const override;              \
                                                                                                       \
  [[nodiscard]] std::unique_ptr<class SetterImpl> create_setter() const override;                      \
                                                                                                       \
  [[nodiscard]] std::unique_ptr<class GetterImpl> create_getter() const override;                      \
                                                                                                       \
  VLINK_EXPORT friend std::ostream& operator<<(std::ostream& ostream, const classname& conf) noexcept; \
                                                                                                       \
 public:                                                                                               \
  classname() = default;                                                                               \
                                                                                                       \
  ~classname() = default;                                                                              \
                                                                                                       \
  [[nodiscard]] bool is_valid() const override;

/**
 * @def VLINK_ALLOW_IMPL_TYPE
 * @brief Records the bitmask of @c ImplType values supported by a conf.
 *
 * @details
 * Expands to a public @c get_allow_impl_type() that returns @p type so the
 * @c Node<> template can validate at compile time that the conf supports the
 * requested node role.  Combine roles with bitwise OR, e.g.
 * @code
 * VLINK_ALLOW_IMPL_TYPE(kServer | kClient | kPublisher | kSubscriber | kSetter | kGetter)
 * @endcode
 *
 * @param type  Bitmask of @c ImplType values supported by the conf.
 */
#define VLINK_ALLOW_IMPL_TYPE(type) \
 public:                            \
  [[nodiscard]] static constexpr int get_allow_impl_type() { return type; }

/**
 * @def VLINK_DECLARE_GLOBAL_PROPERTY
 * @brief Declares per-transport static configuration storage and access helpers.
 *
 * @details
 * Inject into a concrete @c Conf subclass body to expose:
 * - @c thread_count_, @c global_properties_ and @c global_mtx_ static members.
 * - @c get_thread_count() / @c set_thread_count() accessors.
 * - @c set_global_property() / @c get_global_property() / @c get_global_all_properties().
 * - A @c global_init() declaration whose definition the subclass supplies.
 *
 * Pair with @c VLINK_DEFINE_GLOBAL_PROPERTY in the matching translation unit.
 */
#define VLINK_DECLARE_GLOBAL_PROPERTY()                                                \
 private:                                                                              \
  static size_t thread_count_;                                                         \
  static PropertiesMap global_properties_;                                             \
  static std::shared_mutex global_mtx_;                                                \
                                                                                       \
 public:                                                                               \
  [[nodiscard]] static size_t get_thread_count() { return thread_count_; }             \
                                                                                       \
  static void set_thread_count(size_t thread_count) { thread_count_ = thread_count; }  \
                                                                                       \
  static void set_global_property(const std::string& prop, const std::string& value) { \
    std::lock_guard lock(global_mtx_);                                                 \
    global_properties_[prop] = value;                                                  \
  }                                                                                    \
                                                                                       \
  [[nodiscard]] static std::string get_global_property(const std::string& prop) {      \
    std::shared_lock lock(global_mtx_);                                                \
    auto iter = global_properties_.find(prop);                                         \
    return iter != global_properties_.end() ? iter->second : std::string();            \
  }                                                                                    \
                                                                                       \
  [[nodiscard]] static PropertiesMap get_global_all_properties() {                     \
    std::shared_lock lock(global_mtx_);                                                \
    return global_properties_;                                                         \
  }                                                                                    \
                                                                                       \
  static void global_init();

/**
 * @def VLINK_DEFINE_GLOBAL_PROPERTY
 * @brief Provides storage for the statics declared by @c VLINK_DECLARE_GLOBAL_PROPERTY.
 *
 * @details
 * Place once in the @c .cc file of the matching subclass.  Sets
 * @c thread_count_ to @c 1, default-constructs the property map, and
 * default-initialises the shared mutex.
 *
 * @param classname  Subclass that owns the static members.
 */
#define VLINK_DEFINE_GLOBAL_PROPERTY(classname)      \
  size_t classname::thread_count_{1};                \
  Conf::PropertiesMap classname::global_properties_; \
  std::shared_mutex classname::global_mtx_;
