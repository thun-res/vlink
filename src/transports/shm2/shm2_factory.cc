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

#include <algorithm>
#include <charconv>
#include <chrono>
#include <exception>
#include <new>
#include <string>
#include <thread>
#include <utility>

#include "./base/utils.h"
#include "./impl/server_impl.h"

#ifndef VLINK_SHM2_NO_FD_LISTENER
#define VLINK_SHM2_NO_FD_LISTENER 0
#endif

namespace vlink {

static constexpr uint32_t kShm2IdleWaitTimeoutNs = 1U * 1000U * 1000U;

static void shm2_drain_event_noop(const iox2_event_id_t* event_id, iox2_callback_context ctx) {
  (void)event_id;
  (void)ctx;
}

static void drain_listener_events(iox2_listener_h_ref listener, const char* label) {
  const int drain_ret = iox2_listener_try_wait_all(listener, shm2_drain_event_noop, nullptr);

  if VUNLIKELY (drain_ret != IOX2_OK) {
    VLOG_E("Shm2Factory: Failed to drain ", label, ", error: ", drain_ret, ".");
  }
}

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

  if VUNLIKELY (Shm2Transport::get_thread_count() != 1) {
    VLOG_W("Shm2Factory: Shm2 does not support setting thread count.");
  }

  message_loop_.set_name("SHM2-FACTORY");

  detect_timer_.attach(&message_loop_);
  detect_timer_.set_interval(10);
  detect_timer_.set_loop_count(Timer::kInfinite);
  detect_timer_.set_callback([this]() {
    detect_timer_.set_interval(50);

    std::vector<DetectCallback> callbacks;

    {
      std::shared_lock lock(detect_mtx_);

      callbacks.reserve(detect_event_map_.size() + detect_method_map_.size());

      for (const auto& [handle, callback] : detect_event_map_) {
        callbacks.push_back(callback);
      }

      for (const auto& [handle, callback] : detect_method_map_) {
        callbacks.push_back(callback);
      }
    }

    for (const auto& callback : callbacks) {
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
  iox2_waitset_builder_set_signal_handling_mode(&waitset_builder, iox2_signal_handling_mode_e_DISABLED);

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
  poll_thread_id_ = poll_thread_.get_id();
  Utils::set_thread_name("VShm2Poll", &poll_thread_);

  message_loop_.async_run();
}

Shm2Factory::~Shm2Factory() {
#ifdef _WIN32
  if (Utils::is_terminating()) {
    if (poll_thread_.joinable()) {
      poll_thread_.detach();
    }

    (void)::new (std::nothrow) auto(std::move(blocking_waiters_));
    return;
  }
#endif

  poll_quit_.store(true, std::memory_order_release);

  {
    std::lock_guard lock(poll_cmd_mtx_);

    if (wakeup_notifier_) {
      iox2_notifier_notify(&wakeup_notifier_, nullptr);
    }
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

  flush_deferred_waitset_ops();

  std::vector<PollEntry> remaining_entries;

  {
    std::lock_guard lock(poll_cmd_mtx_);

    if (wakeup_guard_) {
      iox2_waitset_guard_drop(wakeup_guard_);
      wakeup_guard_ = nullptr;
    }

    for (auto& entry : poll_entries_) {
      if (entry.guard) {
        iox2_waitset_guard_drop(entry.guard);
        entry.guard = nullptr;
      }
    }

    remaining_entries.swap(poll_entries_);
  }

  remaining_entries.clear();

  if (waitset_) {
    iox2_waitset_drop(waitset_);
    waitset_ = nullptr;
  }

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

  if (node_) {
    iox2_node_drop(node_);
    node_ = nullptr;
  }

  if (config_) {
    iox2_config_drop(config_);
    config_ = nullptr;
  }
}

std::string Shm2Factory::make_service_name(const std::string& address, const std::string& suffix, int32_t domain,
                                           const std::string& event) {
  std::string n = "vlink/" + address + "/" + suffix;

  if (domain != 0) {
    n += "_" + std::to_string(domain);
  }

  if (!event.empty()) {
    n += '@' + event;
  }

  return n;
}

iox2_node_h_ref Shm2Factory::get_node() const { return &node_; }

int Shm2Factory::register_waitset_poll(void* handle, iox2_file_descriptor_ptr fd, PollCallback&& callback) {
  int ret = -1;
  PollCallback old_callback;

  run_on_poll_thread([this, handle, fd, &callback, &old_callback, &ret]() {
    if VUNLIKELY (poll_quit_.load(std::memory_order_acquire) || !waitset_) {
      ret = -1;
      return;
    }

    if VUNLIKELY (in_on_process_) {
      for (size_t i = 0; i < poll_entries_.size(); ++i) {
        if VUNLIKELY (poll_entries_[i].handle == handle) {
          deferred_guard_drops_.push_back(DeferredGuardDrop{
              std::move(poll_entries_[i].guard_storage), poll_entries_[i].guard, std::move(poll_entries_[i].callback)});

          if (i + 1U < poll_entries_.size()) {
            poll_entries_[i] = std::move(poll_entries_.back());
          }

          poll_entries_.pop_back();
          break;
        }
      }

      for (auto& pending : deferred_attaches_) {
        if (pending.handle == handle) {
          old_callback = std::move(pending.callback);
          pending.fd = fd;
          pending.callback = std::move(callback);
          stop_on_process_ = true;
          ret = IOX2_OK;
          return;
        }
      }

      deferred_attaches_.push_back(DeferredAttach{handle, fd, std::move(callback)});
      stop_on_process_ = true;
      ret = IOX2_OK;
      return;
    }

    size_t existing_idx = poll_entries_.size();

    for (size_t i = 0; i < poll_entries_.size(); ++i) {
      if VUNLIKELY (poll_entries_[i].handle == handle) {
        existing_idx = i;

        if (poll_entries_[i].guard) {
          iox2_waitset_guard_drop(poll_entries_[i].guard);
          poll_entries_[i].guard = nullptr;
        }

        break;
      }
    }

    auto guard_storage = std::make_unique<iox2_waitset_guard_t>();
    iox2_waitset_guard_h guard = nullptr;

    ret = iox2_waitset_attach_notification(&waitset_, fd, guard_storage.get(), &guard);

    if VUNLIKELY (ret != IOX2_OK) {
      if (existing_idx < poll_entries_.size()) {
        old_callback = std::move(poll_entries_[existing_idx].callback);

        if (existing_idx + 1U < poll_entries_.size()) {
          poll_entries_[existing_idx] = std::move(poll_entries_.back());
        }

        poll_entries_.pop_back();
      }

      return;
    }

    if VUNLIKELY (existing_idx < poll_entries_.size()) {
      old_callback = std::move(poll_entries_[existing_idx].callback);
      poll_entries_[existing_idx].callback = std::move(callback);
      poll_entries_[existing_idx].guard_storage = std::move(guard_storage);
      poll_entries_[existing_idx].guard = guard;
      return;
    }

    poll_entries_.push_back(PollEntry{handle, std::move(callback), std::move(guard_storage), guard});
  });

  return ret;
}

void Shm2Factory::run_on_poll_thread(MoveFunction<void()>&& task) {
  if (poll_thread_id_ == std::thread::id() || std::this_thread::get_id() == poll_thread_id_) {
    task();
    return;
  }

  bool done = false;

  {
    std::unique_lock lock(poll_cmd_mtx_);

    if VUNLIKELY (poll_stopped_) {
      task();
      return;
    }

    poll_cmds_.emplace_back([this, &done, task = std::move(task)]() mutable {
      task();

      {
        std::lock_guard done_lock(poll_cmd_mtx_);
        done = true;
      }

      poll_cmd_cv_.notify_all();
    });

    if (wakeup_notifier_) {
      iox2_notifier_notify(&wakeup_notifier_, nullptr);
    }
  }

  std::unique_lock lock(poll_cmd_mtx_);
  poll_cmd_cv_.wait(lock, [&done]() { return done; });
}

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

void Shm2Factory::register_poll(void* handle, PollCallback&& callback, iox2_listener_h_ref listener,
                                bool poll_without_event, iox2_port_factory_event_h_ref event_pf) {
  if VUNLIKELY (listener == nullptr) {
    return;
  }

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

  std::lock_guard lock(blocking_mtx_);
  blocking_waiters_[handle] = waiter;

  waiter->thread = std::thread([waiter]() { Shm2Factory::blocking_wait_func(waiter.get()); });
  Utils::set_thread_name("VShm2Wait", &waiter->thread);
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

  PollCallback callback;

  if (waiter) {
    stop_blocking_waiter(waiter.get());
  } else {
    run_on_poll_thread([this, handle, &callback]() {
      for (size_t i = 0; i < deferred_attaches_.size(); ++i) {
        if (deferred_attaches_[i].handle == handle) {
          callback = std::move(deferred_attaches_[i].callback);

          if (i + 1U < deferred_attaches_.size()) {
            deferred_attaches_[i] = std::move(deferred_attaches_.back());
          }

          deferred_attaches_.pop_back();
          break;
        }
      }

      for (size_t i = 0; i < poll_entries_.size(); ++i) {
        if (poll_entries_[i].handle == handle) {
          if VUNLIKELY (in_on_process_) {
            deferred_guard_drops_.push_back(DeferredGuardDrop{std::move(poll_entries_[i].guard_storage),
                                                              poll_entries_[i].guard,
                                                              std::move(poll_entries_[i].callback)});
            stop_on_process_ = true;
          } else {
            if (poll_entries_[i].guard) {
              iox2_waitset_guard_drop(poll_entries_[i].guard);
              poll_entries_[i].guard = nullptr;
            }

            callback = std::move(poll_entries_[i].callback);
          }

          if (i + 1U < poll_entries_.size()) {
            poll_entries_[i] = std::move(poll_entries_.back());
          }

          poll_entries_.pop_back();
          break;
        }
      }
    });
  }
}

void Shm2Factory::flush_deferred_waitset_ops() {
  if VUNLIKELY (!deferred_guard_drops_.empty()) {
    std::vector<DeferredGuardDrop> drops;
    drops.swap(deferred_guard_drops_);

    for (auto& item : drops) {
      if (item.guard) {
        iox2_waitset_guard_drop(item.guard);
        item.guard = nullptr;
      }
    }

    drops.clear();
  }

  if VUNLIKELY (!deferred_attaches_.empty()) {
    std::vector<DeferredAttach> attaches;
    attaches.swap(deferred_attaches_);

    if VUNLIKELY (poll_quit_.load(std::memory_order_acquire) || !waitset_) {
      attaches.clear();
      return;
    }

    for (auto& item : attaches) {
      auto guard_storage = std::make_unique<iox2_waitset_guard_t>();
      iox2_waitset_guard_h guard = nullptr;
      int ret = iox2_waitset_attach_notification(&waitset_, item.fd, guard_storage.get(), &guard);

      if VUNLIKELY (ret != IOX2_OK) {
        VLOG_E("Shm2Factory: Failed to attach deferred listener fd to waitset, error: ", ret, ".");
        continue;
      }

      poll_entries_.push_back(PollEntry{item.handle, std::move(item.callback), std::move(guard_storage), guard});
    }
  }
}

void Shm2Factory::poll_thread_func() {
  auto drain_commands = [this]() {
    std::vector<MoveFunction<void()>> pending;

    {
      std::lock_guard lock(poll_cmd_mtx_);

      if (poll_cmds_.empty()) {
        return;
      }

      pending.swap(poll_cmds_);
    }

    for (auto& cmd : pending) {
      cmd();
    }
  };

  while (!poll_quit_.load(std::memory_order_acquire)) {
    drain_commands();

    iox2_waitset_run_result_e result = iox2_waitset_run_result_e_STOP_REQUEST;
    int ret = IOX2_OK;

    if (wakeup_guard_ != nullptr && wakeup_notifier_ != nullptr) {
      ret = iox2_waitset_wait_and_process(&waitset_, Shm2Factory::on_process, this, &result);
    } else {
      ret = iox2_waitset_wait_and_process_once_with_timeout(&waitset_, Shm2Factory::on_process, this, 0,
                                                            kShm2IdleWaitTimeoutNs, &result);
    }

    flush_deferred_waitset_ops();
    drain_commands();

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

  flush_deferred_waitset_ops();

  while (true) {
    std::vector<MoveFunction<void()>> cmds;

    {
      std::lock_guard lock(poll_cmd_mtx_);

      if (poll_cmds_.empty()) {
        poll_stopped_ = true;
        break;
      }

      cmds.swap(poll_cmds_);
    }

    for (auto& cmd : cmds) {
      cmd();
    }
  }

  poll_cmd_cv_.notify_all();
}

iox2_callback_progression_e Shm2Factory::on_process(iox2_waitset_attachment_id_h attachment_id, void* context) {
  auto& factory = *static_cast<Shm2Factory*>(context);

  if VUNLIKELY (factory.poll_quit_.load(std::memory_order_acquire)) {
    iox2_waitset_attachment_id_drop(attachment_id);
    return iox2_callback_progression_e_STOP;
  }

  if (factory.wakeup_guard_ && iox2_waitset_attachment_id_has_event_from(&attachment_id, &factory.wakeup_guard_)) {
    drain_listener_events(&factory.wakeup_listener_, "wakeup listener");

    iox2_waitset_attachment_id_drop(attachment_id);
    return iox2_callback_progression_e_STOP;
  }

  PollCallback matched_callback;

  for (const auto& entry : factory.poll_entries_) {
    if VUNLIKELY (entry.guard == nullptr) {
      continue;
    }

    if (iox2_waitset_attachment_id_has_event_from(&attachment_id, &entry.guard)) {
      matched_callback = entry.callback;
      break;
    }
  }

  bool should_stop = false;

  if VLIKELY (matched_callback) {
    struct OnProcessScope final {
      Shm2Factory& owner;

      ~OnProcessScope() { owner.in_on_process_ = false; }
    };

    factory.in_on_process_ = true;
    factory.stop_on_process_ = false;
    OnProcessScope scope{factory};

    try {
      matched_callback();
    } catch (const std::exception& e) {
      VLOG_E("Shm2Factory: WaitSet poll callback failed: ", e.what(), ".");
    } catch (...) {
      VLOG_E("Shm2Factory: WaitSet poll callback failed.");
    }

    should_stop = factory.stop_on_process_;
  }

  iox2_waitset_attachment_id_drop(attachment_id);

  return should_stop ? iox2_callback_progression_e_STOP : iox2_callback_progression_e_CONTINUE;
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

// Shm2Server
Shm2Server::Shm2Server(const ShmID2& id) {
  static auto& factory = Shm2Factory::get();

  const auto& [impl_type, address, domain, depth, history, wait, size, event, owner] = id;

  payload_size_ = size;

  service_name_ = Shm2Factory::make_service_name(address, "method", domain, event);

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

  const std::string notify_svc = service_name_ + "/response";

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

  const std::string req_notify_svc = service_name_ + "/request";

  auto ctx = std::make_shared<Shm2ListenerContext>();

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

  Shm2Factory::PollCallback poll_cb = [this, ctx]() {
    drain_listener_events(&ctx->listener, "server req-listener");

    bool has_requests = false;

    {
      std::lock_guard receive_lock(receive_mtx_);

      if (iox2_server_has_requests(&server_, &has_requests) != IOX2_OK || !has_requests) {
        return;
      }
    }

    auto* impl = get_first_impl();

    if VUNLIKELY (!impl) {
      return;
    }

    auto self = weak_from_this().lock();

    if VUNLIKELY (!self) {
      return;
    }

    MessageLoop* loop = nullptr;
    NodeImpl::LoopGatePtr loop_gate;

    if (!invoke_callback(impl, [&] {
          loop = impl->get_message_loop();

          if (loop) {
            loop_gate = impl->get_loop_gate();
          }
        })) {
      return;
    }

    if (loop) {
      const bool posted = NodeImpl::post_to_loop_gate(loop_gate, loop, [weak = weak_from_this(), loop]() {
        auto self = weak.lock();

        if VUNLIKELY (!self) {
          return;
        }

        auto* impl = self->get_first_impl();

        bool attached = false;
        self->invoke_callback(impl, [&] { attached = impl->get_message_loop() == loop; });

        if VUNLIKELY (!attached) {
          return;
        }

        self->process_message();
      });

      if (posted || !invoke_callback(impl, [&] { loop = impl->get_message_loop(); }) || loop) {
        return;
      }
    }

    try {
      self->process_message();
    } catch (const std::exception& e) {
      VLOG_E("Shm2Factory: Server request callback failed: ", e.what(), ".");
    } catch (...) {
      VLOG_E("Shm2Factory: Server request callback failed.");
    }
  };

#if !VLINK_SHM2_NO_FD_LISTENER
  iox2_file_descriptor_ptr fd = iox2_listener_get_file_descriptor(&ctx->listener);

  if (fd != nullptr) {
    int ret = factory.register_waitset_poll(this, fd, std::move(poll_cb));

    if VUNLIKELY (ret != IOX2_OK) {
      VLOG_F("Shm2Factory: Failed to attach server req-listener fd to waitset, error: ", ret, ".");
      return;
    }

    return;
  }
#endif

  factory.register_poll(this, std::move(poll_cb), &ctx->listener, false, &ctx->pf_handle);
}

Shm2Server::~Shm2Server() {
  is_offering_.store(false, std::memory_order_relaxed);
  is_suspend_.store(true, std::memory_order_relaxed);

  Shm2Factory::get().unregister_poll(this);

  std::lock_guard lock(mtx_);

  for (auto& [_, entry] : loan_map_) {
    iox2_response_mut_drop(entry.handle);
  }

  loan_map_.clear();

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

  std::exception_ptr callback_error;

  while (true) {
    iox2_active_request_t active_req_storage{};
    iox2_active_request_h active_req_handle{nullptr};

    {
      std::lock_guard receive_lock(receive_mtx_);

      bool has_requests = false;

      if (iox2_server_has_requests(&server_, &has_requests) != IOX2_OK || !has_requests) {
        break;
      }

      {
        std::lock_guard lock(mtx_);

        if VUNLIKELY (active_req_ != nullptr) {
          break;
        }
      }

      if VUNLIKELY (iox2_server_receive(&server_, &active_req_storage, &active_req_handle) != IOX2_OK ||
                    !active_req_handle) {
        break;
      }
    }

    std::unique_ptr<iox2_active_request_h_t, decltype(&iox2_active_request_drop)> request(active_req_handle,
                                                                                          &iox2_active_request_drop);

    if (is_suspend_.load(std::memory_order_relaxed)) {
      continue;
    }

    const void* payload_ptr = nullptr;
    size_t num_elements = 0;
    iox2_active_request_payload(&active_req_handle, &payload_ptr, &num_elements);

    if VUNLIKELY (!payload_ptr || num_elements == 0) {
      continue;
    }

    const auto* read_req = static_cast<const uint8_t*>(payload_ptr);

    uint64_t channel = 0;
    uint64_t seq = 0;
    Bytes req_bytes;
    Shm2Factory::read_data(read_req, static_cast<uint64_t>(num_elements), channel, seq, req_bytes);
    (void)seq;

    try {
      traverse_req_resp_callback([this, channel, &req_bytes, &request](NodeImpl* impl, const auto& callback) {
        const auto* transport_ptr = impl->get_target_transport<Shm2Transport>();

        if (static_cast<uint64_t>(transport_ptr->hash_code) != channel) {
          ignore_called();
          return;
        }

        if VUNLIKELY (has_called()) {
          VLOG_F(*transport_ptr, "Two identical service requests.");
          return;
        }

        if (static_cast<ServerImpl*>(impl)->is_resp_type) {
          {
            std::lock_guard lock(mtx_);
            active_req_ = request.get();
          }

          auto release = [this](iox2_active_request_h handle) {
            std::lock_guard lock(mtx_);

            for (auto& [_, entry] : loan_map_) {
              iox2_response_mut_drop(entry.handle);
            }

            loan_map_.clear();
            iox2_active_request_drop(handle);
            active_req_ = nullptr;
          };
          std::unique_ptr<iox2_active_request_h_t, decltype(release)> active(request.release(), release);

          Bytes resp_bytes;
          callback(seq_.load(std::memory_order_relaxed), req_bytes, &resp_bytes);
        } else {
          callback(0, req_bytes, nullptr);
        }
      });
    } catch (...) {
      if (callback_error) {
        VLOG_E("Shm2Factory: Additional server request callback failed.");
      } else {
        callback_error = std::current_exception();
      }
    }
  }

  if VUNLIKELY (callback_error) {
    std::rethrow_exception(callback_error);
  }
}

void Shm2Server::start() { is_offering_.store(true, std::memory_order_relaxed); }
void Shm2Server::stop() { is_offering_.store(false, std::memory_order_relaxed); }

bool Shm2Server::has_clients() const {
  if VUNLIKELY (!pf_handle_) {
    return false;
  }

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

  std::lock_guard lock(mtx_);

  if VUNLIKELY (!active_req_) {
    return Bytes();
  }

  if VUNLIKELY (iox2_active_request_loan_slice_uninit(&active_req_, storage.get(), &resp_handle, total) != IOX2_OK) {
    VLOG_E("Shm2Factory: Failed to loan server response buffer, size: ", total, ".");
    return Bytes();
  }

  void* write_ptr = nullptr;
  size_t write_elems = 0;
  iox2_response_mut_payload_mut(&resp_handle, &write_ptr, &write_elems);

  auto* write_buf = static_cast<uint8_t*>(write_ptr);
  const uint8_t* user_ptr = write_buf + Shm2Factory::get_loaned_offset();
  loan_map_.emplace(user_ptr, ServerLoanEntry{std::move(storage), resp_handle});

  return Bytes::loan_internal(user_ptr, size);
}

bool Shm2Server::release(const Bytes& bytes) {
  if VUNLIKELY (!bytes.is_loaned()) {
    return false;
  }

  std::lock_guard lock(mtx_);
  auto it = loan_map_.find(bytes.data());

  if (it == loan_map_.end()) {
    return false;
  }

  iox2_response_mut_drop(it->second.handle);
  loan_map_.erase(it);
  return true;
}

bool Shm2Server::reply(uint64_t channel, const Bytes& resp_data) {
  std::lock_guard lock(mtx_);

  if VUNLIKELY (!active_req_) {
    VLOG_E("Shm2Factory: Server reply called without active request.");
    return false;
  }

  if (resp_data.is_loaned()) {
    auto it = loan_map_.find(resp_data.data());

    if VUNLIKELY (it == loan_map_.end()) {
      VLOG_E("Shm2Factory: reply() on loaned bytes from a foreign server.");
      return false;
    }

    auto entry = std::move(it->second);
    loan_map_.erase(it);

    const uint64_t seq = seq_.fetch_add(1, std::memory_order_relaxed) + 1;

    auto* buf_base = const_cast<uint8_t*>(resp_data.data()) - Shm2Factory::get_loaned_offset();
    Shm2Factory::write_header(buf_base, channel, seq);

    auto ret = iox2_response_mut_send(entry.handle);

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
    return false;
  }

  const uint64_t seq = seq_.fetch_add(1, std::memory_order_relaxed) + 1;

  void* write_ptr = nullptr;
  size_t write_elems = 0;
  iox2_response_mut_payload_mut(&resp_handle, &write_ptr, &write_elems);

  auto* write_buf = static_cast<uint8_t*>(write_ptr);
  Shm2Factory::write_data(write_buf, channel, seq, resp_data);

  auto ret = iox2_response_mut_send(resp_handle);

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

  const auto& [impl_type, address, domain, depth, history, wait, size, event, owner] = id;

  payload_size_ = size;

  service_name_ = Shm2Factory::make_service_name(address, "method", domain, event);

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

  const std::string req_notify_svc = service_name_ + "/request";

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

  const std::string resp_notify_svc = service_name_ + "/response";
  auto listener_ctx = std::make_shared<Shm2ListenerContext>();

  if VUNLIKELY (!open_or_create_event_service(factory.get_node(), resp_notify_svc, listener_ctx->pf_storage,
                                              listener_ctx->pf_handle)) {
    VLOG_F("Shm2Factory: Failed to open client resp-event service: ", resp_notify_svc, ".");
    return;
  }

  iox2_port_factory_listener_builder_t listener_builder_storage{};
  auto* listener_builder =
      iox2_port_factory_event_listener_builder(&listener_ctx->pf_handle, &listener_builder_storage);

  if VUNLIKELY (iox2_port_factory_listener_builder_create(listener_builder, &listener_ctx->listener_storage,
                                                          &listener_ctx->listener) != IOX2_OK) {
    VLOG_F("Shm2Factory: Failed to create client resp-listener for: ", resp_notify_svc, ".");
    return;
  }

  listener_ctx_ = listener_ctx;

  Shm2Factory::PollCallback poll_cb = [this, listener_ctx]() {
    drain_listener_events(&listener_ctx->listener, "client resp-listener");

    process_message();
  };

  bool attached_waitset = false;

#if !VLINK_SHM2_NO_FD_LISTENER
  iox2_file_descriptor_ptr fd = iox2_listener_get_file_descriptor(&listener_ctx->listener);

  if (fd != nullptr) {
    int ret = factory.register_waitset_poll(this, fd, std::move(poll_cb));

    if VUNLIKELY (ret != IOX2_OK) {
      VLOG_F("Shm2Factory: Failed to attach client resp-listener fd to waitset, error: ", ret, ".");
      listener_ctx_.reset();
      return;
    }

    attached_waitset = true;
  }
#endif

  if (!attached_waitset) {
    factory.register_poll(this, std::move(poll_cb), &listener_ctx->listener, false, &listener_ctx->pf_handle);
  }

  req_notifier_fn_ = [req_ctx = std::move(req_ctx)]() {
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

  listener_ctx_.reset();

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

  std::shared_ptr<Shm2Client> self;
  std::unique_lock lock(mtx_);

  if (pending_map_.empty()) {
    return;
  }

  self = weak_from_this().lock();

  if VUNLIKELY (!self) {
    return;
  }

  bool progressed = true;

  while (progressed) {
    progressed = false;

    for (auto iter = pending_map_.begin(); iter != pending_map_.end();) {
      auto seq = iter->first;
      auto& handle = iter->second;
      auto cb_iter = callbacks_.find(seq);
      const bool retain = cb_iter != callbacks_.end() && cb_iter->second.dispatch;

      const bool disconnected = !iox2_pending_response_is_connected(&handle);
      iox2_response_t resp_storage{};
      iox2_response_h resp_handle{nullptr};

      auto ret = iox2_pending_response_receive(&handle, retain ? nullptr : &resp_storage, &resp_handle);

      if (ret == IOX2_OK && resp_handle) {
        std::unique_ptr<iox2_response_h_t, decltype(&iox2_response_drop)> response(resp_handle, &iox2_response_drop);
        const void* payload_ptr = nullptr;
        size_t num_elements = 0;
        iox2_response_payload(&resp_handle, &payload_ptr, &num_elements);

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

          MessageLoop* message_loop = nullptr;
          NodeImpl::LoopGatePtr loop_gate;

          if (!callback.dispatch) {
            callback.callback(resp_bytes);
          } else {
            invoke_callback(callback.owner, [&]() {
              message_loop = callback.owner->get_message_loop();

              if (message_loop) {
                loop_gate = callback.owner->get_loop_gate();
              } else {
                callback.callback(resp_bytes);
              }
            });
          }

          if (message_loop) {
            auto drop_response = [self = shared_from_this()](iox2_response_h_t* handle_ptr) {
              std::lock_guard drop_lock(self->mtx_);

              iox2_response_drop(handle_ptr);
            };
            std::unique_ptr<iox2_response_h_t, decltype(drop_response)> retained(response.release(),
                                                                                 std::move(drop_response));

            NodeImpl::post_to_loop_gate(loop_gate, message_loop,
                                        [this, owner = callback.owner, message_loop, response = std::move(retained),
                                         bytes = std::move(resp_bytes), cb = std::move(callback.callback)]() {
                                          bool attached = false;
                                          invoke_callback(
                                              owner, [&]() { attached = owner->get_message_loop() == message_loop; });

                                          if VLIKELY (attached) {
                                            cb(bytes);
                                          }
                                        });
          }

          callback.callback = {};

          lock.lock();

          response.reset();

          progressed = true;
          break;
        }

        response.reset();
        iox2_pending_response_drop(handle);
        iter = pending_map_.erase(iter);
        pending_storage_map_.erase(seq);
      } else if (disconnected) {
        iox2_pending_response_drop(handle);
        iter = pending_map_.erase(iter);
        pending_storage_map_.erase(seq);
        callbacks_.erase(seq);
      } else {
        ++iter;
      }
    }
  }
}

bool Shm2Client::is_connected() const {
  if VUNLIKELY (!pf_handle_) {
    return false;
  }

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

  {
    std::lock_guard lock(mtx_);

    if VUNLIKELY (!client_ || iox2_client_loan_slice_uninit(&client_, storage.get(), &req_handle, total) != IOX2_OK) {
      VLOG_E("Shm2Factory: Failed to loan client request buffer, size: ", total, ".");
      return Bytes();
    }
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

  std::lock_guard lock(mtx_);
  iox2_request_mut_drop(entry.handle);
  return true;
}

bool Shm2Client::call(NodeImpl* owner, uint64_t channel, const Bytes& req_data, NodeImpl::MsgCallback&& callback,
                      uint64_t* seq_out, bool dispatch) {
  if VUNLIKELY (!is_connected()) {
    release(req_data);
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
    iox2_pending_response_t* storage;
    iox2_pending_response_h* pending;

    try {
      storage = &pending_storage_map_[seq];
      pending = &pending_map_[seq];
      callbacks_[seq] = ResponseCallback{owner, dispatch, std::move(callback)};
    } catch (...) {
      iox2_request_mut_drop(req_handle);
      pending_storage_map_.erase(seq);
      pending_map_.erase(seq);
      throw;
    }

    ret = iox2_request_mut_send(req_handle, storage, pending);

    if VUNLIKELY (ret != IOX2_OK) {
      VLOG_E("Shm2Factory: Failed to send client request, error: ", ret, ".");

      callbacks_.erase(seq);
      pending_map_.erase(seq);
      pending_storage_map_.erase(seq);
      return false;
    }

    if (seq_out) {
      *seq_out = seq;
    }
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

void Shm2Client::cancel_calls(NodeImpl* owner) {
  std::lock_guard lock(mtx_);

  for (auto iter = callbacks_.begin(); iter != callbacks_.end();) {
    if (iter->second.owner != owner) {
      ++iter;
      continue;
    }

    const auto seq = iter->first;
    auto pending = pending_map_.find(seq);

    if (pending != pending_map_.end()) {
      iox2_pending_response_drop(pending->second);
      pending_map_.erase(pending);
    }

    pending_storage_map_.erase(seq);
    iter = callbacks_.erase(iter);
  }
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

  const auto& [impl_type, address, domain, depth, history, wait, size, event, owner] = id;

  wait_ = wait;
  payload_size_ = size;
  history_ = history;
  depth_ = depth;

  const auto notify_every_env = Utils::get_env("VLINK_SHM2_NOTIFY_EVERY", "1");

  int parsed_notify_every = 1;

  if (!notify_every_env.empty()) {
    std::from_chars(notify_every_env.data(), notify_every_env.data() + notify_every_env.size(), parsed_notify_every);
  }

  notify_every_ = parsed_notify_every > 0 ? static_cast<uint32_t>(parsed_notify_every) : 1U;

  service_name_ = Shm2Factory::make_service_name(address, impl_type == kSetter ? "field" : "event", domain, event);

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

  const std::string notify_svc = service_name_ + "/notify";

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
    std::string sem_address = service_name_;
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
  if VUNLIKELY (!pf_handle_) {
    return false;
  }

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
    std::lock_guard connection_lock(connection_mtx_);

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

    std::lock_guard connection_lock(connection_mtx_);
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

  std::lock_guard connection_lock(connection_mtx_);
  iox2_sample_mut_drop(entry.handle);
  return true;
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
      std::lock_guard connection_lock(connection_mtx_);
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

  iox2_sample_mut_t sample_storage{};
  iox2_sample_mut_h sample_handle{nullptr};

  constexpr size_t kSmallPayloadLockThreshold = 4096U;
  const bool keep_lock = total <= kSmallPayloadLockThreshold;

  std::unique_lock connection_lock(connection_mtx_);

  if VUNLIKELY (iox2_publisher_loan_slice_uninit(&publisher_, &sample_storage, &sample_handle, total) != IOX2_OK) {
    VLOG_E("Shm2Factory: Failed to loan publisher buffer, size: ", total, ".");
    return false;
  }

  if (!keep_lock) {
    connection_lock.unlock();
  }

  const uint64_t seq = seq_.fetch_add(1, std::memory_order_relaxed) + 1;

  size_t recipients = 0;
  void* write_ptr = nullptr;
  size_t write_elems = 0;
  iox2_sample_mut_payload_mut(&sample_handle, &write_ptr, &write_elems);

  auto* write_buf = static_cast<uint8_t*>(write_ptr);

  if VUNLIKELY (write_buf == nullptr || write_elems < total) {
    VLOG_E("Shm2Factory: Invalid loaned publisher payload, size: ", write_elems, ", expected: ", total, ".");

    if (!keep_lock) {
      connection_lock.lock();
    }

    iox2_sample_mut_drop(sample_handle);
    return false;
  }

  Shm2Factory::write_data(write_buf, channel, seq, bytes);

  if (!keep_lock) {
    connection_lock.lock();
  }

  const int send_ret = iox2_sample_mut_send(sample_handle, &recipients);
  connection_lock.unlock();

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

  discovery_subscribers(iox2_port_factory_pub_sub_dynamic_config_number_of_subscribers(&pf_handle_));
}

void Shm2Publisher::discovery_subscribers(size_t count) {
  const bool has_subs = count > 0;

  if (count != last_subscriber_count_ && has_subs && history_ > 0) {
    update_connections();

    if (notifier_) {
      iox2_notifier_notify(&notifier_, nullptr);
    }
  }

  last_subscriber_count_ = count;

  if (last_has_subscribers_.exchange(has_subs, std::memory_order_relaxed) != has_subs) {
    traverse_sub_connect_callback([has_subs](NodeImpl*, const auto& callback) { callback(has_subs); });
  }
}

void Shm2Publisher::notify_and_wait(size_t recipients) {
  if (notifier_ && recipients > 0) {
    bool notify = true;

    if VUNLIKELY (notify_every_ > 1U) {
      auto count = notify_counter_.load(std::memory_order_relaxed);

      do {
        notify = count + 1U == notify_every_;
      } while (!notify_counter_.compare_exchange_weak(count, notify ? 0U : count + 1U, std::memory_order_relaxed));
    }

    if (notify) {
      iox2_notifier_notify(&notifier_, nullptr);
    }
  }

  if VUNLIKELY (wait_ > 0) {
    sem_->acquire(recipients, wait_);
  }
}

// Shm2Subscriber
Shm2Subscriber::Shm2Subscriber(const ShmID2& id) {
  static auto& factory = Shm2Factory::get();

  const auto& [impl_type, address, domain, depth, history, wait, size, event, owner] = id;

  wait_ = wait;
  payload_size_ = size;
  history_ = history;
  depth_ = depth;

  service_name_ = Shm2Factory::make_service_name(address, impl_type == kGetter ? "field" : "event", domain, event);

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
    std::string sem_address = service_name_;
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

void Shm2Subscriber::process_message(MessageLoop* message_loop, bool can_use_stack_sample) {
  struct SampleGuard final {
    iox2_sample_h handle;
    std::unique_lock<std::mutex>& lock;
    std::optional<SysSemaphore>& sem;

    ~SampleGuard() {
      if VUNLIKELY (handle != nullptr) {
        if (!lock.owns_lock()) {
          lock.lock();
        }

        iox2_sample_drop(handle);

        if VUNLIKELY (sem) {
          lock.unlock();
          sem->release();
        }
      }
    }

    void release_now() {
      if (!lock.owns_lock()) {
        lock.lock();
      }

      iox2_sample_drop(handle);
      handle = nullptr;

      if VUNLIKELY (sem) {
        lock.unlock();
        sem->release();
        lock.lock();
      }
    }

    iox2_sample_h disarm() {
      iox2_sample_h h = handle;
      handle = nullptr;
      return h;
    }
  };

  const bool use_stack_sample = can_use_stack_sample && !sem_;
  std::vector<LoopRoute> routes;
  std::unique_lock lock(receive_mtx_);

  while (true) {
    if VUNLIKELY (!subscriber_) {
      break;
    }

    iox2_sample_t sample_storage{};
    iox2_sample_h sample_handle{nullptr};
    auto ret = iox2_subscriber_receive(&subscriber_, use_stack_sample ? &sample_storage : nullptr, &sample_handle);

    if (ret != IOX2_OK || !sample_handle) {
      break;
    }

    SampleGuard sample_guard{sample_handle, lock, sem_};

    if VUNLIKELY (is_suspend_.load(std::memory_order_relaxed)) {
      sample_guard.release_now();
      continue;
    }

    const void* payload_ptr = nullptr;
    size_t num_elements = 0;
    iox2_sample_payload(&sample_handle, &payload_ptr, &num_elements);

    if VUNLIKELY (!payload_ptr || num_elements == 0) {
      sample_guard.release_now();
      continue;
    }

    const auto* read_msg = static_cast<const uint8_t*>(payload_ptr);

    uint64_t channel = 0;
    uint64_t seq = 0;
    Bytes msg_bytes;
    Shm2Factory::read_data(read_msg, static_cast<uint64_t>(num_elements), channel, seq, msg_bytes);

    if VUNLIKELY (is_latency_and_lost_enabled_.load(std::memory_order_acquire) && seq > 0) {
      iox2_publish_subscribe_header_t header_storage{};
      iox2_publish_subscribe_header_h header{nullptr};
      iox2_sample_header(&sample_handle, &header_storage, &header);
      iox2_unique_publisher_id_t id_storage{};
      iox2_unique_publisher_id_h id{nullptr};
      iox2_publish_subscribe_header_publisher_id(&header, &id_storage, &id);
      uint64_t value[2]{};
      iox2_unique_publisher_id_value(id, reinterpret_cast<uint8_t*>(value), sizeof(value));
      calc_sample_.update(seq, Helpers::hash_combine(value[0], value[1]));
      iox2_unique_publisher_id_drop(id);
      iox2_publish_subscribe_header_drop(header);
    }

    lock.unlock();

    routes.clear();

    traverse_msg_callback([channel, message_loop, &msg_bytes, &routes](NodeImpl* impl, const auto& callback) {
      const auto* transport_ptr = impl->get_target_transport<Shm2Transport>();

      if VUNLIKELY (static_cast<uint64_t>(transport_ptr->hash_code) != channel ||
                    impl->has_suspend.load(std::memory_order_acquire)) {
        return;
      }

      auto* loop = impl->get_message_loop();

      if VLIKELY (!loop || loop == message_loop) {
        callback(msg_bytes);
      } else if (std::find(routes.begin(), routes.end(), loop) == routes.end()) {
        routes.push_back({loop, impl->get_loop_gate()});
      }
    });

    if VLIKELY (routes.empty()) {
      sample_guard.release_now();
      continue;
    }

    if (use_stack_sample) {
      auto owned_bytes = std::make_shared<Bytes>(Bytes::deep_copy(msg_bytes.data(), msg_bytes.size()));

      lock.lock();
      iox2_sample_drop(sample_guard.disarm());
      lock.unlock();

      for (const auto& route : routes) {
        post_to_subscriber_loop(route, [weak = weak_from_this(), loop = route.loop, channel, owned_bytes]() {
          auto self = weak.lock();

          if VUNLIKELY (!self) {
            return;
          }

          self->traverse_msg_callback([loop, channel, &owned_bytes](NodeImpl* impl, const auto& callback) {
            if (impl->get_message_loop() == loop && !impl->has_suspend.load(std::memory_order_acquire) &&
                static_cast<uint64_t>(impl->get_target_transport<Shm2Transport>()->hash_code) == channel) {
              callback(*owned_bytes);
            }
          });
        });
      }

      lock.lock();
      continue;
    }

    {
      std::shared_ptr<iox2_sample_h_t> retained(sample_guard.disarm(),
                                                [self = shared_from_this()](iox2_sample_h handle) {
                                                  {
                                                    std::lock_guard drop_lock(self->receive_mtx_);

                                                    iox2_sample_drop(handle);
                                                  }

                                                  if (self->sem_) {
                                                    self->sem_->release();
                                                  }
                                                });

      for (const auto& route : routes) {
        post_to_subscriber_loop(
            route, [this, loop = route.loop, channel, retained, data = msg_bytes.data(), size = msg_bytes.size()]() {
              auto bytes = Bytes::shallow_copy(data, size);

              traverse_msg_callback([loop, channel, &bytes](NodeImpl* impl, const auto& callback) {
                if (impl->get_message_loop() == loop && !impl->has_suspend.load(std::memory_order_acquire) &&
                    static_cast<uint64_t>(impl->get_target_transport<Shm2Transport>()->hash_code) == channel) {
                  callback(bytes);
                }
              });
            });
      }
    }

    lock.lock();
  }
}

void Shm2Subscriber::subscribe() {
  bool expected = false;

  if VUNLIKELY (!is_subscribed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                        std::memory_order_relaxed)) {
    return;
  }

  if VUNLIKELY (!pf_handle_) {
    is_subscribed_.store(false, std::memory_order_relaxed);
    return;
  }

  static auto& factory = Shm2Factory::get();

  iox2_port_factory_subscriber_builder_t sub_builder_storage{};
  auto* sub_builder = iox2_port_factory_pub_sub_subscriber_builder(&pf_handle_, &sub_builder_storage);

  if (depth_ > 0) {
    iox2_port_factory_subscriber_builder_set_buffer_size(&sub_builder, static_cast<size_t>(depth_));
  }

  if VUNLIKELY (iox2_port_factory_subscriber_builder_create(sub_builder, &subscriber_storage_, &subscriber_) !=
                IOX2_OK) {
    VLOG_F("Shm2Factory: Failed to create subscriber for: ", service_name_, ".");
    is_subscribed_.store(false, std::memory_order_relaxed);
    return;
  }

  const std::string notify_svc = service_name_ + "/notify";
  auto listener_ctx = std::make_shared<Shm2ListenerContext>();

  if VUNLIKELY (!open_or_create_event_service(factory.get_node(), notify_svc, listener_ctx->pf_storage,
                                              listener_ctx->pf_handle)) {
    VLOG_F("Shm2Factory: Failed to open subscriber event service: ", notify_svc, ".");

    if (subscriber_) {
      iox2_subscriber_drop(subscriber_);
      subscriber_ = nullptr;
    }

    is_subscribed_.store(false, std::memory_order_relaxed);
    return;
  }

  iox2_port_factory_listener_builder_t listener_builder_storage{};
  auto* listener_builder =
      iox2_port_factory_event_listener_builder(&listener_ctx->pf_handle, &listener_builder_storage);

  if VUNLIKELY (iox2_port_factory_listener_builder_create(listener_builder, &listener_ctx->listener_storage,
                                                          &listener_ctx->listener) != IOX2_OK) {
    VLOG_F("Shm2Factory: Failed to create subscriber listener for: ", notify_svc, ".");

    if (subscriber_) {
      iox2_subscriber_drop(subscriber_);
      subscriber_ = nullptr;
    }

    is_subscribed_.store(false, std::memory_order_relaxed);
    return;
  }

  listener_ctx_ = listener_ctx;

  auto self = shared_from_this();

  Shm2Factory::PollCallback poll_cb = [weak = weak_from_this(), listener_ctx]() {
    auto self = weak.lock();

    if VUNLIKELY (!self) {
      return;
    }

    drain_listener_events(&listener_ctx->listener, "subscriber listener");

    if VLIKELY (!NodeImpl::has_any_attached_loop()) {
      self->process_message(nullptr, true);
      return;
    }

    LoopRoute route;
    bool first = true;
    bool has_any_loop = false;

    self->traverse_msg_callback([&](NodeImpl* target, const auto&) {
      auto* loop = target->get_message_loop();

      if (loop) {
        has_any_loop = true;
      }

      if (first) {
        first = false;

        if (loop) {
          route = {loop, target->get_loop_gate()};
        }
      } else if (loop != route.loop) {
        route.loop = nullptr;
      }
    });

    if (route.loop) {
      bool has_samples = false;

      {
        std::lock_guard receive_lock(self->receive_mtx_);

        if VUNLIKELY (!self->subscriber_ || iox2_subscriber_has_samples(&self->subscriber_, &has_samples) != IOX2_OK ||
                      !has_samples) {
          return;
        }
      }

      if (self->post_to_subscriber_loop(route, [weak, ml = route.loop]() {
            auto self = weak.lock();

            if VUNLIKELY (!self) {
              return;
            }

            self->process_message(ml, true);
          })) {
        return;
      }
    }

    self->process_message(nullptr, !has_any_loop);
  };

  bool attached_waitset = false;

#if !VLINK_SHM2_NO_FD_LISTENER
  iox2_file_descriptor_ptr fd = iox2_listener_get_file_descriptor(&listener_ctx->listener);

  if (fd != nullptr) {
    int ret = factory.register_waitset_poll(this, fd, std::move(poll_cb));

    if VUNLIKELY (ret != IOX2_OK) {
      VLOG_F("Shm2Factory: Failed to attach subscriber listener fd to waitset, error: ", ret, ".");
      listener_ctx_.reset();

      if (subscriber_) {
        iox2_subscriber_drop(subscriber_);
        subscriber_ = nullptr;
      }

      is_subscribed_.store(false, std::memory_order_relaxed);
      return;
    }

    attached_waitset = true;
  }
#endif

  if (!attached_waitset) {
    factory.register_poll(this, std::move(poll_cb), &listener_ctx->listener, true, &listener_ctx->pf_handle);
  }

  if (history_ > 0) {
    self->process_message();
  }
}

void Shm2Subscriber::unsubscribe() {
  bool expected = true;

  if VUNLIKELY (!is_subscribed_.compare_exchange_strong(expected, false, std::memory_order_acq_rel,
                                                        std::memory_order_relaxed)) {
    return;
  }

  Shm2Factory::get().unregister_poll(this);

  listener_ctx_.reset();

  std::lock_guard lock(receive_mtx_);

  if (subscriber_) {
    iox2_subscriber_drop(subscriber_);
    subscriber_ = nullptr;
  }
}

void Shm2Subscriber::set_latency_and_lost_enabled(bool enable) {
  is_latency_and_lost_enabled_.store(enable, std::memory_order_release);
}

bool Shm2Subscriber::is_latency_and_lost_enabled() const {
  return is_latency_and_lost_enabled_.load(std::memory_order_acquire);
}

const CalculateSample& Shm2Subscriber::get_calculate_sample() const { return calc_sample_; }

}  // namespace vlink
