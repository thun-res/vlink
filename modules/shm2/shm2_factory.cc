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

#include "./shm2_factory.h"

#include <charconv>
#include <chrono>
#include <string>
#include <thread>
#include <utility>

#include "./impl/server_impl.h"

#ifndef VLINK_SHM2_NO_FD_LISTENER
#define VLINK_SHM2_NO_FD_LISTENER 0
#endif

namespace vlink {

static constexpr uint32_t kShm2IdleWaitTimeoutNs = 1U * 1000U * 1000U;

static bool make_iox2_service_name(const std::string& name, iox2_service_name_t& storage,
                                   iox2_service_name_h& out_handle) {
  out_handle = nullptr;

  return iox2_service_name_new(&storage, name.c_str(), name.size(), &out_handle) == IOX2_OK;
}

static bool open_or_create_event_service(iox2_node_h_ref node, const std::string& svc_name,
                                         iox2_port_factory_event_t& pf_storage, iox2_port_factory_event_h& pf_handle) {
  iox2_service_name_t sn_storage{};
  iox2_service_name_h sn_handle{nullptr};

  if VUNLIKELY (!make_iox2_service_name(svc_name, sn_storage, sn_handle)) {
    VLOG_E("Shm2Factory: Bad event service name: ", svc_name, ".");
    return false;
  }

  iox2_service_builder_t sb_storage{};
  iox2_service_builder_h sb_handle =
      iox2_node_service_builder(node, &sb_storage, iox2_cast_service_name_ptr(sn_handle));
  iox2_service_name_drop(sn_handle);

  iox2_service_builder_event_h ev_builder = iox2_service_builder_event(sb_handle);

  iox2_service_builder_event_set_max_notifiers(&ev_builder, 512);
  iox2_service_builder_event_set_max_listeners(&ev_builder, 512);

  int ret = iox2_service_builder_event_open_or_create(ev_builder, &pf_storage, &pf_handle);

  if VUNLIKELY (ret != IOX2_OK) {
    VLOG_E("Shm2Factory: Failed to open or create event service '", svc_name, "', error: ", ret, ".");
    return false;
  }

  return true;
}

// Shm2Factory
Shm2Factory::Shm2Factory() {
  Bytes::init_memory_pool();

  if VUNLIKELY (Shm2Conf::get_thread_count() != 1) {
    VLOG_W("Shm2Factory: Shm2 does not support setting thread count.");
  }

  message_loop_.set_name("SHM2-FACTORY");

  detect_timer_.attach(&message_loop_);
  detect_timer_.set_interval(10);
  detect_timer_.set_loop_count(Timer::kInfinite);
  detect_timer_.set_callback([this]() {
    detect_timer_.set_interval(50);

    std::shared_lock lock(detect_mtx_);

    for (const auto& [node_ptr, callback] : detect_event_map_) {
      callback();
    }
    for (const auto& [node_ptr, callback] : detect_method_map_) {
      callback();
    }
  });

  static std::string shm_debug_str = Utils::get_env("VLINK_SHM2_DEBUG");
  static std::string depth_env_str = Utils::get_env("VLINK_SHM2_DEPTH");
  static std::string config_str = Utils::get_env("VLINK_SHM2_CONFIG");

  if (shm_debug_str == "1") {
    iox2_set_log_level(iox2_log_level_e_INFO);
  } else {
    iox2_set_log_level(iox2_log_level_e_FATAL);
  }

  if (!depth_env_str.empty()) {
    auto [p, error] =
        std::from_chars(depth_env_str.data(), depth_env_str.data() + depth_env_str.size(), default_depth_);

    if VUNLIKELY (error != std::errc() || default_depth_ <= 0) {
      default_depth_ = kDefaultSubDepth2;
    }
  }

  int ret = 0;

  if (config_str.empty()) {
    ret = iox2_config_default(&config_storage_, &config_);
  } else {
    ret = iox2_config_from_file(&config_storage_, &config_, config_str.c_str());
  }

  if VUNLIKELY (ret != IOX2_OK) {
    VLOG_F("Shm2Factory: Failed to init config.");
    return;
  }

  if (!depth_env_str.empty()) {
    iox2_config_defaults_publish_subscribe_set_publisher_max_loaned_samples(&config_, default_depth_);
    iox2_config_defaults_publish_subscribe_set_subscriber_max_buffer_size(&config_, default_depth_);
    iox2_config_defaults_request_response_set_max_loaned_requests(&config_, default_depth_);
  } else if (config_str.empty()) {
    iox2_config_defaults_publish_subscribe_set_subscriber_max_buffer_size(&config_, default_depth_);
  }

  std::string name = Utils::get_app_name() + "_" + Utils::get_pid_str();

  iox2_node_name_t node_name_storage{};
  iox2_node_name_h node_name_handle{nullptr};

  ret = iox2_node_name_new(&node_name_storage, name.c_str(), name.size(), &node_name_handle);

  if VUNLIKELY (ret != IOX2_OK) {
    VLOG_F("Shm2Factory: Failed to create node name: ", name, ".");
    return;
  }

  auto* node_builder_handle = iox2_node_builder_new(nullptr);
  iox2_node_builder_set_name(&node_builder_handle, iox2_cast_node_name_ptr(node_name_handle));
  iox2_node_builder_set_signal_handling_mode(&node_builder_handle, iox2_signal_handling_mode_e_DISABLED);
  iox2_node_builder_set_config(&node_builder_handle, &config_);

  ret = iox2_node_builder_create(node_builder_handle, nullptr, iox2_service_type_e_IPC, &node_);
  iox2_node_name_drop(node_name_handle);

  if VUNLIKELY (ret != IOX2_OK) {
    VLOG_F("Shm2Factory: Failed to create node, error: ", ret, ".");
    return;
  }

  iox2_waitset_builder_h waitset_builder = nullptr;
  iox2_waitset_builder_new(nullptr, &waitset_builder);

  ret = iox2_waitset_builder_create(waitset_builder, iox2_service_type_e_IPC, nullptr, &waitset_);

  if VUNLIKELY (ret != IOX2_OK) {
    VLOG_F("Shm2Factory: Failed to create waitset, error: ", ret, ".");
    return;
  }

  const std::string wakeup_svc = "vlink/__factory_wakeup__/" + Utils::get_pid_str();

  if (open_or_create_event_service(&node_, wakeup_svc, wakeup_event_pf_storage_, wakeup_event_pf_handle_)) {
    auto* lb = iox2_port_factory_event_listener_builder(&wakeup_event_pf_handle_, nullptr);
    iox2_port_factory_listener_builder_create(lb, &wakeup_listener_storage_, &wakeup_listener_);

#if !VLINK_SHM2_NO_FD_LISTENER
    iox2_file_descriptor_ptr fd = iox2_listener_get_file_descriptor(&wakeup_listener_);

    if (fd != nullptr) {
      iox2_waitset_attach_notification(&waitset_, fd, &wakeup_guard_storage_, &wakeup_guard_);
    }
#endif

    auto* nb = iox2_port_factory_event_notifier_builder(&wakeup_event_pf_handle_, nullptr);
    iox2_port_factory_notifier_builder_create(nb, &wakeup_notifier_storage_, &wakeup_notifier_);
  }

  poll_thread_ = std::thread([this]() { poll_thread_func(); });
  Utils::set_thread_name("VShm2Poll", &poll_thread_);

  message_loop_.async_run();
}

Shm2Factory::~Shm2Factory() {
  poll_quit_.store(true, std::memory_order_release);

  if (wakeup_notifier_) {
    iox2_notifier_notify(&wakeup_notifier_, nullptr);
  }

  if (poll_thread_.joinable()) {
    poll_thread_.join();
  }

  std::vector<std::shared_ptr<BlockingWaiter>> blocking_waiters;

  {
    std::lock_guard lock(blocking_mtx_);

    for (auto& [_, waiter] : blocking_waiters_) {
      blocking_waiters.emplace_back(std::move(waiter));
    }

    blocking_waiters_.clear();
  }

  for (auto& waiter : blocking_waiters) {
    stop_blocking_waiter(waiter.get());
  }

  if (wakeup_guard_) {
    iox2_waitset_guard_drop(wakeup_guard_);
    wakeup_guard_ = nullptr;
  }

  {
    std::unique_lock lock(sub_list_mtx_);
    for (auto& [handle, entry] : poll_map_) {
      if (entry.guard) {
        iox2_waitset_guard_drop(entry.guard);
        entry.guard = nullptr;
      }
    }

    poll_map_.clear();
  }

  iox2_waitset_drop(waitset_);
  waitset_ = nullptr;

  if (wakeup_notifier_) {
    iox2_notifier_drop(wakeup_notifier_);
    wakeup_notifier_ = nullptr;
  }

  if (wakeup_listener_) {
    iox2_listener_drop(wakeup_listener_);
    wakeup_listener_ = nullptr;
  }

  if (wakeup_event_pf_handle_) {
    iox2_port_factory_event_drop(wakeup_event_pf_handle_);
    wakeup_event_pf_handle_ = nullptr;
  }

  detect_timer_.stop();
  detect_timer_.detach();

  message_loop_.quit();
  message_loop_.wait_for_quit();

  iox2_node_drop(node_);
  node_ = nullptr;

  if (config_) {
    iox2_config_drop(config_);
    config_ = nullptr;
  }
}

std::string Shm2Factory::make_service_name(const std::string& address, const std::string& suffix, int32_t domain) {
  std::string n = "vlink/" + address + "/" + suffix;

  if (domain != 0) {
    n += "_" + std::to_string(domain);
  }

  return n;
}

iox2_node_h_ref Shm2Factory::get_node() const { return &node_; }

void Shm2Factory::add_detect_event_callback(iox2_port_factory_pub_sub_h handle, DetectCallback&& callback) {
  std::lock_guard lock(detect_mtx_);

  if (detect_event_map_.empty()) {
    detect_timer_.restart();
  }

  detect_event_map_[handle] = callback;
  message_loop_.post_task([callback = std::move(callback)]() { callback(); });
}

void Shm2Factory::remove_detect_event_callback(iox2_port_factory_pub_sub_h handle) {
  std::lock_guard lock(detect_mtx_);
  detect_event_map_.erase(handle);

  if (detect_event_map_.empty() && detect_method_map_.empty()) {
    detect_timer_.stop();
  }
}

void Shm2Factory::add_detect_method_callback(iox2_port_factory_request_response_h handle, DetectCallback&& callback) {
  std::lock_guard lock(detect_mtx_);

  if (detect_method_map_.empty()) {
    detect_timer_.restart();
  }

  detect_method_map_[handle] = callback;
  message_loop_.post_task([callback = std::move(callback)]() { callback(); });
}

void Shm2Factory::remove_detect_method_callback(iox2_port_factory_request_response_h handle) {
  std::lock_guard lock(detect_mtx_);
  detect_method_map_.erase(handle);

  if (detect_event_map_.empty() && detect_method_map_.empty()) {
    detect_timer_.stop();
  }
}

int Shm2Factory::get_default_depth() const { return default_depth_; }

void Shm2Factory::register_poll(void* handle, PollCallback&& callback, iox2_waitset_guard_h guard,
                                iox2_listener_h_ref listener, bool poll_without_event,
                                iox2_port_factory_event_h_ref event_pf) {
  if (listener != nullptr) {
    auto waiter = std::make_shared<BlockingWaiter>();

    waiter->callback = std::move(callback);
    waiter->listener = listener;
    waiter->poll_without_event = poll_without_event;

    if (event_pf != nullptr) {
      auto* nb = iox2_port_factory_event_notifier_builder(event_pf, nullptr);

      if VUNLIKELY (iox2_port_factory_notifier_builder_create(nb, &waiter->wake_notifier_storage,
                                                              &waiter->wake_notifier) != IOX2_OK) {
        waiter->wake_notifier = nullptr;
        VLOG_E("Shm2Factory: Failed to create wake notifier for non-fd listener; using bounded wait fallback.");
      }
    }

    {
      std::lock_guard lock(blocking_mtx_);
      blocking_waiters_[handle] = waiter;

      waiter->thread = std::thread([waiter]() { Shm2Factory::blocking_wait_func(waiter.get()); });
      Utils::set_thread_name("VShm2Wait", &waiter->thread);
    }

    return;
  }

  std::unique_lock lock(sub_list_mtx_);
  auto& entry = poll_map_[handle];

  entry.callback = std::move(callback);
  entry.guard = guard;
}

void Shm2Factory::unregister_poll(void* handle) {
  std::shared_ptr<BlockingWaiter> waiter;

  {
    std::lock_guard lock(blocking_mtx_);
    auto it = blocking_waiters_.find(handle);

    if (it != blocking_waiters_.end()) {
      waiter = std::move(it->second);
      blocking_waiters_.erase(it);
    }
  }

  if (waiter) {
    stop_blocking_waiter(waiter.get());
  }

  iox2_waitset_guard_h guard_to_drop = nullptr;

  {
    std::unique_lock lock(sub_list_mtx_);
    auto it = poll_map_.find(handle);

    if (it != poll_map_.end()) {
      guard_to_drop = it->second.guard;
      poll_map_.erase(it);
    }
  }

  if (guard_to_drop) {
    iox2_waitset_guard_drop(guard_to_drop);
  }

  message_loop_.wait_for_idle();
}

void Shm2Factory::poll_thread_func() {
  while (!poll_quit_.load(std::memory_order_acquire)) {
    iox2_waitset_run_result_e result = iox2_waitset_run_result_e_STOP_REQUEST;
    int ret = IOX2_OK;

    if (wakeup_guard_ != nullptr) {
      ret = iox2_waitset_wait_and_process(&waitset_, Shm2Factory::on_process, this, &result);
    } else {
      ret = iox2_waitset_wait_and_process_once_with_timeout(&waitset_, Shm2Factory::on_process, this, 0,
                                                            kShm2IdleWaitTimeoutNs, &result);
    }

    if VUNLIKELY (ret != IOX2_OK && ret != iox2_waitset_run_error_e_NO_ATTACHMENTS &&
                  !poll_quit_.load(std::memory_order_acquire)) {
      VLOG_E("Shm2Factory: Failure in WaitSet::wait_and_process, ret=", ret, " result=", static_cast<int>(result), ".");
    }

    if (poll_quit_.load(std::memory_order_acquire)) {
      break;
    }

    if (ret == iox2_waitset_run_error_e_NO_ATTACHMENTS) {
      std::this_thread::sleep_for(std::chrono::nanoseconds(kShm2IdleWaitTimeoutNs));
    }
  }
}

void Shm2Factory::blocking_wait_func(BlockingWaiter* waiter) {
  const bool can_block = waiter->wake_notifier != nullptr;

  static constexpr uint64_t kWaitSeconds = kShm2IdleWaitTimeoutNs / (1000U * 1000U * 1000U);
  static constexpr uint32_t kWaitNanoseconds = kShm2IdleWaitTimeoutNs % (1000U * 1000U * 1000U);

  while (!waiter->quit.load(std::memory_order_acquire)) {
    bool has_received_event = false;
    iox2_event_id_t event_id;
    int ret = IOX2_OK;

    if (can_block) {
      ret = iox2_listener_blocking_wait_one(waiter->listener, &event_id, &has_received_event);
    } else {
      ret = iox2_listener_timed_wait_one(waiter->listener, &event_id, &has_received_event, kWaitSeconds,
                                         kWaitNanoseconds);
    }

    if (waiter->quit.load(std::memory_order_acquire)) {
      break;
    }

    if VUNLIKELY (ret != IOX2_OK) {
      VLOG_E("Shm2Factory: Failed to wait on non-fd listener, error: ", ret, ".");
      std::this_thread::sleep_for(std::chrono::nanoseconds(kShm2IdleWaitTimeoutNs));
      continue;
    }

    if (has_received_event || waiter->poll_without_event) {
      waiter->callback();
    }
  }
}

void Shm2Factory::stop_blocking_waiter(BlockingWaiter* waiter) {
  waiter->quit.store(true, std::memory_order_release);

  if (waiter->wake_notifier) {
    iox2_notifier_notify(&waiter->wake_notifier, nullptr);
  }

  if (waiter->thread.joinable()) {
    if (waiter->thread.get_id() == std::this_thread::get_id()) {
      waiter->thread.detach();
    } else {
      waiter->thread.join();
    }
  }

  if (waiter->wake_notifier) {
    iox2_notifier_drop(waiter->wake_notifier);
    waiter->wake_notifier = nullptr;
  }
}

iox2_callback_progression_e Shm2Factory::on_process(iox2_waitset_attachment_id_h attachment_id, void* context) {
  auto& factory = *static_cast<Shm2Factory*>(context);

  if VUNLIKELY (factory.poll_quit_.load(std::memory_order_acquire)) {
    iox2_waitset_attachment_id_drop(attachment_id);
    return iox2_callback_progression_e_STOP;
  }

  if (factory.wakeup_guard_ && iox2_waitset_attachment_id_has_event_from(&attachment_id, &factory.wakeup_guard_)) {
    bool has_received_event = false;
    iox2_event_id_t event_id;

    do {
      int drain_ret = iox2_listener_try_wait_one(&factory.wakeup_listener_, &event_id, &has_received_event);

      if VUNLIKELY (drain_ret != IOX2_OK) {
        VLOG_E("Shm2Factory: Failed to drain wakeup listener, error: ", drain_ret, ".");
        break;
      }
    } while (has_received_event);

    iox2_waitset_attachment_id_drop(attachment_id);
    return iox2_callback_progression_e_STOP;
  }

  {
    std::shared_lock lock(factory.sub_list_mtx_);

    for (auto& [handle, entry] : factory.poll_map_) {
      if (entry.guard == nullptr) {
        continue;
      }

      if (iox2_waitset_attachment_id_has_event_from(&attachment_id, &entry.guard)) {
        entry.callback();
        break;
      }
    }
  }

  iox2_waitset_attachment_id_drop(attachment_id);

  return iox2_callback_progression_e_CONTINUE;
}

// Shm2Server
Shm2Server::Shm2Server(const ShmID2& id) {
  static auto& factory = Shm2Factory::get();

  const auto& [impl_type, address, domain, depth, history, wait, size] = id;

  domain_ = domain;
  payload_size_ = size;

  service_name_ = Shm2Factory::make_service_name(address, "method", domain);

  iox2_service_name_t sn_storage{};
  iox2_service_name_h sn_handle{nullptr};

  if VUNLIKELY (!make_iox2_service_name(service_name_, sn_storage, sn_handle)) {
    VLOG_F("Shm2Factory: Failed to create server service name: ", service_name_, ".");
    return;
  }

  iox2_service_builder_t sb_storage{};
  iox2_service_builder_h sb_handle =
      iox2_node_service_builder(factory.get_node(), &sb_storage, iox2_cast_service_name_ptr(sn_handle));
  iox2_service_name_drop(sn_handle);

  iox2_service_builder_request_response_h rr_builder = iox2_service_builder_request_response(sb_handle);

  static const char kTypeName[] = "u8";
  iox2_service_builder_request_response_set_request_payload_type_details(
      &rr_builder, iox2_type_variant_e_DYNAMIC, kTypeName, sizeof(kTypeName) - 1, sizeof(uint8_t), alignof(uint8_t));
  iox2_service_builder_request_response_set_response_payload_type_details(
      &rr_builder, iox2_type_variant_e_DYNAMIC, kTypeName, sizeof(kTypeName) - 1, sizeof(uint8_t), alignof(uint8_t));
  iox2_service_builder_request_response_max_clients(&rr_builder, 512);

  if (depth > 0) {
    iox2_service_builder_request_response_max_loaned_requests(&rr_builder, static_cast<size_t>(depth));
  }

  if VUNLIKELY (iox2_service_builder_request_response_open_or_create(rr_builder, &pf_storage_, &pf_handle_) !=
                IOX2_OK) {
    VLOG_F("Shm2Factory: Failed to open/create server request-response service: ", service_name_, ".");
    return;
  }

  iox2_port_factory_server_builder_t server_builder_storage{};
  auto* server_builder_handle = iox2_port_factory_request_response_server_builder(&pf_handle_, &server_builder_storage);

  iox2_port_factory_server_builder_set_initial_max_slice_len(
      &server_builder_handle, static_cast<size_t>(payload_size_) + Shm2Factory::get_loaned_offset());
  iox2_port_factory_server_builder_set_allocation_strategy(&server_builder_handle, iox2_allocation_strategy_e_BEST_FIT);

  if VUNLIKELY (iox2_port_factory_server_builder_create(server_builder_handle, &server_storage_, &server_) != IOX2_OK) {
    VLOG_F("Shm2Factory: Failed to create server for: ", service_name_, ".");
    return;
  }

  const std::string notify_svc = Shm2Factory::make_service_name(address, "method_notify", domain);

  if VUNLIKELY (!open_or_create_event_service(factory.get_node(), notify_svc, event_pf_storage_, event_pf_handle_)) {
    VLOG_F("Shm2Factory: Failed to open server event service: ", notify_svc, ".");
    return;
  }

  iox2_port_factory_notifier_builder_t notifier_builder_storage{};
  auto* notifier_builder = iox2_port_factory_event_notifier_builder(&event_pf_handle_, &notifier_builder_storage);

  if VUNLIKELY (iox2_port_factory_notifier_builder_create(notifier_builder, &notifier_storage_, &notifier_) !=
                IOX2_OK) {
    VLOG_F("Shm2Factory: Failed to create server notifier for: ", notify_svc, ".");
    return;
  }

  const std::string req_notify_svc = Shm2Factory::make_service_name(address, "method_req_notify", domain);

  struct ReqListenerCtx final {
    iox2_port_factory_event_t pf_storage;
    iox2_port_factory_event_h pf_handle{nullptr};
    iox2_listener_t listener_storage;
    iox2_listener_h listener{nullptr};

    ReqListenerCtx() = default;

    ReqListenerCtx(const ReqListenerCtx&) = delete;

    ReqListenerCtx& operator=(const ReqListenerCtx&) = delete;

    ~ReqListenerCtx() {
      if (listener) {
        iox2_listener_drop(listener);
        listener = nullptr;
      }

      if (pf_handle) {
        iox2_port_factory_event_drop(pf_handle);
        pf_handle = nullptr;
      }
    }
  };

  auto ctx = std::make_shared<ReqListenerCtx>();

  if VUNLIKELY (!open_or_create_event_service(factory.get_node(), req_notify_svc, ctx->pf_storage, ctx->pf_handle)) {
    VLOG_F("Shm2Factory: Failed to open server req-event service: ", req_notify_svc, ".");
    return;
  }

  iox2_port_factory_listener_builder_t req_listener_builder_storage{};
  auto* req_listener_builder = iox2_port_factory_event_listener_builder(&ctx->pf_handle, &req_listener_builder_storage);

  if VUNLIKELY (iox2_port_factory_listener_builder_create(req_listener_builder, &ctx->listener_storage,
                                                          &ctx->listener) != IOX2_OK) {
    VLOG_F("Shm2Factory: Failed to create server req-listener for: ", req_notify_svc, ".");
    return;
  }

  iox2_listener_h_ref listener_poll = nullptr;

#if VLINK_SHM2_NO_FD_LISTENER
  listener_poll = &ctx->listener;
#else
  iox2_file_descriptor_ptr fd = iox2_listener_get_file_descriptor(&ctx->listener);

  if (fd != nullptr) {
    int ret = iox2_waitset_attach_notification(factory.get_waitset(), fd, &guard_storage_, &guard_);

    if VUNLIKELY (ret != IOX2_OK) {
      VLOG_F("Shm2Factory: Failed to attach server req-listener fd to waitset, error: ", ret, ".");
      return;
    }
  } else {
    listener_poll = &ctx->listener;
  }
#endif

  factory.register_poll(
      this,
      [this, ctx]() {
        bool has_received_event = false;
        iox2_event_id_t event_id;

        do {
          int drain_ret = iox2_listener_try_wait_one(&ctx->listener, &event_id, &has_received_event);

          if VUNLIKELY (drain_ret != IOX2_OK) {
            VLOG_E("Shm2Factory: Failed to drain server req-listener, error: ", drain_ret, ".");
            break;
          }
        } while (has_received_event);

        bool has_requests = false;

        if (iox2_server_has_requests(&server_, &has_requests) != IOX2_OK || !has_requests) {
          return;
        }

        auto* impl = get_first_impl();

        if VUNLIKELY (!impl) {
          return;
        }

        auto* ml = impl->get_message_loop();

        if (ml) {
          std::weak_ptr<Shm2Server> weak_self = weak_from_this();
          ml->post_task([weak_self]() {
            auto self = weak_self.lock();

            if VUNLIKELY (!self) {
              return;
            }

            auto* impl = self->get_first_impl();

            if VUNLIKELY (!impl || !impl->get_message_loop()) {
              return;
            }

            self->process_message();
          });
        } else {
          process_message();
        }
      },
      guard_, listener_poll, false, &ctx->pf_handle);
}

Shm2Server::~Shm2Server() {
  is_offering_.store(false, std::memory_order_relaxed);
  is_suspend_.store(true, std::memory_order_relaxed);

  Shm2Factory::get().unregister_poll(this);

  std::lock_guard lock(mtx_);

  {
    std::lock_guard loan_lock(loan_mtx_);

    for (auto& [_, entry] : loan_map_) {
      iox2_response_mut_drop(entry.handle);
    }

    loan_map_.clear();
  }

  if (active_req_) {
    iox2_active_request_drop(active_req_);
    active_req_ = nullptr;
  }

  if (notifier_) {
    iox2_notifier_drop(notifier_);
    notifier_ = nullptr;
  }

  if (event_pf_handle_) {
    iox2_port_factory_event_drop(event_pf_handle_);
    event_pf_handle_ = nullptr;
  }

  if (server_) {
    iox2_server_drop(server_);
    server_ = nullptr;
  }

  if (pf_handle_) {
    iox2_port_factory_request_response_drop(pf_handle_);
    pf_handle_ = nullptr;
  }
}

std::any Shm2Server::get_native_handle() const { return this; }

bool Shm2Server::suspend() {
  is_suspend_.store(true, std::memory_order_relaxed);
  return true;
}
bool Shm2Server::resume() {
  is_suspend_.store(false, std::memory_order_relaxed);
  return true;
}
bool Shm2Server::is_suspend() const { return is_suspend_.load(std::memory_order_relaxed); }

void Shm2Server::process_message() {
  if VUNLIKELY (!server_) {
    return;
  }

  bool has_requests = false;

  while (iox2_server_has_requests(&server_, &has_requests) == IOX2_OK && has_requests) {
    {
      std::lock_guard lock(mtx_);

      if VUNLIKELY (active_req_ != nullptr) {
        break;
      }
    }

    iox2_active_request_t active_req_storage{};
    iox2_active_request_h active_req_handle{nullptr};

    if VUNLIKELY (iox2_server_receive(&server_, &active_req_storage, &active_req_handle) != IOX2_OK ||
                  !active_req_handle) {
      break;
    }

    if (is_suspend_.load(std::memory_order_relaxed)) {
      iox2_active_request_drop(active_req_handle);
      continue;
    }

    const void* payload_ptr = nullptr;
    size_t num_elements = 0;
    iox2_active_request_payload(&active_req_handle, &payload_ptr, &num_elements);

    if VUNLIKELY (!payload_ptr || num_elements == 0) {
      iox2_active_request_drop(active_req_handle);
      break;
    }

    const auto* read_req = static_cast<const uint8_t*>(payload_ptr);

    uint64_t channel = 0;
    uint64_t seq = 0;
    Bytes req_bytes;
    Shm2Factory::read_data(read_req, static_cast<uint64_t>(num_elements), channel, seq, req_bytes);
    (void)seq;

    bool handle_consumed = false;

    traverse_req_resp_callback(
        [this, channel, &req_bytes, &active_req_handle, &handle_consumed](NodeImpl* impl, const auto& callback) {
          const auto* conf_ptr = impl->get_target_conf<Shm2Conf>();

          if (static_cast<uint64_t>(conf_ptr->hash_code) != channel) {
            ignore_called();
            return;
          }

          if VUNLIKELY (has_called()) {
            VLOG_F(*conf_ptr, "Two identical service requests.");
            return;
          }

          if (static_cast<ServerImpl*>(impl)->is_resp_type) {
            {
              std::lock_guard lock(mtx_);
              active_req_ = active_req_handle;
            }
            handle_consumed = true;
            Bytes resp_bytes;
            callback(seq_.load(std::memory_order_relaxed), req_bytes, &resp_bytes);
            {
              std::lock_guard lock(mtx_);
              if (active_req_ == active_req_handle) {
                iox2_active_request_drop(active_req_);
                active_req_ = nullptr;
              }
            }
          } else {
            callback(0, req_bytes, nullptr);
            iox2_active_request_drop(active_req_handle);
            handle_consumed = true;
          }
        });

    if VUNLIKELY (!handle_consumed) {
      iox2_active_request_drop(active_req_handle);
    }
  }
}

void Shm2Server::start() { is_offering_.store(true, std::memory_order_relaxed); }
void Shm2Server::stop() { is_offering_.store(false, std::memory_order_relaxed); }

bool Shm2Server::has_clients() const {
  int count = iox2_port_factory_request_response_dynamic_config_number_of_clients(&pf_handle_);
  return count > 0;
}

Bytes Shm2Server::loan(uint64_t channel, int64_t size) {
  (void)channel;

  if VUNLIKELY (size <= 0) {
    return Bytes();
  }

  size_t total = static_cast<size_t>(size) + Shm2Factory::get_loaned_offset();

  auto storage = std::make_unique<iox2_response_mut_t>();
  iox2_response_mut_h resp_handle{nullptr};

  {
    std::lock_guard lock(mtx_);

    if VUNLIKELY (!active_req_) {
      return Bytes();
    }

    if VUNLIKELY (iox2_active_request_loan_slice_uninit(&active_req_, storage.get(), &resp_handle, total) != IOX2_OK) {
      resp_handle = nullptr;
    }
  }

  if VUNLIKELY (!resp_handle) {
    VLOG_E("Shm2Factory: Failed to loan server response buffer, size: ", total, ".");
    return Bytes();
  }

  void* write_ptr = nullptr;
  size_t write_elems = 0;
  iox2_response_mut_payload_mut(&resp_handle, &write_ptr, &write_elems);

  auto* write_buf = static_cast<uint8_t*>(write_ptr);
  const uint8_t* user_ptr = write_buf + Shm2Factory::get_loaned_offset();

  {
    std::lock_guard lock(loan_mtx_);
    loan_map_.emplace(user_ptr, ServerLoanEntry{std::move(storage), resp_handle});
  }

  return Bytes::loan_internal(user_ptr, size);
}

bool Shm2Server::release(const Bytes& bytes) {
  if VUNLIKELY (!bytes.is_loaned()) {
    return false;
  }

  ServerLoanEntry entry;

  {
    std::lock_guard lock(loan_mtx_);
    auto it = loan_map_.find(bytes.data());

    if (it == loan_map_.end()) {
      return false;
    }

    entry = std::move(it->second);
    loan_map_.erase(it);
  }

  iox2_response_mut_drop(entry.handle);
  return true;
}

bool Shm2Server::reply(uint64_t channel, const Bytes& resp_data) {
  if VUNLIKELY (!active_req_) {
    VLOG_E("Shm2Factory: Server reply called without active request.");
    return false;
  }

  std::lock_guard lock(mtx_);

  if (resp_data.is_loaned()) {
    ServerLoanEntry entry;

    {
      std::lock_guard loan_lock(loan_mtx_);
      auto it = loan_map_.find(resp_data.data());

      if VUNLIKELY (it == loan_map_.end()) {
        VLOG_E("Shm2Factory: reply() on loaned bytes from a foreign server.");
        iox2_active_request_drop(active_req_);
        active_req_ = nullptr;
        return false;
      }

      entry = std::move(it->second);
      loan_map_.erase(it);
    }

    const uint64_t seq = seq_.fetch_add(1, std::memory_order_relaxed) + 1;

    auto* buf_base = const_cast<uint8_t*>(resp_data.data()) - Shm2Factory::get_loaned_offset();
    Shm2Factory::write_header(buf_base, channel, seq);

    auto ret = iox2_response_mut_send(entry.handle);

    iox2_active_request_drop(active_req_);
    active_req_ = nullptr;

    if VUNLIKELY (ret != IOX2_OK) {
      VLOG_E("Shm2Factory: Failed to send loaned server response, error: ", ret, ".");
      return false;
    }

    if (notifier_) {
      iox2_notifier_notify(&notifier_, nullptr);
    }

    return true;
  }

  size_t total = resp_data.size() + Shm2Factory::get_loaned_offset();

  iox2_response_mut_t resp_storage{};
  iox2_response_mut_h resp_handle{nullptr};

  if VUNLIKELY (iox2_active_request_loan_slice_uninit(&active_req_, &resp_storage, &resp_handle, total) != IOX2_OK) {
    VLOG_E("Shm2Factory: Failed to loan server response buffer for reply, size: ", total, ".");
    iox2_active_request_drop(active_req_);
    active_req_ = nullptr;
    return false;
  }

  const uint64_t seq = seq_.fetch_add(1, std::memory_order_relaxed) + 1;

  void* write_ptr = nullptr;
  size_t write_elems = 0;
  iox2_response_mut_payload_mut(&resp_handle, &write_ptr, &write_elems);

  auto* write_buf = static_cast<uint8_t*>(write_ptr);
  Shm2Factory::write_data(write_buf, channel, seq, resp_data);

  auto ret = iox2_response_mut_send(resp_handle);

  iox2_active_request_drop(active_req_);
  active_req_ = nullptr;

  if VUNLIKELY (ret != IOX2_OK) {
    VLOG_E("Shm2Factory: Failed to send server response, error: ", ret, ".");
    return false;
  }

  if (notifier_) {
    iox2_notifier_notify(&notifier_, nullptr);
  }

  return true;
}

// Shm2Client
Shm2Client::Shm2Client(const ShmID2& id) {
  static auto& factory = Shm2Factory::get();

  const auto& [impl_type, address, domain, depth, history, wait, size] = id;

  domain_ = domain;
  payload_size_ = size;

  service_name_ = Shm2Factory::make_service_name(address, "method", domain);

  iox2_service_name_t sn_storage{};
  iox2_service_name_h sn_handle{nullptr};

  if VUNLIKELY (!make_iox2_service_name(service_name_, sn_storage, sn_handle)) {
    VLOG_F("Shm2Factory: Failed to create client service name: ", service_name_, ".");
    return;
  }

  iox2_service_builder_t sb_storage{};
  iox2_service_builder_h sb_handle =
      iox2_node_service_builder(factory.get_node(), &sb_storage, iox2_cast_service_name_ptr(sn_handle));
  iox2_service_name_drop(sn_handle);

  iox2_service_builder_request_response_h rr_builder = iox2_service_builder_request_response(sb_handle);

  static const char kTypeName[] = "u8";
  iox2_service_builder_request_response_set_request_payload_type_details(
      &rr_builder, iox2_type_variant_e_DYNAMIC, kTypeName, sizeof(kTypeName) - 1, sizeof(uint8_t), alignof(uint8_t));
  iox2_service_builder_request_response_set_response_payload_type_details(
      &rr_builder, iox2_type_variant_e_DYNAMIC, kTypeName, sizeof(kTypeName) - 1, sizeof(uint8_t), alignof(uint8_t));
  iox2_service_builder_request_response_max_clients(&rr_builder, 512);

  if (depth > 0) {
    iox2_service_builder_request_response_max_loaned_requests(&rr_builder, static_cast<size_t>(depth));
  }

  if VUNLIKELY (iox2_service_builder_request_response_open_or_create(rr_builder, &pf_storage_, &pf_handle_) !=
                IOX2_OK) {
    VLOG_F("Shm2Factory: Failed to open/create client request-response service: ", service_name_, ".");
    return;
  }

  iox2_port_factory_client_builder_t client_builder_storage{};
  auto* client_builder_handle = iox2_port_factory_request_response_client_builder(&pf_handle_, &client_builder_storage);

  iox2_port_factory_client_builder_set_initial_max_slice_len(
      &client_builder_handle, static_cast<size_t>(payload_size_) + Shm2Factory::get_loaned_offset());
  iox2_port_factory_client_builder_set_allocation_strategy(&client_builder_handle, iox2_allocation_strategy_e_BEST_FIT);

  if VUNLIKELY (iox2_port_factory_client_builder_create(client_builder_handle, &client_storage_, &client_) != IOX2_OK) {
    VLOG_F("Shm2Factory: Failed to create client for: ", service_name_, ".");
    return;
  }

  const std::string req_notify_svc = Shm2Factory::make_service_name(address, "method_req_notify", domain);

  struct ReqNotifierCtx final {
    iox2_port_factory_event_t pf_storage;
    iox2_port_factory_event_h pf_handle{nullptr};
    iox2_notifier_t notifier_storage;
    iox2_notifier_h notifier{nullptr};

    ReqNotifierCtx() = default;

    ReqNotifierCtx(const ReqNotifierCtx&) = delete;

    ReqNotifierCtx& operator=(const ReqNotifierCtx&) = delete;

    ~ReqNotifierCtx() {
      if (notifier) {
        iox2_notifier_drop(notifier);
        notifier = nullptr;
      }

      if (pf_handle) {
        iox2_port_factory_event_drop(pf_handle);
        pf_handle = nullptr;
      }
    }
  };

  auto req_ctx = std::make_shared<ReqNotifierCtx>();

  if VUNLIKELY (!open_or_create_event_service(factory.get_node(), req_notify_svc, req_ctx->pf_storage,
                                              req_ctx->pf_handle)) {
    VLOG_F("Shm2Factory: Failed to open client req-event service: ", req_notify_svc, ".");
    return;
  }

  iox2_port_factory_notifier_builder_t req_notifier_builder_storage{};
  auto* req_notifier_builder =
      iox2_port_factory_event_notifier_builder(&req_ctx->pf_handle, &req_notifier_builder_storage);

  if VUNLIKELY (iox2_port_factory_notifier_builder_create(req_notifier_builder, &req_ctx->notifier_storage,
                                                          &req_ctx->notifier) != IOX2_OK) {
    VLOG_F("Shm2Factory: Failed to create client req-notifier for: ", req_notify_svc, ".");
    return;
  }

  const std::string resp_notify_svc = Shm2Factory::make_service_name(address, "method_notify", domain);

  if VUNLIKELY (!open_or_create_event_service(factory.get_node(), resp_notify_svc, event_pf_storage_,
                                              event_pf_handle_)) {
    VLOG_F("Shm2Factory: Failed to open client resp-event service: ", resp_notify_svc, ".");
    return;
  }

  iox2_port_factory_listener_builder_t listener_builder_storage{};
  auto* listener_builder = iox2_port_factory_event_listener_builder(&event_pf_handle_, &listener_builder_storage);

  if VUNLIKELY (iox2_port_factory_listener_builder_create(listener_builder, &listener_storage_, &listener_) !=
                IOX2_OK) {
    VLOG_F("Shm2Factory: Failed to create client resp-listener for: ", resp_notify_svc, ".");
    return;
  }

  iox2_listener_h_ref listener_poll = nullptr;

#if VLINK_SHM2_NO_FD_LISTENER
  listener_poll = &listener_;
#else
  iox2_file_descriptor_ptr fd = iox2_listener_get_file_descriptor(&listener_);

  if (fd != nullptr) {
    int ret = iox2_waitset_attach_notification(factory.get_waitset(), fd, &guard_storage_, &guard_);

    if VUNLIKELY (ret != IOX2_OK) {
      VLOG_F("Shm2Factory: Failed to attach client resp-listener fd to waitset, error: ", ret, ".");
      return;
    }
  } else {
    listener_poll = &listener_;
  }
#endif

  factory.register_poll(
      this,
      [this]() {
        bool has_received_event = false;
        iox2_event_id_t event_id;

        do {
          int drain_ret = iox2_listener_try_wait_one(&listener_, &event_id, &has_received_event);

          if VUNLIKELY (drain_ret != IOX2_OK) {
            VLOG_E("Shm2Factory: Failed to drain client resp-listener, error: ", drain_ret, ".");
            break;
          }
        } while (has_received_event);

        auto* impl = get_first_impl();

        if VUNLIKELY (!impl) {
          return;
        }

        auto* ml = impl->get_message_loop();

        if (ml) {
          std::weak_ptr<Shm2Client> weak_self = weak_from_this();
          ml->post_task([weak_self]() {
            auto self = weak_self.lock();

            if VUNLIKELY (!self) {
              return;
            }

            auto* impl = self->get_first_impl();

            if VUNLIKELY (!impl || !impl->get_message_loop()) {
              return;
            }

            self->process_message();
          });
        } else {
          process_message();
        }
      },
      guard_, listener_poll, false, &event_pf_handle_);

  req_notifier_fn_ = [req_ctx]() {
    if (req_ctx->notifier) {
      iox2_notifier_notify(&req_ctx->notifier, nullptr);
    }
  };
}

Shm2Client::~Shm2Client() {
  quit_flag_.store(true, std::memory_order_release);
  disable_detect_timer();

  Shm2Factory::get().unregister_poll(this);

  req_notifier_fn_ = nullptr;

  std::lock_guard lock(mtx_);

  {
    std::lock_guard loan_lock(loan_mtx_);

    for (auto& [_, entry] : loan_map_) {
      iox2_request_mut_drop(entry.handle);
    }

    loan_map_.clear();
  }

  for (auto& [seq, handle] : pending_map_) {
    iox2_pending_response_drop(handle);
  }

  pending_map_.clear();
  pending_storage_map_.clear();

  if (listener_) {
    iox2_listener_drop(listener_);
    listener_ = nullptr;
  }

  if (event_pf_handle_) {
    iox2_port_factory_event_drop(event_pf_handle_);
    event_pf_handle_ = nullptr;
  }

  if (client_) {
    iox2_client_drop(client_);
    client_ = nullptr;
  }

  if (pf_handle_) {
    iox2_port_factory_request_response_drop(pf_handle_);
    pf_handle_ = nullptr;
  }
}

std::any Shm2Client::get_native_handle() const { return this; }

void Shm2Client::process_message() {
  if VUNLIKELY (!client_) {
    return;
  }

  std::unique_lock lock(mtx_);

  bool progressed = true;

  while (progressed) {
    progressed = false;

    for (auto iter = pending_map_.begin(); iter != pending_map_.end();) {
      auto seq = iter->first;
      auto& handle = iter->second;

      iox2_response_t resp_storage{};
      iox2_response_h resp_handle{nullptr};

      auto ret = iox2_pending_response_receive(&handle, &resp_storage, &resp_handle);

      if (ret == IOX2_OK && resp_handle) {
        const void* payload_ptr = nullptr;
        size_t num_elements = 0;
        iox2_response_payload(&resp_handle, &payload_ptr, &num_elements);

        auto cb_iter = callbacks_.find(seq);

        if (cb_iter != callbacks_.end()) {
          auto callback = std::move(cb_iter->second);
          callbacks_.erase(cb_iter);

          iox2_pending_response_drop(handle);
          pending_map_.erase(iter);
          pending_storage_map_.erase(seq);

          lock.unlock();

          const auto* read_resp = static_cast<const uint8_t*>(payload_ptr);
          uint64_t channel = 0;
          uint64_t resp_seq = 0;
          Bytes resp_bytes;
          Shm2Factory::read_data(read_resp, static_cast<uint64_t>(num_elements), channel, resp_seq, resp_bytes);
          (void)resp_seq;

          callback(channel, resp_bytes);
          iox2_response_drop(resp_handle);

          lock.lock();

          progressed = true;
          break;
        }

        iox2_response_drop(resp_handle);
        iox2_pending_response_drop(handle);
        iter = pending_map_.erase(iter);
        pending_storage_map_.erase(seq);
      } else {
        ++iter;
      }
    }
  }
}

bool Shm2Client::is_connected() const {
  int count = iox2_port_factory_request_response_dynamic_config_number_of_servers(&pf_handle_);
  return count > 0;
}

void Shm2Client::enable_detect_timer() {
  if (!has_detect_timer_.load(std::memory_order_relaxed)) {
    has_detect_timer_.store(true, std::memory_order_relaxed);
    Shm2Factory::get().add_detect_method_callback(pf_handle_, [weak = weak_from_this()]() {
      auto self = weak.lock();

      if VLIKELY (self) {
        self->detect_server();
      }
    });
  }
}

void Shm2Client::disable_detect_timer() {
  if (has_detect_timer_.load(std::memory_order_relaxed)) {
    has_detect_timer_.store(false, std::memory_order_relaxed);
    Shm2Factory::get().remove_detect_method_callback(pf_handle_);
  }
}

Bytes Shm2Client::loan(uint64_t channel, int64_t size) {
  (void)channel;

  if VUNLIKELY (size <= 0 || !client_) {
    return Bytes();
  }

  size_t total = static_cast<size_t>(size) + Shm2Factory::get_loaned_offset();

  auto storage = std::make_unique<iox2_request_mut_t>();
  iox2_request_mut_h req_handle{nullptr};

  if VUNLIKELY (iox2_client_loan_slice_uninit(&client_, storage.get(), &req_handle, total) != IOX2_OK) {
    VLOG_E("Shm2Factory: Failed to loan client request buffer, size: ", total, ".");
    return Bytes();
  }

  void* write_ptr = nullptr;
  size_t write_elems = 0;
  iox2_request_mut_payload_mut(&req_handle, &write_ptr, &write_elems);

  auto* write_buf = static_cast<uint8_t*>(write_ptr);
  const uint8_t* user_ptr = write_buf + Shm2Factory::get_loaned_offset();

  {
    std::lock_guard lock(loan_mtx_);
    loan_map_.emplace(user_ptr, ClientLoanEntry{std::move(storage), req_handle});
  }

  return Bytes::loan_internal(user_ptr, size);
}

bool Shm2Client::release(const Bytes& bytes) {
  if VUNLIKELY (!bytes.is_loaned()) {
    return false;
  }

  ClientLoanEntry entry;

  {
    std::lock_guard lock(loan_mtx_);
    auto it = loan_map_.find(bytes.data());

    if (it == loan_map_.end()) {
      return false;
    }

    entry = std::move(it->second);
    loan_map_.erase(it);
  }

  iox2_request_mut_drop(entry.handle);
  return true;
}

bool Shm2Client::call(uint64_t channel, const Bytes& req_data, NodeImpl::MsgCallback&& callback, uint64_t* seq_out) {
  if VUNLIKELY (!is_connected()) {
    return false;
  }

  std::lock_guard lock(mtx_);

  std::unique_ptr<iox2_request_mut_t> loaned_storage;
  iox2_request_mut_t stack_storage{};
  iox2_request_mut_h req_handle{nullptr};
  uint8_t* write_buf{nullptr};
  uint64_t seq{0};

  if (req_data.is_loaned()) {
    ClientLoanEntry entry;

    {
      std::lock_guard loan_lock(loan_mtx_);
      auto it = loan_map_.find(req_data.data());

      if VUNLIKELY (it == loan_map_.end()) {
        VLOG_E("Shm2Factory: call() on loaned bytes from a foreign client.");
        return false;
      }

      entry = std::move(it->second);
      loan_map_.erase(it);
    }

    loaned_storage = std::move(entry.storage);
    req_handle = entry.handle;
    write_buf = const_cast<uint8_t*>(req_data.data()) - Shm2Factory::get_loaned_offset();
    seq = seq_.fetch_add(1, std::memory_order_relaxed) + 1;
    Shm2Factory::write_header(write_buf, channel, seq);
  } else {
    size_t total = req_data.size() + Shm2Factory::get_loaned_offset();

    if VUNLIKELY (iox2_client_loan_slice_uninit(&client_, &stack_storage, &req_handle, total) != IOX2_OK) {
      VLOG_E("Shm2Factory: Failed to loan client request, size: ", total, ".");
      return false;
    }

    seq = seq_.fetch_add(1, std::memory_order_relaxed) + 1;

    void* write_ptr = nullptr;
    size_t write_elems = 0;
    iox2_request_mut_payload_mut(&req_handle, &write_ptr, &write_elems);

    write_buf = static_cast<uint8_t*>(write_ptr);
    Shm2Factory::write_data(write_buf, channel, seq, req_data);
  }

  int ret = 0;

  if (callback) {
    const auto response_seq = seq;
    callbacks_[response_seq] = [cb = std::move(callback), channel](uint64_t target_channel, const Bytes& bytes) {
      if (channel != target_channel) {
        return;
      }

      cb(bytes);
    };

    if (seq_out) {
      *seq_out = response_seq;
    }

    auto& pr_storage = pending_storage_map_[response_seq];
    iox2_pending_response_h pr_handle{nullptr};
    ret = iox2_request_mut_send(req_handle, &pr_storage, &pr_handle);

    if VUNLIKELY (ret != IOX2_OK) {
      VLOG_E("Shm2Factory: Failed to send client request, error: ", ret, ".");

      callbacks_.erase(response_seq);
      pending_storage_map_.erase(response_seq);
      return false;
    }

    pending_map_[response_seq] = pr_handle;
  } else {
    iox2_pending_response_t pr_storage{};
    iox2_pending_response_h pr_handle;
    ret = iox2_request_mut_send(req_handle, &pr_storage, &pr_handle);

    if VUNLIKELY (ret != IOX2_OK) {
      VLOG_E("Shm2Factory: Failed to send client request, error: ", ret, ".");
      return false;
    }

    if (pr_handle) {
      iox2_pending_response_drop(pr_handle);
    }
  }

  if (req_notifier_fn_) {
    req_notifier_fn_();
  }

  return true;
}

void Shm2Client::remove_response_callback(uint64_t seq) {
  std::lock_guard lock(mtx_);

  callbacks_.erase(seq);

  auto pending_iter = pending_map_.find(seq);

  if (pending_iter != pending_map_.end()) {
    iox2_pending_response_drop(pending_iter->second);
    pending_map_.erase(pending_iter);
  }

  pending_storage_map_.erase(seq);
}

void Shm2Client::detect_server() {
  if VUNLIKELY (quit_flag_.load(std::memory_order_acquire)) {
    return;
  }

  discovery_server(is_connected());
}

void Shm2Client::discovery_server(bool connect) {
  if VLIKELY (last_connected_.load(std::memory_order_relaxed) == connect) {
    return;
  }

  traverse_server_connect_callback([connect](NodeImpl*, const auto& callback) { callback(connect); });
  last_connected_.store(connect, std::memory_order_relaxed);
}

// Shm2Publisher
Shm2Publisher::Shm2Publisher(const ShmID2& id) {
  static auto& factory = Shm2Factory::get();

  const auto& [impl_type, address, domain, depth, history, wait, size] = id;

  domain_ = domain;
  wait_ = wait;
  payload_size_ = size;
  history_ = history;
  depth_ = depth;

  const auto notify_every_env = Utils::get_env("VLINK_SHM2_NOTIFY_EVERY", "1");
  const auto loan_min_env = Utils::get_env("VLINK_SHM2_LOAN_MIN", std::to_string(loan_send_threshold_));

  int parsed_notify_every = 1;

  if (!notify_every_env.empty()) {
    std::from_chars(notify_every_env.data(), notify_every_env.data() + notify_every_env.size(), parsed_notify_every);
  }

  notify_every_ = parsed_notify_every > 0 ? static_cast<uint32_t>(parsed_notify_every) : 1U;

  if (!loan_min_env.empty()) {
    auto [ptr, error] =
        std::from_chars(loan_min_env.data(), loan_min_env.data() + loan_min_env.size(), loan_send_threshold_);

    if VUNLIKELY (error != std::errc() || ptr != loan_min_env.data() + loan_min_env.size()) {
      loan_send_threshold_ = 65536;
    }
  }

  service_name_ = Shm2Factory::make_service_name(address, "event", domain);

  iox2_service_name_t sn_storage{};
  iox2_service_name_h sn_handle{nullptr};

  if VUNLIKELY (!make_iox2_service_name(service_name_, sn_storage, sn_handle)) {
    VLOG_F("Shm2Factory: Failed to create publisher service name: ", service_name_, ".");
    return;
  }

  iox2_service_builder_t sb_storage{};
  iox2_service_builder_h sb_handle =
      iox2_node_service_builder(factory.get_node(), &sb_storage, iox2_cast_service_name_ptr(sn_handle));
  iox2_service_name_drop(sn_handle);

  iox2_service_builder_pub_sub_h ps_builder = iox2_service_builder_pub_sub(sb_handle);

  static const char kTypeName[] = "u8";

  iox2_service_builder_pub_sub_set_payload_type_details(&ps_builder, iox2_type_variant_e_DYNAMIC, kTypeName,
                                                        sizeof(kTypeName) - 1, sizeof(uint8_t), alignof(uint8_t));
  iox2_service_builder_pub_sub_set_max_publishers(&ps_builder, 512);
  iox2_service_builder_pub_sub_set_max_subscribers(&ps_builder, 512);

  if (depth > 0) {
    iox2_service_builder_pub_sub_set_subscriber_max_buffer_size(&ps_builder, static_cast<size_t>(depth));
  }

  iox2_service_builder_pub_sub_set_enable_safe_overflow(&ps_builder, true);

  if (history > 0) {
    iox2_service_builder_pub_sub_set_history_size(&ps_builder, static_cast<size_t>(history));
  }

  if VUNLIKELY (iox2_service_builder_pub_sub_open_or_create(ps_builder, &pf_storage_, &pf_handle_) != IOX2_OK) {
    VLOG_F("Shm2Factory: Failed to open/create publisher pub-sub service: ", service_name_, ".");
    return;
  }

  iox2_port_factory_publisher_builder_t pub_builder_storage{};
  auto* pub_builder_handle = iox2_port_factory_pub_sub_publisher_builder(&pf_handle_, &pub_builder_storage);

  iox2_port_factory_publisher_builder_set_initial_max_slice_len(
      &pub_builder_handle, static_cast<size_t>(payload_size_) + Shm2Factory::get_loaned_offset());
  iox2_port_factory_publisher_builder_set_allocation_strategy(&pub_builder_handle, iox2_allocation_strategy_e_BEST_FIT);

  if (depth > 0) {
    iox2_port_factory_publisher_builder_set_max_loaned_samples(&pub_builder_handle, static_cast<size_t>(depth));
  }

  if VUNLIKELY (iox2_port_factory_publisher_builder_create(pub_builder_handle, &publisher_storage_, &publisher_) !=
                IOX2_OK) {
    VLOG_F("Shm2Factory: Failed to create publisher for: ", service_name_, ".");
    return;
  }

  const std::string notify_svc = Shm2Factory::make_service_name(address, "event_notify", domain);

  if VUNLIKELY (!open_or_create_event_service(factory.get_node(), notify_svc, event_pf_storage_, event_pf_handle_)) {
    VLOG_F("Shm2Factory: Failed to open publisher event service: ", notify_svc, ".");
    return;
  }

  iox2_port_factory_notifier_builder_t notifier_builder_storage{};
  auto* notifier_builder = iox2_port_factory_event_notifier_builder(&event_pf_handle_, &notifier_builder_storage);

  if VUNLIKELY (iox2_port_factory_notifier_builder_create(notifier_builder, &notifier_storage_, &notifier_) !=
                IOX2_OK) {
    VLOG_F("Shm2Factory: Failed to create publisher notifier for: ", notify_svc, ".");
    return;
  }

  if (wait > 0) {
    sem_.emplace();
    std::string sem_address = address;
    std::replace(sem_address.begin(), sem_address.end(), '/', '@');
#ifdef __FreeBSD__
    sem_->attach("/vlink@shm2@" + sem_address);
#else
    sem_->attach("vlink@shm2@" + sem_address);
#endif
  }
}

Shm2Publisher::~Shm2Publisher() {
  quit_flag_.store(true, std::memory_order_release);

  if (sem_) {
    sem_->detach(true);
  }

  disable_detect_timer();

  {
    std::lock_guard lock(loan_mtx_);

    for (auto& [_, entry] : loan_map_) {
      iox2_sample_mut_drop(entry.handle);
    }

    loan_map_.clear();
  }

  if (notifier_) {
    iox2_notifier_drop(notifier_);
    notifier_ = nullptr;
  }

  if (event_pf_handle_) {
    iox2_port_factory_event_drop(event_pf_handle_);
    event_pf_handle_ = nullptr;
  }

  if (publisher_) {
    iox2_publisher_drop(publisher_);
    publisher_ = nullptr;
  }

  if (pf_handle_) {
    iox2_port_factory_pub_sub_drop(pf_handle_);
    pf_handle_ = nullptr;
  }
}

std::any Shm2Publisher::get_native_handle() const { return this; }

bool Shm2Publisher::has_subscribers() const {
  int count = iox2_port_factory_pub_sub_dynamic_config_number_of_subscribers(&pf_handle_);

  return count > 0;
}

Bytes Shm2Publisher::loan(uint64_t channel, int64_t size) {
  (void)channel;

  if VUNLIKELY (size <= 0) {
    return Bytes();
  }

  size_t total = static_cast<size_t>(size) + Shm2Factory::get_loaned_offset();

  auto storage = std::make_unique<iox2_sample_mut_t>();
  iox2_sample_mut_h sample_handle{nullptr};

  void* write_ptr = nullptr;
  size_t write_elems = 0;

  {
    std::unique_lock connection_lock(connection_mtx_, std::defer_lock);

    if VUNLIKELY (history_ > 0) {
      connection_lock.lock();
    }

    if VUNLIKELY (!publisher_ ||
                  iox2_publisher_loan_slice_uninit(&publisher_, storage.get(), &sample_handle, total) != IOX2_OK) {
      VLOG_E("Shm2Factory: Failed to loan publisher buffer, size: ", total, ".");
      return Bytes();
    }

    iox2_sample_mut_payload_mut(&sample_handle, &write_ptr, &write_elems);
  }

  auto* write_buf = static_cast<uint8_t*>(write_ptr);

  if VUNLIKELY (write_buf == nullptr || write_elems < total) {
    VLOG_E("Shm2Factory: Invalid loaned publisher payload, size: ", write_elems, ", expected: ", total, ".");

    iox2_sample_mut_drop(sample_handle);
    return Bytes();
  }

  const uint8_t* user_ptr = write_buf + Shm2Factory::get_loaned_offset();

  {
    std::lock_guard lock(loan_mtx_);
    loan_map_.emplace(user_ptr, PublisherLoanEntry{std::move(storage), sample_handle});
  }

  return Bytes::loan_internal(user_ptr, size);
}

bool Shm2Publisher::release(const Bytes& bytes) {
  if VUNLIKELY (!bytes.is_loaned()) {
    return false;
  }

  PublisherLoanEntry entry;

  {
    std::lock_guard lock(loan_mtx_);
    auto it = loan_map_.find(bytes.data());

    if (it == loan_map_.end()) {
      return false;
    }

    entry = std::move(it->second);
    loan_map_.erase(it);
  }

  iox2_sample_mut_drop(entry.handle);
  return true;
}

void Shm2Publisher::notify_and_wait(size_t recipients) {
  if (notifier_ && recipients > 0) {
    auto notify_count = notify_counter_.fetch_add(1, std::memory_order_relaxed) + 1;

    if (notify_count >= notify_every_) {
      notify_counter_.store(0, std::memory_order_relaxed);
      iox2_notifier_notify(&notifier_, nullptr);
    }
  }

  if VUNLIKELY (wait_ > 0) {
    sem_->acquire(recipients, wait_);
  }
}

bool Shm2Publisher::publish(uint64_t channel, const Bytes& bytes) {
  if VUNLIKELY (!publisher_) {
    return false;
  }

  if VUNLIKELY (wait_ > 0) {
    uint64_t sem_count = sem_->get_count();

    if (sem_count > 0) {
      sem_->acquire(sem_count, wait_);
    }
  }

  if (bytes.is_loaned()) {
    PublisherLoanEntry entry;

    {
      std::lock_guard lock(loan_mtx_);
      auto it = loan_map_.find(bytes.data());

      if VUNLIKELY (it == loan_map_.end()) {
        VLOG_E("Shm2Factory: publish() on loaned bytes from a foreign publisher.");
        return false;
      }

      entry = std::move(it->second);
      loan_map_.erase(it);
    }

    const uint64_t seq = seq_.fetch_add(1, std::memory_order_relaxed) + 1;

    auto* buf_base = const_cast<uint8_t*>(bytes.data()) - Shm2Factory::get_loaned_offset();
    Shm2Factory::write_header(buf_base, channel, seq);

    size_t recipients = 0;
    int ret = IOX2_OK;

    {
      std::unique_lock connection_lock(connection_mtx_, std::defer_lock);

      if VUNLIKELY (history_ > 0) {
        connection_lock.lock();
      }

      ret = iox2_sample_mut_send(entry.handle, &recipients);
    }

    if VUNLIKELY (ret != IOX2_OK) {
      VLOG_E("Shm2Factory: Failed to publish loaned sample, error: ", ret, ".");
      return false;
    }

    notify_and_wait(recipients);

    return true;
  }

  const size_t total = bytes.size() + Shm2Factory::get_loaned_offset();

  if (bytes.size() < loan_send_threshold_) {
    thread_local std::vector<uint8_t> scratch;

    if (scratch.size() < total) {
      scratch.resize(total);
    }

    const uint64_t seq = seq_.fetch_add(1, std::memory_order_relaxed) + 1;
    Shm2Factory::write_data(scratch.data(), channel, seq, bytes);

    size_t recipients = 0;
    int send_ret = IOX2_OK;

    {
      std::unique_lock connection_lock(connection_mtx_, std::defer_lock);

      if VUNLIKELY (history_ > 0) {
        connection_lock.lock();
      }

      send_ret = iox2_publisher_send_slice_copy(&publisher_, scratch.data(), sizeof(uint8_t), total, &recipients);
    }

    if VUNLIKELY (send_ret != IOX2_OK) {
      VLOG_E("Shm2Factory: Failed to send_slice_copy, error: ", send_ret, ".");
      return false;
    }

    notify_and_wait(recipients);

    return true;
  }

  iox2_sample_mut_t sample_storage{};
  iox2_sample_mut_h sample_handle{nullptr};

  {
    std::unique_lock connection_lock(connection_mtx_, std::defer_lock);

    if VUNLIKELY (history_ > 0) {
      connection_lock.lock();
    }

    if VUNLIKELY (iox2_publisher_loan_slice_uninit(&publisher_, &sample_storage, &sample_handle, total) != IOX2_OK) {
      VLOG_E("Shm2Factory: Failed to loan publisher buffer, size: ", total, ".");
      return false;
    }
  }

  const uint64_t seq = seq_.fetch_add(1, std::memory_order_relaxed) + 1;

  size_t recipients = 0;
  void* write_ptr = nullptr;
  size_t write_elems = 0;
  iox2_sample_mut_payload_mut(&sample_handle, &write_ptr, &write_elems);

  auto* write_buf = static_cast<uint8_t*>(write_ptr);

  if VUNLIKELY (write_buf == nullptr || write_elems < total) {
    VLOG_E("Shm2Factory: Invalid loaned publisher payload, size: ", write_elems, ", expected: ", total, ".");

    iox2_sample_mut_drop(sample_handle);
    return false;
  }

  Shm2Factory::write_data(write_buf, channel, seq, bytes);

  int send_ret = IOX2_OK;

  {
    std::unique_lock connection_lock(connection_mtx_, std::defer_lock);

    if VUNLIKELY (history_ > 0) {
      connection_lock.lock();
    }

    send_ret = iox2_sample_mut_send(sample_handle, &recipients);
  }

  if VUNLIKELY (send_ret != IOX2_OK) {
    VLOG_E("Shm2Factory: Failed to publish sample, error: ", send_ret, ".");
    return false;
  }

  notify_and_wait(recipients);

  return true;
}

void Shm2Publisher::update_connections() {
  if (history_ <= 0) {
    return;
  }

  std::lock_guard lock(connection_mtx_);

  if VLIKELY (publisher_) {
    iox2_publisher_update_connections(&publisher_);
  }
}

void Shm2Publisher::enable_detect_timer() {
  if (!has_detect_timer_.load(std::memory_order_relaxed)) {
    has_detect_timer_.store(true, std::memory_order_relaxed);

    Shm2Factory::get().add_detect_event_callback(pf_handle_, [weak = weak_from_this()]() {
      auto self = weak.lock();

      if VLIKELY (self) {
        self->detect_subscribers();
      }
    });
  }
}

void Shm2Publisher::disable_detect_timer() {
  if (has_detect_timer_.load(std::memory_order_relaxed)) {
    has_detect_timer_.store(false, std::memory_order_relaxed);
    Shm2Factory::get().remove_detect_event_callback(pf_handle_);
  }
}

void Shm2Publisher::detect_subscribers() {
  if VUNLIKELY (quit_flag_.load(std::memory_order_acquire)) {
    return;
  }

  discovery_subscribers(has_subscribers());
}

void Shm2Publisher::discovery_subscribers(bool has_subs) {
  if VLIKELY (last_has_subscribers_.load(std::memory_order_relaxed) == has_subs) {
    return;
  }

  traverse_sub_connect_callback([has_subs](NodeImpl*, const auto& callback) { callback(has_subs); });
  last_has_subscribers_.store(has_subs, std::memory_order_relaxed);

  if (has_subs && history_ > 0) {
    update_connections();

    if (notifier_) {
      iox2_notifier_notify(&notifier_, nullptr);
    }
  }
}

// Shm2Subscriber
Shm2Subscriber::Shm2Subscriber(const ShmID2& id) {
  static auto& factory = Shm2Factory::get();

  const auto& [impl_type, address, domain, depth, history, wait, size] = id;

  domain_ = domain;
  wait_ = wait;
  payload_size_ = size;
  history_ = history;
  depth_ = depth;

  service_name_ = Shm2Factory::make_service_name(address, "event", domain);

  iox2_service_name_t sn_storage{};
  iox2_service_name_h sn_handle{nullptr};

  if VUNLIKELY (!make_iox2_service_name(service_name_, sn_storage, sn_handle)) {
    VLOG_F("Shm2Factory: Failed to create subscriber service name: ", service_name_, ".");
    return;
  }

  iox2_service_builder_t sb_storage{};
  iox2_service_builder_h sb_handle =
      iox2_node_service_builder(factory.get_node(), &sb_storage, iox2_cast_service_name_ptr(sn_handle));
  iox2_service_name_drop(sn_handle);

  iox2_service_builder_pub_sub_h ps_builder = iox2_service_builder_pub_sub(sb_handle);

  static const char kTypeName[] = "u8";
  iox2_service_builder_pub_sub_set_payload_type_details(&ps_builder, iox2_type_variant_e_DYNAMIC, kTypeName,
                                                        sizeof(kTypeName) - 1, sizeof(uint8_t), alignof(uint8_t));
  iox2_service_builder_pub_sub_set_max_publishers(&ps_builder, 512);
  iox2_service_builder_pub_sub_set_max_subscribers(&ps_builder, 512);

  if (depth > 0) {
    iox2_service_builder_pub_sub_set_subscriber_max_buffer_size(&ps_builder, static_cast<size_t>(depth));
  }

  iox2_service_builder_pub_sub_set_enable_safe_overflow(&ps_builder, true);

  if (history > 0) {
    iox2_service_builder_pub_sub_set_history_size(&ps_builder, static_cast<size_t>(history));
  }

  int ret = iox2_service_builder_pub_sub_open_or_create(ps_builder, &pf_storage_, &pf_handle_);

  if VUNLIKELY (ret != IOX2_OK) {
    VLOG_F("Shm2Factory: Failed to open/create subscriber pub-sub service: ", service_name_, ".");
    return;
  }

  if (wait_ > 0) {
    sem_.emplace();
    std::string sem_address = address;
    std::replace(sem_address.begin(), sem_address.end(), '/', '@');
#ifdef __FreeBSD__
    sem_->attach("/vlink@shm2@" + sem_address);
#else
    sem_->attach("vlink@shm2@" + sem_address);
#endif
  }
}

Shm2Subscriber::~Shm2Subscriber() {
  unsubscribe();

  {
    std::lock_guard lock(loan_mtx_);

    for (auto& [_, entry] : loan_map_) {
      iox2_sample_drop(entry.handle);
    }

    loan_map_.clear();
  }

  if (sem_) {
    sem_->detach(false);
  }

  if (pf_handle_) {
    iox2_port_factory_pub_sub_drop(pf_handle_);
    pf_handle_ = nullptr;
  }
}

std::any Shm2Subscriber::get_native_handle() const { return this; }

bool Shm2Subscriber::suspend() {
  is_suspend_.store(true, std::memory_order_relaxed);
  return true;
}
bool Shm2Subscriber::resume() {
  is_suspend_.store(false, std::memory_order_relaxed);
  return true;
}
bool Shm2Subscriber::is_suspend() const { return is_suspend_.load(std::memory_order_relaxed); }

void Shm2Subscriber::process_message() {
  if VUNLIKELY (!subscriber_) {
    return;
  }

  bool has_samples = false;

  while (iox2_subscriber_has_samples(&subscriber_, &has_samples) == IOX2_OK && has_samples) {
    std::unique_ptr<iox2_sample_t> heap_storage;
    iox2_sample_t stack_storage{};
    iox2_sample_t* storage_ptr = nullptr;

    if VUNLIKELY (manual_unloan_.load(std::memory_order_relaxed)) {
      heap_storage = std::make_unique<iox2_sample_t>();
      storage_ptr = heap_storage.get();
    } else {
      storage_ptr = &stack_storage;
    }

    iox2_sample_h sample_handle{nullptr};
    auto ret = iox2_subscriber_receive(&subscriber_, storage_ptr, &sample_handle);

    if VUNLIKELY (ret != IOX2_OK || !sample_handle) {
      break;
    }

    if (is_suspend_.load(std::memory_order_relaxed)) {
      iox2_sample_drop(sample_handle);
      continue;
    }

    const void* payload_ptr = nullptr;
    size_t num_elements = 0;
    iox2_sample_payload(&sample_handle, &payload_ptr, &num_elements);

    if VUNLIKELY (!payload_ptr || num_elements == 0) {
      iox2_sample_drop(sample_handle);
      continue;
    }

    const auto* read_msg = static_cast<const uint8_t*>(payload_ptr);

    uint64_t channel = 0;
    uint64_t seq = 0;
    Bytes msg_bytes;
    Shm2Factory::read_data(read_msg, static_cast<uint64_t>(num_elements), channel, seq, msg_bytes);

    if VUNLIKELY (is_latency_and_lost_enabled_.load(std::memory_order_acquire) && seq > 0) {
      calc_sample_.update(seq, 0);
    }

    if VUNLIKELY (manual_unloan_.load(std::memory_order_relaxed)) {
      std::lock_guard lock(loan_mtx_);
      loan_map_.emplace(msg_bytes.data(), SubscriberLoanEntry{std::move(heap_storage), sample_handle});
    }

    bool called = false;

    traverse_msg_callback([channel, &msg_bytes, &called](NodeImpl* impl, const auto& callback) {
      const auto* conf_ptr = impl->get_target_conf<Shm2Conf>();

      if (static_cast<uint64_t>(conf_ptr->hash_code) != channel) {
        return;
      }

      called = true;
      callback(msg_bytes);
    });

    if VUNLIKELY (manual_unloan_.load(std::memory_order_relaxed) && !called) {
      SubscriberLoanEntry entry;

      {
        std::lock_guard lock(loan_mtx_);
        auto it = loan_map_.find(msg_bytes.data());

        if (it != loan_map_.end()) {
          entry = std::move(it->second);
          loan_map_.erase(it);
        }
      }

      if (entry.handle) {
        iox2_sample_drop(entry.handle);
      }

      if (sem_) {
        sem_->release();
      }
    } else if VLIKELY (!manual_unloan_.load(std::memory_order_relaxed)) {
      iox2_sample_drop(sample_handle);

      if (sem_) {
        sem_->release();
      }
    }
  }
}

void Shm2Subscriber::subscribe() {
  bool expected = false;

  if VUNLIKELY (!is_subscribed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                        std::memory_order_relaxed)) {
    return;
  }

  static auto& factory = Shm2Factory::get();

  iox2_port_factory_subscriber_builder_t sub_builder_storage{};
  sub_builder_ = iox2_port_factory_pub_sub_subscriber_builder(&pf_handle_, &sub_builder_storage);

  if (depth_ > 0) {
    iox2_port_factory_subscriber_builder_set_buffer_size(&sub_builder_, static_cast<size_t>(depth_));
  }

  if VUNLIKELY (iox2_port_factory_subscriber_builder_create(sub_builder_, &subscriber_storage_, &subscriber_) !=
                IOX2_OK) {
    VLOG_F("Shm2Factory: Failed to create subscriber for: ", service_name_, ".");
    is_subscribed_.store(false, std::memory_order_relaxed);
    return;
  }

  const std::string notify_svc = [this]() {
    auto slash = service_name_.rfind('/');
    std::string base = (slash != std::string::npos) ? service_name_.substr(0, slash + 1) : "";

    if (domain_ != 0) {
      return base + "event_notify_" + std::to_string(domain_);
    }

    return base + "event_notify";
  }();

  if VUNLIKELY (!open_or_create_event_service(factory.get_node(), notify_svc, event_pf_storage_, event_pf_handle_)) {
    VLOG_F("Shm2Factory: Failed to open subscriber event service: ", notify_svc, ".");
    if (subscriber_) {
      iox2_subscriber_drop(subscriber_);
      subscriber_ = nullptr;
    }
    is_subscribed_.store(false, std::memory_order_relaxed);
    return;
  }

  iox2_port_factory_listener_builder_t listener_builder_storage{};
  auto* listener_builder = iox2_port_factory_event_listener_builder(&event_pf_handle_, &listener_builder_storage);

  if VUNLIKELY (iox2_port_factory_listener_builder_create(listener_builder, &listener_storage_, &listener_) !=
                IOX2_OK) {
    VLOG_F("Shm2Factory: Failed to create subscriber listener for: ", notify_svc, ".");
    if (event_pf_handle_) {
      iox2_port_factory_event_drop(event_pf_handle_);
      event_pf_handle_ = nullptr;
    }
    if (subscriber_) {
      iox2_subscriber_drop(subscriber_);
      subscriber_ = nullptr;
    }
    is_subscribed_.store(false, std::memory_order_relaxed);
    return;
  }

  iox2_listener_h_ref listener_poll = nullptr;

#if VLINK_SHM2_NO_FD_LISTENER
  listener_poll = &listener_;
#else
  iox2_file_descriptor_ptr fd = iox2_listener_get_file_descriptor(&listener_);

  if (fd != nullptr) {
    int ret = iox2_waitset_attach_notification(factory.get_waitset(), fd, &guard_storage_, &guard_);

    if VUNLIKELY (ret != IOX2_OK) {
      VLOG_F("Shm2Factory: Failed to attach subscriber listener fd to waitset, error: ", ret, ".");

      if (listener_) {
        iox2_listener_drop(listener_);
        listener_ = nullptr;
      }

      if (event_pf_handle_) {
        iox2_port_factory_event_drop(event_pf_handle_);
        event_pf_handle_ = nullptr;
      }

      if (subscriber_) {
        iox2_subscriber_drop(subscriber_);
        subscriber_ = nullptr;
      }

      is_subscribed_.store(false, std::memory_order_relaxed);
      return;
    }
  } else {
    listener_poll = &listener_;
  }
#endif

  factory.register_poll(
      this,
      [this]() {
        bool has_received_event = false;
        iox2_event_id_t event_id;

        do {
          int drain_ret = iox2_listener_try_wait_one(&listener_, &event_id, &has_received_event);

          if VUNLIKELY (drain_ret != IOX2_OK) {
            VLOG_E("Shm2Factory: Failed to drain subscriber listener, error: ", drain_ret, ".");
            break;
          }
        } while (has_received_event);

        bool has_samples = false;

        if (iox2_subscriber_has_samples(&subscriber_, &has_samples) != IOX2_OK || !has_samples) {
          return;
        }

        auto* impl = get_first_impl();

        if VUNLIKELY (!impl) {
          return;
        }

        auto* ml = impl->get_message_loop();

        if (ml) {
          std::weak_ptr<Shm2Subscriber> weak_self = weak_from_this();
          ml->post_task([weak_self]() {
            auto self = weak_self.lock();

            if VUNLIKELY (!self) {
              return;
            }

            auto* impl = self->get_first_impl();

            if VUNLIKELY (!impl || !impl->get_message_loop()) {
              return;
            }

            self->process_message();
          });
        } else {
          process_message();
        }
      },
      guard_, listener_poll, listener_poll != nullptr, &event_pf_handle_);
}

void Shm2Subscriber::unsubscribe() {
  bool expected = true;

  if VUNLIKELY (!is_subscribed_.compare_exchange_strong(expected, false, std::memory_order_acq_rel,
                                                        std::memory_order_relaxed)) {
    return;
  }

  Shm2Factory::get().unregister_poll(this);

  if (listener_) {
    iox2_listener_drop(listener_);
    listener_ = nullptr;
  }

  if (event_pf_handle_) {
    iox2_port_factory_event_drop(event_pf_handle_);
    event_pf_handle_ = nullptr;
  }

  if (subscriber_) {
    iox2_subscriber_drop(subscriber_);
    subscriber_ = nullptr;
  }
}

void Shm2Subscriber::set_manual_unloan(bool manual_unloan) {
  manual_unloan_.store(manual_unloan, std::memory_order_relaxed);
}

bool Shm2Subscriber::release(const Bytes& bytes) {
  if VUNLIKELY (!bytes.is_loaned()) {
    return false;
  }

  if VUNLIKELY (!manual_unloan_.load(std::memory_order_relaxed)) {
    VLOG_F("Shm2Factory: Manual release is not supported without manual_unloan mode.");
    return false;
  }

  SubscriberLoanEntry entry;

  {
    std::lock_guard lock(loan_mtx_);
    auto it = loan_map_.find(bytes.data());

    if VUNLIKELY (it == loan_map_.end()) {
      VLOG_F("Shm2Factory: release() called on bytes not tracked in loan_map.");
      return false;
    }

    entry = std::move(it->second);
    loan_map_.erase(it);
  }

  iox2_sample_drop(entry.handle);

  if (sem_) {
    sem_->release();
  }

  return true;
}

void Shm2Subscriber::set_latency_and_lost_enabled(bool enable) {
  is_latency_and_lost_enabled_.store(enable, std::memory_order_release);
}

bool Shm2Subscriber::is_latency_and_lost_enabled() const {
  return is_latency_and_lost_enabled_.load(std::memory_order_acquire);
}

const CalculateSample& Shm2Subscriber::get_calculate_sample() const { return calc_sample_; }

}  // namespace vlink
