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

#include "foxglove_rpc.h"

//
#include <vlink/base/helpers.h>
#include <vlink/base/logger.h>

//
#include <algorithm>
#include <filesystem>
#include <fstream>

//
#include "../../webviz_loader_utils.h"

namespace vlink {
namespace webviz {

FoxgloveRpc::FoxgloveRpc(const Config& config, VlinkConvert* vlink_convert, MessageLoop* loop)
    : config_(config), vlink_convert_(vlink_convert), loop_(loop) {
  lifetime_handle_ = std::make_shared<LifetimeHandle>();

  if VLIKELY (loop_ && rpc_timeout_timer_.attach(loop_)) {
    rpc_timeout_timer_.set_interval(10);
    rpc_timeout_timer_.set_loop_count(Timer::kInfinite);
    rpc_timeout_timer_.set_callback([this]() { process_rpc_timeout(); });
    rpc_timeout_timer_.start();
  }

  load_rpc_msgs();
}

FoxgloveRpc::~FoxgloveRpc() {
  stopping_.store(true);
  lifetime_handle_.reset();

  rpc_timeout_timer_.stop();
  rpc_timeout_timer_.detach();

  {
    std::lock_guard lock(pending_rpc_mtx_);
    pending_rpc_calls_.clear();
  }
}

bool FoxgloveRpc::has_rpcs() const { return has_rpcs({}); }

bool FoxgloveRpc::has_rpcs(MoveFunction<bool(std::string_view)>&& allow_url) const {
  std::shared_lock lock(rpc_mtx_);

  for (const auto& rpc_entry : rpcs_) {
    const auto& state = rpc_entry.second;

    if VLIKELY (!allow_url || allow_url(state.target.url)) {
      return true;
    }
  }

  return false;
}

std::vector<Json> FoxgloveRpc::get_rpcs() const { return get_rpcs({}); }

std::vector<Json> FoxgloveRpc::get_rpcs(MoveFunction<bool(std::string_view)>&& allow_url) const {
  std::vector<Json> rpcs;
  std::shared_lock lock(rpc_mtx_);

  for (const auto& [id, state] : rpcs_) {
    if VUNLIKELY (allow_url && !allow_url(state.target.url)) {
      continue;
    }

    Json rpc;
    rpc["id"] = id;
    rpc["name"] = state.name;

    if VLIKELY (!state.type.empty()) {
      rpc["type"] = state.type;
    }

    if VLIKELY (state.has_request_schema || !state.request_schema.encoding.empty()) {
      Json request;
      request["encoding"] = state.request_schema.encoding;

      if VLIKELY (!state.request_schema.schema_name.empty()) {
        request["schemaName"] = state.request_schema.schema_name;
      }

      if VLIKELY (!state.request_schema.schema_encoding.empty()) {
        request["schemaEncoding"] = state.request_schema.schema_encoding;
      }

      if VLIKELY (!state.request_schema.protocol_schema.empty()) {
        request["schema"] = state.request_schema.protocol_schema;
      }

      rpc["request"] = std::move(request);
    }

    if VLIKELY (state.has_response_schema || !state.response_schema.encoding.empty()) {
      Json response;
      response["encoding"] = state.response_schema.encoding;

      if VLIKELY (!state.response_schema.schema_name.empty()) {
        response["schemaName"] = state.response_schema.schema_name;
      }

      if VLIKELY (!state.response_schema.schema_encoding.empty()) {
        response["schemaEncoding"] = state.response_schema.schema_encoding;
      }

      if VLIKELY (!state.response_schema.protocol_schema.empty()) {
        response["schema"] = state.response_schema.protocol_schema;
      }

      rpc["response"] = std::move(response);
    }

    rpcs.emplace_back(std::move(rpc));
  }

  std::sort(rpcs.begin(), rpcs.end(),
            [](const Json& lhs, const Json& rhs) { return lhs.value("id", 0U) < rhs.value("id", 0U); });
  return rpcs;
}

bool FoxgloveRpc::is_rpc_allowed(uint32_t rpc_id, MoveFunction<bool(std::string_view)>&& allow_url) const {
  if VUNLIKELY (!allow_url) {
    return true;
  }

  std::shared_lock lock(rpc_mtx_);
  auto rpc_iter = rpcs_.find(rpc_id);

  if VUNLIKELY (rpc_iter == rpcs_.end()) {
    return false;
  }

  return allow_url(rpc_iter->second.target.url);
}

bool FoxgloveRpc::call_rpc(uint64_t client_key, uint32_t rpc_id, uint32_t call_id, const std::string& request_encoding,
                           const Bytes& request, RpcResponseCallback&& on_response, RpcErrorCallback&& on_error) {
  auto response_callback = std::move(on_response);
  auto error_callback = std::move(on_error);
  RpcState state;
  bool has_state = false;

  {
    std::shared_lock lock(rpc_mtx_);
    auto rpc_iter = rpcs_.find(rpc_id);

    if VUNLIKELY (rpc_iter == rpcs_.end()) {
      if VLIKELY (error_callback) {
        error_callback(rpc_id, call_id, "Unknown RPC id");
      }
      return false;
    }

    state = rpc_iter->second;
    has_state = true;
  }

  if VUNLIKELY (!has_state || !state.client) {
    if VLIKELY (error_callback) {
      error_callback(rpc_id, call_id, "RPC client not initialized");
    }
    return false;
  }

  const auto expected_request_encoding = std::string("json");

  if VUNLIKELY (!request_encoding.empty() && expected_request_encoding != request_encoding) {
    if VLIKELY (error_callback) {
      error_callback(rpc_id, call_id, "RPC request encoding mismatch");
    }
    return false;
  }

  CommandMapping request_mapping;

  if VUNLIKELY (!build_request_mapping(state, request_mapping)) {
    if VLIKELY (error_callback) {
      error_callback(rpc_id, call_id, "Failed to build RPC request route");
    }
    return false;
  }

  CommandRoute route;
  CommandChannel request_channel;

  request_channel.topic = state.target.url;
  request_channel.encoding = expected_request_encoding;
  request_channel.schema_name = state.request_schema.schema_name;
  request_channel.schema_encoding = state.request_schema.schema_encoding;
  request_channel.schema = state.request_schema.schema;

  if VUNLIKELY (!vlink_convert_ || !vlink_convert_->build_route(request_mapping, request_channel, route)) {
    if VLIKELY (error_callback) {
      error_callback(rpc_id, call_id, "Failed to resolve RPC request route");
    }
    return false;
  }

  auto converted = vlink_convert_ ? vlink_convert_->encode_frontend_message(route, request) : CommandMessage{};

  if VUNLIKELY (!converted.success) {
    if VLIKELY (error_callback) {
      error_callback(rpc_id, call_id, "Failed to convert RPC request");
    }
    return false;
  }

  PendingRpcKey pending_key;
  pending_key.client_key = client_key;
  pending_key.rpc_id = rpc_id;
  pending_key.call_id = call_id;

  {
    std::lock_guard lock(pending_rpc_mtx_);

    if VUNLIKELY (pending_rpc_calls_.find(pending_key) != pending_rpc_calls_.end()) {
      if VLIKELY (error_callback) {
        error_callback(rpc_id, call_id, "Duplicate in-flight RPC call");
      }

      return false;
    }

    PendingRpcCall pending;
    pending.deadline_ms =
        ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli, false) + static_cast<uint64_t>(state.timeout_ms);
    pending.rpc_id = rpc_id;
    pending.call_id = call_id;
    pending.error_callback = std::move(error_callback);
    pending_rpc_calls_.emplace(pending_key, std::move(pending));
  }

  auto weak_lifetime = std::weak_ptr<LifetimeHandle>(lifetime_handle_);

  if VUNLIKELY (!state.client->invoke(converted.payload, [this, weak_lifetime, pending_key, state, rpc_id, call_id,
                                                          response_callback](const Bytes& response_raw) {
                  auto lifetime_guard = weak_lifetime.lock();

                  if VUNLIKELY (!lifetime_guard) {
                    return;
                  }

                  PendingRpcCall pending;

                  if VUNLIKELY (!take_pending_rpc(pending_key, pending)) {
                    return;
                  }

                  if VUNLIKELY (stopping_.load()) {
                    return;
                  }

                  if VUNLIKELY (!vlink_convert_) {
                    if VLIKELY (pending.error_callback) {
                      pending.error_callback(rpc_id, call_id, "Vlink convert is not initialized");
                    }

                    return;
                  }

                  Bytes response_payload;

                  if VUNLIKELY (!vlink_convert_->decode_backend_message_to_json(state.response_ser, state.response_type,
                                                                                response_raw, response_payload)) {
                    if VLIKELY (pending.error_callback) {
                      pending.error_callback(rpc_id, call_id, "Failed to convert RPC response");
                    }

                    return;
                  }

                  if VLIKELY (response_callback) {
                    response_callback(rpc_id, call_id, "json", response_payload);
                  }
                })) {
    PendingRpcCall pending;
    take_pending_rpc(pending_key, pending);

    if VLIKELY (pending.error_callback) {
      pending.error_callback(rpc_id, call_id, "Failed to dispatch RPC request");
    }

    return false;
  }

  return true;
}

void FoxgloveRpc::cancel_client(uint64_t client_key) {
  std::lock_guard lock(pending_rpc_mtx_);

  for (auto pending_iter = pending_rpc_calls_.begin(); pending_iter != pending_rpc_calls_.end();) {
    if VUNLIKELY (pending_iter->first.client_key != client_key) {
      ++pending_iter;
      continue;
    }

    pending_iter = pending_rpc_calls_.erase(pending_iter);
  }
}

void FoxgloveRpc::load_rpc_msgs() {
  for (const auto& path : config_.rpc_msgs) {
    if VUNLIKELY (!load_rpc_file(path)) {
      MLOG_W("Failed to load Foxglove rpc_msgs mapping: {}", path);
    }
  }
}

bool FoxgloveRpc::load_rpc_file(const std::string& path) {
  std::error_code ec;

  if VUNLIKELY (!std::filesystem::exists(path, ec) || ec) {
    return false;
  }

  std::ifstream ifs(path);

  if VUNLIKELY (!ifs.is_open()) {
    return false;
  }

  try {
    Json root;
    ifs >> root;
    std::vector<RpcState> loaded_states;
    const auto base_dir = std::filesystem::path(path).parent_path();
    uint32_t next_rpc_id = 1;

    {
      std::unique_lock lock(rpc_mtx_);
      next_rpc_id = next_rpc_id_;
    }

    auto parse_one = [this, &base_dir, &loaded_states, &next_rpc_id, &path](const Json& item) -> bool {
      try {
        if VUNLIKELY (!item.is_object() || !item.contains("request") || !item["request"].is_object()) {
          return false;
        }

        const auto& request = item["request"];
        RpcState state;

        if VLIKELY (item.contains("id")) {
          state.id = item["id"].get<uint32_t>();

          if VLIKELY (state.id >= next_rpc_id) {
            next_rpc_id = state.id + 1;
          }
        } else {
          state.id = next_rpc_id++;
        }

        state.name = item.value("name", std::string());
        state.timeout_ms = item.value("timeout_ms", 2000);

        if VUNLIKELY (state.timeout_ms <= 0) {
          MLOG_W("Foxglove RPC '{}' timeout_ms must be > 0", state.name);
          return false;
        }

        if VUNLIKELY (request.contains("field_mappings") || item.contains("field_mappings")) {
          MLOG_W("Foxglove RPC '{}' no longer supports field_mappings in request downlink", state.name);
          return false;
        }

        if VUNLIKELY (item.contains("target")) {
          MLOG_W("Foxglove RPC '{}' target object has been removed; flatten url/ser/encoding", state.name);
          return false;
        }

        if VUNLIKELY (request.contains("encoding")) {
          MLOG_W("Foxglove RPC '{}' request.encoding has been removed; frontend requests are always JSON", state.name);
          return false;
        }

        if VUNLIKELY (!parse_target(item, state.target, state.name)) {
          MLOG_W("Foxglove RPC '{}' is missing url or ser", state.name);
          return false;
        }

        if VUNLIKELY (state.name.empty()) {
          state.name = state.target.url;
        }

        state.type = item.value("type", state.name);

        state.request_schema.encoding = "json";
        const bool has_request_schema_config = request.contains("schema") || request.contains("schema_path") ||
                                               request.contains("schema_name") || request.contains("schema_encoding");
        state.has_request_schema =
            load_schema_info(request, base_dir, "schema_name", "schema_encoding", state.request_schema);

        if VUNLIKELY (has_request_schema_config && !state.has_request_schema) {
          MLOG_W("Foxglove RPC '{}' has invalid request schema", state.name);
          return false;
        }

        CommandMapping request_mapping;

        if VUNLIKELY (!build_request_mapping(state, request_mapping)) {
          MLOG_W("Foxglove RPC '{}' has invalid request schema/target", state.name);
          return false;
        }

        state.has_request_schema = !request_mapping.schema.empty() || !request_mapping.schema_name.empty();
        state.request_schema.encoding = request_mapping.encoding;
        state.request_schema.schema_name = request_mapping.schema_name;
        state.request_schema.schema_encoding = request_mapping.schema_encoding;
        state.request_schema.schema = request_mapping.schema;
        state.request_schema.protocol_schema = state.request_schema.schema;

        if VUNLIKELY (!item.contains("response") || !item["response"].is_object()) {
          MLOG_W("Foxglove RPC '{}' is missing response mapping", state.name);
          return false;
        }

        {
          const auto& response = item["response"];

          if VUNLIKELY (response.contains("passthrough") || response.contains("schema_type")) {
            MLOG_W("Foxglove RPC '{}' response does not support passthrough/schema_type; use response.encoding",
                   state.name);
            return false;
          }

          state.response_ser = response.value("ser", std::string());
          const bool has_response_schema_config = response.contains("schema") || response.contains("schema_path") ||
                                                  response.contains("schema_name") ||
                                                  response.contains("schema_encoding");
          state.has_response_schema =
              load_schema_info(response, base_dir, "schema_name", "schema_encoding", state.response_schema, true);

          if VUNLIKELY (has_response_schema_config && !state.has_response_schema) {
            MLOG_W("Foxglove RPC '{}' has invalid response schema", state.name);
            return false;
          }

          if VUNLIKELY (state.response_ser.empty()) {
            MLOG_W("Foxglove RPC '{}' response must provide response.ser", state.name);
            return false;
          }

          auto response_encoding = response.value("encoding", std::string());

          if VUNLIKELY (response_encoding.empty()) {
            if VLIKELY (is_json_ser(state.response_ser)) {
              response_encoding = "json";
            } else if VLIKELY (is_text_ser(state.response_ser)) {
              response_encoding = "text";
            }
          }

          state.response_type = SchemaData::convert_encoding(response_encoding);

          if VUNLIKELY (state.response_type == SchemaType::kZeroCopy) {
            MLOG_W("Foxglove RPC '{}' response does not support response.encoding=zerocopy", state.name);
            return false;
          }

          if VUNLIKELY (state.response_type == SchemaType::kUnknown) {
            MLOG_W("Foxglove RPC '{}' response must provide response.encoding for ser {}", state.name,
                   state.response_ser);
            return false;
          }
        }

        if VUNLIKELY (!build_response_schema(state)) {
          MLOG_W("Foxglove RPC '{}' has invalid response schema", state.name);
          return false;
        }

        state.client = RawClient::create_shared(state.target.url, InitType::kWithoutInit);
        apply_transport(*state.client);
        state.client->set_ser_type(state.target.ser, state.target.schema_type);

        if VUNLIKELY (!state.client->init()) {
          MLOG_W("Failed to initialize RPC client: {}", state.target.url);
          state.client.reset();
          return false;
        }

        if VUNLIKELY (std::any_of(loaded_states.begin(), loaded_states.end(), [&state](const RpcState& loaded) {
                        return loaded.id == state.id || loaded.name == state.name;
                      })) {
          MLOG_W("Foxglove rpc_msgs mapping duplicated id/name: {} [{}]", state.name, state.id);
          return false;
        }

        loaded_states.emplace_back(std::move(state));
        return true;
      } catch (const std::exception& e) {
        MLOG_W("Invalid rpc_msgs entry in {}: {}", path, e.what());
        return false;
      }
    };

    if VLIKELY (root.is_array()) {
      bool ok = true;

      for (const auto& item : root) {
        ok = parse_one(item) && ok;
      }

      if VLIKELY (ok) {
        std::unique_lock lock(rpc_mtx_);
        next_rpc_id_ = std::max(next_rpc_id_, next_rpc_id);

        for (auto& state : loaded_states) {
          if VUNLIKELY (rpcs_.find(state.id) != rpcs_.end()) {
            MLOG_W("Foxglove rpc_msgs mapping id duplicated: {}", state.id);
            continue;
          }

          if VUNLIKELY (std::any_of(rpcs_.begin(), rpcs_.end(),
                                    [&state](const auto& entry) { return entry.second.name == state.name; })) {
            MLOG_W("Foxglove rpc_msgs mapping name duplicated: {}", state.name);
            continue;
          }

          rpcs_[state.id] = std::move(state);
        }
      }

      return ok;
    }

    auto ok = parse_one(root);

    if VLIKELY (ok) {
      std::unique_lock lock(rpc_mtx_);
      next_rpc_id_ = std::max(next_rpc_id_, next_rpc_id);

      for (auto& state : loaded_states) {
        if VUNLIKELY (rpcs_.find(state.id) != rpcs_.end()) {
          MLOG_W("Foxglove rpc_msgs mapping id duplicated: {}", state.id);
          continue;
        }

        if VUNLIKELY (std::any_of(rpcs_.begin(), rpcs_.end(),
                                  [&state](const auto& entry) { return entry.second.name == state.name; })) {
          MLOG_W("Foxglove rpc_msgs mapping name duplicated: {}", state.name);
          continue;
        }

        rpcs_[state.id] = std::move(state);
      }
    }

    return ok;
  } catch (const std::exception& e) {
    MLOG_E("Failed to parse rpc_msgs mapping {}: {}", path, e.what());
    return false;
  }
}

bool FoxgloveRpc::load_schema_info(const Json& root, const std::filesystem::path& base_dir,
                                   const std::string& schema_key, const std::string& encoding_key, SchemaInfo& schema,
                                   bool allow_target_encoding) {
  schema = SchemaInfo{};
  schema.schema_name = root.value(schema_key, std::string());
  schema.schema_encoding = root.value(encoding_key, std::string());

  if VUNLIKELY (root.contains("schema_base64")) {
    MLOG_W("Foxglove RPC frontend schema does not support schema_base64");
    return false;
  }

  if VUNLIKELY (!allow_target_encoding && root.contains("encoding") && root["encoding"].is_string() &&
                root["encoding"].get<std::string>() != "json") {
    MLOG_W("Foxglove RPC frontend schema encoding must be json");
    return false;
  }

  if VLIKELY (root.contains("schema")) {
    if VLIKELY (root["schema"].is_string()) {
      schema.schema = root["schema"].get<std::string>();
    } else {
      schema.schema = root["schema"].dump();
    }

    if VUNLIKELY (schema.schema.empty()) {
      MLOG_W("Foxglove RPC frontend schema must not be empty");
      return false;
    }
  } else if VUNLIKELY (root.contains("schema_path")) {
    auto schema_path = std::filesystem::path(root["schema_path"].get<std::string>());

    if VLIKELY (!schema_path.is_absolute()) {
      schema_path = base_dir / schema_path;
    }

    std::ifstream ifs(schema_path);

    if VUNLIKELY (!ifs.is_open()) {
      schema.schema.clear();
    } else {
      schema.schema.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    }

    if VUNLIKELY (schema.schema.empty()) {
      MLOG_W("Foxglove RPC frontend schema_path could not be read: {}", Helpers::path_to_string(schema_path));
      return false;
    }
  }

  if VUNLIKELY (schema.schema.empty() && schema.schema_name.empty()) {
    return false;
  }

  if VUNLIKELY (!schema.schema_encoding.empty() && schema.schema_encoding != "jsonschema" &&
                schema.schema_encoding != "json") {
    MLOG_W("Foxglove RPC frontend schema_encoding must be jsonschema/json");
    return false;
  }

  if VLIKELY (!schema.schema.empty()) {
    if VUNLIKELY (schema.schema_encoding.empty()) {
      schema.schema_encoding = "jsonschema";
    }

    try {
      auto parsed_schema = Json::parse(schema.schema);

      if VUNLIKELY (!parsed_schema.is_object()) {
        return false;
      }

      schema.schema = parsed_schema.dump();
    } catch (const std::exception&) {
      return false;
    }
  }

  schema.encoding = "json";
  schema.protocol_schema = schema.schema;
  return true;
}

bool FoxgloveRpc::parse_target(const Json& item, Target& target, const std::string& default_name) {
  target = Target{};
  target.url = item.value("url", std::string());
  target.ser = item.value("ser", std::string());

  if VUNLIKELY (item.contains("converter") || item.contains("passthrough") || item.contains("vlink_encoding") ||
                item.contains("payload_encoding") || item.contains("schema_type")) {
    MLOG_W("Invalid Foxglove RPC target '{}': use top-level encoding only", default_name);
    return false;
  }

  target.encoding = item.value("encoding", std::string());

  if VUNLIKELY (target.encoding.empty()) {
    if VLIKELY (is_json_ser(target.ser)) {
      target.encoding = "json";
    } else if VLIKELY (is_text_ser(target.ser)) {
      target.encoding = "text";
    } else {
      target.encoding = "protobuf";
    }
  }

  if VUNLIKELY (target.url.empty() || target.ser.empty()) {
    MLOG_W("Invalid Foxglove RPC target '{}': missing url or ser", default_name);
    return false;
  }

  if VUNLIKELY (target.encoding.empty() ||
                (target.encoding != "json" && target.encoding != "text" && target.encoding != "protobuf" &&
                 !is_flatbuffers_encoding(target.encoding))) {
    MLOG_W("Invalid Foxglove RPC target '{}': unsupported encoding {}", default_name, target.encoding);
    return false;
  }

  target.schema_type = SchemaData::convert_encoding(target.encoding);

  if VUNLIKELY (target.schema_type == SchemaType::kFlatbuffers) {
    target.encoding = "flatbuffers";
  }

  if VUNLIKELY (target.schema_type == SchemaType::kUnknown || target.schema_type == SchemaType::kZeroCopy) {
    MLOG_W("Invalid Foxglove RPC target '{}': unsupported schema route {}", default_name, target.encoding);
    return false;
  }

  if VUNLIKELY (!is_target_encoding_compatible(target.ser, target.encoding)) {
    MLOG_W(
        "Invalid Foxglove RPC target '{}': ser={} is incompatible with target encoding {}; frontend requests are "
        "always JSON, and encoding selects the backend target encoding",
        default_name, target.ser, target.encoding);
    return false;
  }

  return true;
}

bool FoxgloveRpc::build_request_mapping(const RpcState& state, CommandMapping& mapping) const {
  mapping = CommandMapping{};
  mapping.topic = state.name;
  mapping.encoding = "json";
  mapping.schema_name = state.request_schema.schema_name;
  mapping.schema_encoding = state.request_schema.schema_encoding;
  mapping.schema = state.request_schema.schema;
  mapping.url_selector.configured = true;
  mapping.url_selector.whitelist_exact = {state.target.url};
  mapping.ser = state.target.ser;
  mapping.payload_encoding = state.target.encoding;
  mapping.schema_type = state.target.schema_type;

  if VUNLIKELY (state.target.url.empty() || mapping.ser.empty()) {
    return false;
  }

  if VUNLIKELY (mapping.payload_encoding.empty()) {
    return false;
  }

  if VUNLIKELY (!vlink_convert_ || !vlink_convert_->resolve_input_schema(mapping)) {
    return false;
  }

  return true;
}

bool FoxgloveRpc::build_response_schema(RpcState& state) const {
  CommandMapping mapping;
  mapping.topic = state.name;
  mapping.encoding = "json";
  mapping.schema_name = state.response_schema.schema_name;
  mapping.schema_encoding = state.response_schema.schema_encoding;
  mapping.schema = state.response_schema.schema;
  mapping.ser = state.response_ser;
  mapping.schema_type = state.response_type;

  if VUNLIKELY (mapping.ser.empty() || mapping.schema_type == SchemaType::kUnknown) {
    return false;
  }

  if VUNLIKELY (!vlink_convert_ || !vlink_convert_->resolve_input_schema(mapping)) {
    return false;
  }

  state.has_response_schema = true;
  state.response_type = mapping.schema_type;
  state.response_schema.encoding = mapping.encoding;
  state.response_schema.schema_name = mapping.schema_name;
  state.response_schema.schema_encoding = mapping.schema_encoding;
  state.response_schema.schema = mapping.schema;
  state.response_schema.protocol_schema = mapping.schema;
  return true;
}

void FoxgloveRpc::process_rpc_timeout() {
  if VUNLIKELY (stopping_.load()) {
    return;
  }

  const auto now_ms = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli, false);
  std::vector<std::pair<PendingRpcKey, PendingRpcCall>> expired_calls;

  {
    std::lock_guard lock(pending_rpc_mtx_);

    for (auto pending_iter = pending_rpc_calls_.begin(); pending_iter != pending_rpc_calls_.end();) {
      if VLIKELY (pending_iter->second.deadline_ms > now_ms) {
        ++pending_iter;
        continue;
      }

      expired_calls.emplace_back(pending_iter->first, std::move(pending_iter->second));
      pending_iter = pending_rpc_calls_.erase(pending_iter);
    }
  }

  for (auto& expired_call : expired_calls) {
    auto& pending = expired_call.second;

    if VLIKELY (pending.error_callback) {
      pending.error_callback(pending.rpc_id, pending.call_id, "RPC call timed out");
    }
  }
}

bool FoxgloveRpc::take_pending_rpc(const PendingRpcKey& key, PendingRpcCall& pending) {
  std::lock_guard lock(pending_rpc_mtx_);
  auto pending_iter = pending_rpc_calls_.find(key);

  if VUNLIKELY (pending_iter == pending_rpc_calls_.end()) {
    return false;
  }

  pending = std::move(pending_iter->second);
  pending_rpc_calls_.erase(pending_iter);
  return true;
}

template <typename NodeT>
void FoxgloveRpc::apply_transport(NodeT& node) const {
  ProxyBridge::apply_transport(node, config_.transport, false);
}

template void FoxgloveRpc::apply_transport(FoxgloveRpc::RawClient& node) const;

}  // namespace webviz
}  // namespace vlink
