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

#include <nanobind/stl/string.h>
#include <nanobind/stl/unordered_map.h>
#include <vlink/extension/qos_profile.h>
#include <vlink/extension/status.h>

#include <cstring>

#include "bindings.h"
#include "strings.h"

namespace vlink::python {

using namespace nb::literals;  // NOLINT

void bind_qos(nb::module_& m) {
  nb::class_<vlink::Qos> qos_cls(m, "Qos", "DDS-compatible Quality of Service");

  nb::class_<vlink::Qos::Reliability> qos_rel(qos_cls, "Reliability");
  nb::enum_<vlink::Qos::Reliability::Kind>(qos_rel, "Kind")
      .value("BestEffort", vlink::Qos::Reliability::kBestEffort)
      .value("Reliable", vlink::Qos::Reliability::kReliable);
  qos_rel.def(nb::init<>())
      .def_rw("kind", &vlink::Qos::Reliability::kind)
      .def_rw("block_time", &vlink::Qos::Reliability::block_time)
      .def_rw("heartbeat_time", &vlink::Qos::Reliability::heartbeat_time);

  nb::class_<vlink::Qos::History> qos_hist(qos_cls, "History");
  nb::enum_<vlink::Qos::History::Kind>(qos_hist, "Kind")
      .value("KeepLast", vlink::Qos::History::kKeepLast)
      .value("KeepAll", vlink::Qos::History::kKeepAll);
  qos_hist.def(nb::init<>()).def_rw("kind", &vlink::Qos::History::kind).def_rw("depth", &vlink::Qos::History::depth);

  nb::class_<vlink::Qos::Durability> qos_dur(qos_cls, "Durability");
  nb::enum_<vlink::Qos::Durability::Kind>(qos_dur, "Kind")
      .value("Volatile", vlink::Qos::Durability::kVolatile)
      .value("TransientLocal", vlink::Qos::Durability::kTransientLocal)
      .value("Transient", vlink::Qos::Durability::kTransient)
      .value("Persistent", vlink::Qos::Durability::kPersistent);
  qos_dur.def(nb::init<>()).def_rw("kind", &vlink::Qos::Durability::kind);

  nb::class_<vlink::Qos::PublishMode> qos_pm(qos_cls, "PublishMode");
  nb::enum_<vlink::Qos::PublishMode::Kind>(qos_pm, "Kind")
      .value("Sync", vlink::Qos::PublishMode::kSync)
      .value("ASync", vlink::Qos::PublishMode::kASync);
  qos_pm.def(nb::init<>()).def_rw("kind", &vlink::Qos::PublishMode::kind);

  nb::class_<vlink::Qos::Liveliness> qos_lv(qos_cls, "Liveliness");
  nb::enum_<vlink::Qos::Liveliness::Kind>(qos_lv, "Kind")
      .value("Automatic", vlink::Qos::Liveliness::kAutomatic)
      .value("ManualParticipant", vlink::Qos::Liveliness::kManualParticipant)
      .value("ManualTopic", vlink::Qos::Liveliness::kManualTopic);
  qos_lv.def(nb::init<>())
      .def_rw("kind", &vlink::Qos::Liveliness::kind)
      .def_rw("duration", &vlink::Qos::Liveliness::duration);

  nb::class_<vlink::Qos::DestinationOrder> qos_do(qos_cls, "DestinationOrder");
  nb::enum_<vlink::Qos::DestinationOrder::Kind>(qos_do, "Kind")
      .value("ReceptionTimestamp", vlink::Qos::DestinationOrder::kReceptionTimestamp)
      .value("SourceTimestamp", vlink::Qos::DestinationOrder::kSourceTimestamp);
  qos_do.def(nb::init<>()).def_rw("kind", &vlink::Qos::DestinationOrder::kind);

  nb::class_<vlink::Qos::Ownership> qos_own(qos_cls, "Ownership");
  nb::enum_<vlink::Qos::Ownership::Kind>(qos_own, "Kind")
      .value("Shared", vlink::Qos::Ownership::kShared)
      .value("Exclusive", vlink::Qos::Ownership::kExclusive);
  qos_own.def(nb::init<>()).def_rw("kind", &vlink::Qos::Ownership::kind);

  nb::class_<vlink::Qos::Deadline>(qos_cls, "Deadline")
      .def(nb::init<>())
      .def_rw("period", &vlink::Qos::Deadline::period);
  nb::class_<vlink::Qos::Lifespan>(qos_cls, "Lifespan")
      .def(nb::init<>())
      .def_rw("duration", &vlink::Qos::Lifespan::duration);
  nb::class_<vlink::Qos::LatencyBudget>(qos_cls, "LatencyBudget")
      .def(nb::init<>())
      .def_rw("duration", &vlink::Qos::LatencyBudget::duration);

  nb::class_<vlink::Qos::ResourceLimits>(qos_cls, "ResourceLimits")
      .def(nb::init<>())
      .def_rw("max_samples", &vlink::Qos::ResourceLimits::max_samples)
      .def_rw("max_instances", &vlink::Qos::ResourceLimits::max_instances)
      .def_rw("max_samples_per_instance", &vlink::Qos::ResourceLimits::max_samples_per_instance);

  nb::class_<vlink::Qos::Additions> qos_add(qos_cls, "Additions");
  nb::enum_<vlink::Qos::Additions::Priority>(qos_add, "Priority")
      .value("RealTime", vlink::Qos::Additions::kPriorityRealTime)
      .value("High", vlink::Qos::Additions::kPriorityHigh)
      .value("Normal", vlink::Qos::Additions::kPriorityNormal)
      .value("Low", vlink::Qos::Additions::kPriorityLow)
      .value("Background", vlink::Qos::Additions::kPriorityBackground);
  qos_add.def(nb::init<>())
      .def_rw("priority", &vlink::Qos::Additions::priority)
      .def_rw("is_express", &vlink::Qos::Additions::is_express);

  qos_cls.def(nb::init<>())
      .def_prop_rw(
          "name", [](const vlink::Qos& q) { return std::string(q.name); },
          [](vlink::Qos& q, const std::string& s) {
            constexpr size_t kMax = sizeof(vlink::Qos::name) - 1;
            const auto prefix = utf8_prefix(s, kMax);
            std::memcpy(q.name, prefix.data(), prefix.size());
            q.name[prefix.size()] = '\0';
          })
      .def_rw("valid", &vlink::Qos::valid)
      .def_rw("reliability", &vlink::Qos::reliability)
      .def_rw("history", &vlink::Qos::history)
      .def_rw("durability", &vlink::Qos::durability)
      .def_rw("publish_mode", &vlink::Qos::publish_mode)
      .def_rw("liveliness", &vlink::Qos::liveliness)
      .def_rw("destination_order", &vlink::Qos::destination_order)
      .def_rw("ownership", &vlink::Qos::ownership)
      .def_rw("deadline", &vlink::Qos::deadline)
      .def_rw("lifespan", &vlink::Qos::lifespan)
      .def_rw("latency_budget", &vlink::Qos::latency_budget)
      .def_rw("resource_limits", &vlink::Qos::resource_limits)
      .def_rw("additions", &vlink::Qos::additions)
      .def("__repr__", [](const vlink::Qos& q) {
        return std::string("Qos(name='") + q.name + "', valid=" + (q.valid ? "True" : "False") + ")";
      });

  auto qos_profile = m.def_submodule("QosProfile", "Pre-defined QoS profiles");
  qos_profile.attr("Event") = vlink::QosProfile::kEvent;
  qos_profile.attr("Method") = vlink::QosProfile::kMethod;
  qos_profile.attr("Field") = vlink::QosProfile::kField;
  qos_profile.attr("Sensor") = vlink::QosProfile::kSensor;
  qos_profile.attr("Parameter") = vlink::QosProfile::kParameter;
  qos_profile.attr("Service") = vlink::QosProfile::kService;
  qos_profile.attr("Clock") = vlink::QosProfile::kClock;
  qos_profile.attr("Static") = vlink::QosProfile::kStatic;
  qos_profile.attr("Light") = vlink::QosProfile::kLight;
  qos_profile.attr("Poor") = vlink::QosProfile::kPoor;
  qos_profile.attr("Better") = vlink::QosProfile::kBetter;
  qos_profile.attr("Best") = vlink::QosProfile::kBest;
  qos_profile.attr("Stream") = vlink::QosProfile::kStream;
  qos_profile.attr("Alarm") = vlink::QosProfile::kAlarm;
  qos_profile.attr("Command") = vlink::QosProfile::kCommand;
  qos_profile.attr("Log") = vlink::QosProfile::kLog;
  qos_profile.def("get_available_qos_map", &vlink::QosProfile::get_available_qos_map);
}

void bind_status(nb::module_& m) {
  nb::enum_<vlink::Status::Type>(m, "StatusType")
      .value("PublicationMatched", vlink::Status::kPublicationMatched)
      .value("SubscriptionMatched", vlink::Status::kSubscriptionMatched)
      .value("OfferedDeadlineMissed", vlink::Status::kOfferedDeadlineMissed)
      .value("RequestedDeadlineMissed", vlink::Status::kRequestedDeadlineMissed)
      .value("OfferedIncompatibleQos", vlink::Status::kOfferedIncompatibleQos)
      .value("RequestedIncompatibleQos", vlink::Status::kRequestedIncompatibleQos)
      .value("LivelinessLost", vlink::Status::kLivelinessLost)
      .value("LivelinessChanged", vlink::Status::kLivelinessChanged)
      .value("SampleRejected", vlink::Status::kSampleRejected)
      .value("SampleLost", vlink::Status::kSampleLost)
      .value("Unknown", vlink::Status::kUnknown);

  auto status = m.def_submodule("Status", "Status helper functions");
  status.def("is_for_writer", &vlink::Status::is_for_writer, "type"_a);
  status.def("is_for_reader", &vlink::Status::is_for_reader, "type"_a);
}

}  // namespace vlink::python
