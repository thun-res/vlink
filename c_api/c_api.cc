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

#include "./external/c_api.h"

#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "./base/memory_pool.h"
#include "./extension/security.h"
#include "./impl/ssl_options.h"
#include "./vlink.h"

static constexpr size_t kPoolBufferHeaderSize = sizeof(size_t);

struct vlink_security final {  // NOLINT(readability-identifier-naming)
  vlink::Security inner;

  explicit vlink_security(const vlink::Security::Config& cfg) : inner(cfg) {}
  explicit vlink_security(vlink::Security::Config&& cfg) : inner(std::move(cfg)) {}
};

template <typename T, typename... Args>
static T* pool_new(Args&&... args) {
  void* mem = vlink::MemoryPool::global_instance().allocate(sizeof(T), alignof(T));

  if VUNLIKELY (!mem) {
    return nullptr;
  }

  try {
    return ::new (mem) T{std::forward<Args>(args)...};
  } catch (...) {
    vlink::MemoryPool::global_instance().deallocate(mem, sizeof(T), alignof(T));
    throw;
  }
}

template <typename T>
static void pool_delete(T* obj) noexcept {
  if VUNLIKELY (!obj) {
    return;
  }

  obj->~T();

  vlink::MemoryPool::global_instance().deallocate(obj, sizeof(T), alignof(T));
}

static uint8_t* pool_alloc_buffer(size_t size) noexcept {
  if VUNLIKELY (size == 0U) {
    return nullptr;
  }

  if VUNLIKELY (size > std::numeric_limits<size_t>::max() - kPoolBufferHeaderSize) {
    return nullptr;
  }

  void* raw = vlink::MemoryPool::global_instance().allocate(kPoolBufferHeaderSize + size);

  if VUNLIKELY (raw == nullptr) {
    return nullptr;
  }

  *static_cast<size_t*>(raw) = size;

  return static_cast<uint8_t*>(raw) + kPoolBufferHeaderSize;
}

static void pool_free_buffer(uint8_t* ptr) noexcept {
  if VUNLIKELY (ptr == nullptr) {
    return;
  }

  uint8_t* raw = ptr - kPoolBufferHeaderSize;
  const size_t size = *reinterpret_cast<const size_t*>(raw);

  vlink::MemoryPool::global_instance().deallocate(raw, kPoolBufferHeaderSize + size);
}

template <typename T, typename... Args>
static std::shared_ptr<T>* shared_state_new(Args&&... args) {
  try {
    auto state = std::make_shared<T>(std::forward<Args>(args)...);
    return pool_new<std::shared_ptr<T>>(std::move(state));
  } catch (const std::bad_alloc&) {
    return nullptr;
  }
}

template <typename T>
static std::shared_ptr<T> shared_state_get(void* slot) {
  auto* holder = static_cast<std::shared_ptr<T>*>(slot);

  if VUNLIKELY (!holder) {
    return {};
  }

  return *holder;
}

template <typename T>
static void shared_state_delete(std::shared_ptr<T>*& holder) noexcept {
  pool_delete(holder);
  holder = nullptr;
}

template <typename T>
static void shared_state_delete(void*& slot) noexcept {
  auto* holder = static_cast<std::shared_ptr<T>*>(slot);
  pool_delete(holder);
  slot = nullptr;
}

struct c_api_security_state {  // NOLINT(readability-identifier-naming)
  vlink::Security security;

  explicit c_api_security_state(const vlink::Security::Config& cfg) : security(cfg) {}
  explicit c_api_security_state(vlink::Security::Config&& cfg) : security(std::move(cfg)) {}
};

static vlink::Security::Config build_security_config(const vlink_security_config_t* cfg) {
  vlink::Security::Config out;

  if VUNLIKELY (!cfg) {
    return out;
  }

  if (cfg->key && cfg->key[0] != '\0') {
    out.key.assign(cfg->key);
  }

  if (cfg->passphrase && cfg->passphrase[0] != '\0') {
    out.passphrase.assign(cfg->passphrase);
  }

  if (cfg->pbkdf2_salt && cfg->pbkdf2_salt_size > 0U) {
    out.pbkdf2_salt = vlink::Bytes::deep_copy(cfg->pbkdf2_salt, cfg->pbkdf2_salt_size);
  }

  if (cfg->pbkdf2_iterations != 0U) {
    out.pbkdf2_iterations = cfg->pbkdf2_iterations;
  }

  if (cfg->advanced.aad_context && cfg->advanced.aad_context[0] != '\0') {
    out.advanced.aad_context.assign(cfg->advanced.aad_context);
  }

  out.advanced.replay_window = cfg->advanced.replay_window;

  if (cfg->public_key_pem && cfg->public_key_pem[0] != '\0') {
    out.public_key_pem.assign(cfg->public_key_pem);
  }

  if (cfg->private_key_pem && cfg->private_key_pem[0] != '\0') {
    out.private_key_pem.assign(cfg->private_key_pem);
  }

  if (cfg->advanced.signing_key_pem && cfg->advanced.signing_key_pem[0] != '\0') {
    out.advanced.signing_key_pem.assign(cfg->advanced.signing_key_pem);
  }

  if (cfg->advanced.verify_key_pem && cfg->advanced.verify_key_pem[0] != '\0') {
    out.advanced.verify_key_pem.assign(cfg->advanced.verify_key_pem);
  }

  if (cfg->encrypt_callback && cfg->decrypt_callback) {
    auto* user = cfg->callback_user_data;
    auto enc = cfg->encrypt_callback;
    auto dec = cfg->decrypt_callback;

    if (enc) {
      out.encrypt_callback = [enc, user](const vlink::Bytes& in, vlink::Bytes& result) -> bool {
        uint8_t* buf = nullptr;
        size_t size = 0U;
        const int rc = enc(in.data(), in.size(), &buf, &size, user);

        if VUNLIKELY (rc != 0 || !buf) {
          if VUNLIKELY (buf) {
            std::free(buf);  // NOLINT(cppcoreguidelines-no-malloc)
          }

          return false;
        }

        result = vlink::Bytes::deep_copy(buf, size);
        std::free(buf);  // NOLINT(cppcoreguidelines-no-malloc)

        if VUNLIKELY (size > 0U && (result.data() == nullptr || result.size() != size)) {
          VLOG_W("vlink_security_config.encrypt_callback: deep_copy failed, size: ", size, ".");
          result = vlink::Bytes{};
          return false;
        }

        return true;
      };
    }
    if (dec) {
      out.decrypt_callback = [dec, user](const vlink::Bytes& in, vlink::Bytes& result) -> bool {
        uint8_t* buf = nullptr;
        size_t size = 0U;
        const int rc = dec(in.data(), in.size(), &buf, &size, user);

        if VUNLIKELY (rc != 0 || !buf) {
          if VUNLIKELY (buf) {
            std::free(buf);  // NOLINT(cppcoreguidelines-no-malloc)
          }

          return false;
        }

        result = vlink::Bytes::deep_copy(buf, size);
        std::free(buf);  // NOLINT(cppcoreguidelines-no-malloc)

        if VUNLIKELY (size > 0U && (result.data() == nullptr || result.size() != size)) {
          VLOG_W("vlink_security_config.decrypt_callback: deep_copy failed (size=%zu).", size);
          result = vlink::Bytes{};
          return false;
        }

        return true;
      };
    }
  } else if VUNLIKELY (static_cast<bool>(cfg->encrypt_callback) != static_cast<bool>(cfg->decrypt_callback)) {
    VLOG_W(
        "vlink_security_config: encrypt_callback and decrypt_callback must be installed as a pair; "
        "ignoring lone callback to match Security::Config semantics.");
  }

  if VUNLIKELY (out.key.empty() && out.passphrase.empty() && out.public_key_pem.empty() &&
                out.private_key_pem.empty() && out.advanced.signing_key_pem.empty() &&
                out.advanced.verify_key_pem.empty() && !out.encrypt_callback && !out.decrypt_callback) {
    VLOG_W("vlink security config is empty: encrypt/decrypt will silently fail until a real key is provided.");
  }

  return out;
}

static vlink::SslOptions build_ssl_options(const vlink_ssl_options_t* opt) {
  vlink::SslOptions out;

  if VUNLIKELY (!opt) {
    return out;
  }

  out.verify_peer = (opt->verify_peer != 0);

  if (opt->ca_file) {
    out.ca_file = opt->ca_file;
  }

  if (opt->cert_file) {
    out.cert_file = opt->cert_file;
  }

  if (opt->key_file) {
    out.key_file = opt->key_file;
  }

  if (opt->key_password) {
    out.key_password = opt->key_password;
  }

  if (opt->server_name) {
    out.server_name = opt->server_name;
  }

  if (opt->ciphers) {
    out.ciphers = opt->ciphers;
  }

  return out;
}

static std::string build_security_aad_context(const char* url, const vlink_schema_info_t* schema_info) {
  std::string context = url ? url : "";
  context += "|";

  if (schema_info && schema_info->ser && schema_info->ser[0] != '\0') {
    context += schema_info->ser;
  }

  context += "|";
  auto schema = VLINK_SCHEMA_RAW;

  if (schema_info && schema_info->ser && schema_info->ser[0] != '\0') {
    schema = schema_info->schema;
  }

  context += std::to_string(static_cast<uint32_t>(schema));

  return context;
}

static vlink::Security::Config build_node_security_config(const vlink_security_config_t* cfg, const char* url,
                                                          const vlink_schema_info_t* schema_info) {
  auto out = build_security_config(cfg);

  if (out.advanced.aad_context.empty()) {
    out.advanced.aad_context = build_security_aad_context(url, schema_info);
  }

  return out;
}

template <typename NodeT>
static bool apply_schema_info(NodeT& node, const vlink_schema_info_t* schema_info) {
  if (!schema_info) {
    return true;
  }

  if VUNLIKELY (!vlink::SchemaData::is_valid_type(static_cast<vlink::SchemaType>(schema_info->schema))) {
    return false;
  }

  const auto schema_type = static_cast<vlink::SchemaType>(schema_info->schema);

  const bool has_ser = schema_info->ser && schema_info->ser[0] != '\0';
  const bool has_schema = schema_type != vlink::SchemaType::kUnknown;

  if (!has_ser && !has_schema) {
    return true;
  }

  if VUNLIKELY (has_ser != has_schema) {
    return false;
  }

  node.set_ser_type(schema_info->ser, schema_type);
  return true;
}

template <typename NodeT>
static bool apply_create_options(NodeT& node, const vlink_schema_info_t* schema_info,
                                 const vlink_ssl_options_t* ssl_opt) {
  if VUNLIKELY (!apply_schema_info(node, schema_info)) {
    return false;
  }

  if (ssl_opt) {
    node.set_ssl_options(build_ssl_options(ssl_opt));
  }

  return true;
}

////////////////////////////////////////////////////////////////
/// Publisher
////////////////////////////////////////////////////////////////

static int create_publisher_impl(const char* url, const vlink_schema_info_t* schema_info,
                                 vlink_publisher_handle_t* handle, const vlink_ssl_options_t* ssl_opt) {
  if VUNLIKELY (!url || !handle) {
    return VLINK_RET_INVALID_ERROR;
  }

  vlink::Publisher<vlink::Bytes>* ptr = nullptr;

  try {
    ptr = pool_new<vlink::Publisher<vlink::Bytes>>(url, vlink::InitType::kWithoutInit);

    if VUNLIKELY (!ptr) {
      return VLINK_RET_MEMORY_ERROR;
    }

    if VUNLIKELY (!apply_create_options(*ptr, schema_info, ssl_opt)) {
      pool_delete(ptr);
      return VLINK_RET_INVALID_ERROR;
    }

    ptr->init();

    handle->native_handle = ptr;
    std::memset(static_cast<void*>(handle->reserved), 0, sizeof(handle->reserved));

    return VLINK_RET_NO_ERROR;
  } catch (std::exception&) {
    pool_delete(ptr);
    return VLINK_RET_RUNTIME_ERROR;
  }
}

int vlink_create_publisher(const char* url, const vlink_schema_info_t* schema_info, vlink_publisher_handle_t* handle) {
  return create_publisher_impl(url, schema_info, handle, nullptr);
}

static int create_secure_publisher_impl(const char* url, const vlink_schema_info_t* schema_info,
                                        vlink_publisher_handle_t* handle, const vlink_security_config_t* security_cfg,
                                        const vlink_ssl_options_t* ssl_opt) {
  if VUNLIKELY (!url || !handle || !security_cfg) {
    return VLINK_RET_INVALID_ERROR;
  }

  std::shared_ptr<c_api_security_state>* state_holder = nullptr;
  vlink::Publisher<vlink::Bytes>* ptr = nullptr;

  try {
    state_holder = shared_state_new<c_api_security_state>(build_node_security_config(security_cfg, url, schema_info));

    if VUNLIKELY (!state_holder) {
      return VLINK_RET_MEMORY_ERROR;
    }

    if VUNLIKELY (!(*state_holder)->security.can_encrypt()) {
      VLOG_W("vlink_create_secure_publisher: security_cfg cannot encrypt.");
      shared_state_delete(state_holder);
      return VLINK_RET_INVALID_ERROR;
    }

    ptr = pool_new<vlink::Publisher<vlink::Bytes>>(url, vlink::InitType::kWithoutInit);

    if VUNLIKELY (!ptr) {
      shared_state_delete(state_holder);
      return VLINK_RET_MEMORY_ERROR;
    }

    if VUNLIKELY (!apply_create_options(*ptr, schema_info, ssl_opt)) {
      pool_delete(ptr);
      shared_state_delete(state_holder);
      return VLINK_RET_INVALID_ERROR;
    }

    ptr->init();

    handle->native_handle = ptr;
    std::memset(static_cast<void*>(handle->reserved), 0, sizeof(handle->reserved));
    handle->reserved[4] = state_holder;

    return VLINK_RET_NO_ERROR;
  } catch (std::exception&) {
    pool_delete(ptr);
    shared_state_delete(state_holder);
    return VLINK_RET_RUNTIME_ERROR;
  }
}

int vlink_create_secure_publisher(const char* url, const vlink_schema_info_t* schema_info,
                                  vlink_publisher_handle_t* handle, const vlink_security_config_t* security_cfg) {
  return create_secure_publisher_impl(url, schema_info, handle, security_cfg, nullptr);
}

int vlink_destroy_publisher(vlink_publisher_handle_t* handle) {
  if VUNLIKELY (!handle || !handle->native_handle) {
    return VLINK_RET_INVALID_ERROR;
  }

  auto* ptr = static_cast<vlink::Publisher<vlink::Bytes>*>(handle->native_handle);

  if VUNLIKELY (!ptr) {
    return VLINK_RET_INVALID_ERROR;
  }

  pool_delete(ptr);
  handle->native_handle = nullptr;

  if (handle->reserved[4]) {
    shared_state_delete<c_api_security_state>(handle->reserved[4]);
  }

  return VLINK_RET_NO_ERROR;
}

int vlink_has_subscribers(const vlink_publisher_handle_t handle) {
  if VUNLIKELY (!handle.native_handle) {
    return VLINK_RET_INVALID_ERROR;
  }

  auto* ptr = static_cast<vlink::Publisher<vlink::Bytes>*>(handle.native_handle);

  if VUNLIKELY (!ptr) {
    return VLINK_RET_INVALID_ERROR;
  }

  if VUNLIKELY (!ptr->has_subscribers()) {
    return VLINK_RET_UNEXPECTED_ERROR;
  }

  return VLINK_RET_NO_ERROR;
}

int vlink_wait_for_subscribers(const vlink_publisher_handle_t handle, const int timeout) {
  if VUNLIKELY (!handle.native_handle) {
    return VLINK_RET_INVALID_ERROR;
  }

  auto* ptr = static_cast<vlink::Publisher<vlink::Bytes>*>(handle.native_handle);

  if VUNLIKELY (!ptr) {
    return VLINK_RET_INVALID_ERROR;
  }

  if VUNLIKELY (!ptr->wait_for_subscribers(std::chrono::milliseconds(timeout))) {
    return VLINK_RET_UNEXPECTED_ERROR;
  }

  return VLINK_RET_NO_ERROR;
}

int vlink_detect_subscribers(const vlink_publisher_handle_t handle, const vlink_connect_callback_t connect_callback,
                             void* user_data) {
  if VUNLIKELY (!handle.native_handle || !connect_callback) {
    return VLINK_RET_INVALID_ERROR;
  }

  auto* ptr = static_cast<vlink::Publisher<vlink::Bytes>*>(handle.native_handle);

  if VUNLIKELY (!ptr) {
    return VLINK_RET_INVALID_ERROR;
  }

  ptr->detect_subscribers([connect_callback, user_data](bool connected) { connect_callback(connected, user_data); });

  return VLINK_RET_NO_ERROR;
}

int vlink_publish(const vlink_publisher_handle_t handle, const uint8_t* data, const size_t size) {
  if VUNLIKELY (!handle.native_handle) {
    return VLINK_RET_INVALID_ERROR;
  }

  if VUNLIKELY (!data && size > 0U) {
    return VLINK_RET_INVALID_ERROR;
  }

  auto* ptr = static_cast<vlink::Publisher<vlink::Bytes>*>(handle.native_handle);

  if VUNLIKELY (!ptr) {
    return VLINK_RET_INVALID_ERROR;
  }

  auto sec_state = shared_state_get<c_api_security_state>(handle.reserved[4]);

  if VUNLIKELY (sec_state) {
    vlink::Bytes cipher;

    if VUNLIKELY (!sec_state->security.encrypt(vlink::Bytes::shallow_copy(data, size), cipher)) {
      VLOG_W("vlink_publish: encrypt failed.");
      return VLINK_RET_TRANSFER_ERROR;
    }

    if VUNLIKELY (!ptr->publish(cipher)) {
      return VLINK_RET_TRANSFER_ERROR;
    }

    return VLINK_RET_NO_ERROR;
  }

  if VUNLIKELY (!ptr->publish(vlink::Bytes::shallow_copy(data, size))) {
    return VLINK_RET_TRANSFER_ERROR;
  }

  return VLINK_RET_NO_ERROR;
}

int vlink_publish_by_force(const vlink_publisher_handle_t handle, const uint8_t* data, const size_t size) {
  if VUNLIKELY (!handle.native_handle) {
    return VLINK_RET_INVALID_ERROR;
  }

  if VUNLIKELY (!data && size > 0U) {
    return VLINK_RET_INVALID_ERROR;
  }

  auto* ptr = static_cast<vlink::Publisher<vlink::Bytes>*>(handle.native_handle);

  if VUNLIKELY (!ptr) {
    return VLINK_RET_INVALID_ERROR;
  }

  auto sec_state = shared_state_get<c_api_security_state>(handle.reserved[4]);

  if VUNLIKELY (sec_state) {
    vlink::Bytes cipher;

    if VUNLIKELY (!sec_state->security.encrypt(vlink::Bytes::shallow_copy(data, size), cipher)) {
      VLOG_W("vlink_publish_by_force: encrypt failed.");
      return VLINK_RET_TRANSFER_ERROR;
    }

    if VUNLIKELY (!ptr->publish(cipher, true)) {
      return VLINK_RET_TRANSFER_ERROR;
    }

    return VLINK_RET_NO_ERROR;
  }

  if VUNLIKELY (!ptr->publish(vlink::Bytes::shallow_copy(data, size), true)) {
    return VLINK_RET_TRANSFER_ERROR;
  }

  return VLINK_RET_NO_ERROR;
}

////////////////////////////////////////////////////////////////
/// Subscriber
////////////////////////////////////////////////////////////////

static int create_subscriber_impl(const char* url, const vlink_schema_info_t* schema_info,
                                  vlink_subscriber_handle_t* handle, const vlink_msg_callback_t msg_callback,
                                  void* user_data, const vlink_ssl_options_t* ssl_opt) {
  if VUNLIKELY (!url || !handle || !msg_callback) {
    return VLINK_RET_INVALID_ERROR;
  }

  vlink::Subscriber<vlink::Bytes>* ptr = nullptr;

  try {
    ptr = pool_new<vlink::Subscriber<vlink::Bytes>>(url, vlink::InitType::kWithoutInit);

    if VUNLIKELY (!ptr) {
      return VLINK_RET_MEMORY_ERROR;
    }

    if VUNLIKELY (!apply_create_options(*ptr, schema_info, ssl_opt)) {
      pool_delete(ptr);
      return VLINK_RET_INVALID_ERROR;
    }

    ptr->init();

    handle->native_handle = ptr;
    std::memset(static_cast<void*>(handle->reserved), 0, sizeof(handle->reserved));

    bool ret = ptr->listen(
        [msg_callback, user_data](const vlink::Bytes& data) { msg_callback(data.data(), data.size(), user_data); });

    if VUNLIKELY (!ret) {
      pool_delete(ptr);
      handle->native_handle = nullptr;
      return VLINK_RET_TRANSFER_ERROR;
    }

    return VLINK_RET_NO_ERROR;
  } catch (std::exception&) {
    pool_delete(ptr);
    handle->native_handle = nullptr;
    return VLINK_RET_RUNTIME_ERROR;
  }
}

int vlink_create_subscriber(const char* url, const vlink_schema_info_t* schema_info, vlink_subscriber_handle_t* handle,
                            const vlink_msg_callback_t msg_callback, void* user_data) {
  return create_subscriber_impl(url, schema_info, handle, msg_callback, user_data, nullptr);
}

static int create_secure_subscriber_impl(const char* url, const vlink_schema_info_t* schema_info,
                                         vlink_subscriber_handle_t* handle, const vlink_msg_callback_t msg_callback,
                                         void* user_data, const vlink_security_config_t* security_cfg,
                                         const vlink_ssl_options_t* ssl_opt) {
  if VUNLIKELY (!url || !handle || !msg_callback || !security_cfg) {
    return VLINK_RET_INVALID_ERROR;
  }

  std::shared_ptr<c_api_security_state>* state_holder = nullptr;
  vlink::Subscriber<vlink::Bytes>* ptr = nullptr;

  try {
    state_holder = shared_state_new<c_api_security_state>(build_node_security_config(security_cfg, url, schema_info));

    if VUNLIKELY (!state_holder) {
      return VLINK_RET_MEMORY_ERROR;
    }

    if VUNLIKELY (!(*state_holder)->security.can_decrypt()) {
      VLOG_W("vlink_create_secure_subscriber: security_cfg cannot decrypt.");
      shared_state_delete(state_holder);
      return VLINK_RET_INVALID_ERROR;
    }

    ptr = pool_new<vlink::Subscriber<vlink::Bytes>>(url, vlink::InitType::kWithoutInit);

    if VUNLIKELY (!ptr) {
      shared_state_delete(state_holder);
      return VLINK_RET_MEMORY_ERROR;
    }

    if VUNLIKELY (!apply_create_options(*ptr, schema_info, ssl_opt)) {
      pool_delete(ptr);
      shared_state_delete(state_holder);
      return VLINK_RET_INVALID_ERROR;
    }

    ptr->init();

    handle->native_handle = ptr;
    std::memset(static_cast<void*>(handle->reserved), 0, sizeof(handle->reserved));
    handle->reserved[4] = state_holder;

    auto state = *state_holder;
    bool ret = ptr->listen([state, msg_callback, user_data](const vlink::Bytes& data) {
      vlink::Bytes plain;

      if VUNLIKELY (!state->security.decrypt(data, plain)) {
        VLOG_W("vlink_subscriber: decrypt failed, message dropped.");
        return;
      }

      msg_callback(plain.data(), plain.size(), user_data);
    });

    if VUNLIKELY (!ret) {
      pool_delete(ptr);
      shared_state_delete(state_holder);
      handle->native_handle = nullptr;
      handle->reserved[4] = nullptr;
      return VLINK_RET_TRANSFER_ERROR;
    }

    return VLINK_RET_NO_ERROR;
  } catch (std::exception&) {
    pool_delete(ptr);
    shared_state_delete(state_holder);
    handle->native_handle = nullptr;
    std::memset(static_cast<void*>(handle->reserved), 0, sizeof(handle->reserved));
    return VLINK_RET_RUNTIME_ERROR;
  }
}

int vlink_create_secure_subscriber(const char* url, const vlink_schema_info_t* schema_info,
                                   vlink_subscriber_handle_t* handle, const vlink_msg_callback_t msg_callback,
                                   void* user_data, const vlink_security_config_t* security_cfg) {
  return create_secure_subscriber_impl(url, schema_info, handle, msg_callback, user_data, security_cfg, nullptr);
}

int vlink_destroy_subscriber(vlink_subscriber_handle_t* handle) {
  if VUNLIKELY (!handle || !handle->native_handle) {
    return VLINK_RET_INVALID_ERROR;
  }

  auto* ptr = static_cast<vlink::Subscriber<vlink::Bytes>*>(handle->native_handle);

  if VUNLIKELY (!ptr) {
    return VLINK_RET_INVALID_ERROR;
  }

  pool_delete(ptr);
  handle->native_handle = nullptr;

  if (handle->reserved[4]) {
    shared_state_delete<c_api_security_state>(handle->reserved[4]);
  }

  return VLINK_RET_NO_ERROR;
}

////////////////////////////////////////////////////////////////
/// Server
////////////////////////////////////////////////////////////////

struct server_state {  // NOLINT(readability-identifier-naming)
  std::mutex mutex;
  vlink::Bytes response;
  bool request_active{false};
  std::shared_ptr<c_api_security_state> security;

  server_state() = default;
  explicit server_state(std::shared_ptr<c_api_security_state> sec) : security(std::move(sec)) {}
};

static int create_server_impl(const char* url, const vlink_schema_info_t* schema_info, vlink_server_handle_t* handle,
                              const vlink_req_callback_t req_callback, void* user_data,
                              const vlink_ssl_options_t* ssl_opt) {
  if VUNLIKELY (!url || !handle || !req_callback) {
    return VLINK_RET_INVALID_ERROR;
  }

  vlink::Server<vlink::Bytes, vlink::Bytes>* ptr = nullptr;
  std::shared_ptr<server_state>* state_holder = nullptr;

  try {
    state_holder = shared_state_new<server_state>();

    if VUNLIKELY (!state_holder) {
      return VLINK_RET_MEMORY_ERROR;
    }

    ptr = pool_new<vlink::Server<vlink::Bytes, vlink::Bytes>>(url, vlink::InitType::kWithoutInit);

    if VUNLIKELY (!ptr) {
      shared_state_delete(state_holder);
      return VLINK_RET_MEMORY_ERROR;
    }

    if VUNLIKELY (!apply_create_options(*ptr, schema_info, ssl_opt)) {
      pool_delete(ptr);
      shared_state_delete(state_holder);
      return VLINK_RET_INVALID_ERROR;
    }

    ptr->init();

    handle->native_handle = ptr;
    std::memset(static_cast<void*>(handle->reserved), 0, sizeof(handle->reserved));
    handle->reserved[0] = state_holder;

    auto state = *state_holder;
    const std::string server_url = url;
    bool ret = ptr->listen(
        [state, server_url, req_callback, user_data](const vlink::Bytes& req_data, vlink::Bytes& resp_data) {
          std::unique_lock lock(state->mutex);

          state->request_active = true;
          state->response = vlink::Bytes{};

          if VUNLIKELY (state->security) {
            vlink::Bytes plain;

            if VUNLIKELY (!state->security->security.decrypt(req_data, plain)) {
              VLOG_W(
                  "vlink_server: decrypt request failed, dropping (url='%s'). vlink_reply on this request will return "
                  "VLINK_RET_RUNTIME_ERROR.",
                  server_url.c_str());

              state->request_active = false;
              return;
            }

            req_callback(plain.data(), plain.size(), user_data);
          } else {
            req_callback(req_data.data(), req_data.size(), user_data);
          }

          if (state->request_active) {
            state->request_active = false;
            state->response = vlink::Bytes{};
            return;
          }

          resp_data = std::move(state->response);
          state->response = vlink::Bytes{};
        });

    if VUNLIKELY (!ret) {
      shared_state_delete(state_holder);
      pool_delete(ptr);
      handle->native_handle = nullptr;

      std::memset(static_cast<void*>(handle->reserved), 0, sizeof(handle->reserved));

      return VLINK_RET_TRANSFER_ERROR;
    }

    return VLINK_RET_NO_ERROR;
  } catch (std::exception&) {
    pool_delete(ptr);
    shared_state_delete(state_holder);
    handle->native_handle = nullptr;
    std::memset(static_cast<void*>(handle->reserved), 0, sizeof(handle->reserved));
    return VLINK_RET_RUNTIME_ERROR;
  }
}

int vlink_create_server(const char* url, const vlink_schema_info_t* schema_info, vlink_server_handle_t* handle,
                        const vlink_req_callback_t req_callback, void* user_data) {
  return create_server_impl(url, schema_info, handle, req_callback, user_data, nullptr);
}

static int create_secure_server_impl(const char* url, const vlink_schema_info_t* schema_info,
                                     vlink_server_handle_t* handle, const vlink_req_callback_t req_callback,
                                     void* user_data, const vlink_security_config_t* security_cfg,
                                     const vlink_ssl_options_t* ssl_opt) {
  if VUNLIKELY (!url || !handle || !req_callback || !security_cfg) {
    return VLINK_RET_INVALID_ERROR;
  }

  std::shared_ptr<c_api_security_state>* sec_holder = nullptr;
  std::shared_ptr<server_state>* state_holder = nullptr;
  vlink::Server<vlink::Bytes, vlink::Bytes>* ptr = nullptr;

  try {
    sec_holder = shared_state_new<c_api_security_state>(build_node_security_config(security_cfg, url, schema_info));

    if VUNLIKELY (!sec_holder) {
      return VLINK_RET_MEMORY_ERROR;
    }

    if VUNLIKELY (!(*sec_holder)->security.can_encrypt() || !(*sec_holder)->security.can_decrypt()) {
      VLOG_W("vlink_create_secure_server: security_cfg cannot encrypt and decrypt.");
      shared_state_delete(sec_holder);
      return VLINK_RET_INVALID_ERROR;
    }

    state_holder = shared_state_new<server_state>(*sec_holder);

    if VUNLIKELY (!state_holder) {
      shared_state_delete(sec_holder);
      return VLINK_RET_MEMORY_ERROR;
    }

    ptr = pool_new<vlink::Server<vlink::Bytes, vlink::Bytes>>(url, vlink::InitType::kWithoutInit);

    if VUNLIKELY (!ptr) {
      shared_state_delete(state_holder);
      shared_state_delete(sec_holder);
      return VLINK_RET_MEMORY_ERROR;
    }

    if VUNLIKELY (!apply_create_options(*ptr, schema_info, ssl_opt)) {
      pool_delete(ptr);
      shared_state_delete(state_holder);
      shared_state_delete(sec_holder);
      return VLINK_RET_INVALID_ERROR;
    }

    ptr->init();

    handle->native_handle = ptr;
    std::memset(static_cast<void*>(handle->reserved), 0, sizeof(handle->reserved));
    handle->reserved[0] = state_holder;
    handle->reserved[4] = sec_holder;

    auto state = *state_holder;
    const std::string server_url = url;
    bool ret = ptr->listen(
        [state, server_url, req_callback, user_data](const vlink::Bytes& req_data, vlink::Bytes& resp_data) {
          std::unique_lock lock(state->mutex);

          state->request_active = true;
          state->response = vlink::Bytes{};

          vlink::Bytes plain;

          if VUNLIKELY (!state->security || !state->security->security.decrypt(req_data, plain)) {
            VLOG_W(
                "vlink_server: decrypt request failed, dropping (url='%s'). vlink_reply on this request will return "
                "VLINK_RET_RUNTIME_ERROR.",
                server_url.c_str());

            state->request_active = false;
            return;
          }

          req_callback(plain.data(), plain.size(), user_data);

          if (state->request_active) {
            state->request_active = false;
            state->response = vlink::Bytes{};
            return;
          }

          resp_data = std::move(state->response);
          state->response = vlink::Bytes{};
        });

    if VUNLIKELY (!ret) {
      shared_state_delete(state_holder);
      pool_delete(ptr);
      shared_state_delete(sec_holder);
      handle->native_handle = nullptr;

      std::memset(static_cast<void*>(handle->reserved), 0, sizeof(handle->reserved));

      return VLINK_RET_TRANSFER_ERROR;
    }

    return VLINK_RET_NO_ERROR;
  } catch (std::exception&) {
    pool_delete(ptr);
    shared_state_delete(state_holder);
    shared_state_delete(sec_holder);
    handle->native_handle = nullptr;
    std::memset(static_cast<void*>(handle->reserved), 0, sizeof(handle->reserved));
    return VLINK_RET_RUNTIME_ERROR;
  }
}

int vlink_create_secure_server(const char* url, const vlink_schema_info_t* schema_info, vlink_server_handle_t* handle,
                               const vlink_req_callback_t req_callback, void* user_data,
                               const vlink_security_config_t* security_cfg) {
  return create_secure_server_impl(url, schema_info, handle, req_callback, user_data, security_cfg, nullptr);
}

int vlink_destroy_server(vlink_server_handle_t* handle) {
  if VUNLIKELY (!handle || !handle->native_handle) {
    return VLINK_RET_INVALID_ERROR;
  }

  auto* ptr = static_cast<vlink::Server<vlink::Bytes, vlink::Bytes>*>(handle->native_handle);

  if VUNLIKELY (!ptr) {
    return VLINK_RET_INVALID_ERROR;
  }

  pool_delete(ptr);

  handle->native_handle = nullptr;

  if (handle->reserved[0]) {
    shared_state_delete<server_state>(handle->reserved[0]);
  }

  if (handle->reserved[4]) {
    shared_state_delete<c_api_security_state>(handle->reserved[4]);
  }

  std::memset(static_cast<void*>(handle->reserved), 0, sizeof(handle->reserved));

  return VLINK_RET_NO_ERROR;
}

int vlink_reply(vlink_server_handle_t* handle, const uint8_t* data, const size_t size) {
  if VUNLIKELY (!handle || !handle->native_handle) {
    return VLINK_RET_INVALID_ERROR;
  }

  if VUNLIKELY (!data && size > 0U) {
    return VLINK_RET_INVALID_ERROR;
  }

  auto* ptr = static_cast<vlink::Server<vlink::Bytes, vlink::Bytes>*>(handle->native_handle);

  if VUNLIKELY (!ptr) {
    return VLINK_RET_INVALID_ERROR;
  }

  auto state = shared_state_get<server_state>(handle->reserved[0]);

  if VUNLIKELY (!state) {
    return VLINK_RET_INVALID_ERROR;
  }

  if VUNLIKELY (!state->request_active) {
    return VLINK_RET_RUNTIME_ERROR;
  }

  if VUNLIKELY (state->security) {
    if (size == 0U) {
      state->response = vlink::Bytes{};
      state->request_active = false;

      return VLINK_RET_NO_ERROR;
    }

    vlink::Bytes cipher;

    if VUNLIKELY (!state->security->security.encrypt(vlink::Bytes::shallow_copy(data, size), cipher)) {
      VLOG_W("vlink_reply: encrypt response failed.");
      return VLINK_RET_TRANSFER_ERROR;
    }

    state->response = std::move(cipher);
    state->request_active = false;

    return VLINK_RET_NO_ERROR;
  }

  if VLIKELY (size > 0U) {
    auto response = vlink::Bytes::deep_copy(data, size);

    if VUNLIKELY (!response.data() || response.size() != size) {
      return VLINK_RET_MEMORY_ERROR;
    }

    state->response = std::move(response);
  } else {
    state->response = vlink::Bytes{};
  }

  state->request_active = false;

  return VLINK_RET_NO_ERROR;
}

////////////////////////////////////////////////////////////////
/// Client
////////////////////////////////////////////////////////////////

static int create_client_impl(const char* url, const vlink_schema_info_t* schema_info, vlink_client_handle_t* handle,
                              const vlink_ssl_options_t* ssl_opt) {
  if VUNLIKELY (!url || !handle) {
    return VLINK_RET_INVALID_ERROR;
  }

  vlink::Client<vlink::Bytes, vlink::Bytes>* ptr = nullptr;

  try {
    ptr = pool_new<vlink::Client<vlink::Bytes, vlink::Bytes>>(url, vlink::InitType::kWithoutInit);

    if VUNLIKELY (!ptr) {
      return VLINK_RET_MEMORY_ERROR;
    }

    if VUNLIKELY (!apply_create_options(*ptr, schema_info, ssl_opt)) {
      pool_delete(ptr);
      return VLINK_RET_INVALID_ERROR;
    }

    ptr->init();

    handle->native_handle = ptr;
    std::memset(static_cast<void*>(handle->reserved), 0, sizeof(handle->reserved));

    return VLINK_RET_NO_ERROR;
  } catch (std::exception&) {
    pool_delete(ptr);
    return VLINK_RET_RUNTIME_ERROR;
  }
}

int vlink_create_client(const char* url, const vlink_schema_info_t* schema_info, vlink_client_handle_t* handle) {
  return create_client_impl(url, schema_info, handle, nullptr);
}

static int create_secure_client_impl(const char* url, const vlink_schema_info_t* schema_info,
                                     vlink_client_handle_t* handle, const vlink_security_config_t* security_cfg,
                                     const vlink_ssl_options_t* ssl_opt) {
  if VUNLIKELY (!url || !handle || !security_cfg) {
    return VLINK_RET_INVALID_ERROR;
  }

  std::shared_ptr<c_api_security_state>* state_holder = nullptr;
  vlink::Client<vlink::Bytes, vlink::Bytes>* ptr = nullptr;

  try {
    state_holder = shared_state_new<c_api_security_state>(build_node_security_config(security_cfg, url, schema_info));

    if VUNLIKELY (!state_holder) {
      return VLINK_RET_MEMORY_ERROR;
    }

    if VUNLIKELY (!(*state_holder)->security.can_encrypt() || !(*state_holder)->security.can_decrypt()) {
      VLOG_W("vlink_create_secure_client: security_cfg cannot encrypt and decrypt.");
      shared_state_delete(state_holder);
      return VLINK_RET_INVALID_ERROR;
    }

    ptr = pool_new<vlink::Client<vlink::Bytes, vlink::Bytes>>(url, vlink::InitType::kWithoutInit);

    if VUNLIKELY (!ptr) {
      shared_state_delete(state_holder);
      return VLINK_RET_MEMORY_ERROR;
    }

    if VUNLIKELY (!apply_create_options(*ptr, schema_info, ssl_opt)) {
      pool_delete(ptr);
      shared_state_delete(state_holder);
      return VLINK_RET_INVALID_ERROR;
    }

    ptr->init();

    handle->native_handle = ptr;
    std::memset(static_cast<void*>(handle->reserved), 0, sizeof(handle->reserved));
    handle->reserved[4] = state_holder;

    return VLINK_RET_NO_ERROR;
  } catch (std::exception&) {
    pool_delete(ptr);
    shared_state_delete(state_holder);
    return VLINK_RET_RUNTIME_ERROR;
  }
}

int vlink_create_secure_client(const char* url, const vlink_schema_info_t* schema_info, vlink_client_handle_t* handle,
                               const vlink_security_config_t* security_cfg) {
  return create_secure_client_impl(url, schema_info, handle, security_cfg, nullptr);
}

int vlink_destroy_client(vlink_client_handle_t* handle) {
  if VUNLIKELY (!handle || !handle->native_handle) {
    return VLINK_RET_INVALID_ERROR;
  }

  auto* ptr = static_cast<vlink::Client<vlink::Bytes, vlink::Bytes>*>(handle->native_handle);

  if VUNLIKELY (!ptr) {
    return VLINK_RET_INVALID_ERROR;
  }

  pool_delete(ptr);
  handle->native_handle = nullptr;

  if (handle->reserved[4]) {
    shared_state_delete<c_api_security_state>(handle->reserved[4]);
  }

  return VLINK_RET_NO_ERROR;
}

int vlink_has_server(const vlink_client_handle_t handle) {
  if VUNLIKELY (!handle.native_handle) {
    return VLINK_RET_INVALID_ERROR;
  }

  auto* ptr = static_cast<vlink::Client<vlink::Bytes, vlink::Bytes>*>(handle.native_handle);

  if VUNLIKELY (!ptr) {
    return VLINK_RET_INVALID_ERROR;
  }

  if VUNLIKELY (!ptr->is_connected()) {
    return VLINK_RET_UNEXPECTED_ERROR;
  }

  return VLINK_RET_NO_ERROR;
}

int vlink_wait_for_server(const vlink_client_handle_t handle, const int timeout) {
  if VUNLIKELY (!handle.native_handle) {
    return VLINK_RET_INVALID_ERROR;
  }

  auto* ptr = static_cast<vlink::Client<vlink::Bytes, vlink::Bytes>*>(handle.native_handle);

  if VUNLIKELY (!ptr) {
    return VLINK_RET_INVALID_ERROR;
  }

  if VUNLIKELY (!ptr->wait_for_connected(std::chrono::milliseconds(timeout))) {
    return VLINK_RET_UNEXPECTED_ERROR;
  }

  return VLINK_RET_NO_ERROR;
}

int vlink_detect_server(const vlink_client_handle_t handle, const vlink_connect_callback_t connect_callback,
                        void* user_data) {
  if VUNLIKELY (!handle.native_handle || !connect_callback) {
    return VLINK_RET_INVALID_ERROR;
  }

  auto* ptr = static_cast<vlink::Client<vlink::Bytes, vlink::Bytes>*>(handle.native_handle);

  if VUNLIKELY (!ptr) {
    return VLINK_RET_INVALID_ERROR;
  }

  ptr->detect_connected([connect_callback, user_data](bool connected) { connect_callback(connected, user_data); });

  return VLINK_RET_NO_ERROR;
}

int vlink_invoke(const vlink_client_handle_t handle, const uint8_t* data, const size_t size,
                 const vlink_resp_callback_t resp_callback, void* user_data) {
  if VUNLIKELY (!handle.native_handle) {
    return VLINK_RET_INVALID_ERROR;
  }

  if VUNLIKELY (!data && size > 0U) {
    return VLINK_RET_INVALID_ERROR;
  }

  auto* ptr = static_cast<vlink::Client<vlink::Bytes, vlink::Bytes>*>(handle.native_handle);
  auto sec_state = shared_state_get<c_api_security_state>(handle.reserved[4]);

  vlink::Bytes payload;

  if VUNLIKELY (sec_state) {
    if VUNLIKELY (!sec_state->security.encrypt(vlink::Bytes::shallow_copy(data, size), payload)) {
      VLOG_W("vlink_invoke: encrypt request failed.");
      return VLINK_RET_TRANSFER_ERROR;
    }
  } else {
    payload = vlink::Bytes::shallow_copy(data, size);
  }

  const bool ret =
      ptr->invoke(payload, [sec_state = std::move(sec_state), resp_callback, user_data](const vlink::Bytes& resp_data) {
        if (!resp_callback) {
          return;
        }

        if VUNLIKELY (sec_state) {
          if (resp_data.empty()) {
            resp_callback(nullptr, 0U, user_data);
            return;
          }

          vlink::Bytes plain;

          if VUNLIKELY (!sec_state->security.decrypt(resp_data, plain)) {
            VLOG_W("vlink_invoke: decrypt response failed.");
            return;
          }

          resp_callback(plain.data(), plain.size(), user_data);

          return;
        }

        resp_callback(resp_data.data(), resp_data.size(), user_data);
      });

  if VUNLIKELY (!ret) {
    return VLINK_RET_TRANSFER_ERROR;
  }

  return VLINK_RET_NO_ERROR;
}

////////////////////////////////////////////////////////////////
/// Setter
////////////////////////////////////////////////////////////////

static int create_setter_impl(const char* url, const vlink_schema_info_t* schema_info, vlink_setter_handle_t* handle,
                              const vlink_ssl_options_t* ssl_opt) {
  if VUNLIKELY (!url || !handle) {
    return VLINK_RET_INVALID_ERROR;
  }

  vlink::Setter<vlink::Bytes>* ptr = nullptr;

  try {
    ptr = pool_new<vlink::Setter<vlink::Bytes>>(url, vlink::InitType::kWithoutInit);

    if VUNLIKELY (!ptr) {
      return VLINK_RET_MEMORY_ERROR;
    }

    if VUNLIKELY (!apply_create_options(*ptr, schema_info, ssl_opt)) {
      pool_delete(ptr);
      return VLINK_RET_INVALID_ERROR;
    }

    ptr->init();

    handle->native_handle = ptr;
    std::memset(static_cast<void*>(handle->reserved), 0, sizeof(handle->reserved));

    return VLINK_RET_NO_ERROR;
  } catch (std::exception&) {
    pool_delete(ptr);
    return VLINK_RET_RUNTIME_ERROR;
  }
}

int vlink_create_setter(const char* url, const vlink_schema_info_t* schema_info, vlink_setter_handle_t* handle) {
  return create_setter_impl(url, schema_info, handle, nullptr);
}

struct setter_security_state {  // NOLINT(readability-identifier-naming)
  vlink::Security security;
  std::mutex mutex;

  explicit setter_security_state(const vlink::Security::Config& cfg) : security(cfg) {}
  explicit setter_security_state(vlink::Security::Config&& cfg) : security(std::move(cfg)) {}
};

struct getter_security_state {  // NOLINT(readability-identifier-naming)
  vlink::Security security;
  std::vector<uint8_t> last_cipher;
  std::vector<uint8_t> last_plain;
  std::mutex mutex;

  explicit getter_security_state(const vlink::Security::Config& cfg) : security(cfg) {}
  explicit getter_security_state(vlink::Security::Config&& cfg) : security(std::move(cfg)) {}
};

static bool bytes_equal(const std::vector<uint8_t>& lhs, const vlink::Bytes& rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }

  if (lhs.empty()) {
    return true;
  }

  return rhs.data() != nullptr && std::memcmp(lhs.data(), rhs.data(), lhs.size()) == 0;
}

static void assign_bytes(std::vector<uint8_t>& out, const vlink::Bytes& in) {
  out.clear();

  if (!in.empty()) {
    out.assign(in.data(), in.data() + in.size());
  }
}

static bool decrypt_getter_value(getter_security_state* state, const vlink::Bytes& cipher,
                                 std::vector<uint8_t>& plain_out, bool allow_cached_replay) {
  if VUNLIKELY (!state) {
    return false;
  }

  std::lock_guard lock(state->mutex);

  if (allow_cached_replay && bytes_equal(state->last_cipher, cipher)) {
    plain_out = state->last_plain;
    return true;
  }

  vlink::Bytes plain;

  if VUNLIKELY (!state->security.decrypt(cipher, plain)) {
    return false;
  }

  assign_bytes(state->last_cipher, cipher);
  assign_bytes(state->last_plain, plain);
  plain_out = state->last_plain;

  return true;
}

static int create_secure_setter_impl(const char* url, const vlink_schema_info_t* schema_info,
                                     vlink_setter_handle_t* handle, const vlink_security_config_t* security_cfg,
                                     const vlink_ssl_options_t* ssl_opt) {
  if VUNLIKELY (!url || !handle || !security_cfg) {
    return VLINK_RET_INVALID_ERROR;
  }

  std::shared_ptr<setter_security_state>* state_holder = nullptr;
  vlink::Setter<vlink::Bytes>* ptr = nullptr;

  try {
    state_holder = shared_state_new<setter_security_state>(build_node_security_config(security_cfg, url, schema_info));

    if VUNLIKELY (!state_holder) {
      return VLINK_RET_MEMORY_ERROR;
    }

    if VUNLIKELY (!(*state_holder)->security.can_encrypt()) {
      VLOG_W("vlink_create_secure_setter: security_cfg cannot encrypt.");
      shared_state_delete(state_holder);
      return VLINK_RET_INVALID_ERROR;
    }

    ptr = pool_new<vlink::Setter<vlink::Bytes>>(url, vlink::InitType::kWithoutInit);

    if VUNLIKELY (!ptr) {
      shared_state_delete(state_holder);
      return VLINK_RET_MEMORY_ERROR;
    }

    if VUNLIKELY (!apply_create_options(*ptr, schema_info, ssl_opt)) {
      pool_delete(ptr);
      shared_state_delete(state_holder);
      return VLINK_RET_INVALID_ERROR;
    }

    ptr->init();

    handle->native_handle = ptr;
    std::memset(static_cast<void*>(handle->reserved), 0, sizeof(handle->reserved));
    handle->reserved[4] = state_holder;

    return VLINK_RET_NO_ERROR;
  } catch (std::exception&) {
    pool_delete(ptr);
    shared_state_delete(state_holder);
    handle->native_handle = nullptr;
    std::memset(static_cast<void*>(handle->reserved), 0, sizeof(handle->reserved));
    return VLINK_RET_RUNTIME_ERROR;
  }
}

int vlink_create_secure_setter(const char* url, const vlink_schema_info_t* schema_info, vlink_setter_handle_t* handle,
                               const vlink_security_config_t* security_cfg) {
  return create_secure_setter_impl(url, schema_info, handle, security_cfg, nullptr);
}

int vlink_destroy_setter(vlink_setter_handle_t* handle) {
  if VUNLIKELY (!handle || !handle->native_handle) {
    return VLINK_RET_INVALID_ERROR;
  }

  auto* ptr = static_cast<vlink::Setter<vlink::Bytes>*>(handle->native_handle);

  if VUNLIKELY (!ptr) {
    return VLINK_RET_INVALID_ERROR;
  }

  pool_delete(ptr);
  handle->native_handle = nullptr;

  if (handle->reserved[4]) {
    shared_state_delete<setter_security_state>(handle->reserved[4]);
  }

  return VLINK_RET_NO_ERROR;
}

int vlink_set(const vlink_setter_handle_t handle, const uint8_t* data, const size_t size) {
  if VUNLIKELY (!handle.native_handle) {
    return VLINK_RET_INVALID_ERROR;
  }

  if VUNLIKELY (!data && size > 0U) {
    return VLINK_RET_INVALID_ERROR;
  }

  auto* ptr = static_cast<vlink::Setter<vlink::Bytes>*>(handle.native_handle);

  if VUNLIKELY (!ptr) {
    return VLINK_RET_INVALID_ERROR;
  }

  auto state = shared_state_get<setter_security_state>(handle.reserved[4]);

  if VUNLIKELY (state) {
    std::lock_guard lock(state->mutex);
    vlink::Bytes cipher;

    if VUNLIKELY (!state->security.encrypt(vlink::Bytes::shallow_copy(data, size), cipher)) {
      VLOG_W("vlink_set: encrypt failed.");
      return VLINK_RET_TRANSFER_ERROR;
    }

    ptr->set(cipher);

    return VLINK_RET_NO_ERROR;
  }

  ptr->set(vlink::Bytes::shallow_copy(data, size));

  return VLINK_RET_NO_ERROR;
}

////////////////////////////////////////////////////////////////
/// Getter
////////////////////////////////////////////////////////////////

static int create_getter_impl(const char* url, const vlink_schema_info_t* schema_info, vlink_getter_handle_t* handle,
                              const vlink_msg_callback_t msg_callback, void* user_data,
                              const vlink_ssl_options_t* ssl_opt) {
  if VUNLIKELY (!url || !handle) {
    return VLINK_RET_INVALID_ERROR;
  }

  vlink::Getter<vlink::Bytes>* ptr = nullptr;

  try {
    ptr = pool_new<vlink::Getter<vlink::Bytes>>(url, vlink::InitType::kWithoutInit);

    if VUNLIKELY (!ptr) {
      return VLINK_RET_MEMORY_ERROR;
    }

    if VUNLIKELY (!apply_create_options(*ptr, schema_info, ssl_opt)) {
      pool_delete(ptr);
      return VLINK_RET_INVALID_ERROR;
    }

    ptr->init();

    handle->native_handle = ptr;

    std::memset(static_cast<void*>(handle->reserved), 0, sizeof(handle->reserved));

    if (msg_callback) {
      bool ret = ptr->listen(
          [msg_callback, user_data](const vlink::Bytes& data) { msg_callback(data.data(), data.size(), user_data); });

      if VUNLIKELY (!ret) {
        pool_delete(ptr);
        handle->native_handle = nullptr;
        return VLINK_RET_TRANSFER_ERROR;
      }
    }

    return VLINK_RET_NO_ERROR;
  } catch (std::exception&) {
    pool_delete(ptr);
    handle->native_handle = nullptr;
    return VLINK_RET_RUNTIME_ERROR;
  }
}

int vlink_create_getter(const char* url, const vlink_schema_info_t* schema_info, vlink_getter_handle_t* handle,
                        const vlink_msg_callback_t msg_callback, void* user_data) {
  return create_getter_impl(url, schema_info, handle, msg_callback, user_data, nullptr);
}

static int create_secure_getter_impl(const char* url, const vlink_schema_info_t* schema_info,
                                     vlink_getter_handle_t* handle, const vlink_msg_callback_t msg_callback,
                                     void* user_data, const vlink_security_config_t* security_cfg,
                                     const vlink_ssl_options_t* ssl_opt) {
  if VUNLIKELY (!url || !handle || !security_cfg) {
    return VLINK_RET_INVALID_ERROR;
  }

  std::shared_ptr<getter_security_state>* state_holder = nullptr;
  vlink::Getter<vlink::Bytes>* ptr = nullptr;

  try {
    state_holder = shared_state_new<getter_security_state>(build_node_security_config(security_cfg, url, schema_info));

    if VUNLIKELY (!state_holder) {
      return VLINK_RET_MEMORY_ERROR;
    }

    if VUNLIKELY (!(*state_holder)->security.can_decrypt()) {
      VLOG_W("vlink_create_secure_getter: security_cfg cannot decrypt.");
      shared_state_delete(state_holder);
      return VLINK_RET_INVALID_ERROR;
    }

    ptr = pool_new<vlink::Getter<vlink::Bytes>>(url, vlink::InitType::kWithoutInit);

    if VUNLIKELY (!ptr) {
      shared_state_delete(state_holder);
      return VLINK_RET_MEMORY_ERROR;
    }

    if VUNLIKELY (!apply_create_options(*ptr, schema_info, ssl_opt)) {
      pool_delete(ptr);
      shared_state_delete(state_holder);
      return VLINK_RET_INVALID_ERROR;
    }

    ptr->init();

    handle->native_handle = ptr;
    std::memset(static_cast<void*>(handle->reserved), 0, sizeof(handle->reserved));
    handle->reserved[4] = state_holder;

    if (msg_callback) {
      auto state = *state_holder;
      bool ret = ptr->listen([state, msg_callback, user_data](const vlink::Bytes& data) {
        std::vector<uint8_t> plain;

        if VUNLIKELY (!decrypt_getter_value(state.get(), data, plain, false)) {
          VLOG_W("vlink_getter: decrypt failed, update dropped.");
          return;
        }

        msg_callback(plain.data(), plain.size(), user_data);
      });

      if VUNLIKELY (!ret) {
        pool_delete(ptr);
        shared_state_delete(state_holder);
        handle->native_handle = nullptr;
        handle->reserved[4] = nullptr;
        return VLINK_RET_TRANSFER_ERROR;
      }
    }

    return VLINK_RET_NO_ERROR;
  } catch (std::exception&) {
    pool_delete(ptr);
    shared_state_delete(state_holder);
    handle->native_handle = nullptr;
    std::memset(static_cast<void*>(handle->reserved), 0, sizeof(handle->reserved));
    return VLINK_RET_RUNTIME_ERROR;
  }
}

int vlink_create_secure_getter(const char* url, const vlink_schema_info_t* schema_info, vlink_getter_handle_t* handle,
                               const vlink_msg_callback_t msg_callback, void* user_data,
                               const vlink_security_config_t* security_cfg) {
  return create_secure_getter_impl(url, schema_info, handle, msg_callback, user_data, security_cfg, nullptr);
}

int vlink_destroy_getter(vlink_getter_handle_t* handle) {
  if VUNLIKELY (!handle || !handle->native_handle) {
    return VLINK_RET_INVALID_ERROR;
  }

  auto* ptr = static_cast<vlink::Getter<vlink::Bytes>*>(handle->native_handle);

  if VUNLIKELY (!ptr) {
    return VLINK_RET_INVALID_ERROR;
  }

  pool_delete(ptr);
  handle->native_handle = nullptr;

  if (handle->reserved[4]) {
    shared_state_delete<getter_security_state>(handle->reserved[4]);
  }

  return VLINK_RET_NO_ERROR;
}

int vlink_get(const vlink_getter_handle_t handle, uint8_t* data, size_t* size) {
  if VUNLIKELY (!handle.native_handle) {
    return VLINK_RET_INVALID_ERROR;
  }

  if VUNLIKELY (!data || !size) {
    return VLINK_RET_INVALID_ERROR;
  }

  auto* ptr = static_cast<vlink::Getter<vlink::Bytes>*>(handle.native_handle);

  if VUNLIKELY (!ptr) {
    return VLINK_RET_INVALID_ERROR;
  }

  auto result = ptr->get();

  if VUNLIKELY (!result.has_value()) {
    return VLINK_RET_TRANSFER_ERROR;
  }

  const auto& val = result.value();

  auto state = shared_state_get<getter_security_state>(handle.reserved[4]);

  if VUNLIKELY (state) {
    std::vector<uint8_t> plain;

    if VUNLIKELY (!decrypt_getter_value(state.get(), val, plain, true)) {
      VLOG_W("vlink_get: decrypt failed.");
      return VLINK_RET_TRANSFER_ERROR;
    }

    if VUNLIKELY (plain.size() > *size) {
      *size = plain.size();
      return VLINK_RET_MEMORY_ERROR;
    }

    std::memcpy(data, plain.data(), plain.size());
    *size = plain.size();

    return VLINK_RET_NO_ERROR;
  }

  if VUNLIKELY (val.size() > *size) {
    *size = val.size();

    return VLINK_RET_MEMORY_ERROR;
  }

  std::memcpy(data, val.data(), val.size());

  *size = val.size();

  return VLINK_RET_NO_ERROR;
}

////////////////////////////////////////////////////////////////
/// Security
////////////////////////////////////////////////////////////////

void vlink_security_config_init(vlink_security_config_t* cfg) {
  if VUNLIKELY (cfg == nullptr) {
    return;
  }

  *cfg = vlink_security_config_t{};
  cfg->pbkdf2_iterations = 200000U;
  cfg->advanced.replay_window = 4096U;
}

vlink_security_handle_t vlink_security_create(const vlink_security_config_t* cfg) {
  if VUNLIKELY (cfg == nullptr) {
    VLOG_W("vlink_security_create: cfg is NULL; returning NULL.");
    return nullptr;
  }

  try {
    auto* sec = pool_new<vlink_security>(build_security_config(cfg));

    if VUNLIKELY (!sec) {
      return nullptr;
    }

    if VUNLIKELY (!sec->inner.is_configured()) {
      VLOG_W("vlink_security_create: config has no usable cryptographic slot; returning NULL.");
      pool_delete(sec);
      return nullptr;
    }

    return sec;
  } catch (std::exception&) {
    return nullptr;
  }
}

void vlink_security_destroy(vlink_security_handle_t sec) { pool_delete(sec); }

int vlink_security_encrypt(vlink_security_handle_t sec, const uint8_t* in, const size_t in_size, uint8_t** out,
                           size_t* out_size) {
  if VUNLIKELY (!sec || !out || !out_size) {
    return VLINK_RET_INVALID_ERROR;
  }

  *out = nullptr;
  *out_size = 0U;

  if VUNLIKELY (!in || in_size == 0U) {
    return VLINK_RET_INVALID_ERROR;
  }

  vlink::Bytes cipher;

  if VUNLIKELY (!sec->inner.encrypt(vlink::Bytes::shallow_copy(in, in_size), cipher)) {
    VLOG_W("vlink_security_encrypt: encrypt failed.");
    return VLINK_RET_TRANSFER_ERROR;
  }

  if VUNLIKELY (cipher.empty()) {
    *out = nullptr;
    *out_size = 0U;
    return VLINK_RET_NO_ERROR;
  }

  auto* buf = pool_alloc_buffer(cipher.size());

  if VUNLIKELY (!buf) {
    return VLINK_RET_MEMORY_ERROR;
  }

  std::memcpy(buf, cipher.data(), cipher.size());

  *out = buf;
  *out_size = cipher.size();

  return VLINK_RET_NO_ERROR;
}

int vlink_security_decrypt(vlink_security_handle_t sec, const uint8_t* in, const size_t in_size, uint8_t** out,
                           size_t* out_size) {
  if VUNLIKELY (!sec || !out || !out_size) {
    return VLINK_RET_INVALID_ERROR;
  }

  *out = nullptr;
  *out_size = 0U;

  if VUNLIKELY (!in || in_size == 0U) {
    return VLINK_RET_INVALID_ERROR;
  }

  vlink::Bytes plain;

  if VUNLIKELY (!sec->inner.decrypt(vlink::Bytes::shallow_copy(in, in_size), plain)) {
    VLOG_W("vlink_security_decrypt: decrypt failed.");
    return VLINK_RET_TRANSFER_ERROR;
  }

  if VUNLIKELY (plain.empty()) {
    *out = nullptr;
    *out_size = 0U;
    return VLINK_RET_NO_ERROR;
  }

  auto* buf = pool_alloc_buffer(plain.size());

  if VUNLIKELY (!buf) {
    return VLINK_RET_MEMORY_ERROR;
  }

  std::memcpy(buf, plain.data(), plain.size());

  *out = buf;
  *out_size = plain.size();

  return VLINK_RET_NO_ERROR;
}

void vlink_security_free_buffer(uint8_t* buf) { pool_free_buffer(buf); }

////////////////////////////////////////////////////////////////
/// SslOptions
////////////////////////////////////////////////////////////////

template <typename NodeT>
static int apply_ssl_options(void* native_handle, const vlink_ssl_options_t* opt) {
  if VUNLIKELY (!native_handle || !opt) {
    return VLINK_RET_INVALID_ERROR;
  }

  try {
    auto* ptr = static_cast<NodeT*>(native_handle);

    if VUNLIKELY (ptr->has_inited()) {
      VLOG_W("vlink_*_set_ssl_options: C API handles are already initialised; use *_with_ssl_options at create time.");
      return VLINK_RET_RUNTIME_ERROR;
    }

    ptr->set_ssl_options(build_ssl_options(opt));

    return VLINK_RET_NO_ERROR;
  } catch (std::exception&) {
    return VLINK_RET_RUNTIME_ERROR;
  }
}

void vlink_ssl_options_init(vlink_ssl_options_t* opt) {
  if VUNLIKELY (opt == nullptr) {
    return;
  }

  *opt = vlink_ssl_options_t{};
  opt->verify_peer = 1;
}

int vlink_create_publisher_with_ssl_options(const char* url, const vlink_schema_info_t* schema_info,
                                            vlink_publisher_handle_t* handle, const vlink_ssl_options_t* opt) {
  if VUNLIKELY (!opt) {
    return VLINK_RET_INVALID_ERROR;
  }

  return create_publisher_impl(url, schema_info, handle, opt);
}

int vlink_create_subscriber_with_ssl_options(const char* url, const vlink_schema_info_t* schema_info,
                                             vlink_subscriber_handle_t* handle, const vlink_msg_callback_t msg_callback,
                                             void* user_data, const vlink_ssl_options_t* opt) {
  if VUNLIKELY (!opt) {
    return VLINK_RET_INVALID_ERROR;
  }

  return create_subscriber_impl(url, schema_info, handle, msg_callback, user_data, opt);
}

int vlink_create_server_with_ssl_options(const char* url, const vlink_schema_info_t* schema_info,
                                         vlink_server_handle_t* handle, const vlink_req_callback_t req_callback,
                                         void* user_data, const vlink_ssl_options_t* opt) {
  if VUNLIKELY (!opt) {
    return VLINK_RET_INVALID_ERROR;
  }

  return create_server_impl(url, schema_info, handle, req_callback, user_data, opt);
}

int vlink_create_client_with_ssl_options(const char* url, const vlink_schema_info_t* schema_info,
                                         vlink_client_handle_t* handle, const vlink_ssl_options_t* opt) {
  if VUNLIKELY (!opt) {
    return VLINK_RET_INVALID_ERROR;
  }

  return create_client_impl(url, schema_info, handle, opt);
}

int vlink_create_setter_with_ssl_options(const char* url, const vlink_schema_info_t* schema_info,
                                         vlink_setter_handle_t* handle, const vlink_ssl_options_t* opt) {
  if VUNLIKELY (!opt) {
    return VLINK_RET_INVALID_ERROR;
  }

  return create_setter_impl(url, schema_info, handle, opt);
}

int vlink_create_getter_with_ssl_options(const char* url, const vlink_schema_info_t* schema_info,
                                         vlink_getter_handle_t* handle, const vlink_msg_callback_t msg_callback,
                                         void* user_data, const vlink_ssl_options_t* opt) {
  if VUNLIKELY (!opt) {
    return VLINK_RET_INVALID_ERROR;
  }

  return create_getter_impl(url, schema_info, handle, msg_callback, user_data, opt);
}

int vlink_create_secure_publisher_with_ssl_options(const char* url, const vlink_schema_info_t* schema_info,
                                                   vlink_publisher_handle_t* handle,
                                                   const vlink_security_config_t* security_cfg,
                                                   const vlink_ssl_options_t* opt) {
  if VUNLIKELY (!opt) {
    return VLINK_RET_INVALID_ERROR;
  }

  return create_secure_publisher_impl(url, schema_info, handle, security_cfg, opt);
}

int vlink_create_secure_subscriber_with_ssl_options(const char* url, const vlink_schema_info_t* schema_info,
                                                    vlink_subscriber_handle_t* handle,
                                                    const vlink_msg_callback_t msg_callback, void* user_data,
                                                    const vlink_security_config_t* security_cfg,
                                                    const vlink_ssl_options_t* opt) {
  if VUNLIKELY (!opt) {
    return VLINK_RET_INVALID_ERROR;
  }

  return create_secure_subscriber_impl(url, schema_info, handle, msg_callback, user_data, security_cfg, opt);
}

int vlink_create_secure_server_with_ssl_options(const char* url, const vlink_schema_info_t* schema_info,
                                                vlink_server_handle_t* handle, const vlink_req_callback_t req_callback,
                                                void* user_data, const vlink_security_config_t* security_cfg,
                                                const vlink_ssl_options_t* opt) {
  if VUNLIKELY (!opt) {
    return VLINK_RET_INVALID_ERROR;
  }

  return create_secure_server_impl(url, schema_info, handle, req_callback, user_data, security_cfg, opt);
}

int vlink_create_secure_client_with_ssl_options(const char* url, const vlink_schema_info_t* schema_info,
                                                vlink_client_handle_t* handle,
                                                const vlink_security_config_t* security_cfg,
                                                const vlink_ssl_options_t* opt) {
  if VUNLIKELY (!opt) {
    return VLINK_RET_INVALID_ERROR;
  }

  return create_secure_client_impl(url, schema_info, handle, security_cfg, opt);
}

int vlink_create_secure_setter_with_ssl_options(const char* url, const vlink_schema_info_t* schema_info,
                                                vlink_setter_handle_t* handle,
                                                const vlink_security_config_t* security_cfg,
                                                const vlink_ssl_options_t* opt) {
  if VUNLIKELY (!opt) {
    return VLINK_RET_INVALID_ERROR;
  }

  return create_secure_setter_impl(url, schema_info, handle, security_cfg, opt);
}

int vlink_create_secure_getter_with_ssl_options(const char* url, const vlink_schema_info_t* schema_info,
                                                vlink_getter_handle_t* handle, const vlink_msg_callback_t msg_callback,
                                                void* user_data, const vlink_security_config_t* security_cfg,
                                                const vlink_ssl_options_t* opt) {
  if VUNLIKELY (!opt) {
    return VLINK_RET_INVALID_ERROR;
  }

  return create_secure_getter_impl(url, schema_info, handle, msg_callback, user_data, security_cfg, opt);
}

int vlink_publisher_set_ssl_options(vlink_publisher_handle_t* handle, const vlink_ssl_options_t* opt) {
  if VUNLIKELY (!handle) {
    return VLINK_RET_INVALID_ERROR;
  }

  return apply_ssl_options<vlink::Publisher<vlink::Bytes>>(handle->native_handle, opt);
}

int vlink_subscriber_set_ssl_options(vlink_subscriber_handle_t* handle, const vlink_ssl_options_t* opt) {
  if VUNLIKELY (!handle) {
    return VLINK_RET_INVALID_ERROR;
  }

  return apply_ssl_options<vlink::Subscriber<vlink::Bytes>>(handle->native_handle, opt);
}

int vlink_server_set_ssl_options(vlink_server_handle_t* handle, const vlink_ssl_options_t* opt) {
  if VUNLIKELY (!handle) {
    return VLINK_RET_INVALID_ERROR;
  }

  return apply_ssl_options<vlink::Server<vlink::Bytes, vlink::Bytes>>(handle->native_handle, opt);
}

int vlink_client_set_ssl_options(vlink_client_handle_t* handle, const vlink_ssl_options_t* opt) {
  if VUNLIKELY (!handle) {
    return VLINK_RET_INVALID_ERROR;
  }

  return apply_ssl_options<vlink::Client<vlink::Bytes, vlink::Bytes>>(handle->native_handle, opt);
}

int vlink_setter_set_ssl_options(vlink_setter_handle_t* handle, const vlink_ssl_options_t* opt) {
  if VUNLIKELY (!handle) {
    return VLINK_RET_INVALID_ERROR;
  }

  return apply_ssl_options<vlink::Setter<vlink::Bytes>>(handle->native_handle, opt);
}

int vlink_getter_set_ssl_options(vlink_getter_handle_t* handle, const vlink_ssl_options_t* opt) {
  if VUNLIKELY (!handle) {
    return VLINK_RET_INVALID_ERROR;
  }

  return apply_ssl_options<vlink::Getter<vlink::Bytes>>(handle->native_handle, opt);
}
