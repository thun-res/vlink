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

#include <vlink/base/elapsed_timer.h>
#include <vlink/base/functional.h>
#include <vlink/base/message_loop.h>
#include <vlink/base/timer.h>
#include <vlink/client.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "proxy_bridge.h"
#include "vlink_convert.h"

namespace vlink {
namespace webviz {

using Json = nlohmann::json;

class FoxgloveRpc final {
 public:
  struct Config final {
    std::vector<std::string> rpc_msgs;
    ProxyBridge::TransportConfig transport;
  };

  using RpcResponseCallback = Function<void(uint32_t, uint32_t, const std::string&, const Bytes&)>;
  using RpcErrorCallback = Function<void(uint32_t, uint32_t, const std::string&)>;
  using RawClient = Client<Bytes, Bytes>;

  FoxgloveRpc(const Config& config, VlinkConvert* vlink_convert, MessageLoop* loop);

  ~FoxgloveRpc();

  bool has_rpcs() const;

  bool has_rpcs(MoveFunction<bool(std::string_view)>&& allow_url) const;

  std::vector<Json> get_rpcs() const;

  std::vector<Json> get_rpcs(MoveFunction<bool(std::string_view)>&& allow_url) const;

  bool is_rpc_allowed(uint32_t rpc_id, MoveFunction<bool(std::string_view)>&& allow_url) const;

  bool call_rpc(uint64_t client_key, uint32_t rpc_id, uint32_t call_id, const std::string& request_encoding,
                const Bytes& request, RpcResponseCallback&& on_response, RpcErrorCallback&& on_error);

  void cancel_client(uint64_t client_key);

 private:
  struct SchemaInfo final {
    std::string encoding;
    std::string schema_name;
    std::string schema_encoding;
    std::string schema;
    std::string protocol_schema;
  };

  struct Target final {
    std::string url;
    std::string ser;
    std::string encoding;
    SchemaType schema_type{SchemaType::kUnknown};
  };

  struct RpcState final {
    uint32_t id{0};
    std::string name;
    std::string type;
    int timeout_ms{2000};
    SchemaInfo request_schema;
    bool has_request_schema{false};
    Target target;
    std::string response_ser;
    SchemaType response_type{SchemaType::kUnknown};
    SchemaInfo response_schema;
    bool has_response_schema{false};
    RawClient::SharedPtr client;
  };

  struct PendingRpcCall final {
    uint64_t deadline_ms{0};
    uint32_t rpc_id{0};
    uint32_t call_id{0};
    RpcErrorCallback error_callback;
  };

  struct PendingRpcKey final {
    uint64_t client_key{0};
    uint32_t rpc_id{0};
    uint32_t call_id{0};

    bool operator==(const PendingRpcKey& other) const {
      return client_key == other.client_key && rpc_id == other.rpc_id && call_id == other.call_id;
    }
  };

  struct PendingRpcKeyHash final {
    size_t operator()(const PendingRpcKey& key) const {
      auto seed = std::hash<uint64_t>{}(key.client_key);
      seed ^= std::hash<uint32_t>{}(key.rpc_id) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
      seed ^= std::hash<uint32_t>{}(key.call_id) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
      return seed;
    }
  };

  struct LifetimeHandle final {};

  void load_rpc_msgs();

  bool load_rpc_file(const std::string& path);

  static bool load_schema_info(const Json& root, const std::filesystem::path& base_dir, const std::string& schema_key,
                               const std::string& encoding_key, SchemaInfo& schema, bool allow_target_encoding = false);

  static bool parse_target(const Json& item, Target& target, const std::string& default_name);

  bool build_request_mapping(const RpcState& state, CommandMapping& mapping) const;

  bool build_response_schema(RpcState& state) const;

  void process_rpc_timeout();

  bool take_pending_rpc(const PendingRpcKey& key, PendingRpcCall& pending);

  template <typename NodeT>
  void apply_transport(NodeT& node) const;

  Config config_;
  VlinkConvert* vlink_convert_{nullptr};
  MessageLoop* loop_{nullptr};

  mutable std::shared_mutex rpc_mtx_;
  std::unordered_map<uint32_t, RpcState> rpcs_;
  uint32_t next_rpc_id_{1};

  std::atomic_bool stopping_{false};
  std::shared_ptr<LifetimeHandle> lifetime_handle_;
  Timer rpc_timeout_timer_;
  mutable std::mutex pending_rpc_mtx_;
  std::unordered_map<PendingRpcKey, PendingRpcCall, PendingRpcKeyHash> pending_rpc_calls_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(FoxgloveRpc)
};

}  // namespace webviz
}  // namespace vlink
