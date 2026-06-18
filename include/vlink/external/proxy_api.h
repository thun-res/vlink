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
 * @file proxy_api.h
 * @brief Client-side C++ API for connecting to a @c ProxyServer daemon.
 *
 * @details
 * @c ProxyAPI is the consumer-facing half of the VLink proxy subsystem.  It connects
 * to a running @c ProxyServer over a security-authenticated DDS channel set and
 * exposes asynchronous callbacks for connection state, error transitions, server
 * heartbeats, per-topic statistics, and (when active) raw payload bytes.  The class
 * inherits @c MessageLoop, so posted tasks, timers, handshake retries, and async
 * control publishes execute inside the inherited loop once @c run() or
 * @c async_run() has been started.  Incoming DDS/SHM callbacks may still arrive on
 * transport-managed receive threads.
 *
 * @par Roles
 * Each instance is constructed with exactly one role.
 *
 * | Role             | Description                                                             |
 * | ---------------- | ----------------------------------------------------------------------- |
 * | @c kController   | Drives the server: may call @c send_control() and @c send_data().       |
 * | @c kListener     | Passive observer only; @c send_control() / @c send_data() return false. |
 *
 * @par Inheritance Pattern
 * Both @c ProxyAPI and @c ProxyServer derive from @c MessageLoop.  Application code
 * may either embed a @c ProxyAPI directly or subclass it to centralise callback
 * wiring.
 * @code
 *  MessageLoop
 *     ^
 *     |
 *  ProxyAPI                 <-- this class (publicly inherits MessageLoop)
 *     ^
 *     |
 *  MyMonitor : public ProxyAPI
 *     - register_connect_callback()
 *     - register_info_callback()
 *     - register_data_callback()
 * @endcode
 *
 * @par Operation Modes
 * Selectable by a @c kController instance through @c Control::mode.
 *
 * | Mode                    | Value | Description                                              |
 * | ----------------------- | ----- | -------------------------------------------------------- |
 * | @c kOffline             |   0   | Disconnected; server releases all subscriptions.         |
 * | @c kObserveOne          |   1   | Observe a single selected topic.                         |
 * | @c kObserveAll          |   2   | Observe every discovered topic.                          |
 * | @c kRecord              |   3   | Record every topic in the subscription URL list.         |
 * | @c kPlay                |   4   | Playback / injection, usually fed by recorded data.      |
 * | @c kEdit                |   5   | Forward data injected by the controller.                 |
 * | @c kAuto                |   6   | Observe selected topics and accept controller injection. |
 * | @c kAutoAndObserveAll   |   7   | Observe every topic and accept controller injection.     |
 *
 * @par Error Codes
 *
 * | Error                   | Value | Description                                                     |
 * | ----------------------- | ----- | --------------------------------------------------------------- |
 * | @c kNoError             |   0   | No error.                                                       |
 * | @c kModeError           |   1   | Reserved legacy code for unsupported modes.                     |
 * | @c kControlError        |   2   | Control ID mismatch with the server.                            |
 * | @c kReliableCompError   |   3   | @c reliable setting mismatch between client and server.         |
 * | @c kTcpCompError        |   4   | @c enable_tcp setting mismatch.                                 |
 * | @c kDirectCompError     |   5   | @c direct setting mismatch.                                     |
 * | @c kMultiProxyError     |   7   | Multiple proxy servers or @c Time identity mismatch.            |
 * | @c kVersionCompError    |   8   | VLink version mismatch between client and server.               |
 * | @c kTokenError          |   9   | Handshake refused/empty token, or same-identity token mismatch. |
 * | @c kUnknownError        |  10   | Unknown or unclassified error.                                  |
 *
 * @par Connectivity, Handshake, and Heartbeat
 * Internally the API subscribes to the 1-second @c Time heartbeat published by
 * @c ProxyServer.  When @c VLINK_PROXY_ENABLE_HANDSHAKE is non-zero (default), it
 * also runs an RPC handshake against the server's security-authenticated handshake
 * service before any @c Control may be published; the server replies with a
 * per-process @em token that the API caches under a dedicated mutex and stamps onto
 * every outgoing @c Control.  A refused or empty token raises @c kTokenError, while
 * handshake setup/connect/invoke timeouts are treated as channel-not-ready and
 * retried silently.  Every incoming heartbeat must come from the same server
 * identity and carry the same token: identity mismatch raises @c kMultiProxyError,
 * an identity-matching token mismatch raises @c kTokenError; both paths clear the
 * cached token and retry the handshake.  After five consecutive seconds without a
 * heartbeat the connection is declared lost and @c ConnectCallback fires with
 * @c connected=false.  A @c kController instance caches and posts an initial
 * @c kAuto @c Control at construction and re-sends the last control automatically
 * once the server reconnects; actual publication waits for the loop, handshake, and
 * transport handles to be ready.  When the macro is @c 0, the handshake is bypassed
 * and controls flow without any token field.
 *
 * @par Channel Configuration
 *
 * | @c direct | Data path                                  | Control / Info / Time / Handshake path |
 * | --------- | ------------------------------------------ | -------------------------------------- |
 * |  false    | Server-relayed DDS data channel            | DDS (security-enabled)                 |
 * |  true     | Local direct SHM publishers / subscribers  | DDS (security-enabled)                 |
 *
 * @par Version Matching
 * When @c Config::match_version is @c true (default) the client cross-checks the
 * server's @c VLINK_VERSION string at connection time and reports
 * @c kVersionCompError on mismatch.
 *
 * @par Example (Controller)
 * @code
 * vlink::ProxyAPI::Config cfg;
 * cfg.role          = vlink::ProxyAPI::kController;
 * cfg.dds_impl      = "dds";
 * cfg.domain_id     = 0;
 * cfg.reliable      = false;
 * cfg.match_version = true;
 *
 * vlink::ProxyAPI api(cfg);
 * api.register_connect_callback([](bool connected) {
 *   if (connected) {
 *     // server online
 *   }
 * });
 * api.register_info_callback([](const std::vector<vlink::ProxyAPI::Info>& list) {
 *   for (const auto& info : list) {
 *     // info.url, info.freq, info.status, ...
 *   }
 * });
 * api.async_run(); // start the loop so handshake/timers can run
 *
 * vlink::ProxyAPI::Control ctrl;
 * ctrl.mode = vlink::ProxyAPI::kObserveOne;
 * ctrl.url_meta_list.push_back(
 *     {"dds://my/topic", "demo.proto.PointCloud", vlink::SchemaType::kProtobuf, vlink::kSubscriber});
 * api.send_control(ctrl);
 * @endcode
 *
 * @note
 * - Only one @c ProxyServer should exist on a given DDS domain at a time; reaching
 *   two will trigger @c kMultiProxyError.
 * - @c send_control() and @c send_data() return @c false immediately for
 *   @c kListener instances.
 */

#pragma once

#undef VLINK_PROXY_API_EXPORT
#ifdef VLINK_PROXY_API_LIBRARY_STATIC
#define VLINK_PROXY_API_EXPORT
#elif defined(_WIN32) || defined(__CYGWIN__)
#ifdef VLINK_PROXY_API_LIBRARY
#define VLINK_PROXY_API_EXPORT __declspec(dllexport)
#else
#define VLINK_PROXY_API_EXPORT __declspec(dllimport)
#endif
#else
#define VLINK_PROXY_API_EXPORT __attribute__((visibility("default")))
#endif

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "../base/bytes.h"
#include "../base/functional.h"
#include "../base/message_loop.h"
#include "../impl/types.h"

namespace vlink {

/**
 * @class ProxyAPI
 * @brief Client-side proxy monitoring and control surface backed by a @c MessageLoop.
 *
 * @details
 * Connects to a @c ProxyServer over security-authenticated DDS control channels and
 * exposes registration entry points for connection state, error transitions,
 * heartbeat timestamps, per-topic statistics, and raw data payloads.  In direct mode
 * data payloads ride on locally-created SHM channels instead of the server-relayed
 * data channel.  Inherits @c MessageLoop; start the loop with @c run() or
 * @c async_run() so the handshake retries, heartbeat timer, handle reset, and async
 * control posts can execute.
 */
class VLINK_PROXY_API_EXPORT ProxyAPI : public MessageLoop {
 public:
  /**
   * @enum Mode
   * @brief Proxy operation modes published via @c send_control().
   *
   * @details
   * The mode governs what the @c ProxyServer observes or what the controller may
   * inject.  Only a @c kController instance may switch the mode.
   */
  enum Mode : uint8_t {
    kOffline = 0,            ///< Disconnected; server releases every subscription.
    kObserveOne = 1,         ///< Observe a single topic taken from the URL list.
    kObserveAll = 2,         ///< Observe every discovered topic on the network.
    kRecord = 3,             ///< Record data from topics in the URL list.
    kPlay = 4,               ///< Injection/playback mode, usually fed by recorded data.
    kEdit = 5,               ///< Forward data injected by the controller.
    kAuto = 6,               ///< Observe specified URLs and accept controller injection.
    kAutoAndObserveAll = 7,  ///< Observe every topic and accept controller injection.
  };

  /**
   * @enum Error
   * @brief Compatibility and protocol error codes reported through @c ErrorCallback.
   *
   * @details
   * Errors are produced when the client parses an incoming @c Time heartbeat or a
   * handshake response; the callback fires only when the error state changes.
   */
  enum Error : uint8_t {
    kNoError = 0,            ///< No error; the connection is healthy.
    kModeError = 1,          ///< Reserved legacy code; unsupported modes are ignored server-side.
    kControlError = 2,       ///< Server responded with a different @c control_id.
    kReliableCompError = 3,  ///< @c reliable setting differs between client and server.
    kTcpCompError = 4,       ///< @c enable_tcp setting differs between client and server.
    kDirectCompError = 5,    ///< @c direct setting differs between client and server.
    kMultiProxyError = 7,    ///< Multiple proxy servers, or Time identity differs from the handshake.
    kVersionCompError = 8,   ///< @c VLINK_VERSION string differs between client and server.
    kTokenError = 9,         ///< Handshake refused/empty token or identity-matching token mismatch.
    kUnknownError = 10,      ///< Unknown or unclassified error.
  };

  /**
   * @enum Status
   * @brief Per-topic activity status reported inside @c Info records.
   */
  enum Status : uint8_t {
    kActive = 0,    ///< Topic is actively receiving data.
    kInActive = 1,  ///< Topic exists but has not produced data recently.
    kPending = 2,   ///< Topic was just discovered; statistics are still accumulating.
    kInvalid = 3,   ///< Topic type does not support observation (e.g. subscriber-only).
  };

  /**
   * @enum Role
   * @brief Role this @c ProxyAPI instance plays towards the @c ProxyServer.
   *
   * @details
   * @c kController may call @c send_control() and @c send_data().  @c kListener is
   * a passive observer; send calls return @c false immediately.
   */
  enum Role : uint8_t { kController = 0, kListener };

  /**
   * @struct Process
   * @brief Description of a VLink node process attached to a topic endpoint.
   */
  struct Process final {
    uint32_t type{0};  ///< Node-type bitmask (kPublisher / kSubscriber / kServer / kClient / kSetter / kGetter).
    std::string host;  ///< Hostname of the machine running the process.
    uint32_t pid{0};   ///< Operating-system process ID.
    std::string name;  ///< Human-readable process name.
    std::string ip;    ///< IP address of the network interface in use.
  };

  /**
   * @struct Info
   * @brief Statistics and metadata describing one discovered topic endpoint.
   *
   * @details
   * Delivered in batches through @c InfoCallback once per second.  @c freq, @c rate,
   * @c loss, and @c latency are weighted moving averages over the last two
   * one-second collection intervals.
   */
  struct Info final {
    uint32_t type{0};                         ///< Node-type bitmask for this endpoint.
    std::string url;                          ///< Full topic URL, e.g. @c "dds://my/topic".
    std::string ser;                          ///< Serialisation type, e.g. @c "demo.proto.PointCloud".
    SchemaType schema{SchemaType::kUnknown};  ///< Coarse schema family of the payload.
    Status status{kInvalid};                  ///< Current activity status of the topic.
    float freq{0};                            ///< Observed message rate in messages/s.
    uint64_t rate{0};                         ///< Observed throughput in bytes/s.
    float loss{0};                            ///< Sample loss ratio in the range [0, 1].
    float latency{0};                         ///< Latency in ms; @c -1 = no sample, @c -2 = invalid.
    std::vector<Process> process_list;        ///< Connected publisher/subscriber processes.
  };

  /**
   * @struct UrlMeta
   * @brief Pairs a topic URL with its serialisation type and node role.
   *
   * @details
   * Carried in @c Control::url_meta_list to tell the server which topics to
   * subscribe to or publish on.  @c type describes the proxy route direction; for
   * direct field relays, setter/getter peers may be mapped to the matching field
   * reader/writer semantics internally.
   */
  struct UrlMeta final {
    std::string url;                          ///< Full topic URL.
    std::string ser;                          ///< Required serialisation type on this proxy route.
    SchemaType schema{SchemaType::kUnknown};  ///< Required coarse schema family on this proxy route.
    ImplType type{kSubscriber};               ///< Whether the server should act as publisher or subscriber here.
  };

  /**
   * @struct Control
   * @brief Control message sent from a @c kController instance to @c ProxyServer.
   *
   * @details
   * Sets the server's operating mode and the list of topics to observe, record, or
   * inject/play.  @c filter_str is a space- or comma-separated set of substrings; entries
   * must contain at least one substring to be included.  When @c filter_by_process
   * is @c true, the filter is matched against process names instead of URLs.
   */
  struct Control final {
    Mode mode{kOffline};                 ///< Target operation mode.
    std::vector<UrlMeta> url_meta_list;  ///< Topics to observe / record / inject (mode-dependent).
    bool filter_by_process{false};       ///< When true, @c filter_str matches process names; otherwise URLs.
    std::string filter_str;              ///< Space- or comma-separated filter keywords (case-insensitive).
    uint32_t filter_type{0};             ///< Type filter: 0=all, 1=pub+sub pair, 2=srv+cli pair, etc.
  };

  /**
   * @struct Data
   * @brief Raw message payload delivered via @c DataCallback or sent via @c send_data().
   *
   * @details
   * In non-direct observe/record modes the server relays raw serialised bytes plus
   * routing metadata.  In direct mode the callback is fed by the local direct
   * subscriber created by @c ProxyAPI.  When sending data, @c timestamp and @c seq
   * are caller-defined and forwarded verbatim.
   */
  struct Data final {
    std::string url;                          ///< Topic URL the data was captured on.
    std::string ser;                          ///< Serialisation type of the payload.
    SchemaType schema{SchemaType::kUnknown};  ///< Coarse schema family of the payload.
    Bytes raw;                                ///< Raw serialised message bytes.
    int64_t timestamp{-1};                    ///< Sender/session timestamp in microseconds; @c -1 if unset.
    int64_t seq{0};                           ///< Sender or relay sequence number for the URL.
  };

  /**
   * @struct Config
   * @brief Construction-time configuration aggregate for @c ProxyAPI.
   *
   * @details
   * Every field must be set consistently with the @c ProxyServer::Config the
   * instance connects to.  A mismatch on @c reliable, @c enable_tcp, or @c direct
   * raises the corresponding @c Error code on the first heartbeat received.
   */
  struct Config final {
    Role role{kController};       ///< Role of this client instance.
    int domain_id{0};             ///< DDS domain ID; must match the server's @c domain_id.
    std::string dds_impl{"dds"};  ///< DDS implementation: "dds", "ddsc", "ddsr", etc.
    std::string security_key;     ///< Optional security key; empty selects the default slot.
    bool native{false};           ///< When true, restrict all DDS traffic to 127.0.0.1.
    bool reliable{false};         ///< Use reliable DDS QoS; must match the server.
    bool direct{false};           ///< Use direct SHM channels for data; must match the server.
    bool enable_tcp{false};       ///< Use TCP transport for data channels; must match the server.
    bool match_version{true};     ///< Reject when the server's @c VLINK_VERSION differs from this client.
    std::string allow_ip;         ///< Bind DDS sockets to this IP (empty = any).
    std::string peer_ip;          ///< Unicast peer IP for DDS discovery (empty = multicast).
    int buf_size{0};              ///< Socket send/receive buffer in bytes; 0 = default.
    int mtu_size{0};              ///< DDS MTU size in bytes; 0 = default.
  };

  /**
   * @brief Callback fired when the connection state with @c ProxyServer changes.
   *
   * @details
   * @c connected becomes @c true on the first valid heartbeat and @c false after
   * five seconds of heartbeat silence or when the control channel reports
   * disconnection.
   */
  using ConnectCallback = MoveFunction<void(bool connected)>;

  /**
   * @brief Callback fired on each error-state transition.
   *
   * @details
   * Only invoked when @c Error changes -- for instance from @c kNoError to
   * @c kVersionCompError, or back to @c kNoError.
   */
  using ErrorCallback = MoveFunction<void(Error error)>;

  /**
   * @brief Callback delivering the server's wall-clock and boot-time from each heartbeat.
   *
   * @param sys_time   Server system time in microseconds since the Unix epoch.
   * @param boot_time  Server uptime in microseconds since boot.
   */
  using TimeCallback = MoveFunction<void(uint64_t sys_time, uint64_t boot_time)>;

  /**
   * @brief Callback delivering the per-topic statistics list once per second.
   *
   * @param info_list  Filtered discovery records relevant to the current mode,
   *                   including their status fields.
   */
  using InfoCallback = MoveFunction<void(const std::vector<Info>& info_list)>;

  /**
   * @brief Callback delivering raw message data from the active proxy data path.
   *
   * @details
   * Fires for every message forwarded in observe, record, or auto modes.  In
   * non-direct mode the bytes are relayed by @c ProxyServer; in direct mode they
   * come from local direct subscribers.  @c Data::raw is shallow-borrowed -- copy
   * it if you must retain it past the callback.
   */
  using DataCallback = MoveFunction<void(const Data& data)>;

  /**
   * @brief Constructs a @c ProxyAPI using the supplied configuration.
   *
   * @details
   * Stores @p config, starts the internal heartbeat timer, and derives a unique
   * @c control_id from the CPU timestamp so the server can distinguish concurrent
   * controllers.  Transport handles are created by @c reset_handle(), which runs
   * when the inherited @c MessageLoop starts and again whenever the reconnect logic
   * resets the connection.  A controller also caches and posts an initial @c kAuto
   * @c Control, but it is only published once the loop, handshake, and transport
   * handles are all ready.
   *
   * @param config  Configuration for the proxy connection.
   */
  explicit ProxyAPI(const Config& config);

  /**
   * @brief Destroys the @c ProxyAPI and releases every transport handle.
   *
   * @details
   * Quits the inherited @c MessageLoop (if running), waits for it to stop, then
   * releases DDS/SHM handles.  For controller instances, normal @c MessageLoop
   * shutdown publishes @c kOffline from @c on_end().
   */
  ~ProxyAPI() override;

  /**
   * @brief Registers a callback for connection-state changes.
   *
   * @details
   * If the API is already connected when the registration runs, the callback is
   * invoked immediately with @c connected=true inside this call.  Callbacks are
   * replaced atomically; only one callback is active at a time.
   *
   * @param callback  Callable with signature @c void(bool connected).
   */
  void register_connect_callback(ConnectCallback&& callback);

  /**
   * @brief Registers a callback for error-state transitions.
   *
   * @details
   * If a non-zero error is already active at registration time, the callback fires
   * immediately with the current error inside this call.
   *
   * @param callback  Callable with signature @c void(Error error).
   */
  void register_error_callback(ErrorCallback&& callback);

  /**
   * @brief Registers a callback for heartbeat timestamp delivery.
   *
   * @details
   * If timestamps have already been received (timers active) the callback is
   * invoked immediately with the latest extrapolated values inside this call.
   *
   * @param callback  Callable with signature @c void(uint64_t sys_time, uint64_t boot_time).
   */
  void register_time_callback(TimeCallback&& callback);

  /**
   * @brief Registers a callback for per-topic statistics updates.
   *
   * @details
   * Invoked once per second with the full @c Info list from the server.  There is
   * no immediate invocation at registration; data arrives on the next server
   * broadcast cycle.
   *
   * @param callback  Callable with signature @c void(const std::vector<Info>& info_list).
   */
  void register_info_callback(InfoCallback&& callback);

  /**
   * @brief Registers a callback for raw message data relayed by the server.
   *
   * @details
   * There is no immediate invocation at registration time.  Data arrives when the
   * server is in a mode that forwards messages: @c kObserveOne, @c kObserveAll,
   * @c kRecord, @c kAuto, or @c kAutoAndObserveAll.
   *
   * @param callback  Callable with signature @c void(const Data& data).
   */
  void register_data_callback(DataCallback&& callback);

  /**
   * @brief Publishes a @c Control message to the @c ProxyServer.
   *
   * @details
   * Valid only for the @c kController role; returns @c false immediately for
   * @c kListener.  The control is cached internally and automatically re-sent when
   * the server reconnects after a dropout.
   *
   * When @p async is @c true (default) the DDS @c Control publish is posted to the
   * @c MessageLoop thread and the return value reports only whether posting was
   * accepted.  When @p async is @c false the publish runs synchronously on the
   * calling thread and the return value reports the publish result.  In direct
   * mode, direct-map synchronisation is still queued on the @c MessageLoop, so the
   * return value also depends on whether that enqueue was accepted.
   *
   * Entries in @c control.url_meta_list are encoded using their supplied @c ser and
   * @c schema verbatim.  Proxy no longer back-fills missing routing metadata from
   * discovery caches: entries with an empty @c ser or unknown @c schema are ignored
   * by the direct-map sync path and cannot become usable server-side routes.
   *
   * In @c direct mode, matching local SHM publishers are created or destroyed to
   * mirror the publisher entries.  Direct subscribers are synchronised either from
   * subscriber entries in @c url_meta_list or, for @c kObserveAll and
   * @c kAutoAndObserveAll, from the latest @c Info list reported by the server.
   * Setter endpoints are observed with getter semantics so field updates keep
   * their last-value behaviour.
   *
   * @param control  Control message to send.
   * @param async    @c true to post asynchronously (default); @c false to block.
   * @return         When @p async=false, @c true means the DDS publish succeeded and
   *                 any direct-map sync was queued.  When @p async=true, @c true
   *                 means the publish task was accepted.  @c false on listener role,
   *                 shutdown, or enqueue/publish failure.
   *
   * @note Thread-safe.  The control is also stored as the last-known control for
   *       automatic resend on reconnect.
   */
  bool send_control(const Control& control, bool async = true);

  /**
   * @brief Injects raw message data into the proxy data path.
   *
   * @details
   * Valid only for the @c kController role; returns @c false for @c kListener.  In
   * @c direct mode the data is published through the local SHM publisher matching
   * @c data.url and @c ProxyServer is not on the data relay path.  In non-direct
   * mode the payload is wrapped in a @c ProxyData envelope and forwarded over the
   * DDS data channel.  Returns @c false if no subscribers are listening on the
   * target channel.
   *
   * The caller must provide both @c data.ser and a known @c data.schema.  Proxy no
   * longer guesses the decode stack from cached discovery metadata.
   *
   * @param data  Data to inject: URL, serialisation type, schema family, raw bytes,
   *              timestamp, and sequence number.
   * @return      In non-direct mode, @c true only if the underlying data publish
   *              succeeds.  In direct mode, @c true means a matching local direct
   *              publisher and subscriber were found, metadata matched, and the
   *              publish attempt was made (backend result is not surfaced).
   *              @c false otherwise.
   */
  bool send_data(const Data& data);

  /**
   * @brief Returns the configuration supplied at construction.
   *
   * @return Const reference to the internal @c Config aggregate.
   */
  [[nodiscard]] const Config& get_current_config() const;

  /**
   * @brief Returns the most recently requested proxy operation mode.
   *
   * @details
   * Reflects the mode most recently set through @c send_control().  Updated on the
   * calling thread before any async DDS publish.
   *
   * @return Current @c Mode value.
   */
  [[nodiscard]] Mode get_current_mode() const;

  /**
   * @brief Returns the current error state.
   *
   * @return Current @c Error value; @c kNoError when no error is active.
   */
  [[nodiscard]] Error get_current_error() const;

  /**
   * @brief Returns the hostname of the connected @c ProxyServer.
   *
   * @details
   * With handshake enabled, initialised from the successful handshake response and
   * cross-checked against @c Time heartbeats.  Without handshake, populated from
   * the first valid heartbeat.  Empty before any server identity is accepted and
   * after disconnection.
   *
   * @return Server hostname, or empty when unavailable.
   */
  [[nodiscard]] std::string get_current_hostname() const;

  /**
   * @brief Returns the machine ID of the connected @c ProxyServer.
   *
   * @details
   * With handshake enabled, initialised from the successful handshake response and
   * cross-checked against @c Time heartbeats.  Without handshake, populated from
   * the first valid heartbeat.  Empty before any server identity is accepted and
   * after disconnection.
   *
   * @return Server machine ID, or empty when unavailable.
   */
  [[nodiscard]] std::string get_current_machine_id() const;

  /**
   * @brief Returns the best estimate of the server's current wall-clock time.
   *
   * @details
   * Computed as the last received @c sys_time plus the elapsed microseconds since
   * that heartbeat, extrapolated via an @c ElapsedTimer.  Returns the raw
   * @c sys_time field when the timer is not yet running.
   *
   * @return Estimated server system time in microseconds since the Unix epoch.
   */
  [[nodiscard]] uint64_t get_current_sys_time() const;

  /**
   * @brief Returns the best estimate of the server's current boot time.
   *
   * @details
   * Computed as the last received @c boot_time plus the elapsed microseconds since
   * that heartbeat, extrapolated via an @c ElapsedTimer.
   *
   * @return Estimated server uptime in microseconds since boot.
   */
  [[nodiscard]] uint64_t get_current_boot_time() const;

  /**
   * @brief Returns the server's most recently reported CPU utilisation.
   *
   * @return CPU usage percentage in the range [0, 100]; @c 0 when disconnected.
   */
  [[nodiscard]] double get_current_cpu_usage() const;

  /**
   * @brief Returns the server's most recently reported memory utilisation.
   *
   * @return Memory usage percentage in the range [0, 100]; @c 0 when disconnected.
   */
  [[nodiscard]] double get_current_memory_usage() const;

  /**
   * @brief Returns the latest backend-reported latency on the data channel.
   *
   * @details
   * In direct (SHM) mode, or before the DDS data subscriber is initialised, this
   * returns @c 0.  Otherwise it delegates to the underlying data subscriber's
   * latency tracker.  Main backends report one-way/end-to-end data latency in
   * nanoseconds.
   *
   * @return Latency in nanoseconds, or @c 0 when unavailable.
   */
  [[nodiscard]] int64_t get_latency() const;

  /**
   * @brief Returns the sample-loss statistics on the data channel.
   *
   * @details
   * In direct (SHM) mode, or before the DDS data subscriber is initialised, this
   * returns a default-constructed (zero) @c SampleLostInfo.  Otherwise it
   * delegates to the underlying data subscriber.
   *
   * @return @c SampleLostInfo holding total and lost sample counters.
   */
  [[nodiscard]] SampleLostInfo get_lost() const;

  /**
   * @brief Returns whether a valid connection to @c ProxyServer exists.
   *
   * @details
   * @c true once the first valid heartbeat arrives; @c false after five seconds
   * without a heartbeat or when the control channel reports disconnection.
   *
   * @return @c true if connected, @c false otherwise.
   */
  [[nodiscard]] bool is_connected() const;

  /**
   * @brief Returns the @c VLINK_VERSION string reported by the server.
   *
   * @details
   * With handshake enabled, initialised from the successful handshake response and
   * subsequently refreshed from @c Time metadata after token/control checks.
   * Without handshake, populated from the first valid heartbeat.  Empty before any
   * server version is accepted and after a handle reset.
   *
   * @return Server VLink version string, e.g. @c "2.0.0".
   */
  [[nodiscard]] std::string get_proxy_version() const;

  /**
   * @brief Returns every server hostname observed during this session.
   *
   * @details
   * Hostnames are accumulated for the lifetime of the connection.  They are NOT
   * cleared on disconnect, only when @c reset_handle() runs.
   *
   * @return Unordered set of observed hostnames.
   */
  [[nodiscard]] std::unordered_set<std::string> get_proxy_hostnames() const;

  /**
   * @brief Returns every server machine ID observed during this session.
   *
   * @details
   * Analogous to @c get_proxy_hostnames(); accumulated for the lifetime of the
   * connection.
   *
   * @return Unordered set of observed machine IDs.
   */
  [[nodiscard]] std::unordered_set<std::string> get_proxy_machine_ids() const;

  /**
   * @brief Returns @c true when the build includes SHM (Iceoryx) support.
   *
   * @details
   * Decided at compile time by @c VLINK_SUPPORT_SHM.  Useful to gate
   * @c Config::direct.
   *
   * @return @c true when SHM support is compiled in.
   */
  [[nodiscard]] static bool is_support_shm();

  /**
   * @brief Returns @c true when topic filtering support is compiled in.
   *
   * @details
   * Decided at compile time by @c VLINK_PROXY_ENABLE_FILTER.  When @c false the
   * @c filter_str and @c filter_by_process fields in @c Control are ignored by the
   * server.
   *
   * @return @c true when filtering is enabled.
   */
  [[nodiscard]] static bool is_enable_filter();

  /**
   * @brief Formats a microsecond wall-clock timestamp as a human-readable string.
   *
   * @details
   * Output format: @c "YYYY/MM/DD HH:MM:SS:mmm" where @c mmm is milliseconds.
   * Uses @c localtime_r by default; @c gmtime_r when @p enable_utc is @c true.
   *
   * @param time        Microseconds since the Unix epoch.
   * @param enable_utc  @c true for UTC; @c false for local time (default).
   * @return            Formatted timestamp string, or empty on conversion error.
   */
  [[nodiscard]] static std::string get_format_sys_time(uint64_t time, bool enable_utc = false);

  /**
   * @brief Formats a microsecond boot-time duration as a human-readable string.
   *
   * @details
   * Converts @p time (microseconds since boot) into a formatted elapsed string,
   * e.g. @c "0d 01:23:45.678".
   *
   * @param time  Microseconds since boot.
   * @return      Formatted elapsed-time string.
   */
  [[nodiscard]] static std::string get_format_boot_time(uint64_t time);

 protected:
  size_t get_max_task_count() const override;

  uint32_t get_max_elapsed_time() const override;

  void on_begin() override;

  void on_end() override;

 private:
  bool send_control_sync(const Control& control);

  void sync_direct_maps(const Control& control);

  bool do_handshake(Error& out_err);

  void reset_handle();

  void process_connected(bool connected);

  void process_time(uint64_t sys_time, uint64_t boot_time);

  void process_error(Error error);

  struct Impl;
  std::unique_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(ProxyAPI)
};

}  // namespace vlink
