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
 * @file node_impl.h
 * @brief Foundational base classes shared by every transport-backed VLink node.
 *
 * @details
 * This is an internal implementation header used by the public node templates
 * (@c Publisher, @c Subscriber, @c Client, @c Server, @c Setter, @c Getter)
 * and by every transport backend; it is not part of the public API surface.
 * The header introduces two cooperating base classes:
 *
 * - @c AbstractNode -- exposes a single @c get_native_handle() escape hatch so
 *   advanced users can reach the transport-native object behind a node.
 * - @c NodeImpl -- the heavyweight base that every @c PublisherImpl,
 *   @c SubscriberImpl, @c ServerImpl, @c ClientImpl, @c SetterImpl and
 *   @c GetterImpl ultimately derive from.  It centralises lifecycle hooks,
 *   property storage, message-loop attachment, interruption signalling and the
 *   ancillary observability paths used by discovery, recording and proxying.
 *
 * @par NodeImpl interface contract
 * | Concern                | API surface                                                   |
 * | ---------------------- | ------------------------------------------------------------- |
 * | Transport lifecycle    | @c init() / @c deinit()                                       |
 * | Zero-copy loans        | @c loan() / @c return_loan() / @c set_manual_unloan           |
 * | Suspend / resume       | @c suspend() / @c resume() / @c is_suspend()                  |
 * | Interrupt              | @c interrupt() / @c reset_interrupted() / @c is_interrupted   |
 * | Property store         | @c set_property() / @c get_property() / @c get_all_properties |
 * | Message loop binding   | @c attach() / @c detach() / @c get_message_loop()             |
 * | Status callbacks       | @c register_status_handler() / @c call_status()               |
 * | Recording              | @c set_record_path() / @c try_record()                        |
 * | Extension / security   | @c init_ext() / @c deinit_ext() / @c enable_security()        |
 * | Version probe          | @c check_version()                                            |
 * | Process bootstrap      | static @c global_init()                                       |
 *
 * @par Lifecycle
 * @code
 *      construct -> set_property -> enable_security -> init -> init_ext
 *                                                       ^         ^
 *                                                       |         |
 *                                       attach(loop) ---+         | active
 *                                                                 |
 *                                       interrupt -> reset_interrupted (optional)
 *                                                                 |
 *                                                                 v
 *                                                deinit_ext -> deinit -> destroy
 * @endcode
 *
 * @par ImplType / SecurityType / InitType
 * - @c ImplType (defined in @c types.h) is captured at construction time and
 *   exposes the node role to discovery / proxy.
 * - @c SecurityType is the template parameter on the public node templates that
 *   selects whether @c enable_security() runs.
 * - @c InitType (also from @c types.h) controls whether the public template
 *   calls @c init() immediately or defers it until the user invokes @c init()
 *   manually.
 *
 * @par Callback signatures
 * | Alias                | Signature                                | Used by                          |
 * | -------------------- | ---------------------------------------- | -------------------------------- |
 * | @c ConnectCallback   | @c void(bool)                            | Publisher/Client/Server peers    |
 * | @c StatusCallback    | @c void(const Status::BasePtr&)          | DDS-family status reporting      |
 * | @c SyncCallback      | @c void()                                | SetterImpl late-getter sync      |
 * | @c ReqRespCallback   | @c void(uint64_t, const Bytes&, Bytes*)  | ServerImpl request handling      |
 * | @c MsgCallback       | @c void(const Bytes&)                    | SubscriberImpl / GetterImpl      |
 * | @c IntraMsgCallback  | @c void(const IntraData&)                | intra:// subscriber              |
 */

#pragma once

#include <any>
#include <atomic>
#include <memory>
#include <string>

#include "../base/bytes.h"
#include "../base/cpu_profiler.h"
#include "../base/functional.h"
#include "../extension/security.h"
#include "../extension/status.h"
#include "../impl/conf.h"
#include "../impl/types.h"
#include "./intra_data.h"
#include "./ssl_options.h"

namespace vlink {

/**
 * @class AbstractNode
 * @brief Tiny mix-in that exposes the backend-native handle behind a node.
 *
 * @details
 * Transport backends commonly inherit from both @c AbstractNode and the matching
 * @c NodeImpl subclass.  @c get_native_handle() returns an @c std::any wrapping
 * the native object (for instance a DDS @c DataWriter pointer) so advanced users
 * can interact with backend internals without leaking transport types through
 * the rest of the VLink API.
 *
 * @note The base implementation returns an @c std::any holding @c nullptr; the
 *       caller must @c std::any_cast to the expected backend type.
 */
class VLINK_EXPORT AbstractNode {
 public:
  /**
   * @brief Returns the backend-native handle wrapped in an @c std::any.
   *
   * @details
   * Backends override this method to expose, for example, a DDS @c DataWriter
   * or @c DataReader pointer.  Callers must @c std::any_cast the result.
   *
   * @return Backend handle, or @c nullptr-bearing @c std::any in the base.
   */
  virtual std::any get_native_handle() const;

 protected:
  AbstractNode();
  virtual ~AbstractNode();

 private:
  VLINK_DISALLOW_COPY_AND_ASSIGN(AbstractNode)
};

/**
 * @class NodeImpl
 * @brief Backbone of every transport-specific VLink node implementation.
 *
 * @details
 * Every concrete transport backend ultimately derives from @c NodeImpl.  The
 * class centralises lifecycle, instrumentation and observability so that
 * subclass authors only need to focus on transport mechanics.  The contract
 * table at file scope summarises which knobs the base owns and which require
 * a transport override.
 *
 * @note @c suspend() and @c resume() are optional capabilities; the base
 *       implementation logs a warning and returns @c false so that backends
 *       lacking the operation can be detected gracefully.
 *
 * @note @c register_status_handler() and @c call_status() are only meaningful
 *       on DDS-family transports (@c kDds, @c kDdsc, @c kDdsr, @c kDdst);
 *       other backends log a warning and treat them as no-ops.
 */
class VLINK_EXPORT NodeImpl {
 public:
  /**
   * @brief Callback fired when peer presence changes.
   *
   * The boolean argument is @c true when the peer becomes reachable and
   * @c false when it disappears.
   */
  using ConnectCallback = Function<void(bool)>;

  /**
   * @brief Callback fired on DDS-family status notifications (e.g. deadline missed).
   *
   * @param ptr Polymorphic status object; downcast to the concrete subtype.
   */
  using StatusCallback = Function<void(const Status::BasePtr& ptr)>;

  /**
   * @brief Callback fired when a @c SetterImpl completes a late-getter sync.
   */
  using SyncCallback = Function<void()>;

  /**
   * @brief Request / response handler installed by @c ServerImpl::listen().
   *
   * @details
   * Parameters are @c (req_id, request_bytes, response_bytes_ptr).  The handler
   * writes the response into @c *response_bytes_ptr; when the pointer is
   * @c nullptr the server is running in fire-and-forget mode.
   */
  using ReqRespCallback = Function<void(uint64_t, const Bytes&, Bytes*)>;

  /**
   * @brief Callback delivering a serialised payload to a subscriber or getter.
   *
   * @param bytes Payload bytes; lifetime is scoped to the callback.
   */
  using MsgCallback = Function<void(const Bytes&)>;

  /**
   * @brief Callback delivering an in-process payload to an intra:// subscriber.
   *
   * @details
   * @c IntraData is a @c shared_ptr to the payload type, so no copy occurs as
   * the callback is dispatched.
   */
  using IntraMsgCallback = Function<void(const IntraData&)>;

  /**
   * @brief Brings the underlying transport channel online.
   *
   * @details
   * Called by the public Node<> template after properties have been wired in.
   * Backends create their DDS entities, shared-memory channels or any other
   * resources here.
   */
  virtual void init() = 0;

  /**
   * @brief Tears the underlying transport channel down.
   *
   * @details
   * Called by the Node<> template destructor; releases all transport-owned
   * resources.
   */
  virtual void deinit() = 0;

  /**
   * @brief Pauses message delivery without releasing the channel.
   *
   * @details
   * The base implementation logs a warning and returns @c false.  Only a few
   * backends implement true suspension; others should leave the default.
   *
   * @return @c true on success; @c false when the operation is unsupported.
   */
  virtual bool suspend();

  /**
   * @brief Resumes message delivery after @c suspend().
   *
   * @details
   * Default no-op returns @c false.  Override in backends that support
   * suspension.
   *
   * @return @c true on success; @c false otherwise.
   */
  virtual bool resume();

  /**
   * @brief Reports whether the node is currently suspended.
   *
   * @details
   * Default implementation logs a warning and returns @c false.  Override
   * alongside @c suspend() / @c resume() when relevant.
   *
   * @return @c true when the node is paused.
   */
  [[nodiscard]] virtual bool is_suspend() const;

  /**
   * @brief Marks the node interrupted and wakes blocking operations.
   *
   * @details
   * Sets the internal interrupted flag and is overridden by subclasses to
   * signal additional condition variables (e.g. @c wait_for_subscribers()).
   * Pair with @c reset_interrupted() before reusing blocking helpers.
   */
  virtual void interrupt();

  /**
   * @brief Indicates whether zero-copy loaning is available on this backend.
   *
   * @return @c true when @c loan() / @c return_loan() can be used.
   */
  [[nodiscard]] virtual bool is_support_loan() const;

  /**
   * @brief Borrows a write buffer of @p size bytes from the transport.
   *
   * @details
   * Backends that support zero-copy override this method to return a @c Bytes
   * view onto pre-allocated transport memory.  The default returns an empty
   * @c Bytes.
   *
   * @param size  Requested buffer size in bytes.
   * @return Borrowed @c Bytes; empty when loaning is unsupported or failed.
   */
  [[nodiscard]] virtual Bytes loan(int64_t size);

  /**
   * @brief Returns a previously loaned buffer to the transport.
   *
   * @details
   * Must be called whenever @c loan() succeeded but the caller decided not to
   * publish (for example because serialisation failed).
   *
   * @param bytes  Buffer previously obtained via @c loan().
   * @return @c true on success; @c false when loaning is unsupported.
   */
  virtual bool return_loan(const Bytes& bytes);

  /**
   * @brief Toggles automatic versus manual release of received loaned buffers.
   *
   * @details
   * With @p manual_unloan set to @c true the backend does not release loaned
   * buffers after callback dispatch; the application must call @c return_loan()
   * itself.
   *
   * @param manual_unloan  @c true for manual release; @c false for automatic.
   */
  virtual void set_manual_unloan(bool manual_unloan);

  /**
   * @brief Returns the associated transport @c Conf, or @c nullptr in the base.
   *
   * @details
   * Concrete backends override the method to expose their typed @c Conf so the
   * public node templates can resolve transport-specific options.
   *
   * @return Pointer to the owning @c Conf.
   */
  [[nodiscard]] virtual const struct Conf* get_conf() const;

  /**
   * @brief Returns the companion @c AbstractNode, when the impl is split.
   *
   * @return Pointer to the abstract node, or @c nullptr.
   */
  [[nodiscard]] virtual const AbstractNode* get_abstract_node() const;

  /**
   * @brief Retrieves a transport-specific status object.
   *
   * @details
   * Only meaningful on DDS-family transports; the base logs a warning and
   * returns @c Status::Unknown.
   *
   * @param type  Requested status category.
   * @return Polymorphic status object; never @c nullptr.
   */
  [[nodiscard]] virtual Status::BasePtr get_status(Status::Type type) const;

  /**
   * @brief Compares @p version with the runtime VLink library version.
   *
   * @details
   * Logs a warning (once per process) on the first mismatch.  Version checks
   * are advisory and do not block node creation.
   *
   * @param version  Compile-time version constants embedded by the caller.
   * @return @c true when the versions agree.
   */
  virtual bool check_version(const Version& version);

  /**
   * @brief Attaches the node to a @c MessageLoop for callback dispatch.
   *
   * @details
   * Records @p message_loop atomically; subsequent @c call_status() posts onto
   * the loop thread.  When another non-null loop is already attached the call
   * is rejected.  Passing @c nullptr does not establish a usable binding even
   * though it may succeed when the node was already detached.
   *
   * @param message_loop  Loop to bind to.
   * @return @c true when the pointer was stored; @c false on conflict.
   */
  virtual bool attach(class MessageLoop* message_loop);

  /**
   * @brief Detaches the node from its @c MessageLoop.
   *
   * @details
   * Clears the cached pointer and, when called from a thread different from
   * the loop, blocks until the previous loop becomes idle so no callback is
   * still in flight on return.
   *
   * @return @c true on success; @c false if no loop was attached.
   */
  virtual bool detach();

  /**
   * @brief Returns the currently attached @c MessageLoop.
   *
   * @return Loop pointer or @c nullptr when none is bound.
   */
  [[nodiscard]] class MessageLoop* get_message_loop() const;

  /**
   * @brief Convenience downcast wrapper around @c get_conf().
   *
   * @tparam T  Expected concrete @c Conf subclass.
   * @return Typed pointer; @c nullptr when @c get_conf() returns @c nullptr.
   */
  template <typename T>
  [[nodiscard]] const T* get_target_conf() const;

  /**
   * @brief Installs a DDS-family status handler.
   *
   * @details
   * No-op on non-DDS transports.  When a @c MessageLoop is attached the
   * callback is invoked on the loop thread.
   *
   * @param callback  Handler invoked for each status notification.
   */
  void register_status_handler(StatusCallback&& callback);

  /**
   * @brief Reports whether a status handler has been registered.
   *
   * @return @c true when a non-null status handler exists.
   */
  [[nodiscard]] bool has_register_status() const;

  /**
   * @brief Delivers a status notification to the registered handler.
   *
   * @details
   * Posts the call onto the attached @c MessageLoop when one exists; otherwise
   * invokes the handler synchronously.
   *
   * @param ptr  Status object to forward.
   */
  void call_status(Status::BasePtr ptr);

  /**
   * @brief Sets a named property on this node.
   *
   * @details
   * Properties are key / value strings consumed during @c init().  The
   * underlying map is guarded by a shared mutex for thread safety.
   *
   * @param prop   Property key.
   * @param value  Property value.
   */
  void set_property(const std::string& prop, const std::string& value);

  /**
   * @brief Retrieves the value of a named property.
   *
   * @param prop  Property key.
   * @return Value, or empty string when no entry exists.
   */
  [[nodiscard]] std::string get_property(const std::string& prop) const;

  /**
   * @brief Returns a copy of every property currently set on this node.
   *
   * @return Snapshot of the internal @c PropertiesMap.
   */
  [[nodiscard]] Conf::PropertiesMap get_all_properties() const;

  /**
   * @brief Toggles whether this node is reported to the discovery layer.
   *
   * @details
   * Must be called before @c init_ext().  Disabling discovery hides the node
   * from @c DiscoveryReporter / @c DiscoveryViewer; the proxy implementation
   * uses this to keep its internal channels off topology views.
   *
   * @param enable  @c true to report to discovery (default); @c false to hide.
   */
  void set_discovery_enabled(bool enable);

  /**
   * @brief Returns the current discovery-reporting flag.
   *
   * @return @c true when the node will be visible to discovery.
   */
  [[nodiscard]] bool get_discovery_enabled() const;

  /**
   * @brief Configures per-node message recording.
   *
   * @details
   * A non-empty @p path acquires (or shares) a @c BagWriter so that subsequent
   * @c try_record() calls capture frames.  An empty path turns recording off.
   *
   * @param path  File path; empty or unsupported suffix disables recording.
   */
  void set_record_path(const std::string& path);

  /**
   * @brief Installs application-layer security from a const-reference config.
   *
   * @details
   * Performs the wire-metadata validation and AAD-context plumbing, then
   * forwards a fully prepared @c Security::Config into a @c Security instance
   * stored in @c security.  Unsupported transports return @c false without
   * installing anything.
   *
   * @param cfg  Security configuration supplied by the public node wrapper.
   * @return @c true once a usable @c Security instance is installed.
   */
  bool enable_security(const Security::Config& cfg);

  /**
   * @brief Installs application-layer security by consuming @p cfg.
   *
   * @details
   * Same validation path as the const-reference overload, but avoids copying
   * callback targets and key material when the caller owns the config.  The
   * AAD context may be filled before the config is moved into @c Security.
   *
   * @param cfg  Security configuration to consume.
   * @return @c true once a usable @c Security instance is installed.
   */
  bool enable_security(Security::Config&& cfg);

  /**
   * @brief Merges SSL options into the node property map.
   *
   * @details
   * Acquires the helper mutex and delegates to @c SslOptions::parse_to() so
   * the transport factory reads the resulting @c ssl.* entries during
   * connection setup.
   *
   * @param options  SSL / TLS configuration to merge.
   *
   * @see SslOptions::parse_to(), Node::set_ssl_options()
   */
  void set_ssl_options(const SslOptions& options);

  /**
   * @brief Records a message to the global and / or per-node bag writers.
   *
   * @details
   * Skips DDS CDR payloads and (when the ignore-intra filter is on) intra
   * messages.  Used by every node role to feed @c BagWriter pipelines.
   *
   * @param action_type  Logical role under which the message is being recorded.
   * @param data         Raw serialised payload bytes.
   */
  void try_record(ActionType action_type, const Bytes& data);

  /**
   * @brief Clears the interrupted flag set by @c interrupt().
   *
   * @details
   * Required before re-running a blocking helper such as
   * @c wait_for_subscribers() after a prior interruption.
   */
  void reset_interrupted();

  /**
   * @brief Reports whether @c interrupt() has been called and not yet reset.
   *
   * @return @c true when the interrupted flag is set.
   */
  [[nodiscard]] bool is_interrupted() const;

  /**
   * @brief Registers the node with the global discovery reporter.
   *
   * @details
   * Invoked at the end of @c init() by every public Node<> template.  Also
   * starts the per-node @c CpuProfiler when global profiling is enabled.
   * Skips registration for CDR, security, and (by default) intra nodes.
   */
  void init_ext();

  /**
   * @brief Deregisters the node from the global discovery reporter.
   *
   * @details
   * Mirrors @c init_ext(); called after @c deinit() to release the discovery
   * entry and restart the global profiler if it had been paused.
   */
  void deinit_ext();

  /**
   * @brief Initialises process-wide VLink singletons.
   *
   * @details
   * Brings the logger, memory pool, global bag writer and discovery reporter
   * online.  Safe to call multiple times; only the first call has an effect.
   * The base constructor invokes this automatically.
   */
  static void global_init();

  std::string url;                               ///< Full URL string of the node, e.g. @c "dds://my/topic".
  std::string ser_type;                          ///< Concrete serialisation type tag (e.g. @c "demo.proto.PointCloud").
  ImplType impl_type{kUnknownImplType};          ///< Role of this implementation node.
  SchemaType schema_type{SchemaType::kUnknown};  ///< Coarse schema family reported to discovery and bag / proxy paths.
  TransportType transport_type{TransportType::kUnknown};  ///< Transport backend identifier for this node.
  bool is_cdr_type{false};                                ///< @c true when DDS native CDR serialisation is in use.
  bool is_security_type{false};                           ///< @c true when an authenticated transport is enabled.
  bool is_discovery_enabled{true};                        ///< Whether the node is reported to the discovery layer.
  std::atomic_bool has_suspend{false};    ///< Atomic suspend flag (currently unused by the default impls).
  std::unique_ptr<CpuProfiler> profiler;  ///< Optional per-node CPU profiler activated under global profiling.
  std::unique_ptr<Security> security;     ///< Installed per-node message-security context, or @c nullptr.

 protected:
  explicit NodeImpl(ImplType type);

  virtual ~NodeImpl();

 private:
  std::unique_ptr<struct NodeImplHelper> helper_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(NodeImpl)
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

template <typename T>
inline const T* NodeImpl::get_target_conf() const {
  return static_cast<const T*>(get_conf());
}

}  // namespace vlink
