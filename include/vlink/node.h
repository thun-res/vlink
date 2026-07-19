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
 * @file node.h
 * @brief Common CRTP base for every VLink communication primitive.
 *
 * @details
 * @c Node\<ImplT, SecT\> is the polymorphic base class shared by all six
 * VLink communication primitives.  It owns the transport-specific
 * implementation pointer (@c impl_), drives the node lifecycle, and exposes
 * the cross-cutting services that every primitive needs: zero-copy loans,
 * peer-discovery toggling, message-loop attachment, recording, security
 * installation, profiling, and SSL/TLS configuration.
 *
 * @par Class Hierarchy
 * @verbatim
 *   +--------------------------------------------------------------+
 *   |                  User-facing primitives                      |
 *   |  Publisher<T>   Subscriber<T>   Setter<T>    Getter<T>       |
 *   |  Client<Req,Resp>               Server<Req,Resp>             |
 *   +-----------------------------+--------------------------------+
 *                                 | inherits
 *   +-----------------------------v--------------------------------+
 *   |                  Node<ImplT, SecT>                           |
 *   |  lifecycle | loans | properties | profiler | ssl | security  |
 *   +-----------------------------+--------------------------------+
 *                                 | owns std::unique_ptr<ImplT>
 *   +-----------------------------v--------------------------------+
 *   |     ImplT  (PublisherImpl, SubscriberImpl, SetterImpl, ...)  |
 *   +-----------------------------+--------------------------------+
 *                                 | dispatches to transport
 *   +-----------------------------v--------------------------------+
 *   |    Transport back-ends                                       |
 *   |    intra | shm | shm2 | dds | ddsc | zenoh | someip | ...    |
 *   +--------------------------------------------------------------+
 * @endverbatim
 *
 * @par Lifecycle State Diagram
 * @verbatim
 *      +------------+    constructor    +------------+    init()    +------------+
 *      | (uninited) |------------------>|  parsed    |------------->|   active   |
 *      |            |  parse URL / Conf |  (impl_)   |  init_ext    | (using API)|
 *      +------------+                   +------------+              +------------+
 *                                              ^                          |
 *                                              |  init()                  | interrupt()
 *                                              |                          v
 *                                       +------+------+            +-------------+
 *                                       |  deinited   |<-----------|  blocking   |
 *                                       |             |  deinit()  |  wait aborts|
 *                                       +-------------+            +-------------+
 *                                              |
 *                                              | destructor (calls deinit if active)
 *                                              v
 *                                         destroyed
 * @endverbatim
 *
 * | Step              | Method                  | Notes                                       |
 * | ----------------- | ----------------------- | ------------------------------------------- |
 * | Construction      | constructor             | Parses URL, creates impl via Conf factory.  |
 * | Initialisation    | @c init()               | Runs impl init + init_ext, samples loans.   |
 * | Active            | publish / listen / ...  | Normal operation.                           |
 * | Interrupt         | @c interrupt()          | Aborts blocking waits immediately.          |
 * | Deinitialisation  | @c deinit()             | interrupt() then impl deinit + deinit_ext.  |
 * | Destruction       | destructor              | Auto-deinits if still active.               |
 *
 * @par ImplType Roles
 * | @c ImplType         | Primitive that uses it      | Direction      |
 * | ------------------- | --------------------------- | -------------- |
 * | @c kPublisher       | @c Publisher\<T\>           | Event write    |
 * | @c kSubscriber      | @c Subscriber\<T\>          | Event read     |
 * | @c kClient          | @c Client\<Req,Resp\>       | RPC caller     |
 * | @c kServer          | @c Server\<Req,Resp\>       | RPC handler    |
 * | @c kSetter          | @c Setter\<T\>              | Field write    |
 * | @c kGetter          | @c Getter\<T\>              | Field read     |
 *
 * @par Deferred Initialisation
 * @code
 * vlink::Publisher<MyMsg> pub("dds://topic", vlink::InitType::kWithoutInit);
 * pub.set_ser_type("my.custom.Type");
 * pub.set_discovery_enabled(false);
 * pub.init();
 * @endcode
 *
 * @par Security
 * Use the @c Security* primitive aliases to enable per-message encryption.
 * The @c Security::Config aggregate is passed as the second constructor
 * argument; omitting it (or passing an empty aggregate) uses the built-in
 * default symmetric slot:
 * @code
 * vlink::Security::Config cfg;
 * cfg.key = "my-secret";
 * vlink::SecurityPublisher<MyMsg> pub("shm://topic", cfg);
 * @endcode
 *
 * @note @c intra:// and @c dds:// CDR payloads do not support per-message
 *       security; constructing a @c Security* primitive there is a fatal.
 *
 * @par Zero-copy Loans
 * On loan-capable transports the loan API avoids extra copies:
 * @code
 * vlink::Publisher<vlink::Bytes> pub("shm://topic");
 * if (pub.is_support_loan()) {
 *   vlink::Bytes buf = pub.loan(payload_size);
 *   write_into(buf);
 *   pub.publish(buf);  // loan is returned automatically
 * }
 * @endcode
 *
 * @tparam ImplT  Concrete transport implementation derived from @c NodeImpl.
 * @tparam SecT   Security mode: @c kWithoutSecurity (default) or @c kWithSecurity.
 *
 * @see publisher.h, subscriber.h, client.h, server.h, getter.h, setter.h,
 *      extension/security.h, extension/ssl_options.h
 */

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "./extension/security.h"
#include "./impl/node_impl.h"

namespace vlink {

/**
 * @class Node
 * @brief Transport-agnostic CRTP base for all VLink communication primitives.
 *
 * @details
 * Provides the lifecycle, loan, security, property, profiler, message-loop,
 * recording, discovery, and TLS APIs shared by every primitive in the
 * library.  Subclasses fill in the role-specific operations (publish,
 * listen, invoke, set, get, etc.).
 *
 * @tparam ImplT  Concrete implementation class (e.g. @c PublisherImpl).
 * @tparam SecT   Security mode (@c kWithoutSecurity or @c kWithSecurity).
 */
template <typename ImplT, SecurityType SecT>
class Node {
 public:
  using StatusCallback = NodeImpl::StatusCallback;  ///< Handler signature for status-change notifications.

  /**
   * @brief Initialises the node and its transport back-end.
   *
   * @details
   * Uses an atomic compare-exchange to guard against double-initialisation.
   * On success the method runs @c impl_->init() then @c impl_->init_ext()
   * and finally samples the transport's loan capability flag.  Calling
   * @c init() on an already-initialised node is a no-op.
   *
   * @return @c true on first successful initialisation; @c false otherwise.
   */
  virtual bool init();

  /**
   * @brief Tears the node down and releases all transport resources.
   *
   * @details
   * Atomically guards against double-deinit, then runs @c interrupt(),
   * @c impl_->deinit(), and @c impl_->deinit_ext().  When safe-quit mode is
   * active the sequence runs under the safe-quit mutex.  The destructor
   * calls this automatically so explicit calls are only required for early
   * shutdown.
   *
   * @return @c true on first successful deinit; @c false if not initialised.
   */
  virtual bool deinit();

  /**
   * @brief Aborts any blocking wait on this node.
   *
   * @details
   * Signals the internal interrupted flag and notifies the condition
   * variable so that @c wait_for_subscribers(), @c wait_for_connected(), and
   * @c wait_for_value() return immediately with @c false.  @c Getter
   * overrides this to additionally wake its own condition variable used by
   * @c wait_for_value().
   */
  virtual void interrupt();

  /**
   * @brief Reports whether @c init() has been successfully called.
   *
   * @return @c true when the node is currently in the initialised state.
   */
  [[nodiscard]] bool has_inited() const;

  /**
   * @brief Reports whether the transport supports zero-copy loaned buffers.
   *
   * @details
   * Delegates to the transport implementation.  When loans are supported,
   * @c publish() / @c set() / @c reply() automatically use them to avoid an
   * extra memory copy.
   *
   * @return @c true if @c loan() / @c return_loan() are meaningful for this transport.
   */
  [[nodiscard]] bool is_support_loan() const;

  /**
   * @brief Allocates a loaned buffer from the transport memory pool.
   *
   * @details
   * Returns a @c Bytes backed by transport-managed memory of @p size bytes.
   * The caller must either pass it to a publish/write call (which returns
   * the loan automatically) or call @c return_loan() explicitly.  Returns
   * an empty @c Bytes on failure or when the transport has no loan pool.
   *
   * @param size  Requested byte count; @c 0 is valid for empty messages.
   * @return      Loaned @c Bytes, or an empty @c Bytes on failure.
   */
  [[nodiscard]] Bytes loan(int64_t size);

  /**
   * @brief Returns a previously loaned buffer to the transport pool.
   *
   * @details
   * Must be called whenever a loan obtained via @c loan() is not consumed by
   * a publish/write call; failing to return loans can exhaust the shared
   * memory pool.
   *
   * @param bytes  The loaned @c Bytes to return.
   * @return       @c true on success; @c false if the buffer is not a valid loan.
   */
  bool return_loan(const Bytes& bytes);

  /**
   * @brief Suspends message delivery on this node.
   *
   * @details
   * Transport-dependent behaviour: some back-ends buffer incoming messages
   * while suspended, others drop them.  Pair with @c resume().
   *
   * @return @c true if suspension succeeded.
   */
  bool suspend();

  /**
   * @brief Resumes message delivery after a prior @c suspend().
   *
   * @return @c true if resumption succeeded.
   */
  bool resume();

  /**
   * @brief Reports whether the node is currently suspended.
   *
   * @return @c true while @c suspend() is in effect.
   */
  [[nodiscard]] bool is_suspend() const;

  /**
   * @brief Attaches the node to a @c MessageLoop for callback dispatch.
   *
   * @details
   * After attachment, inbound callbacks are posted onto @p message_loop
   * rather than invoked on the transport delivery thread.  This serialises
   * dispatch onto the loop's thread, which is convenient for
   * single-threaded user code.
   *
   * @param message_loop  Pointer to the target @c MessageLoop.
   * @return              @c true on success; @c false if a loop is already attached.
   */
  bool attach(class MessageLoop* message_loop);

  /**
   * @brief Detaches the node from its current @c MessageLoop.
   *
   * @details
   * After detachment the callback returns to running on the transport
   * delivery thread.
   *
   * @return @c true on success; @c false if no loop was attached.
   */
  bool detach();

  /**
   * @brief Returns the @c MessageLoop this node is currently attached to.
   *
   * @return Pointer to the attached @c MessageLoop, or @c nullptr.
   */
  [[nodiscard]] class MessageLoop* get_message_loop() const;

  /**
   * @brief Returns the abstract-graph handle for runtime topology inspection.
   *
   * @details
   * The @c AbstractNode pointer is usable with @c AbstractFactory and the
   * proxy monitoring API to enumerate peer nodes in the same transport
   * graph.
   *
   * @return Non-owning pointer to the @c AbstractNode, or @c nullptr if the
   *         transport does not expose one.
   */
  [[nodiscard]] const AbstractNode* get_abstract_node() const;

  /**
   * @brief Retrieves the current status object for the requested category.
   *
   * @details
   * Returns a polymorphic shared pointer.  The concrete type and set of
   * supported categories depend on the active transport; an unsupported
   * @p type yields a @c Status::Unknown instance and logs a warning.
   *
   * @param type  Status category to retrieve.
   * @return      Shared pointer to status data, or @c Status::Unknown when unsupported.
   */
  [[nodiscard]] Status::BasePtr get_status(Status::Type type) const;

  /**
   * @brief Registers a handler invoked whenever the node's status changes.
   *
   * @details
   * Only one handler can be registered at a time; subsequent calls replace
   * the previous handler.  The handler receives a @c Status::BasePtr
   * describing the new state (connected, disconnected, error, etc.).
   *
   * @param callback  @c void(const Status::BasePtr&) handler.
   */
  void register_status_handler(StatusCallback&& callback);

  /**
   * @brief Sets a transport-specific string-keyed property.
   *
   * @details
   * Extensibility mechanism for back-end-specific tuning knobs that do not
   * have a dedicated method.  Recognised keys depend on the active
   * transport.
   *
   * @param prop   Property key string.
   * @param value  Property value string.
   */
  void set_property(const std::string& prop, const std::string& value);

  /**
   * @brief Retrieves a previously set transport-specific property value.
   *
   * @param prop  Property key string.
   * @return      Property value string; empty if the key is unknown.
   */
  [[nodiscard]] std::string get_property(const std::string& prop) const;

  /**
   * @brief Returns the @c TransportType this node is bound to.
   *
   * @return Enumerator such as @c kDds, @c kShm, @c kIntra, etc.
   */
  [[nodiscard]] TransportType get_transport_type() const;

  /**
   * @brief Returns the URL string used to construct this node.
   *
   * @details
   * Non-empty only when the node was constructed via a URL string or @c Url
   * object; typed @c ConfT-based construction leaves this empty.
   *
   * @return Const reference to the URL string.
   */
  [[nodiscard]] const std::string& get_url() const;

  /**
   * @brief Enables recording of inbound or outbound messages to a bag file.
   *
   * @details
   * Not supported on @c intra:// or @c dds:// CDR nodes (triggers a fatal log).
   * Supported file suffixes are @c .vdb, @c .vdbx, @c .vcap, and @c .vcapx;
   * an unknown suffix logs an error and disables recording.
   *
   * @param path  Bag file path on disk.
   */
  void set_record_path(const std::string& path);

  /**
   * @brief Overrides the runtime wire-metadata identifiers for this node.
   *
   * @details
   * @p ser_type holds the concrete runtime type identifier; @p schema_type
   * holds the coarse decoder family used by discovery, proxy, and bag
   * metadata.  When @p schema_type is @c SchemaType::kUnknown (the default)
   * the family is inferred from @p ser_type: a @c "vlink::zerocopy::" prefix
   * implies @c kZeroCopy, while values such as @c "raw", @c "string",
   * @c "std::string", @c "text", or @c "json" imply @c kRaw.  If no family
   * can be inferred, an existing @c kRaw or @c kZeroCopy family is reset to
   * @c kUnknown; any other existing family is preserved.  Passing an empty
   * @p ser_type clears both fields.
   *
   * If invoked post-@c init() the transport extension is restarted so that
   * external metadata stays in sync.
   *
   * @param ser_type     Concrete runtime type identifier, or empty to clear.
   * @param schema_type  Optional explicit schema family; default preserves the current family.
   */
  void set_ser_type(const std::string& ser_type, SchemaType schema_type = SchemaType::kUnknown);

  /**
   * @brief Returns the current concrete runtime type identifier.
   *
   * @return Const reference to the type identifier string.
   */
  [[nodiscard]] const std::string& get_ser_type() const;

  /**
   * @brief Returns the current coarse schema family.
   *
   * @return The @c SchemaType stored on the implementation.
   */
  [[nodiscard]] SchemaType get_schema_type() const;

  /**
   * @brief Toggles peer-discovery on this node.
   *
   * @details
   * Disabling discovery reduces CPU and network overhead for nodes that
   * never need to locate peers.  Reinitialises the transport extension if
   * invoked post-@c init() so the change takes effect immediately.
   *
   * @param enable  @c true (default) to enable discovery; @c false to disable.
   */
  void set_discovery_enabled(bool enable);

  /**
   * @brief Reports whether peer-discovery is currently enabled.
   *
   * @return @c true if discovery is active.
   */
  [[nodiscard]] bool get_discovery_enabled() const;

  /**
   * @brief Binds a Protobuf Arena for arena-allocated message objects.
   *
   * @details
   * Required when @c MsgT is a raw Protobuf pointer type (e.g. @c MyProto*).
   * The arena must outlive this node.  Forgetting to bind an arena before
   * the first received message triggers a fatal log.
   *
   * @param proto_arena  Pointer to a @c google::protobuf::Arena instance (typed as @c void*).
   */
  void bind_proto_arena(void* proto_arena);

  /**
   * @brief Returns the cumulative CPU-usage ratio sampled by the profiler.
   *
   * @details
   * Reports the percentage of wall-clock time this node has spent in active
   * publish or receive code since the profiler was started.  Available only
   * when the CPU profiler is built in (@c VLINK_DISABLE_PROFILER not
   * defined) and global profiling is enabled via the @c VLINK_PROFILER_ENABLE
   * environment variable.  Returns @c -1.0 if no profiler is attached.
   *
   * @return CPU usage percentage (may exceed @c 100.0 on multi-core systems);
   *         @c -1.0 if unavailable.
   */
  [[nodiscard]] double get_cpu_usage() const;

  /**
   * @brief Reports whether safe-quit mode is currently active.
   *
   * @details
   * Safe-quit mode holds a @c std::mutex around user callbacks and around
   * @c deinit() to prevent use-after-free races when a node is destroyed
   * while a callback is in flight.
   *
   * @return @c true if the safe-quit mutex is engaged.
   */
  [[nodiscard]] bool get_safety_quit() const;

  /**
   * @brief Enables or disables safe-quit mode.
   *
   * @details
   * When enabled, an internal @c std::mutex is allocated and locked around
   * every callback invocation and around @c deinit().  Enable when the
   * node's lifetime is shorter than the callback scope.  There is a small
   * synchronisation overhead; avoid enabling it on hot paths.
   *
   * @param safety_quit  @c true to enable; @c false to disable (default).
   */
  void set_safety_quit(bool safety_quit);

  /**
   * @brief Configures transport-layer SSL/TLS encryption for this node.
   *
   * @details
   * Merges the fields of @p options into the node's internal property map
   * via @c SslOptions::parse_to().  The transport reads the resulting
   * @c ssl.* properties during @c init() to set up the TLS connection, so
   * this method must be called before @c init() for the settings to take
   * effect.
   *
   * SSL is considered enabled when @c SslOptions::is_valid() returns
   * @c true (i.e. at least @c ca_file or @c cert_file is non-empty).  Not
   * all back-ends support TLS; see @c SslOptions for the per-backend
   * compatibility table.  Thread-safe -- the property map is updated under
   * a mutex.
   *
   * @par Example
   * @code
   * vlink::Publisher<MyMsg> pub("mqtt://sensor/data", vlink::InitType::kWithoutInit);
   * vlink::SslOptions ssl;
   * ssl.ca_file   = "/etc/certs/ca.pem";
   * ssl.cert_file = "/etc/certs/client.pem";
   * ssl.key_file  = "/etc/certs/client-key.pem";
   * pub.set_ssl_options(ssl);
   * pub.init();
   * @endcode
   *
   * @param options  SSL/TLS configuration to apply.
   *
   * @see SslOptions, set_property()
   */
  void set_ssl_options(const SslOptions& options);

 protected:
  Node();

  virtual ~Node();

  /**
   * @brief Installs a @c Security configuration before transport initialisation.
   *
   * @details
   * Internal helper used by the @c Security* primitive constructors after
   * @c impl_ has been created but before @c init().  Delegates default
   * handling, validation, and storage to @c NodeImpl::enable_security().
   *
   * @param cfg  Security configuration aggregate.
   * @return     @c true when @p cfg (including the default empty case) is usable for this role / transport.
   */
  bool enable_security(const Security::Config& cfg);

  /**
   * @brief Move overload for construction-time security installation.
   *
   * @details
   * Used when an internal caller owns the config and can forward it without
   * an extra copy.
   *
   * @param cfg  Security configuration aggregate to consume.
   * @return     @c true when @p cfg (including the default empty case) is usable for this role / transport.
   */
  bool enable_security(Security::Config&& cfg);

  template <typename CallbackT, typename... ArgsT>
  void invoke_callback(const CallbackT& callback, ArgsT&&... args);

  template <typename TypeT>
  TypeT get_default_value();

  void* proto_arena_{nullptr};
  bool is_support_loan_{false};

  std::atomic_bool has_inited_{false};
  std::optional<std::mutex> quit_mtx_;

  std::unique_ptr<ImplT> impl_;

 private:
  VLINK_DISALLOW_COPY_AND_ASSIGN(Node)
};

}  // namespace vlink

#include "./internal/node-inl.h"
