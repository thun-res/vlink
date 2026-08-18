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

#include "./extension/qos_profile.h"

#include <fstream>
#include <string>
#include <unordered_map>

#include "./base/helpers.h"
#include "./base/logger.h"
#include "./base/utils.h"

// json
#include <nlohmann/json.hpp>

namespace vlink {

// GlobalQosProfile
struct GlobalQosProfile final {
  GlobalQosProfile() {
    available_qos_map.emplace(QosProfile::kEvent.name, QosProfile::kEvent);
    available_qos_map.emplace(QosProfile::kMethod.name, QosProfile::kMethod);
    available_qos_map.emplace(QosProfile::kField.name, QosProfile::kField);
    available_qos_map.emplace(QosProfile::kSensor.name, QosProfile::kSensor);
    available_qos_map.emplace(QosProfile::kParameter.name, QosProfile::kParameter);
    available_qos_map.emplace(QosProfile::kService.name, QosProfile::kService);
    available_qos_map.emplace(QosProfile::kClock.name, QosProfile::kClock);
    available_qos_map.emplace(QosProfile::kStatic.name, QosProfile::kStatic);
    available_qos_map.emplace(QosProfile::kLight.name, QosProfile::kLight);
    available_qos_map.emplace(QosProfile::kPoor.name, QosProfile::kPoor);
    available_qos_map.emplace(QosProfile::kBetter.name, QosProfile::kBetter);
    available_qos_map.emplace(QosProfile::kBest.name, QosProfile::kBest);
    available_qos_map.emplace(QosProfile::kStream.name, QosProfile::kStream);
    available_qos_map.emplace(QosProfile::kAlarm.name, QosProfile::kAlarm);
    available_qos_map.emplace(QosProfile::kCommand.name, QosProfile::kCommand);
    available_qos_map.emplace(QosProfile::kLog.name, QosProfile::kLog);

    const std::string qos_path = Utils::get_env("VLINK_QOS_CONFIG");

    if (qos_path.empty()) {
      return;
    }

    std::filesystem::path target_path;

    try {
#ifdef _WIN32
      target_path = std::filesystem::path(Helpers::string_to_wstring(qos_path));
#else
      target_path = std::filesystem::path(qos_path);
#endif

      if VUNLIKELY (!std::filesystem::exists(target_path)) {
        VLOG_E("QosProfile: Qos config file does not exist.");
        return;
      }
    } catch (std::filesystem::filesystem_error&) {            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      VLOG_E("QosProfile: Qos config file does not exist.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

    try {
      nlohmann::json root_json;

      {
        std::ifstream file(target_path);

        file >> root_json;

        file.close();
      }

      if VUNLIKELY (root_json.empty() || !root_json.is_array()) {
        VLOG_E("QosProfile: Qos config file has wrong format.");
        return;
      }

      for (const auto& obj : root_json) {
        if VUNLIKELY (!obj.is_object()) {
          VLOG_E("QosProfile: Qos config file has wrong format.");
          return;
        }

        Qos qos;

        if VUNLIKELY (!obj.contains("name") || !obj["name"].is_string()) {
          VLOG_E("QosProfile: Qos config file qos name missing or invalid.");
          return;
        }

        std::string qos_name = obj["name"];

        if VUNLIKELY (qos_name.empty()) {
          VLOG_E("QosProfile: Qos config file qos name is empty.");
          return;
        }

        if VUNLIKELY (qos_name.size() >= sizeof(qos.name)) {
          VLOG_E("QosProfile: Qos config file qos name length error.");
          return;
        }

        std::memcpy(qos.name, qos_name.data(), qos_name.size());

        if (obj.contains("reliability")) {
          auto tobj = obj["reliability"];

          if VUNLIKELY (!tobj.is_object()) {
            VLOG_E("QosProfile: Qos config file has wrong format.");
            return;
          }

          if (tobj.contains("kind")) {
            auto pkind = tobj["kind"];

            if (pkind == "BestEffort" || pkind == "kBestEffort") {
              qos.reliability.kind = Qos::Reliability::kBestEffort;
            } else if (pkind == "Reliable" || pkind == "kReliable") {
              qos.reliability.kind = Qos::Reliability::kReliable;
            } else {
              qos.reliability.kind = pkind;
            }
          }

          if (tobj.contains("block_time")) {
            qos.reliability.block_time = tobj["block_time"];
          }

          if (tobj.contains("heartbeat_time")) {
            qos.reliability.heartbeat_time = tobj["heartbeat_time"];
          }
        }

        if (obj.contains("history")) {
          auto tobj = obj["history"];

          if VUNLIKELY (!tobj.is_object()) {
            VLOG_E("QosProfile: Qos config file has wrong format.");
            return;
          }

          if (tobj.contains("kind")) {
            auto pkind = tobj["kind"];

            if (pkind == "KeepLast" || pkind == "kKeepLast") {
              qos.history.kind = Qos::History::kKeepLast;
            } else if (pkind == "KeepAll" || pkind == "kKeepAll") {
              qos.history.kind = Qos::History::kKeepAll;
            } else {
              qos.history.kind = pkind;
            }
          }

          if (tobj.contains("depth")) {
            qos.history.depth = tobj["depth"];
          }
        }

        if (obj.contains("durability")) {
          auto tobj = obj["durability"];

          if VUNLIKELY (!tobj.is_object()) {
            VLOG_E("QosProfile: Qos config file has wrong format.");
            return;
          }

          if (tobj.contains("kind")) {
            auto pkind = tobj["kind"];

            if (pkind == "Volatile" || pkind == "kVolatile") {
              qos.durability.kind = Qos::Durability::kVolatile;
            } else if (pkind == "TransientLocal" || pkind == "kTransientLocal") {
              qos.durability.kind = Qos::Durability::kTransientLocal;
            } else if (pkind == "Transient" || pkind == "kTransient") {
              qos.durability.kind = Qos::Durability::kTransient;
            } else if (pkind == "Persistent" || pkind == "kPersistent") {
              qos.durability.kind = Qos::Durability::kPersistent;
            } else {
              qos.durability.kind = pkind;
            }
          }
        }

        if (obj.contains("publish_mode")) {
          auto tobj = obj["publish_mode"];

          if VUNLIKELY (!tobj.is_object()) {
            VLOG_E("QosProfile: Qos config file has wrong format.");
            return;
          }

          if (tobj.contains("kind")) {
            auto pkind = tobj["kind"];

            if (pkind == "Sync" || pkind == "kSync") {
              qos.publish_mode.kind = Qos::PublishMode::kSync;
            } else if (pkind == "ASync" || pkind == "kASync") {
              qos.publish_mode.kind = Qos::PublishMode::kASync;
            } else {
              qos.publish_mode.kind = pkind;
            }
          }
        }

        if (obj.contains("liveliness")) {
          auto tobj = obj["liveliness"];

          if VUNLIKELY (!tobj.is_object()) {
            VLOG_E("QosProfile: Qos config file has wrong format.");
            return;
          }

          if (tobj.contains("kind")) {
            auto pkind = tobj["kind"];

            if (pkind == "Automatic" || pkind == "kAutomatic") {
              qos.liveliness.kind = Qos::Liveliness::kAutomatic;
            } else if (pkind == "ManualParticipant" || pkind == "kManualParticipant") {
              qos.liveliness.kind = Qos::Liveliness::kManualParticipant;
            } else if (pkind == "ManualTopic" || pkind == "kManualTopic") {
              qos.liveliness.kind = Qos::Liveliness::kManualTopic;
            } else {
              qos.liveliness.kind = pkind;
            }
          }

          if (tobj.contains("duration")) {
            qos.liveliness.duration = tobj["duration"];
          }
        }

        if (obj.contains("destination_order")) {
          auto tobj = obj["destination_order"];

          if VUNLIKELY (!tobj.is_object()) {
            VLOG_E("QosProfile: Qos config file has wrong format.");
            return;
          }

          if (tobj.contains("kind")) {
            auto pkind = tobj["kind"];

            if (pkind == "ReceptionTimestamp" || pkind == "kReceptionTimestamp") {
              qos.destination_order.kind = Qos::DestinationOrder::kReceptionTimestamp;
            } else if (pkind == "SourceTimestamp" || pkind == "kSourceTimestamp") {
              qos.destination_order.kind = Qos::DestinationOrder::kSourceTimestamp;
            } else {
              qos.destination_order.kind = pkind;
            }
          }
        }

        if (obj.contains("ownership")) {
          auto tobj = obj["ownership"];

          if VUNLIKELY (!tobj.is_object()) {
            VLOG_E("QosProfile: Qos config file has wrong format.");
            return;
          }

          if (tobj.contains("kind")) {
            auto pkind = tobj["kind"];

            if (pkind == "Shared" || pkind == "kShared") {
              qos.ownership.kind = Qos::Ownership::kShared;
            } else if (pkind == "Exclusive" || pkind == "kExclusive" || pkind == "ExClusive") {
              qos.ownership.kind = Qos::Ownership::kExclusive;
            } else {
              qos.ownership.kind = pkind;
            }
          }
        }

        if (obj.contains("deadline")) {
          auto tobj = obj["deadline"];

          if VUNLIKELY (!tobj.is_object()) {
            VLOG_E("QosProfile: Qos config file has wrong format.");
            return;
          }

          if (tobj.contains("period")) {
            qos.deadline.period = tobj["period"];
          }
        }

        if (obj.contains("lifespan")) {
          auto tobj = obj["lifespan"];

          if VUNLIKELY (!tobj.is_object()) {
            VLOG_E("QosProfile: Qos config file has wrong format.");
            return;
          }

          if (tobj.contains("duration")) {
            qos.lifespan.duration = tobj["duration"];
          }
        }

        if (obj.contains("latency_budget")) {
          auto tobj = obj["latency_budget"];

          if VUNLIKELY (!tobj.is_object()) {
            VLOG_E("QosProfile: Qos config file has wrong format.");
            return;
          }

          if (tobj.contains("duration")) {
            qos.latency_budget.duration = tobj["duration"];
          }
        }

        if (obj.contains("resource_limits")) {
          auto tobj = obj["resource_limits"];

          if VUNLIKELY (!tobj.is_object()) {
            VLOG_E("QosProfile: Qos config file has wrong format.");
            return;
          }

          if (tobj.contains("max_samples")) {
            qos.resource_limits.max_samples = tobj["max_samples"];
          }

          if (tobj.contains("max_instances")) {
            qos.resource_limits.max_instances = tobj["max_instances"];
          }

          if (tobj.contains("max_samples_per_instance")) {
            qos.resource_limits.max_samples_per_instance = tobj["max_samples_per_instance"];
          }
        }

        if (obj.contains("additions")) {
          auto tobj = obj["additions"];

          if VUNLIKELY (!tobj.is_object()) {
            VLOG_E("QosProfile: Qos config file has wrong format.");
            return;
          }

          if (tobj.contains("priority")) {
            qos.additions.priority = static_cast<Qos::Additions::Priority>(tobj["priority"].get<uint8_t>());
          }

          if (tobj.contains("is_express")) {
            qos.additions.is_express = tobj["is_express"];
          }
        }

        qos.valid = true;

        available_qos_map[qos.name] = qos;
      }
    } catch (nlohmann::json::exception& e) {
      VLOG_E("QosProfile: Qos config parse error: ", e.what(), ".");
      return;
    }
  }

  ~GlobalQosProfile() = default;

  std::unordered_map<std::string, Qos> available_qos_map;
};

namespace QosProfile {

const std::unordered_map<std::string, Qos>& get_available_qos_map() noexcept {
  static GlobalQosProfile global_profile;

  return global_profile.available_qos_map;
}

}  // namespace QosProfile

}  // namespace vlink
