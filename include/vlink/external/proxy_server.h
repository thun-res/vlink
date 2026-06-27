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
 * @file proxy_server.h
 * @brief Daemon-side half of the VLink proxy subsystem -- one server per process.
 *
 * @details
 * @c ProxyServer is the in-process daemon that mediates between the VLink core (publishers,
 * subscribers, setters, getters on the local DDS domain) and one or more @c ProxyAPI clients.
 * It derives from @c MessageLoop, so once @c async_run() is called all scheduled work,
 * timers, and asynchronous relays run inside the inherited loop's worker thread.
 *
 * Behaviourally the server is responsible for the following duties:
 *
 * -# Hosts an internal @c DiscoveryViewer that enumerates every active publisher and
 *    subscriber on the DDS domain.
 * -# Accepts @c Control messages from @c ProxyAPI clients over a security-authenticated
 *    DDS channel and dispatches them to the discovery layer.
 * -# Broadcasts a 1-second @c Time heartbeat carrying CPU/memory usage, the VLink version
 *    string, hostname, machine ID, and wall-clock plus boot-time.
 * -# Publishes per-topic statistics -- @c freq, @c rate, @c loss, @c latency -- once per
 *    second through a security-authenticated @c InfoList channel.
 * -# Relays raw payload bytes from observed publishers and setters back to connected
 *    @c ProxyAPI listeners.  In direct mode the data pubs/subs are created by the API
 *    side and the server stays exclusively on the control/discovery path; setter
 *    endpoints are observed with getter semantics so field last-value delivery is
 *    preserved.
 * -# Optionally launches an embedded Iceoryx RouDi daemon when @c Config::use_iox is
 *    @c true.
 * -# Loads @c RunablePluginInterface shared-library plugins listed in
 *    @c Config::runnable_list at construction and drives their lifecycle from the
 *    inherited @c MessageLoop callbacks.
 *
 * @par Singleton Constraint
 * Only one @c ProxyServer may exist per operating-system process.  A second
 * construction logs a fatal message and throws before any DDS channels are wired up;
 * running multiple servers in the same process is not supported and not safe.
 *
 * @par Architecture Diagram
 * @code
 *  +-----------------------+              +---------------------+
 *  | ProxyAPI (Controller) |              | ProxyAPI (Listener) |
 *  +-----------+-----------+              +----------+----------+
 *              |                                     |
 *              |  HandshakeCli  ---[DDS secure]---> HandshakeSrv (issues token)
 *              |<--------------- token --------------|
 *              |  ControlPub    ---[DDS secure]---> ControlSub  (token-stamped)
 *              v                                     v
 *  +-------------------------------------------------------------+
 *  |                       ProxyServer (this)                    |
 *  |  +-------------------+   +-------------------------------+  |
 *  |  | DiscoveryViewer   |   | Heartbeat / Info / Data path |   |
 *  |  +-------------------+   +-------------------------------+  |
 *  |       ^                          |          |               |
 *  +-------|--------------------------|----------|---------------+
 *          |                          v          v
 *     [VLink core nodes]         TimePub /   InfoPub  ---[DDS secure]--->
 *                                DataPub               ---[DDS / SHM]--->
 * @endcode
 *
 * @par Authentication Token
 * At construction the server generates a 128-bit hex session token via
 * @c vlink::Uuid::random_hex() and exposes it through @c get_token().  The token is a
 * process-lifetime protocol nonce, @b not a long-term cryptographic key.  Clients fetch
 * it by invoking the handshake RPC over the security-authenticated DDS channel;
 * subsequent @c Control messages without a matching token are dropped server-side.
 * Restarting the server invalidates every previously-issued token.  Clients re-handshake
 * after a same-identity token mismatch heartbeat, an identity-mismatch heartbeat that
 * also carries a different token, or a local reset, before they will accept further
 * heartbeats or publish new controls.
 *
 * @par Runnable Plugin Lifecycle
 * Plugins named in @c Config::runnable_list are loaded inside the constructor.  When
 * the inherited @c MessageLoop starts (@c on_begin), each plugin's @c on_init() and
 * @c async_run() are invoked in list order.  On loop stop (@c on_end) each plugin's
 * @c on_deinit(), @c quit(), and @c wait_for_quit() are invoked in the same order.
 *
 * @par Environment Variables
 * | Variable             | Meaning                                                       |
 * | -------------------- | ------------------------------------------------------------- |
 * | @c VLINK_INTRA_BIND  | When set to any value, also subscribe to @c intra:// topics.  |
 *
 * @par Example
 * @code
 * vlink::ProxyServer::Config cfg;
 * cfg.dds_impl  = "dds";
 * cfg.domain_id = 0;
 * cfg.reliable  = false;
 * cfg.async     = true;
 * cfg.use_iox   = false;
 *
 * vlink::ProxyServer server(cfg);
 * server.async_run();          // start the inherited MessageLoop in a background thread
 * // ... application runs ...
 * server.quit(true);
 * server.wait_for_quit();
 * @endcode
 *
 * @note
 * - Construction must happen on the main thread, before any @c ProxyAPI client connects
 *   on the same domain.
 * - Destruction stops every timer, joins the @c DiscoveryViewer, and releases each DDS
 *   handle in a deterministic order so no dangling callback can fire after teardown.
 */

#pragma once

#undef VLINK_PROXY_SERVER_EXPORT
#ifdef VLINK_PROXY_SERVER_LIBRARY_STATIC
#define VLINK_PROXY_SERVER_EXPORT
#elif defined(_WIN32) || defined(__CYGWIN__)
#ifdef VLINK_PROXY_SERVER_LIBRARY
#define VLINK_PROXY_SERVER_EXPORT __declspec(dllexport)
#else
#define VLINK_PROXY_SERVER_EXPORT __declspec(dllimport)
#endif
#else
#define VLINK_PROXY_SERVER_EXPORT __attribute__((visibility("default")))
#endif

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../base/message_loop.h"

namespace vlink {

/**
 * @class ProxyServer
 * @brief In-process VLink proxy daemon backed by a @c MessageLoop.
 *
 * @details
 * Owns the discovery layer, the handshake/control/time/info DDS channels, the data
 * relay path, and any embedded Iceoryx daemon or runnable plugins.  Construction
 * starts the @c DiscoveryViewer plus its 1-second heartbeat and statistics timers,
 * but it does @b not start the server's own inherited loop.  Call @c async_run()
 * (or @c run()) on the @c MessageLoop base to enable asynchronous relays and the
 * runnable-plugin lifecycle hooks.  Only one instance is allowed per process.
 */
class VLINK_PROXY_SERVER_EXPORT ProxyServer : public MessageLoop {
 public:
  /**
   * @struct Config
   * @brief Construction-time configuration aggregate for @c ProxyServer.
   *
   * @details
   * Every @c reliable, @c enable_tcp, and @c direct flag is echoed inside each @c Time
   * heartbeat, so connecting @c ProxyAPI clients can verify wire-compatibility and
   * refuse to attach when their own configuration would deviate.
   *
   * | Field                      | Default | Description                                                       |
   * | -------------------------- | ------- | ----------------------------------------------------------------- |
   * | @c async                   | false   | Forward data on the MessageLoop thread; false = inline relay.     |
   * | @c reliable                | false   | Use reliable DDS QoS for data channels.                           |
   * | @c enable_tcp              | false   | Use TCP transport for data channels.                              |
   * | @c direct                  | false   | Use SHM (Iceoryx) instead of DDS for data forwarding.             |
   * | @c native_mode             | false   | Restrict all DDS traffic to 127.0.0.1 (loopback).                 |
   * | @c domain_id               | 0       | DDS domain ID shared with all clients.                            |
   * | @c buf_size                | 0       | DDS socket send/receive buffer in bytes; 0 = built-in default.    |
   * | @c mtu_size                | 0       | DDS MTU size in bytes; 0 = built-in default.                      |
   * | @c max_packet_size         | 0       | Maximum relayed payload in MiB; see note below.                   |
   * | @c security_key            | ""      | Security key for control channels; empty = default slot.          |
   * | @c bind_ip                 | ""      | Bind DDS sockets to this IP; empty = any interface.               |
   * | @c peer_ip                 | ""      | Unicast peer IP for discovery; empty = multicast.                 |
   * | @c dds_impl                | "dds"   | DDS implementation: "dds", "ddsc", "ddsr", etc.                   |
   * | @c use_iox                 | false   | Launch an embedded Iceoryx RouDi daemon at startup.               |
   * | @c iox_monitoring          | true    | Enable Iceoryx introspection/monitoring.                          |
   * | @c iox_strategy            | 1       | Iceoryx memory strategy index passed to @c ShmConf::init_roudi(). |
   * | @c iox_config              | ""      | Path to a custom Iceoryx TOML; empty = default.                   |
   * | @c runnable_version_major  | 1       | Required major ABI version for runnable plugins.                  |
   * | @c runnable_version_minor  | 0       | Required minor ABI version for runnable plugins.                  |
   * | @c runnable_prefix         | ""      | Library filename prefix for plugin discovery.                     |
   * | @c runnable_list           | {}      | Ordered names of @c RunablePluginInterface plugins to load.       |
   *
   * @note
   * @c max_packet_size is interpreted in MiB.  The default value @c 0 drops every
   * non-empty message -- there is no special case in the implementation.  Set it to
   * a positive number to forward larger packets.  The @c vlink-proxy CLI defaults
   * this field to @c 4.0.
   */
  struct Config final {
    bool async{false};        ///< Forward data asynchronously on the MessageLoop thread.
    bool reliable{false};     ///< Use reliable DDS QoS; must match every client.
    bool enable_tcp{false};   ///< Use TCP transport for DDS data channels.
    bool direct{false};       ///< Use ProxyAPI-managed local SHM channels for data.
    bool native_mode{false};  ///< Restrict every DDS endpoint to loopback (127.0.0.1).
    int domain_id{0};         ///< DDS domain ID.
    uint32_t buf_size{0};     ///< DDS socket buffer in bytes; 0 = default.
    uint32_t mtu_size{0};     ///< DDS fragment MTU in bytes; 0 = default.
    double max_packet_size{
        0};  ///< Maximum relayed payload in MiB; 0 drops every non-empty message (set > 0 to forward).
    std::string security_key;                ///< Security key; empty = default security slot.
    std::string bind_ip;                     ///< Local IP for DDS sockets; empty = any.
    std::string peer_ip;                     ///< Peer unicast IP for DDS; empty = multicast.
    std::string dds_impl{"dds"};             ///< DDS implementation transport identifier.
    bool use_iox{false};                     ///< Launch an embedded Iceoryx RouDi.
    bool iox_monitoring{true};               ///< Enable Iceoryx introspection.
    int iox_strategy{1};                     ///< Iceoryx memory allocation strategy index.
    std::string iox_config;                  ///< Iceoryx TOML config path; empty = default.
    uint16_t runnable_version_major{1};      ///< Required major ABI version for plugins.
    uint16_t runnable_version_minor{0};      ///< Required minor ABI version for plugins.
    std::string runnable_prefix;             ///< Plugin library filename prefix.
    std::vector<std::string> runnable_list;  ///< Ordered plugin names to load on startup.
  };

  /**
   * @brief Constructs a @c ProxyServer and brings every proxy subsystem online.
   *
   * @details
   * The constructor performs the following steps in order:
   *
   * -# Acquires the process-global singleton guard; on contention it logs a fatal
   *    message and throws before touching any DDS handle.
   * -# Reads the @c VLINK_INTRA_BIND environment variable.
   * -# When @c config.use_iox is @c true, calls @c init_shm_roudi() to spin up an
   *    embedded Iceoryx RouDi process.
   * -# Calls @c init_server() to create the handshake, control, time, info, and data
   *    channels, subscribe to @c Control, and start the heartbeat plus statistics
   *    timers on the @c DiscoveryViewer's loop.
   * -# Calls @c init_runnable() to load every plugin listed in @c config.runnable_list.
   *
   * The inherited @c MessageLoop is @b not started here -- call @c async_run() or
   * @c run() explicitly when asynchronous relays and plugin lifecycle hooks must run.
   *
   * @param config  Server configuration.  See @c Config for per-field semantics.
   *
   * @note A second @c ProxyServer in the same process is unsupported and the
   *       constructor throws.
   */
  explicit ProxyServer(const Config& config);

  /**
   * @brief Destroys the @c ProxyServer in a deterministic shutdown order.
   *
   * @details
   * Requests the inherited @c MessageLoop to stop, waits for it, then stops the proxy
   * timers, joins the @c DiscoveryViewer, and releases each DDS/SHM handle.  When the
   * loop has been started, runnable plugins receive @c on_deinit(), @c quit(), and
   * @c wait_for_quit() from @c on_end() before the destructor drops them.  The
   * process-global singleton guard remains set for the rest of the process lifetime.
   */
  ~ProxyServer() override;

  /**
   * @brief Returns the authentication token issued by this server.
   *
   * @details
   * When @c VLINK_PROXY_ENABLE_HANDSHAKE is non-zero (the default), the token is
   * generated once at construction via @c vlink::Uuid::random_hex() and remains
   * constant for the server's lifetime.  Clients learn it through the
   * security-authenticated handshake RPC.  The server then validates the token on
   * every inbound @c Control and echoes it (alongside the server identity) inside
   * every @c Time heartbeat so clients can detect both server restarts and identity
   * mismatches.  When the macro is @c 0, the token is empty, validation is disabled,
   * and the handshake channel is not created.
   *
   * @return Hex-encoded token string, or empty when handshake is compiled out.
   *
   * @note Thread-safe; the returned string is a copy.
   */
  [[nodiscard]] std::string get_token() const;

 protected:
  size_t get_max_task_count() const override;

  uint32_t get_max_elapsed_time() const override;

  void on_begin() override;

  void on_end() override;

 private:
  void init_shm_roudi();

  void init_server();

  void init_runnable();

  void send_time();

  void send_control(const void* control_data);

  void update_all();

  struct Impl;
  std::unique_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(ProxyServer)
};

}  // namespace vlink
