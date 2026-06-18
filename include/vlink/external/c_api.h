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
 * @file c_api.h
 * @brief Pure C binding over the VLink communication middleware.
 *
 * @details
 * Exposes a stable, language-agnostic C surface across the three VLink
 * communication models.  Every public function wraps a corresponding C++
 * template instantiation that uses @c vlink::Bytes as its payload type, so the
 * C ABI never has to leak C++ type information.
 *
 * @par Model Mapping
 *
 * | Model   | C++ class                    | C handle types                                             |
 * | ------- | ---------------------------- | ---------------------------------------------------------- |
 * | Event   | @c Publisher / @c Subscriber | @c vlink_publisher_handle_t / @c vlink_subscriber_handle_t |
 * | Method  | @c Server / @c Client        | @c vlink_server_handle_t / @c vlink_client_handle_t        |
 * | Field   | @c Setter / @c Getter        | @c vlink_setter_handle_t / @c vlink_getter_handle_t        |
 *
 * @par C / C++ Boundary
 * @code
 *  +-----------------------+        +---------------------------------+
 *  |   User C / C++ code   |        |  vlink C++ core templates       |
 *  |                       |        |  Publisher<Bytes>, Server<...>, |
 *  |  vlink_publish(...)   |        |  Setter<Bytes>, Security, ...   |
 *  |  vlink_invoke(...)    |        +----------------+----------------+
 *  |  vlink_set(...)       |                         ^
 *  +-----------+-----------+                         |
 *              |   handle.native_handle (opaque)     |
 *              v                                     |
 *  +-----------------------+   shallow Bytes::wrap   |
 *  |   c_api.h boundary    +-------------------------+
 *  |  (this file)          |
 *  +-----------------------+
 * @endcode
 *
 * @par Return Code Family
 *
 * | Code                            | Meaning                                            |
 * | ------------------------------- | -------------------------------------------------- |
 * | @c VLINK_RET_NO_ERROR (0)       | Success.                                           |
 * | @c VLINK_RET_UNEXPECTED_ERROR   | Condition not met yet (e.g. no subscribers).       |
 * | @c VLINK_RET_INVALID_ERROR      | Null pointer / invalid handle / bad arguments.     |
 * | @c VLINK_RET_MEMORY_ERROR       | Allocation failure or output buffer too small.     |
 * | @c VLINK_RET_RUNTIME_ERROR      | Runtime state error or C++ construction exception. |
 * | @c VLINK_RET_TRANSFER_ERROR     | Publish / listen / invoke operation failed.        |
 * | @c VLINK_RET_UNKNOWN_ERROR (-1) | Unclassified internal error.                       |
 *
 * @par Server Reply Protocol
 * The @c vlink_server_handle_t::reserved array coordinates the synchronous
 * request-reply flow.  Inside @c vlink_req_callback_t a call to @c vlink_reply()
 * stores the response.  When the callback returns, the relayed reply is sent to
 * the client.  Calling @c vlink_reply() after the callback returns fails with
 * @c VLINK_RET_RUNTIME_ERROR because no request is in progress.
 * @code
 * static void on_request(const uint8_t* data, size_t size, void* user_data) {
 *     vlink_server_handle_t* handle = (vlink_server_handle_t*) user_data;
 *     vlink_reply(handle, resp_data, resp_size);
 * }
 *
 * vlink_schema_info_t schema = {"demo.raw.Text", VLINK_SCHEMA_RAW};
 * vlink_create_server(url, &schema, &handle, on_request, &handle);
 * @endcode
 *
 * @par Example -- Event (Publisher / Subscriber)
 * @code
 * vlink_publisher_handle_t pub;
 * vlink_schema_info_t schema = {"demo.proto.PointCloud", VLINK_SCHEMA_PROTOBUF};
 * vlink_create_publisher("dds://my/topic", &schema, &pub);
 * vlink_wait_for_subscribers(pub, 1000);
 * vlink_publish(pub, data_buf, data_size);
 * vlink_destroy_publisher(&pub);
 *
 * static void on_message(const uint8_t* data, size_t size, void* user_data) { (void)data; }
 * vlink_subscriber_handle_t sub;
 * vlink_create_subscriber("dds://my/topic", &schema, &sub, on_message, NULL);
 * @endcode
 *
 * @par Example -- Method (Server / Client)
 * @code
 * vlink_client_handle_t cli;
 * vlink_schema_info_t schema = {"demo.raw.Echo", VLINK_SCHEMA_RAW};
 * vlink_create_client("dds://echo", &schema, &cli);
 * vlink_wait_for_server(cli, 1000);
 * vlink_invoke(cli, req_buf, req_size, on_response, NULL);
 * @endcode
 *
 * @par Example -- Field (Setter / Getter)
 * @code
 * vlink_getter_handle_t getter;
 * vlink_schema_info_t schema = {"demo.proto.State", VLINK_SCHEMA_PROTOBUF};
 * vlink_create_getter("dds://state/topic", &schema, &getter, NULL, NULL);
 *
 * uint8_t buf[4096];
 * size_t  sz = sizeof(buf);
 * if (vlink_get(getter, buf, &sz) == VLINK_RET_NO_ERROR) {
 *     // buf[0..sz-1] holds the latest value
 * }
 * @endcode
 *
 * @note
 * - Internally the C API uses @c vlink::Publisher<vlink::Bytes> and equivalents.
 *   The @c vlink_schema_info_t aggregate configures @c ser + @c schema atomically.
 * - Every create/destroy pair must be balanced; handles are not thread-safe for
 *   concurrent create/destroy calls.
 * - @c vlink_get() copies the latest value into the caller-supplied buffer and
 *   returns @c VLINK_RET_MEMORY_ERROR when the buffer is smaller than the payload.
 * - @c vlink_publish_by_force() publishes even with no matched subscribers,
 *   useful for transient-local-durability scenarios.
 */

#pragma once

// NOLINTBEGIN

#undef VLINK_C_API_EXPORT
#ifdef VLINK_C_API_LIBRARY_STATIC
#define VLINK_C_API_EXPORT
#elif defined(_WIN32) || defined(__CYGWIN__)
#ifdef VLINK_C_API_LIBRARY
#define VLINK_C_API_EXPORT __declspec(dllexport)
#else
#define VLINK_C_API_EXPORT __declspec(dllimport)
#endif
#else
#define VLINK_C_API_EXPORT __attribute__((visibility("default")))
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @name Common types and return codes
 * @{
 */

/**
 * @brief Return code for VLink C API functions that report @c vlink_ret_t.
 *
 * @details
 * @c VLINK_RET_NO_ERROR is the only success code.  Positive values classify
 * recoverable API states or errors and @c VLINK_RET_UNKNOWN_ERROR (-1) indicates
 * an unclassified internal error.  Always check the symbolic value -- do not
 * treat every non-negative result as success.
 */
typedef enum {
  VLINK_RET_UNKNOWN_ERROR = -1,   /**< Unclassified or unexpected internal error.                       */
  VLINK_RET_NO_ERROR = 0,         /**< Operation succeeded.                                             */
  VLINK_RET_UNEXPECTED_ERROR = 1, /**< Condition not yet met (e.g. no subscribers matched).             */
  VLINK_RET_INVALID_ERROR = 2,    /**< Null pointer argument or otherwise invalid handle / arguments.   */
  VLINK_RET_MEMORY_ERROR = 3,     /**< Allocation failure or caller-provided buffer is too small.       */
  VLINK_RET_RUNTIME_ERROR = 4,    /**< Runtime state error or C++ construction exception.               */
  VLINK_RET_TRANSFER_ERROR = 5,   /**< Publish, listen, or invoke operation failed.                     */
} vlink_ret_t;

/**
 * @brief Coarse runtime schema family used for raw C API nodes.
 *
 * @details
 * The numeric values are kept in sync with @c vlink::SchemaType (see
 * @c include/vlink/impl/types.h), so the C API implementation can cast between
 * the two enums safely.  Always reference the symbolic names -- the underlying
 * mapping is intentionally opaque at source level.
 */
typedef enum {
  VLINK_SCHEMA_UNKNOWN = 0,     /**< Schema family is not specified.   */
  VLINK_SCHEMA_RAW = 1,         /**< Opaque / raw payload.             */
  VLINK_SCHEMA_ZEROCOPY = 2,    /**< VLink zero-copy payload.          */
  VLINK_SCHEMA_PROTOBUF = 3,    /**< Protocol Buffers payload.         */
  VLINK_SCHEMA_FLATBUFFERS = 4, /**< FlatBuffers payload.              */
} vlink_schema_t;

/**
 * @brief Bundled runtime schema metadata supplied at node creation.
 *
 * @details
 * Mirrors the C++ pair of @c ser_type + @c schema_type so callers can configure
 * both atomically before the underlying node is initialised.  Both fields must
 * either be provided together or left unset (@c ser == @c NULL / empty and
 * @c schema == @c VLINK_SCHEMA_UNKNOWN).
 */
typedef struct {
  const char* ser;       /**< Concrete type name / serialisation identifier, or @c NULL.  */
  vlink_schema_t schema; /**< Coarse schema family.                                       */
} vlink_schema_info_t;

/** @} */

/**
 * @name Opaque node handles
 *
 * @details
 * Each handle is a small POD struct holding @c native_handle (a pointer to the
 * underlying heap-allocated C++ instance) plus a @c reserved scratch area used
 * by the C API for internal state -- security context, request/reply
 * coordination, etc.  Treat every @c reserved slot as opaque: the layout is
 * private and may change between releases.
 *
 * @{
 */

/**
 * @brief Opaque handle for a @c Publisher node.
 *
 * @details
 * Created by @c vlink_create_publisher() and destroyed by
 * @c vlink_destroy_publisher().  @c native_handle references a heap-allocated
 * @c vlink::Publisher<vlink::Bytes>.
 */
typedef struct {
  void* native_handle; /**< Internal C++ Publisher object pointer.   */
  void* reserved[8];   /**< Reserved for internal state; do not touch.*/
} vlink_publisher_handle_t;

/**
 * @brief Opaque handle for a @c Subscriber node.
 *
 * @details
 * Created by @c vlink_create_subscriber() and destroyed by
 * @c vlink_destroy_subscriber().  @c native_handle references a heap-allocated
 * @c vlink::Subscriber<vlink::Bytes>.
 */
typedef struct {
  void* native_handle; /**< Internal C++ Subscriber object pointer.   */
  void* reserved[8];   /**< Reserved for internal state; do not touch.*/
} vlink_subscriber_handle_t;

/**
 * @brief Opaque handle for a @c Server node.
 *
 * @details
 * Created by @c vlink_create_server() and destroyed by
 * @c vlink_destroy_server().  @c native_handle references a heap-allocated
 * @c vlink::Server<vlink::Bytes, vlink::Bytes>.  The @c reserved array holds the
 * coordination data used to implement the synchronous request-reply protocol;
 * its layout is intentionally private.
 */
typedef struct {
  void* native_handle; /**< Internal C++ Server object pointer.        */
  void* reserved[8];   /**< Internal coordination state; do not touch. */
} vlink_server_handle_t;

/**
 * @brief Opaque handle for a @c Client node.
 *
 * @details
 * Created by @c vlink_create_client() and destroyed by
 * @c vlink_destroy_client().  @c native_handle references a heap-allocated
 * @c vlink::Client<vlink::Bytes, vlink::Bytes>.
 */
typedef struct {
  void* native_handle; /**< Internal C++ Client object pointer.        */
  void* reserved[8];   /**< Reserved for internal state; do not touch. */
} vlink_client_handle_t;

/**
 * @brief Opaque handle for a @c Setter node.
 *
 * @details
 * Created by @c vlink_create_setter() and destroyed by
 * @c vlink_destroy_setter().  @c native_handle references a heap-allocated
 * @c vlink::Setter<vlink::Bytes>.
 */
typedef struct {
  void* native_handle; /**< Internal C++ Setter object pointer.        */
  void* reserved[8];   /**< Reserved for internal state; do not touch. */
} vlink_setter_handle_t;

/**
 * @brief Opaque handle for a @c Getter node.
 *
 * @details
 * Created by @c vlink_create_getter() and destroyed by
 * @c vlink_destroy_getter().  @c native_handle references a heap-allocated
 * @c vlink::Getter<vlink::Bytes>.
 */
typedef struct {
  void* native_handle; /**< Internal C++ Getter object pointer.        */
  void* reserved[8];   /**< Reserved for internal state; do not touch. */
} vlink_getter_handle_t;

/** @} */

/**
 * @name Callback typedefs
 * @{
 */

/**
 * @brief Callback fired when the connection state of a Publisher or Client changes.
 *
 * @param is_connected  @c true when at least one peer is matched, @c false otherwise.
 * @param user_data     Opaque pointer supplied at registration time.
 *
 * @note Invoked from a VLink-internal event thread; keep the body short and
 *       avoid blocking calls.
 */
typedef void (*vlink_connect_callback_t)(const bool is_connected, void* user_data);

/**
 * @brief Callback fired when a Subscriber or Getter receives a message.
 *
 * @param data       Pointer to the received payload bytes.
 * @param size       Number of bytes available at @p data.
 * @param user_data  Opaque pointer supplied at creation time.
 *
 * @note Invoked on the underlying receive thread.  The @p data buffer is only
 *       valid for the duration of the callback -- copy if you need to retain it.
 */
typedef void (*vlink_msg_callback_t)(const uint8_t* data, const size_t size, void* user_data);

/**
 * @brief Callback fired when a Server receives an RPC request.
 *
 * @details
 * Invoked synchronously on the Server's dispatch thread while an internal mutex
 * is held.  Call @c vlink_reply() from inside this callback to provide a
 * non-empty response before it returns.  Without a @c vlink_reply() call the
 * request completes with an empty payload.  Calling @c vlink_reply() after the
 * callback returns fails with @c VLINK_RET_RUNTIME_ERROR because no request is
 * in progress.
 *
 * @param data       Pointer to the request payload bytes.
 * @param size       Number of bytes available at @p data.
 * @param user_data  Opaque pointer supplied at creation time.
 */
typedef void (*vlink_req_callback_t)(const uint8_t* data, const size_t size, void* user_data);

/**
 * @brief Callback fired when a Client receives an RPC response.
 *
 * @param data       Pointer to the response payload bytes; may be @c NULL when
 *                   the server did not provide a response.
 * @param size       Number of bytes available at @p data.
 * @param user_data  Opaque pointer supplied at invocation time.
 */
typedef void (*vlink_resp_callback_t)(const uint8_t* data, const size_t size, void* user_data);

/** @} */

/* Forward declaration of the Security configuration aggregate.  The full
 * definition lives further down with the Security API; this forward declaration
 * lets the @c vlink_create_secure_*() node creation entry points reference
 * @c const vlink_security_config_t* before the struct body is in scope.        */
struct vlink_security_config_s;
typedef struct vlink_security_config_s vlink_security_config_t;

////////////////////////////////////////////////////////////////
/// Publisher
////////////////////////////////////////////////////////////////

/**
 * @name Event model -- Publisher
 * @{
 */

/**
 * @brief Creates a Publisher node and initialises it on the given URL.
 *
 * @details
 * Allocates a @c vlink::Publisher<vlink::Bytes> on the heap and stores its
 * pointer in @p handle.  Supply @p schema_info to configure @c ser + @c schema
 * before the underlying node is initialised; pass @c NULL to leave both unset.
 *
 * @param url          VLink topic URL (e.g. @c "dds://my/topic").  Must not be @c NULL.
 * @param schema_info  Optional bundled @c ser + @c schema metadata.
 * @param handle       Output handle.  Must not be @c NULL.
 * @return        @c VLINK_RET_NO_ERROR on success; @c VLINK_RET_INVALID_ERROR when
 *                @p url or @p handle is @c NULL, when @p schema_info is only
 *                partially filled, or when @c schema_info->schema is invalid;
 *                @c VLINK_RET_MEMORY_ERROR on pool allocation failure;
 *                @c VLINK_RET_RUNTIME_ERROR if construction throws.
 *
 * @note Caller owns the handle and must release it through
 *       @c vlink_destroy_publisher().  Not thread-safe with concurrent
 *       create/destroy on the same handle.
 */
VLINK_C_API_EXPORT int vlink_create_publisher(const char* url, const vlink_schema_info_t* schema_info,
                                              vlink_publisher_handle_t* handle);

/**
 * @brief Destroys a Publisher node and releases every associated resource.
 *
 * @param handle  Publisher handle to destroy.  Must not be @c NULL.
 * @return        @c VLINK_RET_NO_ERROR on success; @c VLINK_RET_INVALID_ERROR
 *                when @p handle or its @c native_handle is @c NULL.
 *
 * @note Must not be called concurrently with any other operation on the same
 *       handle.
 */
VLINK_C_API_EXPORT int vlink_destroy_publisher(vlink_publisher_handle_t* handle);

/**
 * @brief Checks whether at least one Subscriber has matched this Publisher.
 *
 * @param handle  Publisher handle.
 * @return        @c VLINK_RET_NO_ERROR if subscribers are present;
 *                @c VLINK_RET_UNEXPECTED_ERROR when none are matched yet;
 *                @c VLINK_RET_INVALID_ERROR on bad handle.
 *
 * @note Thread-safe.
 */
VLINK_C_API_EXPORT int vlink_has_subscribers(const vlink_publisher_handle_t handle);

/**
 * @brief Blocks until at least one Subscriber matches or @p timeout_ms expires.
 *
 * @param handle      Publisher handle.
 * @param timeout_ms  Maximum wait time in milliseconds.
 * @return            @c VLINK_RET_NO_ERROR if a subscriber matched;
 *                    @c VLINK_RET_UNEXPECTED_ERROR on timeout;
 *                    @c VLINK_RET_INVALID_ERROR on bad handle.
 *
 * @note Blocks the calling thread.
 */
VLINK_C_API_EXPORT int vlink_wait_for_subscribers(const vlink_publisher_handle_t handle, const int timeout_ms);

/**
 * @brief Registers a callback fired whenever the Subscriber connection state changes.
 *
 * @param handle            Publisher handle.
 * @param connect_callback  Callback to invoke on every state change.
 * @param user_data         Opaque pointer forwarded to @p connect_callback.
 * @return                  @c VLINK_RET_NO_ERROR on success;
 *                          @c VLINK_RET_INVALID_ERROR on bad handle or a @c NULL
 *                          @p connect_callback.
 *
 * @note The callback runs on the Publisher's internal event thread.
 */
VLINK_C_API_EXPORT int vlink_detect_subscribers(const vlink_publisher_handle_t handle,
                                                const vlink_connect_callback_t connect_callback, void* user_data);

/**
 * @brief Publishes a message to every matched Subscriber.
 *
 * @details
 * The implementation wraps @p data in a shallow-copy @c vlink::Bytes -- zero-copy
 * when the transport supports it.  Returns @c VLINK_RET_TRANSFER_ERROR when no
 * subscribers are matched and the publisher does not allow forced delivery.
 *
 * @param handle  Publisher handle.
 * @param data    Payload to publish.  Must remain valid until the call returns.
 * @param size    Number of bytes in @p data.
 * @return        @c VLINK_RET_NO_ERROR on success;
 *                @c VLINK_RET_TRANSFER_ERROR when publishing fails;
 *                @c VLINK_RET_INVALID_ERROR on bad handle or
 *                @p data == @c NULL with @p size > 0.
 *
 * @note Thread-safe with respect to other publishes on the same handle.
 */
VLINK_C_API_EXPORT int vlink_publish(const vlink_publisher_handle_t handle, const uint8_t* data, const size_t size);

/**
 * @brief Publishes a message even when no Subscribers are matched.
 *
 * @details
 * Identical to @c vlink_publish() but passes @c force=true to the underlying
 * publisher, bypassing the subscriber-presence check.  Useful for
 * transient-local-durability or late-joining subscriber scenarios.
 *
 * @param handle  Publisher handle.
 * @param data    Payload to publish.
 * @param size    Number of bytes in @p data.
 * @return        @c VLINK_RET_NO_ERROR on success;
 *                @c VLINK_RET_TRANSFER_ERROR on failure;
 *                @c VLINK_RET_INVALID_ERROR on bad handle or
 *                @p data == @c NULL with @p size > 0.
 *
 * @note Same threading guarantees as @c vlink_publish().
 */
VLINK_C_API_EXPORT int vlink_publish_by_force(const vlink_publisher_handle_t handle, const uint8_t* data,
                                              const size_t size);

/** @} */

////////////////////////////////////////////////////////////////
/// Subscriber
////////////////////////////////////////////////////////////////

/**
 * @name Event model -- Subscriber
 * @{
 */

/**
 * @brief Creates a Subscriber node, initialises it, and registers the message callback.
 *
 * @details
 * Allocates a @c vlink::Subscriber<vlink::Bytes> and immediately calls
 * @c listen() with @p msg_callback.  The callback runs on the Subscriber's
 * internal receive thread.
 *
 * @param url           VLink topic URL.  Must not be @c NULL.
 * @param schema_info   Optional bundled @c ser + @c schema metadata.
 * @param handle        Output handle.  Must not be @c NULL.
 * @param msg_callback  Message handler.  Must not be @c NULL.
 * @param user_data     Opaque pointer forwarded to @p msg_callback.
 * @return              @c VLINK_RET_NO_ERROR on success;
 *                      @c VLINK_RET_INVALID_ERROR for @c NULL arguments,
 *                      partially-filled @p schema_info, or invalid
 *                      @c schema_info->schema;
 *                      @c VLINK_RET_MEMORY_ERROR on pool allocation failure;
 *                      @c VLINK_RET_TRANSFER_ERROR if @c listen() fails;
 *                      @c VLINK_RET_RUNTIME_ERROR on construction exception.
 *
 * @note Caller owns the handle and must release it with
 *       @c vlink_destroy_subscriber().
 */
VLINK_C_API_EXPORT int vlink_create_subscriber(const char* url, const vlink_schema_info_t* schema_info,
                                               vlink_subscriber_handle_t* handle,
                                               const vlink_msg_callback_t msg_callback, void* user_data);

/**
 * @brief Atomically creates a Subscriber, installs @c Security, and calls @c listen().
 *
 * @details
 * Builds the @c vlink::Security from @p security_cfg @b before the internal
 * @c listen() registration completes, so every inbound frame is run through
 * @c Security::decrypt().  Security configuration is one-shot at creation -- no
 * separate runtime entry point exists.
 *
 * On failure (bad URL/schema/cfg, non-decrypt-capable security state, listen
 * failure) no resources leak; any internal handle stored before the failure is
 * cleared.
 *
 * @param url           VLink subscriber URL.  Must not be @c NULL.
 * @param schema_info   Optional bundled @c ser + @c schema metadata.
 * @param handle        Output handle.  Must not be @c NULL.
 * @param msg_callback  Message handler.  Must not be @c NULL.
 * @param user_data     Opaque pointer forwarded to @p msg_callback.
 * @param security_cfg  Security configuration.  Must not be @c NULL.  A
 *                      zero-initialised configuration uses the built-in default
 *                      symmetric slot with replay protection disabled;
 *                      otherwise a decrypt-capable slot is required.
 * @return              @c VLINK_RET_NO_ERROR on success;
 *                      @c VLINK_RET_INVALID_ERROR on bad arguments (including a
 *                      non-decrypt-capable @p security_cfg);
 *                      @c VLINK_RET_MEMORY_ERROR on pool allocation failure;
 *                      @c VLINK_RET_TRANSFER_ERROR if @c listen() fails;
 *                      @c VLINK_RET_RUNTIME_ERROR on construction exception.
 *
 * @note Caller owns the handle and must release it with
 *       @c vlink_destroy_subscriber().
 */
VLINK_C_API_EXPORT int vlink_create_secure_subscriber(const char* url, const vlink_schema_info_t* schema_info,
                                                      vlink_subscriber_handle_t* handle,
                                                      const vlink_msg_callback_t msg_callback, void* user_data,
                                                      const vlink_security_config_t* security_cfg);

/**
 * @brief Destroys a Subscriber node and releases every associated resource.
 *
 * @param handle  Subscriber handle to destroy.  Must not be @c NULL.
 * @return        @c VLINK_RET_NO_ERROR on success;
 *                @c VLINK_RET_INVALID_ERROR on bad handle.
 *
 * @note Must not be called concurrently with any other operation on the same
 *       handle.
 */
VLINK_C_API_EXPORT int vlink_destroy_subscriber(vlink_subscriber_handle_t* handle);

/** @} */

////////////////////////////////////////////////////////////////
/// Server
////////////////////////////////////////////////////////////////

/**
 * @name Method model -- Server
 * @{
 */

/**
 * @brief Creates a Server node, initialises it, and registers the request callback.
 *
 * @details
 * Allocates a @c vlink::Server<vlink::Bytes, vlink::Bytes> and calls @c listen()
 * with an internal handler that wraps @p req_callback.  The internal handler
 * serialises request/reply state during each invocation.  Call @c vlink_reply()
 * from inside @p req_callback to set a response before the callback returns.
 *
 * @param url           VLink service URL.  Must not be @c NULL.
 * @param schema_info   Optional bundled @c ser + @c schema metadata.
 * @param handle        Output handle.  Must not be @c NULL.
 * @param req_callback  Request handler.  Must not be @c NULL.
 * @param user_data     Opaque pointer forwarded to @p req_callback.
 * @return              @c VLINK_RET_NO_ERROR on success;
 *                      @c VLINK_RET_INVALID_ERROR for @c NULL arguments,
 *                      partially-filled @p schema_info, or invalid
 *                      @c schema_info->schema;
 *                      @c VLINK_RET_MEMORY_ERROR on pool allocation failure;
 *                      @c VLINK_RET_TRANSFER_ERROR if @c listen() fails;
 *                      @c VLINK_RET_RUNTIME_ERROR on construction exception.
 *
 * @note Caller owns the handle and must release it with @c vlink_destroy_server().
 */
VLINK_C_API_EXPORT int vlink_create_server(const char* url, const vlink_schema_info_t* schema_info,
                                           vlink_server_handle_t* handle, const vlink_req_callback_t req_callback,
                                           void* user_data);

/**
 * @brief Atomically creates a Server, installs @c Security, and calls @c listen().
 *
 * @details
 * Builds the @c vlink::Security from @p security_cfg @b before the internal
 * @c listen() registration completes.  Inbound requests are decrypted through
 * @c Security::decrypt(); replies written via @c vlink_reply() are encrypted
 * through @c Security::encrypt().  Without a @c vlink_reply() call, or with
 * @c size == @c 0, the request still completes with an empty response so the C
 * API reply protocol is preserved.  When @c security_cfg->advanced.aad_context
 * is empty the wrapper binds security to @c url|ser|schema; absent
 * @p schema_info defaults to the C API @c Bytes binding @c url||VLINK_SCHEMA_RAW.
 * Security configuration is one-shot at creation -- no separate runtime entry
 * point exists.
 *
 * @param url            VLink service URL.  Must not be @c NULL.
 * @param schema_info    Optional bundled @c ser + @c schema metadata.
 * @param handle         Output handle.  Must not be @c NULL.
 * @param req_callback   Request handler.  Must not be @c NULL.
 * @param user_data      Opaque pointer forwarded to @p req_callback.
 * @param security_cfg   Security configuration.  Must not be @c NULL.  A
 *                       zero-initialised configuration uses the built-in default
 *                       symmetric slot with replay protection disabled;
 *                       otherwise both encrypt- and decrypt-capable slots are
 *                       required.
 * @return               @c VLINK_RET_NO_ERROR on success;
 *                       @c VLINK_RET_INVALID_ERROR on bad arguments;
 *                       @c VLINK_RET_MEMORY_ERROR on pool allocation failure;
 *                       @c VLINK_RET_TRANSFER_ERROR if @c listen() fails;
 *                       @c VLINK_RET_RUNTIME_ERROR on construction exception.
 *
 * @note Caller owns the handle and must release it with @c vlink_destroy_server().
 */
VLINK_C_API_EXPORT int vlink_create_secure_server(const char* url, const vlink_schema_info_t* schema_info,
                                                  vlink_server_handle_t* handle,
                                                  const vlink_req_callback_t req_callback, void* user_data,
                                                  const vlink_security_config_t* security_cfg);

/**
 * @brief Destroys a Server node and frees every internal resource, including the
 *        request/reply coordination state.
 *
 * @param handle  Server handle to destroy.  Must not be @c NULL.
 * @return        @c VLINK_RET_NO_ERROR on success;
 *                @c VLINK_RET_INVALID_ERROR on bad handle.
 *
 * @note Must not be called concurrently with any other operation on the same
 *       handle.
 */
VLINK_C_API_EXPORT int vlink_destroy_server(vlink_server_handle_t* handle);

/**
 * @brief Provides the response data for the current in-progress RPC request.
 *
 * @details
 * Must be called from inside @c vlink_req_callback_t while the internal request
 * context is active.  The response is copied into owned internal storage, or
 * encrypted into owned internal storage for secure servers.  A @p size of @c 0
 * is accepted and produces the protocol's empty response.
 *
 * @param handle  Server handle.  Must not be @c NULL.
 * @param data    Response payload bytes.
 * @param size    Number of bytes in @p data.
 * @return        @c VLINK_RET_NO_ERROR on success;
 *                @c VLINK_RET_INVALID_ERROR on bad handle or
 *                @p data == @c NULL with @p size > 0;
 *                @c VLINK_RET_RUNTIME_ERROR when no pending request is in
 *                progress;
 *                @c VLINK_RET_MEMORY_ERROR if internal allocation fails;
 *                @c VLINK_RET_TRANSFER_ERROR if secure response encryption
 *                fails.
 *
 * @note Only valid inside the @c vlink_req_callback_t.
 */
VLINK_C_API_EXPORT int vlink_reply(vlink_server_handle_t* handle, const uint8_t* data, const size_t size);

/** @} */

////////////////////////////////////////////////////////////////
/// Client
////////////////////////////////////////////////////////////////

/**
 * @name Method model -- Client
 * @{
 */

/**
 * @brief Creates a Client node and initialises it on the given URL.
 *
 * @param url          VLink service URL.  Must not be @c NULL.
 * @param schema_info  Optional bundled @c ser + @c schema metadata.
 * @param handle       Output handle.  Must not be @c NULL.
 * @return        @c VLINK_RET_NO_ERROR on success;
 *                @c VLINK_RET_INVALID_ERROR for @c NULL @p url / @p handle,
 *                partially-filled @p schema_info, or invalid
 *                @c schema_info->schema;
 *                @c VLINK_RET_MEMORY_ERROR on pool allocation failure;
 *                @c VLINK_RET_RUNTIME_ERROR on construction exception.
 *
 * @note Caller owns the handle and must release it with @c vlink_destroy_client().
 */
VLINK_C_API_EXPORT int vlink_create_client(const char* url, const vlink_schema_info_t* schema_info,
                                           vlink_client_handle_t* handle);

/**
 * @brief Destroys a Client node and releases every associated resource.
 *
 * @param handle  Client handle to destroy.  Must not be @c NULL.
 * @return        @c VLINK_RET_NO_ERROR on success;
 *                @c VLINK_RET_INVALID_ERROR on bad handle.
 *
 * @note Must not be called concurrently with any other operation on the same
 *       handle.
 */
VLINK_C_API_EXPORT int vlink_destroy_client(vlink_client_handle_t* handle);

/**
 * @brief Checks whether the Client is connected to a Server.
 *
 * @param handle  Client handle.
 * @return        @c VLINK_RET_NO_ERROR if connected;
 *                @c VLINK_RET_UNEXPECTED_ERROR if not yet connected;
 *                @c VLINK_RET_INVALID_ERROR on bad handle.
 *
 * @note Thread-safe.
 */
VLINK_C_API_EXPORT int vlink_has_server(const vlink_client_handle_t handle);

/**
 * @brief Blocks until a Server is available or @p timeout_ms expires.
 *
 * @param handle      Client handle.
 * @param timeout_ms  Maximum wait time in milliseconds.
 * @return            @c VLINK_RET_NO_ERROR if connected;
 *                    @c VLINK_RET_UNEXPECTED_ERROR on timeout;
 *                    @c VLINK_RET_INVALID_ERROR on bad handle.
 *
 * @note Blocks the calling thread.
 */
VLINK_C_API_EXPORT int vlink_wait_for_server(const vlink_client_handle_t handle, const int timeout_ms);

/**
 * @brief Registers a callback fired whenever the Server connection state changes.
 *
 * @param handle            Client handle.
 * @param connect_callback  Callback to invoke on every state change.
 * @param user_data         Opaque pointer forwarded to @p connect_callback.
 * @return                  @c VLINK_RET_NO_ERROR on success;
 *                          @c VLINK_RET_INVALID_ERROR on bad handle or a @c NULL
 *                          @p connect_callback.
 *
 * @note The callback runs on the Client's internal event thread.
 */
VLINK_C_API_EXPORT int vlink_detect_server(const vlink_client_handle_t handle,
                                           const vlink_connect_callback_t connect_callback, void* user_data);

/**
 * @brief Sends an RPC request and registers a callback for the response.
 *
 * @details
 * Internally invokes @c vlink::Client::invoke() with a shallow-copy @c Bytes
 * wrapping @p data.  @p resp_callback fires asynchronously on the underlying
 * @c vlink::Client callback context once the Server reply arrives.  Pass
 * @c NULL for @p resp_callback when the response is not needed.  Secure clients
 * treat an empty transport response as the protocol's empty response and do not
 * route it through @c Security::decrypt().
 *
 * @param handle         Client handle.
 * @param data           Request payload.  Must remain valid until the call returns.
 * @param size           Number of bytes in @p data.
 * @param resp_callback  Callback invoked with the response, or @c NULL.
 * @param user_data      Opaque pointer forwarded to @p resp_callback.
 * @return               @c VLINK_RET_NO_ERROR on success;
 *                       @c VLINK_RET_TRANSFER_ERROR if encryption or invoke
 *                       fails;
 *                       @c VLINK_RET_INVALID_ERROR on bad handle or
 *                       @p data == @c NULL with @p size > 0.
 *
 * @note Thread-safe.  The @p resp_callback may fire on a transport-managed
 *       thread.
 */
VLINK_C_API_EXPORT int vlink_invoke(const vlink_client_handle_t handle, const uint8_t* data, const size_t size,
                                    const vlink_resp_callback_t resp_callback, void* user_data);

/** @} */

////////////////////////////////////////////////////////////////
/// Setter
////////////////////////////////////////////////////////////////

/**
 * @name Field model -- Setter
 * @{
 */

/**
 * @brief Creates a Setter node and initialises it on the given URL.
 *
 * @param url          VLink field URL.  Must not be @c NULL.
 * @param schema_info  Optional bundled @c ser + @c schema metadata.
 * @param handle       Output handle.  Must not be @c NULL.
 * @return        @c VLINK_RET_NO_ERROR on success;
 *                @c VLINK_RET_INVALID_ERROR for @c NULL @p url / @p handle,
 *                partially-filled @p schema_info, or invalid
 *                @c schema_info->schema;
 *                @c VLINK_RET_MEMORY_ERROR on pool allocation failure;
 *                @c VLINK_RET_RUNTIME_ERROR on construction exception.
 *
 * @note Caller owns the handle and must release it with @c vlink_destroy_setter().
 */
VLINK_C_API_EXPORT int vlink_create_setter(const char* url, const vlink_schema_info_t* schema_info,
                                           vlink_setter_handle_t* handle);

/**
 * @brief Destroys a Setter node and releases every associated resource.
 *
 * @param handle  Setter handle to destroy.  Must not be @c NULL.
 * @return        @c VLINK_RET_NO_ERROR on success;
 *                @c VLINK_RET_INVALID_ERROR on bad handle.
 *
 * @note Must not be called concurrently with any other operation on the same
 *       handle.
 */
VLINK_C_API_EXPORT int vlink_destroy_setter(vlink_setter_handle_t* handle);

/**
 * @brief Publishes the latest field value.
 *
 * @details
 * The new value overwrites the previous one held by every matched Getter.  The
 * input buffer is only read during the @c vlink_set() call.  The underlying
 * @c Setter<vlink::Bytes> keeps its latest-value cache as an owned @c Bytes
 * copy, so callers may reuse or release @p data once the function returns.
 *
 * @param handle  Setter handle.
 * @param data    New field value bytes.  Must remain valid for the call.
 * @param size    Number of bytes in @p data.
 * @return        @c VLINK_RET_NO_ERROR on success;
 *                @c VLINK_RET_TRANSFER_ERROR if secure encryption fails;
 *                @c VLINK_RET_INVALID_ERROR on bad handle or
 *                @p data == @c NULL with @p size > 0.
 *
 * @note Thread-safe with respect to other @c vlink_set() calls on the same handle.
 */
VLINK_C_API_EXPORT int vlink_set(const vlink_setter_handle_t handle, const uint8_t* data, const size_t size);

/** @} */

////////////////////////////////////////////////////////////////
/// Getter
////////////////////////////////////////////////////////////////

/**
 * @name Field model -- Getter
 * @{
 */

/**
 * @brief Creates a Getter node, initialises it, and optionally registers a change callback.
 *
 * @details
 * Allocates a @c vlink::Getter<vlink::Bytes>.  When @p msg_callback is non-NULL,
 * @c listen() is called and the callback fires on every value update.  When
 * @p msg_callback is @c NULL the Getter operates in polling mode -- use
 * @c vlink_get() to retrieve the latest value.
 *
 * @param url           VLink field URL.  Must not be @c NULL.
 * @param schema_info   Optional bundled @c ser + @c schema metadata.
 * @param handle        Output handle.  Must not be @c NULL.
 * @param msg_callback  Push-mode callback, or @c NULL for poll mode.
 * @param user_data     Opaque pointer forwarded to @p msg_callback.
 * @return              @c VLINK_RET_NO_ERROR on success;
 *                      @c VLINK_RET_INVALID_ERROR for @c NULL @p url / @p handle,
 *                      partially-filled @p schema_info, or invalid
 *                      @c schema_info->schema;
 *                      @c VLINK_RET_MEMORY_ERROR on pool allocation failure;
 *                      @c VLINK_RET_TRANSFER_ERROR if @c listen() fails;
 *                      @c VLINK_RET_RUNTIME_ERROR on construction exception.
 *
 * @note Caller owns the handle and must release it with @c vlink_destroy_getter().
 */
VLINK_C_API_EXPORT int vlink_create_getter(const char* url, const vlink_schema_info_t* schema_info,
                                           vlink_getter_handle_t* handle, const vlink_msg_callback_t msg_callback,
                                           void* user_data);

/**
 * @brief Atomically creates a Getter, installs @c Security, and calls @c listen().
 *
 * @details
 * Builds the @c vlink::Security from @p security_cfg @b before the internal
 * push-mode @c listen() registration completes.  Polling-mode Getters
 * (@p msg_callback == @c NULL) see the attached @c Security from the very first
 * @c vlink_get() call.  A secure polling Getter caches the last authenticated
 * ciphertext/plaintext pair internally so repeated @c vlink_get() calls for the
 * same latest field value return the cached plaintext without tripping replay
 * protection.  Fresh inbound frames still flow through @c Security::decrypt().
 * Security configuration is one-shot at creation -- no separate runtime entry
 * point exists.
 *
 * @param url            VLink field URL.  Must not be @c NULL.
 * @param schema_info    Optional bundled @c ser + @c schema metadata.
 * @param handle         Output handle.  Must not be @c NULL.
 * @param msg_callback   Push-mode callback, or @c NULL for poll mode.
 * @param user_data      Opaque pointer forwarded to @p msg_callback.
 * @param security_cfg   Security configuration.  Must not be @c NULL.  A
 *                       zero-initialised configuration uses the built-in default
 *                       symmetric slot with replay protection disabled;
 *                       otherwise a decrypt-capable slot is required.
 * @return               @c VLINK_RET_NO_ERROR on success;
 *                       @c VLINK_RET_INVALID_ERROR on bad arguments;
 *                       @c VLINK_RET_MEMORY_ERROR on pool allocation failure;
 *                       @c VLINK_RET_TRANSFER_ERROR if @c listen() fails;
 *                       @c VLINK_RET_RUNTIME_ERROR on construction exception.
 *
 * @note Caller owns the handle and must release it with @c vlink_destroy_getter().
 */
VLINK_C_API_EXPORT int vlink_create_secure_getter(const char* url, const vlink_schema_info_t* schema_info,
                                                  vlink_getter_handle_t* handle,
                                                  const vlink_msg_callback_t msg_callback, void* user_data,
                                                  const vlink_security_config_t* security_cfg);

/**
 * @brief Destroys a Getter node and releases every associated resource.
 *
 * @param handle  Getter handle to destroy.  Must not be @c NULL.
 * @return        @c VLINK_RET_NO_ERROR on success;
 *                @c VLINK_RET_INVALID_ERROR on bad handle.
 *
 * @note Must not be called concurrently with any other operation on the same
 *       handle.
 */
VLINK_C_API_EXPORT int vlink_destroy_getter(vlink_getter_handle_t* handle);

/**
 * @brief Retrieves the latest field value into a caller-provided buffer.
 *
 * @details
 * Copies the current cached value into @p data.  On entry @c *size must hold
 * the buffer capacity; on success it is updated to the actual byte count.  When
 * the buffer is too small the function returns @c VLINK_RET_MEMORY_ERROR and
 * writes the required byte count into @c *size so the caller can allocate and
 * retry.  @p data is left unmodified in the error case.
 *
 * @param handle  Getter handle.
 * @param data    Output buffer.  Must not be @c NULL.
 * @param size    In/out: buffer capacity on entry; actual or required size on
 *                exit.  Must not be @c NULL.
 * @return        @c VLINK_RET_NO_ERROR on success;
 *                @c VLINK_RET_TRANSFER_ERROR if no value is available yet;
 *                @c VLINK_RET_MEMORY_ERROR if @c *size is too small (required
 *                size written back);
 *                @c VLINK_RET_INVALID_ERROR on bad arguments.
 *
 * @note Thread-safe.
 */
VLINK_C_API_EXPORT int vlink_get(const vlink_getter_handle_t handle, uint8_t* data, size_t* size);

/** @} */

////////////////////////////////////////////////////////////////
/// Security
////////////////////////////////////////////////////////////////

/**
 * @name Security -- standalone Security handle and node-side helpers
 * @{
 */

/**
 * @brief Opaque handle for a standalone @c Security instance.
 *
 * @details
 * Wraps a heap-allocated @c vlink::Security that performs authenticated,
 * message-level encryption.  Construct via @c vlink_security_create() and
 * destroy via @c vlink_security_destroy().  The same handle drives both
 * @c vlink_security_encrypt() and @c vlink_security_decrypt() as long as the
 * configuration supplies the matching key material for each direction.
 */
typedef struct vlink_security* vlink_security_handle_t;

/**
 * @brief Optional user-provided encrypt/decrypt callback for @c vlink_security_config_t.
 *
 * @details
 * Installing both @c encrypt_callback and @c decrypt_callback overrides the
 * built-in AEAD path entirely.  Implementations must allocate @c *out with
 * @c malloc (or another allocator compatible with @c free) and write the byte
 * count into @c *out_size.  The C API releases the buffer with @c free() after
 * copying its contents into the destination supplied to
 * @c vlink_security_encrypt() / @c vlink_security_decrypt().  Custom
 * encrypt/decrypt callbacks attached to the same security handle are serialised
 * by VLink; callbacks shared across handles must protect their own shared state.
 *
 * @param in        Plaintext (encrypt) or ciphertext (decrypt) input pointer.
 * @param in_size   Number of bytes available at @p in.
 * @param out       Output parameter receiving a freshly allocated buffer.
 * @param out_size  Output parameter receiving the byte count of @p out.
 * @param user      Opaque pointer supplied via @c callback_user_data.
 * @return          @c 0 on success, non-zero on failure.
 *
 * @note On Windows the buffer returned through @p out is released inside the
 *       vlink shared library using its own CRT @c free().  The callback
 *       implementation MUST therefore allocate @p *out with the matching CRT
 *       (e.g. the @c msvcrt / UCRT @c malloc linked into the vlink DLL).
 *       Mixing CRTs across DLL boundaries leads to heap corruption.  When in
 *       doubt, build the callback host with the same toolchain/runtime as the
 *       vlink shared library, or expose your own @c free helper through
 *       @c callback_user_data.
 */
typedef int (*vlink_security_callback_t)(const uint8_t* in, size_t in_size, uint8_t** out, size_t* out_size,
                                         void* user);

/**
 * @brief Low-frequency security options: AAD, replay protection, and signing keys.
 */
typedef struct {
  const char* aad_context;     /**< AEAD context binding (<=65535 bytes), or @c NULL.                */
  uint32_t replay_window;      /**< Replay window size; @c 0 disables replay checks.                 */
  const char* signing_key_pem; /**< Local RSA private key (PEM) for RSA-PSS signing, or @c NULL.     */
  const char* verify_key_pem;  /**< Peer RSA public key (PEM) for RSA-PSS verification, or @c NULL.  */
} vlink_security_advanced_config_t;

/**
 * @brief Configuration aggregate for @c vlink_security_create() and the
 *        @c vlink_create_secure_*() entry points.
 *
 * @details
 * Each field maps onto the field of the same name on @c vlink::Security::Config.
 * String fields are null-terminated; @c NULL or empty strings disable the
 * matching explicit field.  When every explicit cryptographic field is empty,
 * the configuration maps to the built-in default symmetric slot, provided
 * built-in algorithms are enabled.  A zero-initialised aggregate leaves
 * @c advanced.replay_window at @c 0, so replay checks are disabled until the
 * caller sets the field or calls @c vlink_security_config_init().
 * @c pbkdf2_salt is a raw byte buffer of @c pbkdf2_salt_size bytes; pass
 * @c NULL / @c 0 to leave it empty.  Setting @c pbkdf2_iterations to @c 0
 * selects the default (200000).
 *
 * @par Mode Selection
 * - When both @c encrypt_callback and @c decrypt_callback are non-NULL the
 *   custom-callback path overrides every other slot.
 * - When @c public_key_pem / @c private_key_pem are installed the RSA hybrid
 *   path drives outbound / inbound messages.
 * - Otherwise the symmetric path is used with a key derived from @c key,
 *   @c passphrase + @c pbkdf2_salt, or the built-in default.
 *
 * @note @c key / @c passphrase are the symmetric key sources.  @c advanced
 *       holds low-frequency options such as AAD, replay protection, and signing.
 */
struct vlink_security_config_s {
  const char* key;                            /**< Raw symmetric seed (SHA-256 truncated), or @c NULL.            */
  const char* passphrase;                     /**< Low-entropy passphrase fed into PBKDF2-HMAC-SHA256, or @c NULL.*/
  const uint8_t* pbkdf2_salt;                 /**< PBKDF2 salt (>=16 bytes), or @c NULL.                          */
  size_t pbkdf2_salt_size;                    /**< Byte count of @c pbkdf2_salt.                                  */
  uint32_t pbkdf2_iterations;                 /**< PBKDF2 iteration count; @c 0 means default (200000).           */
  const char* public_key_pem;                 /**< Peer RSA public key (PEM) for outbound encryption, or @c NULL. */
  const char* private_key_pem;                /**< Local RSA private key (PEM) for inbound decryption, or @c NULL.*/
  vlink_security_callback_t encrypt_callback; /**< Custom encrypt callback, or @c NULL.                           */
  vlink_security_callback_t decrypt_callback; /**< Custom decrypt callback, or @c NULL.                           */
  void* callback_user_data;                   /**< Opaque pointer forwarded to both callbacks.                    */
  vlink_security_advanced_config_t advanced;  /**< Low-frequency security options.                                */
};

/**
 * @brief Zero-initialises @p cfg and applies the C API default PBKDF2 / replay settings.
 *
 * @details
 * Convenience initialiser that avoids relying on @c {0} aggregate initialisation
 * in client code.  Once it returns, every string pointer is @c NULL, the salt
 * buffer is empty, both callbacks and @c callback_user_data are @c NULL,
 * @c pbkdf2_iterations equals 200000, and @c advanced.replay_window equals 4096.
 * Set @c advanced.replay_window back to @c 0 to disable replay checks
 * explicitly.  Safe to call on a stack variable before populating fields.
 * Passing the initialised configuration to a Security constructor uses the
 * built-in default symmetric slot when built-in algorithms are enabled.
 *
 * @par Example
 * @code
 * vlink_security_config_t cfg;
 * vlink_security_config_init(&cfg);
 * cfg.passphrase       = "correct horse battery staple";
 * cfg.pbkdf2_salt      = my_salt_bytes;
 * cfg.pbkdf2_salt_size = my_salt_size;
 * @endcode
 *
 * @param cfg  Configuration aggregate to initialise.  No-op when @p cfg is @c NULL.
 *
 * @note Safe to call on a freshly declared stack variable.
 */
VLINK_C_API_EXPORT void vlink_security_config_init(vlink_security_config_t* cfg);

/**
 * @brief Creates a standalone @c Security instance from @p cfg.
 *
 * @details
 * Allocates a @c vlink::Security on the heap.  When @p cfg is @c NULL the call
 * returns @c NULL and logs a warning.  A zero-initialised @p cfg uses the
 * built-in default symmetric slot when built-in algorithms are enabled, but it
 * leaves @c advanced.replay_window at @c 0 so replay protection is disabled.
 * Call @c vlink_security_config_init() to obtain the C API default PBKDF2/replay
 * settings.  Invalid PEM fields or weak RSA keys are logged via @c VLOG_W and
 * the offending slot is left empty as long as at least one other slot validated.
 *
 * @par Example
 * @code
 * vlink_security_config_t cfg;
 * vlink_security_config_init(&cfg);
 * cfg.passphrase       = "correct horse battery staple";
 * cfg.pbkdf2_salt      = my_salt_bytes;
 * cfg.pbkdf2_salt_size = my_salt_size;
 *
 * vlink_security_handle_t sec = vlink_security_create(&cfg);
 *
 * uint8_t* cipher = NULL;
 * size_t   cipher_size = 0;
 * vlink_security_encrypt(sec, plain, plain_size, &cipher, &cipher_size);
 * vlink_security_free_buffer(cipher);
 *
 * vlink_security_destroy(sec);
 * @endcode
 *
 * @param cfg  Configuration aggregate.  A zero-initialised aggregate uses the
 *             built-in default symmetric slot with replay protection disabled;
 *             otherwise provide a callback pair, symmetric key/passphrase, or
 *             RSA PEM.
 * @return     New @c vlink_security_handle_t handle; @c NULL on @c NULL @p cfg,
 *             on a configuration with no usable cryptographic slot after
 *             validation, on allocation failure, or on a C++ construction
 *             exception.
 *
 * @note Caller owns the handle and must release it via @c vlink_security_destroy().
 */
VLINK_C_API_EXPORT vlink_security_handle_t vlink_security_create(const vlink_security_config_t* cfg);

/**
 * @brief Destroys a standalone @c Security instance.
 *
 * @details
 * Safe to call with @c NULL @p sec (no-op).  Symmetric key material is zeroised
 * in place inside the @c vlink::Security destructor before the buffer is freed.
 *
 * @param sec  Handle returned by @c vlink_security_create(), or @c NULL.
 */
VLINK_C_API_EXPORT void vlink_security_destroy(vlink_security_handle_t sec);

/**
 * @brief Encrypts a plaintext buffer using the active mode configured on @p sec.
 *
 * @details
 * The ciphertext lands in a freshly allocated buffer that the caller owns;
 * release it with @c vlink_security_free_buffer().  Following the underlying
 * @c Security::encrypt() contract, empty inputs (@c in == @c NULL or
 * @c in_size == @c 0) are rejected with @c VLINK_RET_INVALID_ERROR.
 *
 * @param sec       Security handle.
 * @param in        Plaintext input bytes.
 * @param in_size   Number of bytes available at @p in.
 * @param out       Output pointer receiving the allocated ciphertext buffer.
 *                  Must not be @c NULL.
 * @param out_size  Output pointer receiving the ciphertext byte count.  Must
 *                  not be @c NULL.
 * @return          @c VLINK_RET_NO_ERROR on success;
 *                  @c VLINK_RET_INVALID_ERROR on bad arguments;
 *                  @c VLINK_RET_MEMORY_ERROR if output allocation fails;
 *                  @c VLINK_RET_TRANSFER_ERROR if @c Security::encrypt() fails.
 *
 * @note Caller owns @c *out and must release it via @c vlink_security_free_buffer().
 */
VLINK_C_API_EXPORT int vlink_security_encrypt(vlink_security_handle_t sec, const uint8_t* in, const size_t in_size,
                                              uint8_t** out, size_t* out_size);

/**
 * @brief Decrypts a ciphertext buffer using the active mode configured on @p sec.
 *
 * @details
 * The plaintext lands in a freshly allocated buffer that the caller owns;
 * release it with @c vlink_security_free_buffer().  Empty inputs are rejected
 * with @c VLINK_RET_INVALID_ERROR (a valid built-in ciphertext carries an
 * envelope, tag, and at least one ciphertext byte).  Authentication failures --
 * tampered ciphertext, wrong key, invalid signature, or replay -- are reported
 * as @c VLINK_RET_TRANSFER_ERROR.
 *
 * @param sec       Security handle.
 * @param in        Ciphertext input bytes.
 * @param in_size   Number of bytes available at @p in.
 * @param out       Output pointer receiving the allocated plaintext buffer.
 *                  Must not be @c NULL.
 * @param out_size  Output pointer receiving the plaintext byte count.  Must not
 *                  be @c NULL.
 * @return          @c VLINK_RET_NO_ERROR on success;
 *                  @c VLINK_RET_INVALID_ERROR on bad arguments;
 *                  @c VLINK_RET_MEMORY_ERROR if output allocation fails;
 *                  @c VLINK_RET_TRANSFER_ERROR if @c Security::decrypt() fails.
 *
 * @note Caller owns @c *out and must release it via @c vlink_security_free_buffer().
 */
VLINK_C_API_EXPORT int vlink_security_decrypt(vlink_security_handle_t sec, const uint8_t* in, const size_t in_size,
                                              uint8_t** out, size_t* out_size);

/**
 * @brief Releases a buffer returned by @c vlink_security_encrypt() or
 *        @c vlink_security_decrypt().
 *
 * @details
 * Safe to call with @c NULL @p buf (no-op).  Buffers obtained from any other
 * source must not be freed through this function.
 *
 * @param buf  Buffer pointer previously written by an encrypt/decrypt call.
 */
VLINK_C_API_EXPORT void vlink_security_free_buffer(uint8_t* buf);

/**
 * @brief Atomically creates a Publisher and installs @c Security.
 *
 * @details
 * Builds the @c vlink::Security from @p security_cfg @b before @c init() returns,
 * so the encrypt path is wired the first time @c vlink_publish() is called.
 * Security configuration is one-shot at creation -- no separate
 * @c enable_security() entry point exists.
 *
 * @param url           VLink topic URL.  Must not be @c NULL.
 * @param schema_info   Optional bundled @c ser + @c schema metadata.
 * @param handle        Output handle.  Must not be @c NULL.
 * @param security_cfg  Security configuration.  Must not be @c NULL.  A
 *                      zero-initialised configuration uses the built-in default
 *                      symmetric slot with replay protection disabled;
 *                      otherwise an encrypt-capable slot is required.
 * @return              @c VLINK_RET_NO_ERROR on success;
 *                      @c VLINK_RET_INVALID_ERROR on bad arguments (including a
 *                      non-encrypt-capable @p security_cfg);
 *                      @c VLINK_RET_MEMORY_ERROR on pool allocation failure;
 *                      @c VLINK_RET_RUNTIME_ERROR on construction exception.
 *
 * @note Caller owns the handle and must release it with @c vlink_destroy_publisher().
 */
VLINK_C_API_EXPORT int vlink_create_secure_publisher(const char* url, const vlink_schema_info_t* schema_info,
                                                     vlink_publisher_handle_t* handle,
                                                     const vlink_security_config_t* security_cfg);

/**
 * @brief Atomically creates a Client and installs @c Security.
 *
 * @details
 * Builds the @c vlink::Security from @p security_cfg @b before @c init() returns,
 * so outbound requests through @c vlink_invoke() are encrypted from the very
 * first call and inbound responses are decrypted before @c vlink_resp_callback_t
 * fires.
 *
 * @param url           VLink service URL.  Must not be @c NULL.
 * @param schema_info   Optional bundled @c ser + @c schema metadata.
 * @param handle        Output handle.  Must not be @c NULL.
 * @param security_cfg  Security configuration.  Must not be @c NULL.  A
 *                      zero-initialised configuration uses the built-in default
 *                      symmetric slot with replay protection disabled;
 *                      otherwise both encrypt- and decrypt-capable slots are
 *                      required.
 * @return              @c VLINK_RET_NO_ERROR on success;
 *                      @c VLINK_RET_INVALID_ERROR on bad arguments;
 *                      @c VLINK_RET_MEMORY_ERROR on pool allocation failure;
 *                      @c VLINK_RET_RUNTIME_ERROR on construction exception.
 *
 * @note Caller owns the handle and must release it with @c vlink_destroy_client().
 */
VLINK_C_API_EXPORT int vlink_create_secure_client(const char* url, const vlink_schema_info_t* schema_info,
                                                  vlink_client_handle_t* handle,
                                                  const vlink_security_config_t* security_cfg);

/**
 * @brief Atomically creates a Setter and installs @c Security.
 *
 * @details
 * Builds the @c vlink::Security from @p security_cfg @b before @c init() returns,
 * so the encrypt path is wired the first time @c vlink_set() is called.  The
 * Setter's normal latest-value cache owns the encrypted @c Bytes payload, so
 * late-joining Getters receive the current value without depending on the
 * caller's input buffer.
 *
 * @param url           VLink field URL.  Must not be @c NULL.
 * @param schema_info   Optional bundled @c ser + @c schema metadata.
 * @param handle        Output handle.  Must not be @c NULL.
 * @param security_cfg  Security configuration.  Must not be @c NULL.  A
 *                      zero-initialised configuration uses the built-in default
 *                      symmetric slot with replay protection disabled;
 *                      otherwise an encrypt-capable slot is required.
 * @return              @c VLINK_RET_NO_ERROR on success;
 *                      @c VLINK_RET_INVALID_ERROR on bad arguments;
 *                      @c VLINK_RET_MEMORY_ERROR on pool allocation failure;
 *                      @c VLINK_RET_RUNTIME_ERROR on construction exception.
 *
 * @note Caller owns the handle and must release it with @c vlink_destroy_setter().
 */
VLINK_C_API_EXPORT int vlink_create_secure_setter(const char* url, const vlink_schema_info_t* schema_info,
                                                  vlink_setter_handle_t* handle,
                                                  const vlink_security_config_t* security_cfg);

/** @} */

/**
 * @name Transport-layer TLS (SSL options)
 * @{
 */

/**
 * @brief Transport-layer TLS configuration aggregate.
 *
 * @details
 * Mirrors @c vlink::SslOptions.  Populated by the caller and passed to the
 * @c vlink_create_*_with_ssl_options() entry points.  String fields are
 * null-terminated; @c NULL or empty strings disable the matching slot.
 * @c verify_peer uses C semantics -- non-zero enables peer-certificate
 * verification, @c 0 disables it.  Transport backends consider TLS enabled once
 * at least @c ca_file or @c cert_file is non-empty.
 *
 * @note This is the transport-layer (channel) TLS configuration.  For
 *       application-layer per-message AEAD encryption see
 *       @c vlink_security_config_t.
 */
typedef struct {
  int verify_peer;          /**< Non-zero = verify peer certificate (default); @c 0 = skip. */
  const char* ca_file;      /**< CA certificate file path (PEM), or @c NULL.                */
  const char* cert_file;    /**< Client certificate file path (PEM), or @c NULL.            */
  const char* key_file;     /**< Client private key file path (PEM), or @c NULL.            */
  const char* key_password; /**< Passphrase for the encrypted private key, or @c NULL.      */
  const char* server_name;  /**< SNI server name override, or @c NULL.                      */
  const char* ciphers;      /**< Cipher suite string (OpenSSL format), or @c NULL.          */
} vlink_ssl_options_t;

/**
 * @brief Zero-initialises @p opt and applies the canonical TLS defaults.
 *
 * @details
 * After the call every string pointer is @c NULL and @c verify_peer equals @c 1
 * (matching @c vlink::SslOptions defaults).  Safe to call on a stack variable
 * before populating the fields you actually need.
 *
 * @par Example
 * @code
 * vlink_ssl_options_t opt;
 * vlink_ssl_options_init(&opt);
 * opt.ca_file   = "/etc/certs/ca.pem";
 * opt.cert_file = "/etc/certs/client.pem";
 * opt.key_file  = "/etc/certs/client-key.pem";
 * vlink_create_publisher_with_ssl_options("mqtt://sensor/data", &schema, &pub, &opt);
 * @endcode
 *
 * @param opt  Options aggregate to initialise.  No-op when @p opt is @c NULL.
 */
VLINK_C_API_EXPORT void vlink_ssl_options_init(vlink_ssl_options_t* opt);

/**
 * @brief Creates a Publisher and applies TLS options before transport initialisation.
 *
 * @param url          VLink topic URL.  Must not be @c NULL.
 * @param schema_info  Optional bundled @c ser + @c schema metadata.
 * @param handle       Output handle.  Must not be @c NULL.
 * @param opt          TLS options.  Must not be @c NULL.
 * @return             @c VLINK_RET_NO_ERROR on success;
 *                     @c VLINK_RET_INVALID_ERROR on bad arguments including
 *                     @p opt == @c NULL;
 *                     @c VLINK_RET_MEMORY_ERROR on pool allocation failure;
 *                     @c VLINK_RET_RUNTIME_ERROR on construction exception.
 *
 * @note Caller owns the handle and must release it with @c vlink_destroy_publisher().
 */
VLINK_C_API_EXPORT int vlink_create_publisher_with_ssl_options(const char* url, const vlink_schema_info_t* schema_info,
                                                               vlink_publisher_handle_t* handle,
                                                               const vlink_ssl_options_t* opt);

/**
 * @brief Creates a Subscriber and applies TLS options before transport initialisation.
 *
 * @param url           VLink topic URL.  Must not be @c NULL.
 * @param schema_info   Optional bundled @c ser + @c schema metadata.
 * @param handle        Output handle.  Must not be @c NULL.
 * @param msg_callback  Message handler.  Must not be @c NULL.
 * @param user_data     Opaque pointer forwarded to @p msg_callback.
 * @param opt           TLS options.  Must not be @c NULL.
 * @return              @c VLINK_RET_NO_ERROR on success;
 *                      @c VLINK_RET_INVALID_ERROR on bad arguments including
 *                      @p opt == @c NULL;
 *                      @c VLINK_RET_MEMORY_ERROR on pool allocation failure;
 *                      @c VLINK_RET_TRANSFER_ERROR if @c listen() fails;
 *                      @c VLINK_RET_RUNTIME_ERROR on construction exception.
 *
 * @note Caller owns the handle and must release it with @c vlink_destroy_subscriber().
 */
VLINK_C_API_EXPORT int vlink_create_subscriber_with_ssl_options(const char* url, const vlink_schema_info_t* schema_info,
                                                                vlink_subscriber_handle_t* handle,
                                                                const vlink_msg_callback_t msg_callback,
                                                                void* user_data, const vlink_ssl_options_t* opt);

/**
 * @brief Creates a Server and applies TLS options before transport initialisation.
 *
 * @param url           VLink service URL.  Must not be @c NULL.
 * @param schema_info   Optional bundled @c ser + @c schema metadata.
 * @param handle        Output handle.  Must not be @c NULL.
 * @param req_callback  Request handler.  Must not be @c NULL.
 * @param user_data     Opaque pointer forwarded to @p req_callback.
 * @param opt           TLS options.  Must not be @c NULL.
 * @return              @c VLINK_RET_NO_ERROR on success;
 *                      @c VLINK_RET_INVALID_ERROR on bad arguments including
 *                      @p opt == @c NULL;
 *                      @c VLINK_RET_MEMORY_ERROR on pool allocation failure;
 *                      @c VLINK_RET_TRANSFER_ERROR if @c listen() fails;
 *                      @c VLINK_RET_RUNTIME_ERROR on construction exception.
 *
 * @note Caller owns the handle and must release it with @c vlink_destroy_server().
 */
VLINK_C_API_EXPORT int vlink_create_server_with_ssl_options(const char* url, const vlink_schema_info_t* schema_info,
                                                            vlink_server_handle_t* handle,
                                                            const vlink_req_callback_t req_callback, void* user_data,
                                                            const vlink_ssl_options_t* opt);

/**
 * @brief Creates a Client and applies TLS options before transport initialisation.
 *
 * @param url          VLink service URL.  Must not be @c NULL.
 * @param schema_info  Optional bundled @c ser + @c schema metadata.
 * @param handle       Output handle.  Must not be @c NULL.
 * @param opt          TLS options.  Must not be @c NULL.
 * @return             @c VLINK_RET_NO_ERROR on success;
 *                     @c VLINK_RET_INVALID_ERROR on bad arguments including
 *                     @p opt == @c NULL;
 *                     @c VLINK_RET_MEMORY_ERROR on pool allocation failure;
 *                     @c VLINK_RET_RUNTIME_ERROR on construction exception.
 *
 * @note Caller owns the handle and must release it with @c vlink_destroy_client().
 */
VLINK_C_API_EXPORT int vlink_create_client_with_ssl_options(const char* url, const vlink_schema_info_t* schema_info,
                                                            vlink_client_handle_t* handle,
                                                            const vlink_ssl_options_t* opt);

/**
 * @brief Creates a Setter and applies TLS options before transport initialisation.
 *
 * @param url          VLink field URL.  Must not be @c NULL.
 * @param schema_info  Optional bundled @c ser + @c schema metadata.
 * @param handle       Output handle.  Must not be @c NULL.
 * @param opt          TLS options.  Must not be @c NULL.
 * @return             @c VLINK_RET_NO_ERROR on success;
 *                     @c VLINK_RET_INVALID_ERROR on bad arguments including
 *                     @p opt == @c NULL;
 *                     @c VLINK_RET_MEMORY_ERROR on pool allocation failure;
 *                     @c VLINK_RET_RUNTIME_ERROR on construction exception.
 *
 * @note Caller owns the handle and must release it with @c vlink_destroy_setter().
 */
VLINK_C_API_EXPORT int vlink_create_setter_with_ssl_options(const char* url, const vlink_schema_info_t* schema_info,
                                                            vlink_setter_handle_t* handle,
                                                            const vlink_ssl_options_t* opt);

/**
 * @brief Creates a Getter and applies TLS options before transport initialisation.
 *
 * @param url           VLink field URL.  Must not be @c NULL.
 * @param schema_info   Optional bundled @c ser + @c schema metadata.
 * @param handle        Output handle.  Must not be @c NULL.
 * @param msg_callback  Push-mode callback, or @c NULL for poll mode.
 * @param user_data     Opaque pointer forwarded to @p msg_callback.
 * @param opt           TLS options.  Must not be @c NULL.
 * @return              @c VLINK_RET_NO_ERROR on success;
 *                      @c VLINK_RET_INVALID_ERROR on bad arguments including
 *                      @p opt == @c NULL;
 *                      @c VLINK_RET_MEMORY_ERROR on pool allocation failure;
 *                      @c VLINK_RET_TRANSFER_ERROR if @c listen() fails;
 *                      @c VLINK_RET_RUNTIME_ERROR on construction exception.
 *
 * @note Caller owns the handle and must release it with @c vlink_destroy_getter().
 */
VLINK_C_API_EXPORT int vlink_create_getter_with_ssl_options(const char* url, const vlink_schema_info_t* schema_info,
                                                            vlink_getter_handle_t* handle,
                                                            const vlink_msg_callback_t msg_callback, void* user_data,
                                                            const vlink_ssl_options_t* opt);

/**
 * @brief Creates a secure Publisher and applies TLS options before transport initialisation.
 *
 * @param url           VLink topic URL.  Must not be @c NULL.
 * @param schema_info   Optional bundled @c ser + @c schema metadata.
 * @param handle        Output handle.  Must not be @c NULL.
 * @param security_cfg  Security configuration.  Must not be @c NULL.
 * @param opt           TLS options.  Must not be @c NULL.
 * @return              @c VLINK_RET_NO_ERROR on success;
 *                      @c VLINK_RET_INVALID_ERROR on bad arguments including
 *                      @p security_cfg == @c NULL or @p opt == @c NULL;
 *                      @c VLINK_RET_MEMORY_ERROR on pool allocation failure;
 *                      @c VLINK_RET_RUNTIME_ERROR on construction exception.
 *
 * @note Caller owns the handle and must release it with @c vlink_destroy_publisher().
 */
VLINK_C_API_EXPORT int vlink_create_secure_publisher_with_ssl_options(const char* url,
                                                                      const vlink_schema_info_t* schema_info,
                                                                      vlink_publisher_handle_t* handle,
                                                                      const vlink_security_config_t* security_cfg,
                                                                      const vlink_ssl_options_t* opt);

/**
 * @brief Creates a secure Subscriber and applies TLS options before transport initialisation.
 *
 * @param url           VLink subscriber URL.  Must not be @c NULL.
 * @param schema_info   Optional bundled @c ser + @c schema metadata.
 * @param handle        Output handle.  Must not be @c NULL.
 * @param msg_callback  Message handler.  Must not be @c NULL.
 * @param user_data     Opaque pointer forwarded to @p msg_callback.
 * @param security_cfg  Security configuration.  Must not be @c NULL.
 * @param opt           TLS options.  Must not be @c NULL.
 * @return              @c VLINK_RET_NO_ERROR on success;
 *                      @c VLINK_RET_INVALID_ERROR on bad arguments including
 *                      @p security_cfg == @c NULL or @p opt == @c NULL;
 *                      @c VLINK_RET_MEMORY_ERROR on pool allocation failure;
 *                      @c VLINK_RET_TRANSFER_ERROR if @c listen() fails;
 *                      @c VLINK_RET_RUNTIME_ERROR on construction exception.
 *
 * @note Caller owns the handle and must release it with @c vlink_destroy_subscriber().
 */
VLINK_C_API_EXPORT int vlink_create_secure_subscriber_with_ssl_options(
    const char* url, const vlink_schema_info_t* schema_info, vlink_subscriber_handle_t* handle,
    const vlink_msg_callback_t msg_callback, void* user_data, const vlink_security_config_t* security_cfg,
    const vlink_ssl_options_t* opt);

/**
 * @brief Creates a secure Server and applies TLS options before transport initialisation.
 *
 * @param url           VLink service URL.  Must not be @c NULL.
 * @param schema_info   Optional bundled @c ser + @c schema metadata.
 * @param handle        Output handle.  Must not be @c NULL.
 * @param req_callback  Request handler.  Must not be @c NULL.
 * @param user_data     Opaque pointer forwarded to @p req_callback.
 * @param security_cfg  Security configuration.  Must not be @c NULL.
 * @param opt           TLS options.  Must not be @c NULL.
 * @return              @c VLINK_RET_NO_ERROR on success;
 *                      @c VLINK_RET_INVALID_ERROR on bad arguments including
 *                      @p security_cfg == @c NULL or @p opt == @c NULL;
 *                      @c VLINK_RET_MEMORY_ERROR on pool allocation failure;
 *                      @c VLINK_RET_TRANSFER_ERROR if @c listen() fails;
 *                      @c VLINK_RET_RUNTIME_ERROR on construction exception.
 *
 * @note Caller owns the handle and must release it with @c vlink_destroy_server().
 */
VLINK_C_API_EXPORT int vlink_create_secure_server_with_ssl_options(
    const char* url, const vlink_schema_info_t* schema_info, vlink_server_handle_t* handle,
    const vlink_req_callback_t req_callback, void* user_data, const vlink_security_config_t* security_cfg,
    const vlink_ssl_options_t* opt);

/**
 * @brief Creates a secure Client and applies TLS options before transport initialisation.
 *
 * @param url           VLink service URL.  Must not be @c NULL.
 * @param schema_info   Optional bundled @c ser + @c schema metadata.
 * @param handle        Output handle.  Must not be @c NULL.
 * @param security_cfg  Security configuration.  Must not be @c NULL.
 * @param opt           TLS options.  Must not be @c NULL.
 * @return              @c VLINK_RET_NO_ERROR on success;
 *                      @c VLINK_RET_INVALID_ERROR on bad arguments including
 *                      @p security_cfg == @c NULL or @p opt == @c NULL;
 *                      @c VLINK_RET_MEMORY_ERROR on pool allocation failure;
 *                      @c VLINK_RET_RUNTIME_ERROR on construction exception.
 *
 * @note Caller owns the handle and must release it with @c vlink_destroy_client().
 */
VLINK_C_API_EXPORT int vlink_create_secure_client_with_ssl_options(const char* url,
                                                                   const vlink_schema_info_t* schema_info,
                                                                   vlink_client_handle_t* handle,
                                                                   const vlink_security_config_t* security_cfg,
                                                                   const vlink_ssl_options_t* opt);

/**
 * @brief Creates a secure Setter and applies TLS options before transport initialisation.
 *
 * @param url           VLink field URL.  Must not be @c NULL.
 * @param schema_info   Optional bundled @c ser + @c schema metadata.
 * @param handle        Output handle.  Must not be @c NULL.
 * @param security_cfg  Security configuration.  Must not be @c NULL.
 * @param opt           TLS options.  Must not be @c NULL.
 * @return              @c VLINK_RET_NO_ERROR on success;
 *                      @c VLINK_RET_INVALID_ERROR on bad arguments including
 *                      @p security_cfg == @c NULL or @p opt == @c NULL;
 *                      @c VLINK_RET_MEMORY_ERROR on pool allocation failure;
 *                      @c VLINK_RET_RUNTIME_ERROR on construction exception.
 *
 * @note Caller owns the handle and must release it with @c vlink_destroy_setter().
 */
VLINK_C_API_EXPORT int vlink_create_secure_setter_with_ssl_options(const char* url,
                                                                   const vlink_schema_info_t* schema_info,
                                                                   vlink_setter_handle_t* handle,
                                                                   const vlink_security_config_t* security_cfg,
                                                                   const vlink_ssl_options_t* opt);

/**
 * @brief Creates a secure Getter and applies TLS options before transport initialisation.
 *
 * @param url           VLink field URL.  Must not be @c NULL.
 * @param schema_info   Optional bundled @c ser + @c schema metadata.
 * @param handle        Output handle.  Must not be @c NULL.
 * @param msg_callback  Push-mode callback, or @c NULL for poll mode.
 * @param user_data     Opaque pointer forwarded to @p msg_callback.
 * @param security_cfg  Security configuration.  Must not be @c NULL.
 * @param opt           TLS options.  Must not be @c NULL.
 * @return              @c VLINK_RET_NO_ERROR on success;
 *                      @c VLINK_RET_INVALID_ERROR on bad arguments including
 *                      @p security_cfg == @c NULL or @p opt == @c NULL;
 *                      @c VLINK_RET_MEMORY_ERROR on pool allocation failure;
 *                      @c VLINK_RET_TRANSFER_ERROR if @c listen() fails;
 *                      @c VLINK_RET_RUNTIME_ERROR on construction exception.
 *
 * @note Caller owns the handle and must release it with @c vlink_destroy_getter().
 */
VLINK_C_API_EXPORT int vlink_create_secure_getter_with_ssl_options(
    const char* url, const vlink_schema_info_t* schema_info, vlink_getter_handle_t* handle,
    const vlink_msg_callback_t msg_callback, void* user_data, const vlink_security_config_t* security_cfg,
    const vlink_ssl_options_t* opt);

/**
 * @brief Applies TLS options to a Publisher handle.
 *
 * @details
 * Forwards @p opt to @c Node::set_ssl_options() on the underlying transport,
 * which writes the corresponding @c ssl.* property entries.  The transport
 * backend reads those entries during connection setup.  Handles returned by
 * this C API are already initialised, so this setter returns
 * @c VLINK_RET_RUNTIME_ERROR for normal C handles.  Use the matching
 * @c vlink_create_*_with_ssl_options() function when TLS must affect the
 * initial transport connection.
 *
 * @param handle  Publisher handle.  Must not be @c NULL.
 * @param opt     Options aggregate.  Must not be @c NULL.
 * @return        @c VLINK_RET_NO_ERROR on success for an uninitialised internal
 *                handle;
 *                @c VLINK_RET_INVALID_ERROR on bad arguments;
 *                @c VLINK_RET_RUNTIME_ERROR once the handle has been initialised.
 */
VLINK_C_API_EXPORT int vlink_publisher_set_ssl_options(vlink_publisher_handle_t* handle,
                                                       const vlink_ssl_options_t* opt);

/**
 * @brief Applies TLS options to a Subscriber handle.
 *
 * @param handle  Subscriber handle.  Must not be @c NULL.
 * @param opt     Options aggregate.  Must not be @c NULL.
 * @return        Same status codes as @c vlink_publisher_set_ssl_options().
 */
VLINK_C_API_EXPORT int vlink_subscriber_set_ssl_options(vlink_subscriber_handle_t* handle,
                                                        const vlink_ssl_options_t* opt);

/**
 * @brief Applies TLS options to a Server handle.
 *
 * @param handle  Server handle.  Must not be @c NULL.
 * @param opt     Options aggregate.  Must not be @c NULL.
 * @return        Same status codes as @c vlink_publisher_set_ssl_options().
 */
VLINK_C_API_EXPORT int vlink_server_set_ssl_options(vlink_server_handle_t* handle, const vlink_ssl_options_t* opt);

/**
 * @brief Applies TLS options to a Client handle.
 *
 * @param handle  Client handle.  Must not be @c NULL.
 * @param opt     Options aggregate.  Must not be @c NULL.
 * @return        Same status codes as @c vlink_publisher_set_ssl_options().
 */
VLINK_C_API_EXPORT int vlink_client_set_ssl_options(vlink_client_handle_t* handle, const vlink_ssl_options_t* opt);

/**
 * @brief Applies TLS options to a Setter handle.
 *
 * @param handle  Setter handle.  Must not be @c NULL.
 * @param opt     Options aggregate.  Must not be @c NULL.
 * @return        Same status codes as @c vlink_publisher_set_ssl_options().
 */
VLINK_C_API_EXPORT int vlink_setter_set_ssl_options(vlink_setter_handle_t* handle, const vlink_ssl_options_t* opt);

/**
 * @brief Applies TLS options to a Getter handle.
 *
 * @param handle  Getter handle.  Must not be @c NULL.
 * @param opt     Options aggregate.  Must not be @c NULL.
 * @return        Same status codes as @c vlink_publisher_set_ssl_options().
 */
VLINK_C_API_EXPORT int vlink_getter_set_ssl_options(vlink_getter_handle_t* handle, const vlink_ssl_options_t* opt);

/** @} */

#ifdef __cplusplus
}
#endif

// NOLINTEND
