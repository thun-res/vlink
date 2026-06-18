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

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "../base/logger.h"
#include "../impl/types.h"
#include "../node.h"
#include "../version.h"

namespace vlink {

template <typename ImplT, SecurityType SecT>
inline bool Node<ImplT, SecT>::init() {
  if constexpr (SecT == SecurityType::kWithSecurity) {
    if VUNLIKELY (!impl_->security) {
      VLOG_F("Node::init(): security node has no usable Security; check Security::Config. url: ", impl_->url);
    }
  }

  bool expected = false;

  if VUNLIKELY (!has_inited_.compare_exchange_strong(expected, true)) {
    return false;
  }

  impl_->check_version(Version{VLINK_VERSION_MAJOR, VLINK_VERSION_MINOR, VLINK_VERSION_PATCH});

  impl_->init();
  impl_->init_ext();

  is_support_loan_ = impl_->is_support_loan();

  return true;
}

template <typename ImplT, SecurityType SecT>
inline bool Node<ImplT, SecT>::deinit() {
  bool expected = true;

  if VUNLIKELY (!has_inited_.compare_exchange_strong(expected, false)) {
    return false;
  }

  interrupt();  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)

  if (quit_mtx_.has_value()) {
    std::lock_guard quit_lock(quit_mtx_.value());
    impl_->deinit();
    impl_->deinit_ext();
  } else {
    impl_->deinit();
    impl_->deinit_ext();
  }

  return true;
}

template <typename ImplT, SecurityType SecT>
inline bool Node<ImplT, SecT>::has_inited() const {
  return has_inited_;
}

template <typename ImplT, SecurityType SecT>
inline bool Node<ImplT, SecT>::is_support_loan() const {
  return impl_->is_support_loan();
}

template <typename ImplT, SecurityType SecT>
inline Bytes Node<ImplT, SecT>::loan(int64_t size) {
  Bytes bytes = impl_->loan(size);

  return bytes;
}

template <typename ImplT, SecurityType SecT>
inline bool Node<ImplT, SecT>::return_loan(const Bytes& bytes) {
  return impl_->return_loan(bytes);
}

template <typename ImplT, SecurityType SecT>
inline void Node<ImplT, SecT>::set_manual_unloan(bool manual_unloan) {
  (void)manual_unloan;
  VLOG_W("Node: Function [set_manual_unloan] is not supported.");
}

template <typename ImplT, SecurityType SecT>
inline bool Node<ImplT, SecT>::is_manual_unloan() const {
  return is_manual_unloan_;
}

template <typename ImplT, SecurityType SecT>
inline bool Node<ImplT, SecT>::suspend() {
  return impl_->suspend();
}

template <typename ImplT, SecurityType SecT>
inline bool Node<ImplT, SecT>::resume() {
  return impl_->resume();
}

template <typename ImplT, SecurityType SecT>
inline bool Node<ImplT, SecT>::is_suspend() const {
  return impl_->is_suspend();
}

template <typename ImplT, SecurityType SecT>
inline bool Node<ImplT, SecT>::attach(class MessageLoop* message_loop) {
  return impl_->attach(message_loop);
}

template <typename ImplT, SecurityType SecT>
inline bool Node<ImplT, SecT>::detach() {
  return impl_->detach();
}

template <typename ImplT, SecurityType SecT>
inline class MessageLoop* Node<ImplT, SecT>::get_message_loop() const {
  return impl_->get_message_loop();
}

template <typename ImplT, SecurityType SecT>
inline void Node<ImplT, SecT>::interrupt() {
  return impl_->interrupt();
}

template <typename ImplT, SecurityType SecT>
inline const AbstractNode* Node<ImplT, SecT>::get_abstract_node() const {
  return impl_->get_abstract_node();
}

template <typename ImplT, SecurityType SecT>
inline Status::BasePtr Node<ImplT, SecT>::get_status(Status::Type type) const {
  return impl_->get_status(type);
}

template <typename ImplT, SecurityType SecT>
inline void Node<ImplT, SecT>::register_status_handler(StatusCallback&& callback) {
  impl_->register_status_handler(std::move(callback));
}

template <typename ImplT, SecurityType SecT>
inline void Node<ImplT, SecT>::set_property(const std::string& prop, const std::string& value) {
  impl_->set_property(prop, value);
}

template <typename ImplT, SecurityType SecT>
inline std::string Node<ImplT, SecT>::get_property(const std::string& prop) const {
  return impl_->get_property(prop);
}

template <typename ImplT, SecurityType SecT>
inline TransportType Node<ImplT, SecT>::get_transport_type() const {
  return impl_->transport_type;
}

template <typename ImplT, SecurityType SecT>
inline const std::string& Node<ImplT, SecT>::get_url() const {
  return impl_->url;
}

template <typename ImplT, SecurityType SecT>
inline void Node<ImplT, SecT>::set_record_path(const std::string& path) {
  if VUNLIKELY (impl_->transport_type == TransportType::kIntra ||
                (impl_->transport_type == TransportType::kDds && impl_->is_cdr_type)) {
    VLOG_F("Node: Intra or Dds(cdr) type does not support record.");
    return;
  }

  impl_->set_record_path(path);
}

template <typename ImplT, SecurityType SecT>
inline void Node<ImplT, SecT>::set_ser_type(const std::string& ser_type, SchemaType schema_type) {
  auto next_schema_type = impl_->schema_type;

  if (ser_type.empty()) {
    next_schema_type = SchemaType::kUnknown;
  } else if (SchemaData::is_valid_type(schema_type) && schema_type != SchemaType::kUnknown) {
    next_schema_type = schema_type;
  } else {
    const auto inferred_schema_type = SchemaData::infer_ser_type(ser_type);

    if VLIKELY (inferred_schema_type != SchemaType::kUnknown) {
      next_schema_type = inferred_schema_type;
    } else if (impl_->schema_type == SchemaType::kRaw || impl_->schema_type == SchemaType::kZeroCopy) {
      next_schema_type = SchemaType::kUnknown;
    }
  }

  const bool ser_changed = impl_->ser_type != ser_type;
  const bool schema_changed = impl_->schema_type != next_schema_type;

  if VLIKELY (!ser_changed && !schema_changed) {
    return;
  }

  if VUNLIKELY (ser_changed && !impl_->ser_type.empty() && !ser_type.empty()) {
    CLOG_W("Node: Enforce serialization type [%s] => [%s].", impl_->ser_type.c_str(), ser_type.c_str());
  }

  if VUNLIKELY (schema_changed && SchemaData::is_real_type(impl_->schema_type) &&
                SchemaData::is_real_type(next_schema_type)) {
    CLOG_W("Node: Enforce schema type [%d] => [%d].", static_cast<int>(impl_->schema_type),
           static_cast<int>(next_schema_type));
  }

  if VUNLIKELY (has_inited_) {
    impl_->deinit_ext();
  }

  impl_->ser_type = ser_type;
  impl_->schema_type = next_schema_type;

  if VUNLIKELY (has_inited_) {
    impl_->init_ext();
  }
}

template <typename ImplT, SecurityType SecT>
inline const std::string& Node<ImplT, SecT>::get_ser_type() const {
  return impl_->ser_type;
}

template <typename ImplT, SecurityType SecT>
inline SchemaType Node<ImplT, SecT>::get_schema_type() const {
  return impl_->schema_type;
}

template <typename ImplT, SecurityType SecT>
inline void Node<ImplT, SecT>::set_discovery_enabled(bool enable) {
  if VUNLIKELY (has_inited_) {
    impl_->deinit_ext();
    impl_->set_discovery_enabled(enable);
    impl_->init_ext();
  } else {
    impl_->set_discovery_enabled(enable);
  }
}

template <typename ImplT, SecurityType SecT>
inline bool Node<ImplT, SecT>::get_discovery_enabled() const {
  return impl_->get_discovery_enabled();
}

template <typename ImplT, SecurityType SecT>
inline void Node<ImplT, SecT>::bind_proto_arena(void* proto_arena) {
  proto_arena_ = proto_arena;
}

template <typename ImplT, SecurityType SecT>
inline double Node<ImplT, SecT>::get_cpu_usage() const {
  if VUNLIKELY (impl_->profiler) {
    return impl_->profiler->get();
  } else {
    return -1;
  }
}

template <typename ImplT, SecurityType SecT>
inline bool Node<ImplT, SecT>::get_safety_quit() const {
  return quit_mtx_.has_value();
}

template <typename ImplT, SecurityType SecT>
inline void Node<ImplT, SecT>::set_safety_quit(bool safety_quit) {
  if VUNLIKELY (safety_quit) {
    if (!quit_mtx_.has_value()) {
      quit_mtx_.emplace();
    }
  } else {
    if (quit_mtx_.has_value()) {
      quit_mtx_.reset();
    }
  }
}

template <typename ImplT, SecurityType SecT>
inline void Node<ImplT, SecT>::set_ssl_options(const SslOptions& options) {
  impl_->set_ssl_options(options);
}

template <typename ImplT, SecurityType SecT>
inline Node<ImplT, SecT>::Node() {
  static_assert(std::is_base_of_v<NodeImpl, ImplT>, "ImplT must be derived from NodeImpl.");
}

template <typename ImplT, SecurityType SecT>
inline Node<ImplT, SecT>::~Node() {
  deinit();  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
}

template <typename ImplT, SecurityType SecT>
inline bool Node<ImplT, SecT>::enable_security(const Security::Config& cfg) {
  auto sec_cfg = cfg;

  return enable_security(std::move(sec_cfg));
}

template <typename ImplT, SecurityType SecT>
inline bool Node<ImplT, SecT>::enable_security(Security::Config&& cfg) {
  static_assert(SecT == SecurityType::kWithSecurity, "Must be security type.");

  if VUNLIKELY (!impl_) {
    return false;
  }

  if VUNLIKELY (has_inited_) {
    VLOG_W("Node::enable_security(): must run before init(); rejected to avoid live-traffic race.");
    return false;
  }

  return impl_->enable_security(std::move(cfg));
}

template <typename ImplT, SecurityType SecT>
template <typename CallbackT, typename... ArgsT>
inline void Node<ImplT, SecT>::invoke_callback(const CallbackT& callback, ArgsT&&... args) {
  if VUNLIKELY (quit_mtx_.has_value()) {
    std::lock_guard quit_lock(quit_mtx_.value());
    std::invoke(callback, std::forward<ArgsT>(args)...);
  } else {
    std::invoke(callback, std::forward<ArgsT>(args)...);
  }
}

template <typename ImplT, SecurityType SecT>
template <typename TypeT>
inline TypeT Node<ImplT, SecT>::get_default_value() {
  if constexpr (Traits::IsSharedPtr<TypeT>()) {
    return std::make_shared<typename TypeT::element_type>();
  } else if constexpr (Serializer::is_proto_ptr_type<TypeT>()) {
    if VLIKELY (this->proto_arena_) {
      return google::protobuf::Arena::Create<std::remove_pointer_t<TypeT>>(
          static_cast<google::protobuf::Arena*>(this->proto_arena_));
    }

    VLOG_F("Node: Proto arena is not bound, url: ", this->impl_->url, ".");

    return nullptr;
  } else if constexpr (std::is_default_constructible_v<TypeT>) {
    return TypeT{};
  } else {
    static_assert(Traits::ExpectFalse<TypeT>(), "TypeT is not default constructible.");
    return {};
  }
}

}  // namespace vlink
