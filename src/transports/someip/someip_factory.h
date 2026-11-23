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

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <vsomeip/vsomeip.hpp>

#if __has_include(<vsomeip/deprecated.hpp>)
#include <vsomeip/deprecated.hpp>
#endif

#include "./base/functional.h"
#include "./base/utils.h"
#include "./impl/abstract_factory.h"
#include "./transports/someip_transport.h"

namespace vlink {

namespace someip = vsomeip;

using SomeipID = std::tuple<uint8_t, uint16_t, uint16_t>;

// SomeipFactory
class SomeipFactory final : public AbstractFactory<SomeipID> {
 private:
  SomeipFactory();

  ~SomeipFactory() override;

 public:
  static bool load_global_config_file(const std::string& filepath);

 private:
  VLINK_SINGLETON_DECLARE(SomeipFactory)
};

// SomeipServer
class SomeipServer final : public AbstractObject<SomeipID>, public std::enable_shared_from_this<SomeipServer> {
 public:
  explicit SomeipServer(const SomeipID& id);

  ~SomeipServer() override;

  std::any get_native_handle() const override;

  std::shared_ptr<someip::application> app();

  bool is_initialized() const noexcept { return initialized_.load(std::memory_order_acquire); }

  void start();

  std::unordered_map<someip::eventgroup_t, std::unordered_set<someip::client_t>>& get_clients();

  std::mutex& get_client_mtx();

  void acquire_event(someip::event_t event, const std::set<someip::eventgroup_t>& groups,
                     someip::event_type_e event_type, bool register_sub_handler);

  void release_event(someip::event_t event, const std::set<someip::eventgroup_t>& groups, bool unregister_sub_handler);

 private:
  void process_request(NodeImpl* impl, const std::shared_ptr<someip::message>& request,
                       const NodeImpl::ReqRespCallback& callback);

  std::atomic_bool initialized_{false};
  std::atomic_bool has_started_{false};

  uint16_t service_id_{0};
  uint16_t instance_id_{0};
  std::shared_ptr<someip::runtime> runtime_;
  std::shared_ptr<someip::application> app_;
  std::thread thread_;
  std::unordered_map<someip::eventgroup_t, std::unordered_set<someip::client_t>> clients_;
  std::mutex client_mtx_;
  struct EventEntry final {
    size_t ref_count{0};
    someip::event_type_e event_type{someip::event_type_e::ET_EVENT};
    std::unordered_map<someip::eventgroup_t, size_t> group_refs;
  };

  std::unordered_map<someip::event_t, EventEntry> event_entries_;
  std::unordered_map<someip::eventgroup_t, size_t> sub_group_refs_;
  std::mutex event_mtx_;
};

// SomeipClient
class SomeipClient final : public AbstractObject<SomeipID>, public std::enable_shared_from_this<SomeipClient> {
 public:
  explicit SomeipClient(const SomeipID& id);

  ~SomeipClient() override;

  std::any get_native_handle() const override;

  std::shared_ptr<someip::application> app();

  bool is_initialized() const noexcept { return initialized_.load(std::memory_order_acquire); }

  void start();

  bool call(NodeImpl* owner, someip::method_t method, const Bytes& req_data, NodeImpl::MsgCallback&& callback = nullptr,
            uint64_t* seq_out = nullptr, bool dispatch = true, MoveFunction<void()>&& error_callback = nullptr);

  void remove_response_callback(uint64_t seq);

  void cancel_calls(NodeImpl* owner);

  bool is_connected() const;

  void acquire_event(someip::event_t event, const std::set<someip::eventgroup_t>& groups,
                     someip::event_type_e event_type);

  void release_event(someip::event_t event, const std::set<someip::eventgroup_t>& groups);

 private:
  std::atomic_bool initialized_{false};
  std::atomic_bool has_started_{false};
  std::atomic_bool connected_{false};

  uint16_t service_id_{0};
  uint16_t instance_id_{0};
  std::shared_ptr<someip::runtime> runtime_;
  std::shared_ptr<someip::application> app_;
  std::thread thread_;
  std::recursive_mutex mtx_;
  struct ResponseCallback final {
    NodeImpl* owner{nullptr};
    bool dispatch{true};
    NodeImpl::MsgCallback callback;
    MoveFunction<void()> error_callback;
  };
  std::unordered_map<uint64_t, ResponseCallback> resp_callbacks_;
  struct EventEntry final {
    size_t ref_count{0};
    someip::event_type_e event_type{someip::event_type_e::ET_EVENT};
    std::unordered_map<someip::eventgroup_t, size_t> group_refs;
  };

  std::unordered_map<someip::event_t, EventEntry> event_entries_;
  std::unordered_map<someip::eventgroup_t, size_t> sub_group_refs_;
  std::mutex event_mtx_;
};

}  // namespace vlink
