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
 * @file url.h
 * @brief URL-driven configuration dispatcher that selects and forwards to a transport @c Conf.
 *
 * @details
 * This is an internal implementation header used by every public node template
 * to translate a URL string into a concrete transport backend.  It also re-exports
 * all transport @c *Conf headers and the @c *Impl headers so the impl layer can
 * be pulled in with a single include.  Two types are introduced:
 *
 * @par Protocol
 * A small plain-data struct populated by the @c UrlParser pipeline.  It owns
 * the URL string (after any @c VLINK_URL_REMAP rewriting) plus the resolved
 * @c TransportType and the parsed host, path, query dictionary and fragment.
 * Only @c Url may construct a @c Protocol -- the constructor is private and
 * @c Url is its only friend.
 *
 * @par Url
 * A concrete @c Conf subclass that wraps a @c Protocol, builds the matching
 * transport @c Conf in @c init_target_internal() and forwards every virtual
 * @c Conf hook to that target.  Constructing a @c Url is the entry point used
 * by every public Node<> template to set up its transport backend.
 *
 * @par Protocol struct fields
 * | Field          | Meaning                                                  |
 * | -------------- | -------------------------------------------------------- |
 * | @c str         | Full URL string after any @c VLINK_URL_REMAP rewrite.    |
 * | @c transport   | Resolved transport backend identifier.                   |
 * | @c host        | Hostname or IP component, if any.                        |
 * | @c path        | Topic path component.                                    |
 * | @c dictionary  | Query parameters parsed into a @c std::map.              |
 * | @c fragment    | Fragment identifier following @c #.                      |
 *
 * @par Transport prefix to backend
 * | URL prefix    | Conf class created in @c init_target_internal() |
 * | ------------- | ----------------------------------------------- |
 * | @c intra://   | @c IntraConf                                    |
 * | @c shm://     | @c ShmConf                                      |
 * | @c shm2://    | @c Shm2Conf                                     |
 * | @c zenoh://   | @c ZenohConf                                    |
 * | @c dds://     | @c DdsConf                                      |
 * | @c ddsc://    | @c DdscConf                                     |
 * | @c ddsr://    | @c DdsrConf                                     |
 * | @c ddst://    | @c DdstConf                                     |
 * | @c someip://  | @c SomeipConf                                   |
 * | @c mqtt://    | @c MqttConf                                     |
 * | @c fdbus://   | @c FdbusConf                                    |
 * | @c qnx://     | @c QnxConf                                      |
 * | other         | @c load_for_plugin() searches loaded plugins.   |
 *
 * @par Construction flow
 * @code
 *   URL string  -> UrlParser -> Protocol -> init_target_internal() -> *Conf
 *                                              |
 *                                              v
 *                                       parse(impl_type)
 *                                              |
 *                                              v
 *                                       create_publisher() / create_subscriber() / ...
 *                                              |
 *                                              v
 *                                       NodeImpl backend instance
 * @endcode
 *
 * @par URL Remapping
 * When @c VLINK_URL_USE_REMAP is enabled, the @c Protocol constructor inspects
 * the @c VLINK_URL_REMAP environment variable and rewrites the URL before the
 * transport is resolved.  This enables transport switching without touching
 * application code.
 *
 * @par Plugin loading
 * When @c VLINK_URL_USE_PLUGIN is enabled, @c Url::init_plugins() loads shared
 * libraries listed in the @c VLINK_URL_PLUGINS environment variable; the
 * loaded @c ConfPluginInterface entries are consulted by @c load_for_plugin()
 * for any transport that has no built-in match.
 *
 * @par Transport enable flags
 * @c TransportEnableFlag is a bitmask that selects which built-in transports
 * participate in @c global_init() and @c init_plugins().  Embedding
 * environments (Android, QNX, etc.) use it to skip transports they cannot
 * support at runtime.
 *
 * @par Example
 * @code
 * vlink::Url url("dds://vehicle/speed?domain_id=1");
 *
 * if (url.parse(vlink::kSubscriber)) {
 *   auto impl = url.get_target() != nullptr
 *                 ? std::unique_ptr<vlink::SubscriberImpl>{}
 *                 : nullptr;
 *   // Public Subscriber<T> template performs this call internally.
 *   auto sub = vlink::Subscriber<MyMsg>::create_unique(url.get_str());
 * }
 * @endcode
 *
 * @note This file is the single aggregation point for the VLink impl layer; it
 *       transitively includes every transport @c *_conf.h header and every
 *       @c *_impl.h header.
 */

#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>

#include "../base/logger.h"
#include "./conf.h"

// NOLINTBEGIN
#include "../modules/dds_conf.h"
#include "../modules/ddsc_conf.h"
#include "../modules/ddsr_conf.h"
#include "../modules/ddst_conf.h"
#include "../modules/fdbus_conf.h"
#include "../modules/intra_conf.h"
#include "../modules/mqtt_conf.h"
#include "../modules/qnx_conf.h"
#include "../modules/shm2_conf.h"
#include "../modules/shm_conf.h"
#include "../modules/someip_conf.h"
#include "../modules/zenoh_conf.h"
//
#include "./client_impl.h"
#include "./getter_impl.h"
#include "./publisher_impl.h"
#include "./server_impl.h"
#include "./setter_impl.h"
#include "./subscriber_impl.h"
// NOLINTEND

namespace vlink {

/**
 * @struct Protocol
 * @brief Plain-data record describing the parsed components of a VLink URL.
 *
 * @details
 * Built by @c Protocol(const std::string& address), which feeds the URL through
 * @c UrlParser, applies any @c VLINK_URL_REMAP rewriting and resolves the
 * @c TransportType from the URI scheme.  Only @c Url may construct a
 * @c Protocol (the constructor is private and @c Url is the sole friend).
 *
 * @note The @c str field is the URL string after remap, not a reconstruction
 *       from the other fields.
 */
struct VLINK_EXPORT Protocol final {
  std::string str;                                ///< URL string after remap, if any.
  TransportType transport;                        ///< Resolved transport backend identifier.
  std::string host;                               ///< Hostname or IP component, if any.
  std::string path;                               ///< Topic path component.
  std::map<std::string, std::string> dictionary;  ///< Query parameters parsed into a key/value dictionary.
  std::string fragment;                           ///< Fragment identifier (after @c #).

 private:
  friend struct Url;
  explicit Protocol(const std::string& address);
};

/**
 * @struct Url
 * @brief @c Conf subclass that routes virtual calls to the transport selected by a URL string.
 *
 * @details
 * Construction parses the URL into a @c Protocol and then runs
 * @c init_target_internal() to instantiate the matching transport @c Conf
 * (@c target_).  Every @c Conf virtual hook is forwarded to @c target_; the
 * caller need only build one @c Url instance per topic.
 *
 * @par Full lifecycle
 * @code
 *   // 1. Construct with URL string:
 *   Url url("dds://vehicle/speed");
 *   //    -> Protocol("dds://vehicle/speed") -> transport == kDds
 *   //    -> init_target_internal() -> target_ = make_unique<DdsConf>()
 *
 *   // 2. Parse for a specific node role:
 *   url.parse(kSubscriber);
 *   //    -> target_->parse(kSubscriber)
 *   //    -> target_->parse_protocol(&protocol_)
 *
 *   // 3. Create the transport implementation:
 *   auto impl = url.create_subscriber();
 *   //    -> target_->create_subscriber()
 * @endcode
 */
struct Url final : public Conf {
  /**
   * @enum TransportEnableFlag
   * @brief Bitmask that selects which transports participate in @c global_init() / @c init_plugins().
   *
   * @details
   * Embedding environments (e.g. Android, QNX) pass a subset of these flags to
   * skip transports they cannot support at runtime.  Bit positions are
   * independent of the numeric @c TransportType values.
   *
   * | Flag             | Bit position | Transport       |
   * | ---------------- | ------------ | --------------- |
   * | @c kEnableIntra  | 15           | @c intra://     |
   * | @c kEnableShm    | 14           | @c shm://       |
   * | @c kEnableShm2   | 13           | @c shm2://      |
   * | @c kEnableZenoh  | 12           | @c zenoh://     |
   * | @c kEnableDds    | 11           | @c dds://       |
   * | @c kEnableDdsc   | 10           | @c ddsc://      |
   * | @c kEnableDdsr   |  9           | @c ddsr://      |
   * | @c kEnableDdst   |  8           | @c ddst://      |
   * | @c kEnableSomeip |  7           | @c someip://    |
   * | @c kEnableMqtt   |  6           | @c mqtt://      |
   * | @c kEnableFdbus  |  5           | @c fdbus://     |
   * | @c kEnableQnx    |  4           | @c qnx://       |
   * | @c kEnableAll    | all bits set | Every transport |
   */
  enum TransportEnableFlag : uint16_t {
    kEnableEmpty = 0b0000'0000'0000'0000,   ///< No transport enabled.
    kEnableIntra = 0b1000'0000'0000'0000,   ///< Enable the @c intra:// transport.
    kEnableShm = 0b0100'0000'0000'0000,     ///< Enable the @c shm:// (Iceoryx) transport.
    kEnableShm2 = 0b0010'0000'0000'0000,    ///< Enable the @c shm2:// (Iceoryx2) transport.
    kEnableZenoh = 0b0001'0000'0000'0000,   ///< Enable the @c zenoh:// transport.
    kEnableDds = 0b0000'1000'0000'0000,     ///< Enable the @c dds:// (Fast-DDS) transport.
    kEnableDdsc = 0b0000'0100'0000'0000,    ///< Enable the @c ddsc:// (CycloneDDS) transport.
    kEnableDdsr = 0b0000'0010'0000'0000,    ///< Enable the @c ddsr:// (RTI DDS) transport.
    kEnableDdst = 0b0000'0001'0000'0000,    ///< Enable the @c ddst:// (TravoDDS) transport.
    kEnableSomeip = 0b0000'0000'1000'0000,  ///< Enable the @c someip:// transport.
    kEnableMqtt = 0b0000'0000'0100'0000,    ///< Enable the @c mqtt:// transport.
    kEnableFdbus = 0b0000'0000'0010'0000,   ///< Enable the @c fdbus:// transport.
    kEnableQnx = 0b0000'0000'0001'0000,     ///< Enable the @c qnx:// transport (QNX only).
    kEnableAll = 0b1111'1111'1111'1111,     ///< Enable every transport.
  };

  /**
   * @brief Builds a @c Url from a transport address string.
   *
   * @details
   * Parses @p str into a @c Protocol, then delegates to
   * @c init_target_internal() to allocate the matching transport @c Conf.
   * Triggers a fatal log entry when no transport backend matches the URL.
   *
   * @param str  VLink URL string (e.g. @c "dds://vehicle/speed").
   */
  explicit Url(const std::string& str);

  /**
   * @brief Copy constructor.
   *
   * @details
   * Copies the @c Protocol from @p url and rebuilds a fresh @c target_ via
   * @c init_target_internal(), so the two @c Url objects do not share the
   * same transport @c Conf instance.
   *
   * @param url  Source @c Url to copy.
   */
  Url(const Url& url);

  /**
   * @brief Move constructor.
   *
   * @details
   * Transfers both @c protocol_ and @c target_ from @p url; no rebuild is
   * performed.
   *
   * @param url  Source @c Url to move from.
   */
  Url(Url&& url) noexcept;

  /**
   * @brief Destructor.
   */
  ~Url() override;

  /**
   * @brief Copy assignment.
   *
   * @details
   * Copies @c protocol_ and re-runs @c init_target_internal() to rebuild
   * @c target_.
   *
   * @param url  Source @c Url.
   * @return Reference to @c *this.
   */
  Url& operator=(const Url& url);

  /**
   * @brief Move assignment.
   *
   * @param url  Source @c Url.
   * @return Reference to @c *this.
   */
  Url& operator=(Url&& url) noexcept;

  /**
   * @brief Returns the stored URL string (after any @c VLINK_URL_REMAP rewrite).
   *
   * @return Reference to the string stored inside @c Protocol::str.
   */
  [[nodiscard]] const std::string& get_str() const;

  /**
   * @brief Returns the underlying transport @c Conf or @c nullptr.
   *
   * @details
   * Lets callers downcast the active transport conf for transport-specific
   * inspection (for example to a @c DdsConf for native DDS QoS).
   *
   * @return Pointer to the cached transport @c Conf; @c nullptr when the URL
   *         was invalid or @c init_target_internal() failed.
   */
  [[nodiscard]] const Conf* get_target() const;

  /**
   * @brief Parses the URL for @p impl_type by delegating to @c target_.
   *
   * @details
   * Chains @c Conf::parse(impl_type), @c target_->parse(impl_type) and
   * @c target_->parse_protocol(); returns @c false on @c target_ being null
   * or any step failing.
   *
   * @param impl_type  Bitmask of @c ImplType roles to validate.
   * @return @c true when every step succeeds; @c false otherwise.
   */
  bool parse(ImplType impl_type) const override;

  /**
   * @brief Reports whether the underlying @c target_ conf is valid.
   *
   * @return Result of @c target_->is_valid(), or @c false when @c target_ is null.
   */
  [[nodiscard]] bool is_valid() const override;

  /**
   * @brief Returns the @c ImplType cached by the most recent @c target_->parse().
   *
   * @return Cached @c ImplType, or @c kUnknownImplType when @c target_ is null.
   */
  [[nodiscard]] ImplType get_impl_type() const override;

  /**
   * @brief Returns the transport backend identifier resolved from the URL.
   *
   * @return @c TransportType value, or @c TransportType::kUnknown when no transport was resolved.
   */
  [[nodiscard]] TransportType get_transport_type() const override;

  /**
   * @brief Loads and registers transport plugins from @c VLINK_URL_PLUGINS.
   *
   * @details
   * Discovered plugins whose @c TransportType already appears in
   * @p transport_enable_flags are skipped so that linked transports continue
   * to take precedence.  The first @c Url construction triggers this call
   * automatically; explicit invocations are only needed for unusual
   * initialisation sequences.
   *
   * @param transport_enable_flags  Bitmask of built-in transports already
   *                                available; matching plugins are ignored.
   */
  VLINK_EXPORT static void init_plugins(uint16_t transport_enable_flags = 0);

  /**
   * @brief Asks loaded plugins for a @c Conf factory matching @p type.
   *
   * @details
   * Looks up a previously registered @c ConfPluginInterface whose
   * @c get_transport_type() returns @p type and invokes @c create() on it.
   *
   * @param type  Transport backend to look up.
   * @return Newly created @c Conf, or @c nullptr when no plugin matches.
   */
  [[nodiscard]] VLINK_EXPORT static std::unique_ptr<Conf> load_for_plugin(TransportType type);

  /**
   * @brief Returns a numeric sort index for the transport backend of @p url.
   *
   * @details
   * Used to order URLs by transport priority.  Local transports
   * (@c intra://, @c shm://) yield lower indices than network transports.
   * Empty URLs return @c -1, while non-empty URLs whose transport is unknown
   * still return @c 0 so they can participate in low-priority sorting.
   *
   * @param url  URL string to classify.
   * @return Sort index; lower values mean higher priority.
   */
  [[nodiscard]] VLINK_EXPORT static int get_sort_index(std::string_view url);

  /**
   * @brief Returns whether @p url designates a same-machine transport.
   *
   * @details
   * A URL is local when it uses @c intra://, @c shm:// or @c shm2://.
   *
   * @param url  URL string to classify.
   * @return @c true for local transports; @c false for network ones.
   */
  [[nodiscard]] VLINK_EXPORT static bool is_local_type(std::string_view url);

  /**
   * @brief Returns whether @p url designates the @c intra:// in-process transport.
   *
   * @param url  URL string to classify.
   * @return @c true only for @c intra:// URLs.
   */
  [[nodiscard]] VLINK_EXPORT static bool is_intra_type(std::string_view url);

  /**
   * @brief Returns whether @p url uses a shared-memory transport.
   *
   * @param url  URL string to classify.
   * @return @c true for both @c shm:// and @c shm2:// URLs.
   */
  [[nodiscard]] VLINK_EXPORT static bool is_shm_type(std::string_view url);

  /**
   * @brief Initialises the process-wide state for every enabled transport.
   *
   * @details
   * Calls @c NodeImpl::global_init() first and then each @c *Conf::global_init()
   * whose bit appears in @p transport_enable_flags.  Passing @c 0 expands to
   * all compiled-in transports.  Must run once before any @c Url is created
   * when fine-grained transport selection is required; otherwise the
   * transports are lazily initialised on first use.
   *
   * @param transport_enable_flags  Bitmask of @c TransportEnableFlag values.
   */
  static void global_init(uint16_t transport_enable_flags = 0);

  /**
   * @brief Returns a bitmask of all compile-time-enabled transports.
   *
   * @details
   * Computed from the @c VLINK_SUPPORT_* preprocessor flags.  The result can
   * be passed to @c global_init() to initialise the available transports
   * exactly.
   *
   * @return Bitmask of @c TransportEnableFlag values.
   */
  [[nodiscard]] static uint16_t get_transport_enable_flags();

  /**
   * @brief Builds @c target_ for the resolved transport in @p protocol.
   *
   * @details
   * Switches on @c Protocol::transport, allocates the matching @c *Conf class,
   * and falls back to @c load_for_plugin() when no built-in backend matches.
   * Logs a fatal entry when neither path succeeds.
   *
   * @param protocol  Parsed URL information used to select the transport.
   * @param target    Output: receives the newly created @c Conf instance.
   */
  static void init_target_internal(const Protocol& protocol, std::unique_ptr<Conf>& target);

 private:
  std::unique_ptr<ServerImpl> create_server() const override;

  std::unique_ptr<ClientImpl> create_client() const override;

  std::unique_ptr<PublisherImpl> create_publisher() const override;

  std::unique_ptr<SubscriberImpl> create_subscriber() const override;

  std::unique_ptr<SetterImpl> create_setter() const override;

  std::unique_ptr<GetterImpl> create_getter() const override;

  VLINK_EXPORT friend std::ostream& operator<<(std::ostream& ostream, const Url& conf) noexcept;

  mutable Protocol protocol_;
  std::unique_ptr<Conf> target_;
  VLINK_DECLARE_CONF_FRIEND()
  VLINK_ALLOW_IMPL_TYPE(kServer | kClient | kPublisher | kSubscriber | kSetter | kGetter);
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

inline Url::Url(const std::string& str) : protocol_(str) { init_target_internal(protocol_, target_); }

// NOLINTNEXTLINE(bugprone-copy-constructor-init)
inline Url::Url(const Url& url) : protocol_(url.protocol_) { init_target_internal(protocol_, target_); }

inline Url::Url(Url&& url) noexcept : protocol_(std::move(url.protocol_)), target_(std::move(url.target_)) {}

inline Url::~Url() = default;

inline Url& Url::operator=(const Url& url) {
  if VUNLIKELY (this == &url) {
    return *this;
  }

  protocol_ = url.protocol_;

  init_target_internal(protocol_, target_);

  return *this;
}

inline Url& Url::operator=(Url&& url) noexcept {
  if VUNLIKELY (this == &url) {
    return *this;
  }

  protocol_ = std::move(url.protocol_);
  target_ = std::move(url.target_);

  return *this;
}

inline const std::string& Url::get_str() const { return protocol_.str; }

inline const Conf* Url::get_target() const { return target_.get(); }

inline bool Url::parse(ImplType impl_type) const {
  if VUNLIKELY (!target_) {
    return false;
  }

  if VUNLIKELY (!Conf::parse(impl_type) || !target_->parse(impl_type)) {
    return false;
  }

  return target_->parse_protocol(&protocol_);
}

inline bool Url::is_valid() const {
  if VUNLIKELY (!target_) {
    return false;
  }

  return target_->is_valid();
}

inline ImplType Url::get_impl_type() const {
  if VUNLIKELY (!target_) {
    return kUnknownImplType;
  }

  return target_->get_impl_type();
}

inline TransportType Url::get_transport_type() const {
  if VUNLIKELY (!target_) {
    return TransportType::kUnknown;
  }

  return target_->get_transport_type();
}

inline void Url::global_init(uint16_t transport_enable_flags) {
  if (transport_enable_flags == 0) {
    transport_enable_flags = get_transport_enable_flags();
  }

  (void)transport_enable_flags;

  NodeImpl::global_init();

#ifndef VLINK_ENABLE_C_INTERFACE

#ifdef VLINK_SUPPORT_INTRA

  if (transport_enable_flags & kEnableIntra) {
    IntraConf::global_init();
  }
#endif

#ifdef VLINK_SUPPORT_SHM

  if (transport_enable_flags & kEnableShm) {
    ShmConf::global_init();
  }
#endif

#ifdef VLINK_SUPPORT_SHM2

  if (transport_enable_flags & kEnableShm2) {
    Shm2Conf::global_init();
  }
#endif

#ifdef VLINK_SUPPORT_ZENOH

  if (transport_enable_flags & kEnableZenoh) {
    ZenohConf::global_init();
  }
#endif

#ifdef VLINK_SUPPORT_DDS

  if (transport_enable_flags & kEnableDds) {
    DdsConf::global_init();
  }
#endif

#ifdef VLINK_SUPPORT_DDSC

  if (transport_enable_flags & kEnableDdsc) {
    DdscConf::global_init();
  }
#endif

#ifdef VLINK_SUPPORT_DDSR

  if (transport_enable_flags & kEnableDdsr) {
    DdsrConf::global_init();
  }
#endif

#ifdef VLINK_SUPPORT_DDST

  if (transport_enable_flags & kEnableDdst) {
    DdstConf::global_init();
  }
#endif

#ifdef VLINK_SUPPORT_SOMEIP

  if (transport_enable_flags & kEnableSomeip) {
    SomeipConf::global_init();
  }
#endif

#ifdef VLINK_SUPPORT_MQTT

  if (transport_enable_flags & kEnableMqtt) {
    MqttConf::global_init();
  }
#endif

#ifdef VLINK_SUPPORT_FDBUS

  if (transport_enable_flags & kEnableFdbus) {
    FdbusConf::global_init();
  }
#endif

#ifdef VLINK_SUPPORT_QNX

  if (transport_enable_flags & kEnableQnx) {
    QnxConf::global_init();
  }
#endif

#endif
}

inline uint16_t Url::get_transport_enable_flags() {
  uint16_t flags = 0;

#ifdef VLINK_SUPPORT_INTRA
  flags |= kEnableIntra;
#endif

#ifdef VLINK_SUPPORT_SHM
  flags |= kEnableShm;
#endif

#ifdef VLINK_SUPPORT_SHM2
  flags |= kEnableShm2;
#endif

#ifdef VLINK_SUPPORT_ZENOH
  flags |= kEnableZenoh;
#endif

#ifdef VLINK_SUPPORT_DDS
  flags |= kEnableDds;
#endif

#ifdef VLINK_SUPPORT_DDSC
  flags |= kEnableDdsc;
#endif

#ifdef VLINK_SUPPORT_DDSR
  flags |= kEnableDdsr;
#endif

#ifdef VLINK_SUPPORT_DDST
  flags |= kEnableDdst;
#endif

#ifdef VLINK_SUPPORT_SOMEIP
  flags |= kEnableSomeip;
#endif

#ifdef VLINK_SUPPORT_MQTT
  flags |= kEnableMqtt;
#endif

#ifdef VLINK_SUPPORT_FDBUS
  flags |= kEnableFdbus;
#endif

#ifdef VLINK_SUPPORT_QNX
  flags |= kEnableQnx;
#endif

  return flags;
}

inline void Url::init_target_internal(const Protocol& protocol, std::unique_ptr<Conf>& target) {
  static auto transport_enable_flags = get_transport_enable_flags();

  Url::init_plugins(transport_enable_flags);

  // NOLINTBEGIN
  switch (protocol.transport) {
#ifdef VLINK_SUPPORT_INTRA
    case TransportType::kIntra:
      target = std::make_unique<IntraConf>();
      break;
#endif

#ifdef VLINK_SUPPORT_SHM
    case TransportType::kShm:
      target = std::make_unique<ShmConf>();
      break;
#endif

#ifdef VLINK_SUPPORT_SHM2
    case TransportType::kShm2:
      target = std::make_unique<Shm2Conf>();
      break;
#endif

#ifdef VLINK_SUPPORT_ZENOH
    case TransportType::kZenoh:
      target = std::make_unique<ZenohConf>();
      break;
#endif

#ifdef VLINK_SUPPORT_DDS
    case TransportType::kDds:
      target = std::make_unique<DdsConf>();
      break;
#endif

#ifdef VLINK_SUPPORT_DDSC
    case TransportType::kDdsc:
      target = std::make_unique<DdscConf>();
      break;
#endif

#ifdef VLINK_SUPPORT_DDSR
    case TransportType::kDdsr:
      target = std::make_unique<DdsrConf>();
      break;
#endif

#ifdef VLINK_SUPPORT_DDST
    case TransportType::kDdst:
      target = std::make_unique<DdstConf>();
      break;
#endif

#ifdef VLINK_SUPPORT_SOMEIP
    case TransportType::kSomeip:
      target = std::make_unique<SomeipConf>();
      break;
#endif

#ifdef VLINK_SUPPORT_MQTT
    case TransportType::kMqtt:
      target = std::make_unique<MqttConf>();
      break;
#endif

#ifdef VLINK_SUPPORT_FDBUS
    case TransportType::kFdbus:
      target = std::make_unique<FdbusConf>();
      break;
#endif

#ifdef VLINK_SUPPORT_QNX
    case TransportType::kQnx:
      target = std::make_unique<QnxConf>();
      break;
#endif

    default:
      break;
  }
  // NOLINTEND

  if VUNLIKELY (!target) {
    target = Url::load_for_plugin(protocol.transport);
  }

  if VUNLIKELY (!target) {
    CLOG_F("Unsupported url[%s].", protocol.str.c_str());
  }
}

inline std::unique_ptr<ServerImpl> Url::create_server() const {
  if VUNLIKELY (!target_) {
    return nullptr;
  }

  return target_->create_server();
}

inline std::unique_ptr<ClientImpl> Url::create_client() const {
  if VUNLIKELY (!target_) {
    return nullptr;
  }

  return target_->create_client();
}

inline std::unique_ptr<PublisherImpl> Url::create_publisher() const {
  if VUNLIKELY (!target_) {
    return nullptr;
  }

  return target_->create_publisher();
}

inline std::unique_ptr<SubscriberImpl> Url::create_subscriber() const {
  if VUNLIKELY (!target_) {
    return nullptr;
  }

  return target_->create_subscriber();
}

inline std::unique_ptr<SetterImpl> Url::create_setter() const {
  if VUNLIKELY (!target_) {
    return nullptr;
  }

  return target_->create_setter();
}

inline std::unique_ptr<GetterImpl> Url::create_getter() const {
  if VUNLIKELY (!target_) {
    return nullptr;
  }

  return target_->create_getter();
}

}  // namespace vlink
