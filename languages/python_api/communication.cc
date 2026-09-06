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
#include <vlink/extension/status_detail.h>
#include <vlink/vlink.h>

#include <optional>
#include <stdexcept>

#include "callbacks.h"
#include "ownership.h"

namespace vlink::python {

using namespace nb::literals;  // NOLINT

static const void* python_node_status_callback_kind() noexcept {
  static const char kKind{};
  return &kKind;
}

static nb::dict status_to_dict(const vlink::Status::BasePtr& status) {
  nb::dict d;

  if VUNLIKELY (!status) {
    return d;
  }

  const auto type = status->get_type();
  d["type"] = static_cast<int>(type);
  d["status_type"] = type;
  d["description"] = status->get_string();

  auto put_handle = [&d](const char* key, vlink::Status::InstanceHandle handle) {
    if VUNLIKELY (handle == nullptr) {
      d[key] = nb::none();
    } else {
      d[key] = reinterpret_cast<uintptr_t>(handle);
    }
  };

  switch (type) {
    case vlink::Status::kPublicationMatched: {
      const auto publication_matched = std::static_pointer_cast<vlink::Status::PublicationMatched>(status);
      d["total_count"] = publication_matched->total_count;
      d["total_count_change"] = publication_matched->total_count_change;
      d["current_count"] = publication_matched->current_count;
      d["current_count_change"] = publication_matched->current_count_change;
      put_handle("last_subscription_handle", publication_matched->last_subscription_handle);
      break;
    }
    case vlink::Status::kOfferedDeadlineMissed: {
      const auto offered_deadline_missed = std::static_pointer_cast<vlink::Status::OfferedDeadlineMissed>(status);
      d["total_count"] = offered_deadline_missed->total_count;
      d["total_count_change"] = offered_deadline_missed->total_count_change;
      put_handle("last_instance_handle", offered_deadline_missed->last_instance_handle);
      break;
    }
    case vlink::Status::kOfferedIncompatibleQos: {
      const auto offered_incompatible_qos = std::static_pointer_cast<vlink::Status::OfferedIncompatibleQos>(status);
      d["total_count"] = offered_incompatible_qos->total_count;
      d["total_count_change"] = offered_incompatible_qos->total_count_change;
      d["last_policy_id"] = offered_incompatible_qos->last_policy_id;
      break;
    }
    case vlink::Status::kLivelinessLost: {
      const auto liveliness_lost = std::static_pointer_cast<vlink::Status::LivelinessLost>(status);
      d["total_count"] = liveliness_lost->total_count;
      d["total_count_change"] = liveliness_lost->total_count_change;
      break;
    }
    case vlink::Status::kSubscriptionMatched: {
      const auto subscription_matched = std::static_pointer_cast<vlink::Status::SubscriptionMatched>(status);
      d["total_count"] = subscription_matched->total_count;
      d["total_count_change"] = subscription_matched->total_count_change;
      d["current_count"] = subscription_matched->current_count;
      d["current_count_change"] = subscription_matched->current_count_change;
      put_handle("last_publication_handle", subscription_matched->last_publication_handle);
      break;
    }
    case vlink::Status::kRequestedDeadlineMissed: {
      const auto requested_deadline_missed = std::static_pointer_cast<vlink::Status::RequestedDeadlineMissed>(status);
      d["total_count"] = requested_deadline_missed->total_count;
      d["total_count_change"] = requested_deadline_missed->total_count_change;
      put_handle("last_instance_handle", requested_deadline_missed->last_instance_handle);
      break;
    }
    case vlink::Status::kLivelinessChanged: {
      const auto liveliness_changed = std::static_pointer_cast<vlink::Status::LivelinessChanged>(status);
      d["alive_count"] = liveliness_changed->alive_count;
      d["not_alive_count"] = liveliness_changed->not_alive_count;
      d["alive_count_change"] = liveliness_changed->alive_count_change;
      d["not_alive_count_change"] = liveliness_changed->not_alive_count_change;
      put_handle("last_publication_handle", liveliness_changed->last_publication_handle);
      break;
    }
    case vlink::Status::kSampleRejected: {
      const auto sample_rejected = std::static_pointer_cast<vlink::Status::SampleRejected>(status);
      d["total_count"] = sample_rejected->total_count;
      d["total_count_change"] = sample_rejected->total_count_change;
      d["last_reason"] = static_cast<int>(sample_rejected->last_reason);
      put_handle("last_instance_handle", sample_rejected->last_instance_handle);
      break;
    }
    case vlink::Status::kRequestedIncompatibleQos: {
      const auto requested_incompatible_qos = std::static_pointer_cast<vlink::Status::RequestedIncompatibleQos>(status);
      d["total_count"] = requested_incompatible_qos->total_count;
      d["total_count_change"] = requested_incompatible_qos->total_count_change;
      d["last_policy_id"] = requested_incompatible_qos->last_policy_id;
      break;
    }
    case vlink::Status::kSampleLost: {
      const auto sample_lost = std::static_pointer_cast<vlink::Status::SampleLost>(status);
      d["total_count"] = sample_lost->total_count;
      d["total_count_change"] = sample_lost->total_count_change;
      break;
    }
    default: {
      break;
    }
  }

  return d;
}

template <typename NativeT, typename MsgT, typename Codec = PythonCodec<MsgT>>
static auto make_owned_value_callback(NativeT* native, nb::callable py_cb, const char* context) {
  auto activity = std::make_shared<PythonCallbackActivity>();
  auto cb = std::make_shared<GilSafePyFunction>(std::move(py_cb));

  return [native, activity, cb = std::move(cb), context](const MsgT& value) {
    PythonCallbackScope active(activity, native);

    if VUNLIKELY (!Py_IsInitialized()) {
      return;
    }

    nb::gil_scoped_acquire gil;

    if VUNLIKELY (python_native_finalizing().find(native) != python_native_finalizing().end()) {
      return;
    }

    nb::object owner = nb::find(native);

    try {
      cb->fn(Codec::to_python(value));
    } catch (std::exception&) {
      report_current_exception(context);
    }

    defer_last_python_callback_owner(owner, activity);
  };
}

template <typename NativeT>
static auto make_owned_connect_callback(NativeT* native, nb::callable py_cb, const char* context) {
  auto activity = std::make_shared<PythonCallbackActivity>();
  auto cb = std::make_shared<GilSafePyFunction>(std::move(py_cb));

  return [native, activity, cb = std::move(cb), context](bool connected) {
    PythonCallbackScope active(activity, native);

    if VUNLIKELY (!Py_IsInitialized()) {
      return;
    }

    nb::gil_scoped_acquire gil;

    if VUNLIKELY (python_native_finalizing().find(native) != python_native_finalizing().end()) {
      return;
    }

    nb::object owner = nb::find(native);

    try {
      cb->fn(connected);
    } catch (std::exception&) {
      report_current_exception(context);
    }

    defer_last_python_callback_owner(owner, activity);
  };
}

template <typename NodeT>
static NodeT* make_url_node(const std::string& url, const std::string& ser_type, vlink::SchemaType schema_type,
                            bool auto_init) {
  const bool has_ser_type = !ser_type.empty();
  const bool has_schema_type = schema_type != vlink::SchemaType::kUnknown;

  if VUNLIKELY (!has_ser_type && has_schema_type) {
    throw nb::value_error("schema_type requires ser_type");
  }

  auto node = std::make_unique<NodeT>(url, vlink::InitType::kWithoutInit);

  if VLIKELY (has_ser_type) {
    node->set_ser_type(ser_type, schema_type);
  }

  if VLIKELY (auto_init) {
    node->init();
  }

  return node.release();
}

template <typename NodeT>
static NodeT* make_url_security_node(const std::string& url, vlink::Security::Config sec_cfg,
                                     const std::string& ser_type, vlink::SchemaType schema_type, bool auto_init) {
  const bool has_ser_type = !ser_type.empty();
  const bool has_schema_type = schema_type != vlink::SchemaType::kUnknown;

  if VUNLIKELY (!has_ser_type && has_schema_type) {
    throw nb::value_error("schema_type requires ser_type");
  }

  if (has_ser_type && sec_cfg.advanced.aad_context.empty()) {
    const auto resolved_schema_type = has_schema_type ? schema_type : vlink::SchemaData::infer_ser_type(ser_type);
    sec_cfg.advanced.aad_context = url;
    sec_cfg.advanced.aad_context += "|";
    sec_cfg.advanced.aad_context += ser_type;
    sec_cfg.advanced.aad_context += "|";
    sec_cfg.advanced.aad_context += std::to_string(static_cast<uint32_t>(resolved_schema_type));
  }

  auto node = std::make_unique<NodeT>(url, std::move(sec_cfg), vlink::InitType::kWithoutInit);

  if VLIKELY (has_ser_type) {
    node->set_ser_type(ser_type, schema_type);
  }

  if VLIKELY (auto_init) {
    node->init();
  }

  return node.release();
}

template <typename NodeT>
static void ensure_python_node_pre_destroy_hook(nb::handle instance, NodeT* node) {
  ensure_python_pre_destroy_hook(instance, node, [](NodeT& owner) { owner.deinit(); });
}

template <typename Class, typename NodeT>
static void bind_node_common(Class& cls) {
  cls.def("init", &NodeT::init)
      .def("deinit",
           [](NodeT& self) {
             if VUNLIKELY (is_in_python_owner_callback(&self)) {
               throw std::runtime_error("Node.deinit() cannot be called from that node's active Python callback");
             }

             nb::gil_scoped_release release;
             return self.deinit();
           })
      .def("interrupt", &NodeT::interrupt)
      .def("has_inited", &NodeT::has_inited)
      .def("get_url", &NodeT::get_url)
      .def("get_ser_type", &NodeT::get_ser_type)
      .def("set_ser_type", &NodeT::set_ser_type, "ser_type"_a, "schema_type"_a = vlink::SchemaType::kUnknown,
           "Override serialization metadata. While a DDS node is initialized, its raw/CDR mode and CDR type name "
           "are immutable; call deinit(), update them, then init().")
      .def("get_schema_type", &NodeT::get_schema_type)
      .def("get_transport_type", &NodeT::get_transport_type)
      .def("set_property", &NodeT::set_property, "key"_a, "value"_a)
      .def("get_property", &NodeT::get_property, "key"_a)
      .def("set_discovery_enabled", &NodeT::set_discovery_enabled, "enable"_a)
      .def("get_discovery_enabled", &NodeT::get_discovery_enabled)
      .def("set_record_path", &NodeT::set_record_path, "path"_a)
      .def("set_ssl_options", &NodeT::set_ssl_options, "options"_a)
      .def("set_safety_quit", &NodeT::set_safety_quit, "enable"_a)
      .def("get_safety_quit", &NodeT::get_safety_quit)
      .def("is_support_loan", &NodeT::is_support_loan)
      .def(
          "loan", [](NodeT& self, int64_t size) { return self.loan(size); }, "size"_a)
      .def(
          "return_loan",
          [](NodeT& self, vlink::Bytes& bytes) {
            ensure_bytes_not_exported(bytes);
            return self.return_loan(bytes);
          },
          "bytes"_a)
      .def("suspend", &NodeT::suspend)
      .def("resume", &NodeT::resume)
      .def("is_suspend", &NodeT::is_suspend)
      .def(
          "attach",
          [](nb::object instance, nb::object loop) {
            auto& self = nb::cast<NodeT&>(instance);
            auto* loop_ptr = nb::cast<vlink::MessageLoop*>(loop);
            const bool result = self.attach(loop_ptr);

            if (self.get_message_loop() == loop_ptr) {
              ensure_python_node_pre_destroy_hook(instance, &self);
              bind_python_instance_owner(instance, std::move(loop));
            } else if (self.get_message_loop() == nullptr) {
              unbind_python_instance_owner(instance);
            }

            return result;
          },
          "loop"_a)
      .def("detach",
           [](nb::object instance) {
             auto& self = nb::cast<NodeT&>(instance);
             auto* attached_loop = self.get_message_loop();
             const bool called_from_loop = attached_loop != nullptr && attached_loop->is_in_same_thread();
             bool result;
             {
               nb::gil_scoped_release release;
               result = self.detach();
             }

             if (self.get_message_loop() == nullptr) {
               if (called_from_loop) {
                 unbind_python_instance_owner_after_loop_idle(instance, *attached_loop);
               } else {
                 unbind_python_instance_owner(instance);
               }
             }

             return result;
           })
      .def("get_message_loop", &NodeT::get_message_loop, nb::rv_policy::reference)
      .def(
          "get_abstract_node",
          [](const NodeT& self) -> nb::object {
            const auto* node = self.get_abstract_node();

            if VUNLIKELY (node == nullptr) {
              return nb::none();
            }

            return nb::int_(reinterpret_cast<uintptr_t>(node));
          },
          "Return the non-owning AbstractNode address, or None if unavailable.")
      .def("get_cpu_usage", &NodeT::get_cpu_usage)
      .def(
          "get_status",
          [](NodeT& self, vlink::Status::Type type) -> nb::object {
            auto status = self.get_status(type);

            if VUNLIKELY (!status) {
              return nb::none();
            }

            return nb::object(status_to_dict(status));
          },
          "type"_a)
      .def(
          "register_status_handler",
          [](nb::object instance, nb::callable callback) {
            auto& self = nb::cast<NodeT&>(instance);
            if VUNLIKELY (is_in_python_owner_callback(&self, python_node_status_callback_kind())) {
              throw std::runtime_error(
                  "Node status handler cannot be replaced from that node's active Python callback");
            }

            ensure_python_node_pre_destroy_hook(instance, &self);
            auto activity = std::make_shared<PythonCallbackActivity>();
            auto cb = std::make_shared<GilSafePyFunction>(std::move(callback));
            nb::gil_scoped_release release;
            self.register_status_handler([native = &self, activity, cb](const vlink::Status::BasePtr& status) {
              invoke_owned_python_callback(
                  native, activity, "vlink::register_status_handler", [&]() { cb->fn(status_to_dict(status)); },
                  python_node_status_callback_kind());
            });
          },
          "callback"_a);
}

template <typename Class, typename NodeT>
static void bind_node_security_ctor(Class& cls) {
  cls.def(nb::new_([](const std::string& url, vlink::Security::Config sec_cfg, const std::string& ser_type,
                      vlink::SchemaType schema_type, bool auto_init) {
            return make_url_security_node<NodeT>(url, std::move(sec_cfg), ser_type, schema_type, auto_init);
          }),
          "url"_a, "sec_cfg"_a, "ser_type"_a = "", "schema_type"_a = vlink::SchemaType::kUnknown, "auto_init"_a = true);
}

template <typename PubT, typename MsgT, typename Codec = PythonCodec<MsgT>, bool SecurityNode = false>
static void bind_publisher(nb::module_& m, const char* name, const char* doc) {
  auto cls = nb::class_<PubT>(m, name, doc, nb::is_weak_referenceable());
  if constexpr (SecurityNode) {
    bind_node_security_ctor<decltype(cls), PubT>(cls);
  } else {
    cls.def(nb::new_([](const std::string& url, const std::string& ser_type, vlink::SchemaType schema_type,
                        bool auto_init) { return make_url_node<PubT>(url, ser_type, schema_type, auto_init); }),
            "url"_a, "ser_type"_a = "", "schema_type"_a = vlink::SchemaType::kUnknown, "auto_init"_a = true);
  }
  bind_node_common<decltype(cls), PubT>(cls);
  cls.def(
         "detect_subscribers",
         [](nb::object instance, nb::callable callback) {
           auto& self = nb::cast<PubT&>(instance);
           ensure_python_node_pre_destroy_hook(instance, &self);
           self.detect_subscribers(
               make_owned_connect_callback(&self, std::move(callback), "vlink::Publisher.detect_subscribers"));
         },
         "callback"_a)
      .def(
          "wait_for_subscribers",
          [](PubT& self, int timeout_ms) {
            nb::gil_scoped_release release;
            return self.wait_for_subscribers(std::chrono::milliseconds(timeout_ms));
          },
          "timeout_ms"_a = 5000)
      .def("has_subscribers", &PubT::has_subscribers)
      .def(
          "publish",
          [](PubT& self, nb::handle data, bool force) {
            auto value = Codec::from_python_owned(data);
            nb::gil_scoped_release release;
            return self.publish(value, force);
          },
          "data"_a, "force"_a = false)
      .def(
          "publish_fbb",
          [](PubT& self, nb::handle data, bool force) {
            auto value = Codec::from_python_owned(data);
            nb::gil_scoped_release release;
            return self.publish(value, force);
          },
          "data"_a, "force"_a = false, "Publish a finished FlatBuffers byte buffer.")
      .def("mark_as_setter", &PubT::mark_as_setter)
      .def("__repr__",
           [name = std::string(name)](const PubT& self) { return name + "(url='" + self.get_url() + "')"; });
}

template <typename SubT, typename MsgT, typename Codec = PythonCodec<MsgT>, bool SecurityNode = false>
static void bind_subscriber(nb::module_& m, const char* name, const char* doc) {
  auto cls = nb::class_<SubT>(m, name, doc, nb::is_weak_referenceable());
  if constexpr (SecurityNode) {
    bind_node_security_ctor<decltype(cls), SubT>(cls);
  } else {
    cls.def(nb::new_([](const std::string& url, const std::string& ser_type, vlink::SchemaType schema_type,
                        bool auto_init) { return make_url_node<SubT>(url, ser_type, schema_type, auto_init); }),
            "url"_a, "ser_type"_a = "", "schema_type"_a = vlink::SchemaType::kUnknown, "auto_init"_a = true);
  }
  bind_node_common<decltype(cls), SubT>(cls);
  cls.def(
         "listen",
         [](nb::object instance, nb::callable callback) {
           auto& self = nb::cast<SubT&>(instance);
           ensure_python_node_pre_destroy_hook(instance, &self);
           return self.listen(
               make_owned_value_callback<SubT, MsgT, Codec>(&self, std::move(callback), "vlink::Subscriber.listen"));
         },
         "callback"_a)
      .def("set_latency_and_lost_enabled", &SubT::set_latency_and_lost_enabled, "enable"_a)
      .def("is_latency_and_lost_enabled", &SubT::is_latency_and_lost_enabled)
      .def("get_latency", &SubT::get_latency)
      .def("get_lost", &SubT::get_lost)
      .def("mark_as_getter", &SubT::mark_as_getter)
      .def("__repr__",
           [name = std::string(name)](const SubT& self) { return name + "(url='" + self.get_url() + "')"; });
}

template <typename ServerT, typename ReqT, typename RespT, typename ReqCodec = PythonCodec<ReqT>,
          typename RespCodec = PythonCodec<RespT>, bool SecurityNode = false>
static void bind_server(nb::module_& m, const char* name, const char* doc) {
  auto cls = nb::class_<ServerT>(m, name, doc, nb::is_weak_referenceable());
  if constexpr (SecurityNode) {
    bind_node_security_ctor<decltype(cls), ServerT>(cls);
  } else {
    cls.def(nb::new_([](const std::string& url, const std::string& ser_type, vlink::SchemaType schema_type,
                        bool auto_init) { return make_url_node<ServerT>(url, ser_type, schema_type, auto_init); }),
            "url"_a, "ser_type"_a = "", "schema_type"_a = vlink::SchemaType::kUnknown, "auto_init"_a = true);
  }
  bind_node_common<decltype(cls), ServerT>(cls);
  cls.def(
         "listen",
         [](nb::object instance, nb::callable callback) {
           auto& self = nb::cast<ServerT&>(instance);
           ensure_python_node_pre_destroy_hook(instance, &self);
           auto activity = std::make_shared<PythonCallbackActivity>();
           auto cb = std::make_shared<GilSafePyFunction>(std::move(callback));
           return self.listen([native = &self, activity, cb](const ReqT& req, RespT& resp) {
             invoke_owned_python_callback(native, activity, "vlink::Server.listen", [&]() {
               nb::object result = cb->fn(ReqCodec::to_python(req));

               if VLIKELY (!result.is_none()) {
                 resp = RespCodec::from_python_owned(result);
               }
             });
           });
         },
         "callback"_a, "callback(request) -> response or None")
      .def(
          "listen_for_reply",
          [](nb::object instance, nb::callable callback) {
            auto& self = nb::cast<ServerT&>(instance);
            ensure_python_node_pre_destroy_hook(instance, &self);
            auto activity = std::make_shared<PythonCallbackActivity>();
            auto cb = std::make_shared<GilSafePyFunction>(std::move(callback));
            return self.listen_for_reply([native = &self, activity, cb](uint64_t req_id, const ReqT& req) {
              invoke_owned_python_callback(native, activity, "vlink::Server.listen_for_reply",
                                           [&]() { cb->fn(req_id, ReqCodec::to_python(req)); });
            });
          },
          "callback"_a, "callback(req_id, request). Call reply(req_id, response) later")
      .def(
          "reply",
          [](ServerT& self, uint64_t req_id, nb::handle data) {
            return self.reply(req_id, RespCodec::from_python_owned(data));
          },
          "req_id"_a, "data"_a)
      .def("__repr__",
           [name = std::string(name)](const ServerT& self) { return name + "(url='" + self.get_url() + "')"; });
}

template <typename ServerT, typename ReqT, typename ReqCodec = PythonCodec<ReqT>, bool SecurityNode = false>
static void bind_fire_forget_server(nb::module_& m, const char* name, const char* doc) {
  auto cls = nb::class_<ServerT>(m, name, doc, nb::is_weak_referenceable());
  if constexpr (SecurityNode) {
    bind_node_security_ctor<decltype(cls), ServerT>(cls);
  } else {
    cls.def(nb::new_([](const std::string& url, const std::string& ser_type, vlink::SchemaType schema_type,
                        bool auto_init) { return make_url_node<ServerT>(url, ser_type, schema_type, auto_init); }),
            "url"_a, "ser_type"_a = "", "schema_type"_a = vlink::SchemaType::kUnknown, "auto_init"_a = true);
  }
  bind_node_common<decltype(cls), ServerT>(cls);
  cls.def(
         "listen",
         [](nb::object instance, nb::callable callback) {
           auto& self = nb::cast<ServerT&>(instance);
           ensure_python_node_pre_destroy_hook(instance, &self);
           auto activity = std::make_shared<PythonCallbackActivity>();
           auto cb = std::make_shared<GilSafePyFunction>(std::move(callback));
           return self.listen([native = &self, activity, cb](const ReqT& req) {
             invoke_owned_python_callback(native, activity, "vlink::FireForgetServer.listen",
                                          [&]() { cb->fn(ReqCodec::to_python(req)); });
           });
         },
         "callback"_a, "callback(request) -> None")
      .def("__repr__",
           [name = std::string(name)](const ServerT& self) { return name + "(url='" + self.get_url() + "')"; });
}

template <typename ClientT, typename ReqT, typename RespT, typename ReqCodec = PythonCodec<ReqT>,
          typename RespCodec = PythonCodec<RespT>, bool SecurityNode = false>
static void bind_client(nb::module_& m, const char* name, const char* doc) {
  auto cls = nb::class_<ClientT>(m, name, doc, nb::is_weak_referenceable());
  if constexpr (SecurityNode) {
    bind_node_security_ctor<decltype(cls), ClientT>(cls);
  } else {
    cls.def(nb::new_([](const std::string& url, const std::string& ser_type, vlink::SchemaType schema_type,
                        bool auto_init) { return make_url_node<ClientT>(url, ser_type, schema_type, auto_init); }),
            "url"_a, "ser_type"_a = "", "schema_type"_a = vlink::SchemaType::kUnknown, "auto_init"_a = true);
  }
  bind_node_common<decltype(cls), ClientT>(cls);
  cls.def(
         "detect_connected",
         [](nb::object instance, nb::callable callback) {
           auto& self = nb::cast<ClientT&>(instance);
           ensure_python_node_pre_destroy_hook(instance, &self);
           self.detect_connected(
               make_owned_connect_callback(&self, std::move(callback), "vlink::Client.detect_connected"));
         },
         "callback"_a)
      .def(
          "wait_for_connected",
          [](ClientT& self, int timeout_ms) {
            nb::gil_scoped_release release;
            return self.wait_for_connected(std::chrono::milliseconds(timeout_ms));
          },
          "timeout_ms"_a = 5000)
      .def("is_connected", &ClientT::is_connected)
      .def(
          "invoke",
          [](ClientT& self, nb::handle data, int timeout_ms) -> nb::object {
            auto req = ReqCodec::from_python_owned(data);
            std::optional<RespT> res;
            {
              nb::gil_scoped_release release;
              res = self.invoke(req, std::chrono::milliseconds(timeout_ms));
            }

            if VLIKELY (res.has_value()) {
              return RespCodec::to_python(*res);
            }

            return nb::none();
          },
          "data"_a, "timeout_ms"_a = 5000)
      .def(
          "invoke_async",
          [](nb::object instance, nb::handle data, nb::callable callback) {
            auto& self = nb::cast<ClientT&>(instance);
            ensure_python_node_pre_destroy_hook(instance, &self);
            auto req = ReqCodec::from_python_owned(data);
            auto activity = std::make_shared<PythonCallbackActivity>();
            auto cb = std::make_shared<GilSafePyFunction>(std::move(callback));
            return self.invoke(req, [native = &self, activity, cb](const RespT& resp) {
              invoke_owned_python_callback(native, activity, "vlink::Client.invoke_async",
                                           [&]() { cb->fn(RespCodec::to_python(resp)); });
            });
          },
          "data"_a, "callback"_a)
      .def(
          "async_invoke",
          [](nb::object instance, nb::handle data) {
            auto& self = nb::cast<ClientT&>(instance);
            ensure_python_node_pre_destroy_hook(instance, &self);
            auto req = ReqCodec::from_python_owned(data);
            nb::object py_future = nb::module_::import_("concurrent.futures").attr("Future")();
            auto activity = std::make_shared<PythonCallbackActivity>();
            auto future_ref = std::make_shared<GilSafePyObject>(nb::object(py_future));
            const bool accepted = self.invoke(req, [native = &self, activity, future_ref](const RespT& resp) {
              invoke_owned_python_callback(native, activity, "vlink::Client.async_invoke.set_result",
                                           [&]() { future_ref->obj.attr("set_result")(RespCodec::to_python(resp)); });
            });

            if VUNLIKELY (!accepted) {
              nb::object exc =
                  nb::module_::import_("builtins").attr("RuntimeError")("VLink async_invoke failed to submit request");
              py_future.attr("set_exception")(exc);
            }

            return py_future;
          },
          "data"_a, "Return a concurrent.futures.Future resolved with the response bytes.")
      .def("__repr__", [name = std::string(name)](const ClientT& self) {
        const char* connected = "Unknown";
        if (self.has_inited()) {
          connected = self.is_connected() ? "True" : "False";
        }
        return name + "(url='" + self.get_url() + "', connected=" + connected + ")";
      });
}

template <typename ClientT, typename ReqT, typename ReqCodec = PythonCodec<ReqT>, bool SecurityNode = false>
static void bind_fire_forget_client(nb::module_& m, const char* name, const char* doc) {
  auto cls = nb::class_<ClientT>(m, name, doc, nb::is_weak_referenceable());
  if constexpr (SecurityNode) {
    bind_node_security_ctor<decltype(cls), ClientT>(cls);
  } else {
    cls.def(nb::new_([](const std::string& url, const std::string& ser_type, vlink::SchemaType schema_type,
                        bool auto_init) { return make_url_node<ClientT>(url, ser_type, schema_type, auto_init); }),
            "url"_a, "ser_type"_a = "", "schema_type"_a = vlink::SchemaType::kUnknown, "auto_init"_a = true);
  }
  bind_node_common<decltype(cls), ClientT>(cls);
  cls.def(
         "detect_connected",
         [](nb::object instance, nb::callable callback) {
           auto& self = nb::cast<ClientT&>(instance);
           ensure_python_node_pre_destroy_hook(instance, &self);
           self.detect_connected(
               make_owned_connect_callback(&self, std::move(callback), "vlink::FireForgetClient.detect_connected"));
         },
         "callback"_a)
      .def(
          "wait_for_connected",
          [](ClientT& self, int timeout_ms) {
            nb::gil_scoped_release release;
            return self.wait_for_connected(std::chrono::milliseconds(timeout_ms));
          },
          "timeout_ms"_a = 5000)
      .def("is_connected", &ClientT::is_connected)
      .def(
          "send",
          [](ClientT& self, nb::handle data) {
            auto req = ReqCodec::from_python_owned(data);
            nb::gil_scoped_release release;
            return self.send(req);
          },
          "data"_a)
      .def("__repr__", [name = std::string(name)](const ClientT& self) {
        const char* connected = "Unknown";
        if (self.has_inited()) {
          connected = self.is_connected() ? "True" : "False";
        }
        return name + "(url='" + self.get_url() + "', connected=" + connected + ")";
      });
}

template <typename SetterT, typename ValueT, typename Codec = PythonCodec<ValueT>, bool SecurityNode = false>
static void bind_setter(nb::module_& m, const char* name, const char* doc) {
  auto cls = nb::class_<SetterT>(m, name, doc, nb::is_weak_referenceable());
  if constexpr (SecurityNode) {
    bind_node_security_ctor<decltype(cls), SetterT>(cls);
  } else {
    cls.def(nb::new_([](const std::string& url, const std::string& ser_type, vlink::SchemaType schema_type,
                        bool auto_init) { return make_url_node<SetterT>(url, ser_type, schema_type, auto_init); }),
            "url"_a, "ser_type"_a = "", "schema_type"_a = vlink::SchemaType::kUnknown, "auto_init"_a = true);
  }
  bind_node_common<decltype(cls), SetterT>(cls);
  cls.def(
         "set", [](SetterT& self, nb::handle data) { self.set(Codec::from_python_owned(data)); }, "data"_a)
      .def("mark_as_publisher", &SetterT::mark_as_publisher)
      .def("__repr__",
           [name = std::string(name)](const SetterT& self) { return name + "(url='" + self.get_url() + "')"; });
}

template <typename GetterT, typename ValueT, typename Codec = PythonCodec<ValueT>, bool SecurityNode = false>
static void bind_getter(nb::module_& m, const char* name, const char* doc) {
  auto cls = nb::class_<GetterT>(m, name, doc, nb::is_weak_referenceable());
  if constexpr (SecurityNode) {
    bind_node_security_ctor<decltype(cls), GetterT>(cls);
  } else {
    cls.def(nb::new_([](const std::string& url, const std::string& ser_type, vlink::SchemaType schema_type,
                        bool auto_init) { return make_url_node<GetterT>(url, ser_type, schema_type, auto_init); }),
            "url"_a, "ser_type"_a = "", "schema_type"_a = vlink::SchemaType::kUnknown, "auto_init"_a = true);
  }
  bind_node_common<decltype(cls), GetterT>(cls);
  cls.def(
         "listen",
         [](nb::object instance, nb::callable callback) {
           auto& self = nb::cast<GetterT&>(instance);
           ensure_python_node_pre_destroy_hook(instance, &self);
           return self.listen(
               make_owned_value_callback<GetterT, ValueT, Codec>(&self, std::move(callback), "vlink::Getter.listen"));
         },
         "callback"_a)
      .def("get",
           [](GetterT& self) -> nb::object {
             auto result = self.get();

             if VLIKELY (result.has_value()) {
               return Codec::to_python(*result);
             }

             return nb::none();
           })
      .def(
          "wait_for_value",
          [](GetterT& self, int timeout_ms) {
            nb::gil_scoped_release release;
            return self.wait_for_value(std::chrono::milliseconds(timeout_ms));
          },
          "timeout_ms"_a = 5000)
      .def("set_change_reporting", &GetterT::set_change_reporting, "enable"_a)
      .def("get_change_reporting", &GetterT::get_change_reporting)
      .def("set_latency_and_lost_enabled", &GetterT::set_latency_and_lost_enabled, "enable"_a)
      .def("is_latency_and_lost_enabled", &GetterT::is_latency_and_lost_enabled)
      .def("get_latency", &GetterT::get_latency)
      .def("get_lost", &GetterT::get_lost)
      .def("mark_as_subscriber", &GetterT::mark_as_subscriber)
      .def("__repr__",
           [name = std::string(name)](const GetterT& self) { return name + "(url='" + self.get_url() + "')"; });
}

void bind_communication(nb::module_& m) {
  using BytesPub = vlink::Publisher<vlink::Bytes>;
  using BytesSub = vlink::Subscriber<vlink::Bytes>;
  using BytesSrv = vlink::Server<vlink::Bytes, vlink::Bytes>;
  using BytesCli = vlink::Client<vlink::Bytes, vlink::Bytes>;
  using BytesFireSrv = vlink::Server<vlink::Bytes, vlink::Traits::EmptyType>;
  using BytesFireCli = vlink::Client<vlink::Bytes, vlink::Traits::EmptyType>;
  using BytesSet = vlink::Setter<vlink::Bytes>;
  using BytesGet = vlink::Getter<vlink::Bytes>;
  using SecBytesPub = vlink::SecurityPublisher<vlink::Bytes>;
  using SecBytesSub = vlink::SecuritySubscriber<vlink::Bytes>;
  using SecBytesSrv = vlink::SecurityServer<vlink::Bytes, vlink::Bytes>;
  using SecBytesCli = vlink::SecurityClient<vlink::Bytes, vlink::Bytes>;
  using SecBytesFireSrv = vlink::SecurityServer<vlink::Bytes, vlink::Traits::EmptyType>;
  using SecBytesFireCli = vlink::SecurityClient<vlink::Bytes, vlink::Traits::EmptyType>;
  using SecBytesSet = vlink::SecuritySetter<vlink::Bytes>;
  using SecBytesGet = vlink::SecurityGetter<vlink::Bytes>;

  bind_publisher<BytesPub, vlink::Bytes>(m, "Publisher", "Event-model publisher");
  bind_subscriber<BytesSub, vlink::Bytes>(m, "Subscriber", "Event-model subscriber");
  bind_server<BytesSrv, vlink::Bytes, vlink::Bytes>(m, "Server", "Method-model server");
  bind_client<BytesCli, vlink::Bytes, vlink::Bytes>(m, "Client", "Method-model client");
  bind_fire_forget_server<BytesFireSrv, vlink::Bytes>(m, "FireForgetServer", "Fire-and-forget method server");
  bind_fire_forget_client<BytesFireCli, vlink::Bytes>(m, "FireForgetClient", "Fire-and-forget method client");
  bind_setter<BytesSet, vlink::Bytes>(m, "Setter", "Field-model setter");
  bind_getter<BytesGet, vlink::Bytes>(m, "Getter", "Field-model getter");
  bind_publisher<SecBytesPub, vlink::Bytes, PythonCodec<vlink::Bytes>, true>(
      m, "SecurityPublisher", "Event-model publisher with payload security");
  bind_subscriber<SecBytesSub, vlink::Bytes, PythonCodec<vlink::Bytes>, true>(
      m, "SecuritySubscriber", "Event-model subscriber with payload security");
  bind_server<SecBytesSrv, vlink::Bytes, vlink::Bytes, PythonCodec<vlink::Bytes>, PythonCodec<vlink::Bytes>, true>(
      m, "SecurityServer", "Method-model server with payload security");
  bind_client<SecBytesCli, vlink::Bytes, vlink::Bytes, PythonCodec<vlink::Bytes>, PythonCodec<vlink::Bytes>, true>(
      m, "SecurityClient", "Method-model client with payload security");
  bind_fire_forget_server<SecBytesFireSrv, vlink::Bytes, PythonCodec<vlink::Bytes>, true>(
      m, "SecurityFireForgetServer", "Fire-and-forget method server with payload security");
  bind_fire_forget_client<SecBytesFireCli, vlink::Bytes, PythonCodec<vlink::Bytes>, true>(
      m, "SecurityFireForgetClient", "Fire-and-forget method client with payload security");
  bind_setter<SecBytesSet, vlink::Bytes, PythonCodec<vlink::Bytes>, true>(m, "SecuritySetter",
                                                                          "Field-model setter with payload security");
  bind_getter<SecBytesGet, vlink::Bytes, PythonCodec<vlink::Bytes>, true>(m, "SecurityGetter",
                                                                          "Field-model getter with payload security");
}

}  // namespace vlink::python
