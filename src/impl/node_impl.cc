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

#include "./impl/node_impl.h"

#include <atomic>
#include <charconv>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <utility>

#include "./base/bytes.h"
#include "./base/logger.h"
#include "./base/message_loop.h"
#include "./extension/bag_writer.h"
#include "./extension/discovery_reporter.h"
#include "./impl/client_impl.h"
#include "./impl/server_impl.h"
#include "./private/license_check.h"
#include "./version.h"

namespace vlink {

static constexpr bool kIgnoreIntraUrl{false};

// GlobalDiscoveryReporter
struct GlobalDiscoveryReporter final {
 public:
  static DiscoveryReporter* get(bool create_if_missing = true) {
    static GlobalDiscoveryReporter global;
    return global.get_instance(create_if_missing);
  }

 private:
  GlobalDiscoveryReporter() = default;

  DiscoveryReporter* get_instance(bool create_if_missing) {
    std::lock_guard lock(mtx_);

    check_disable_env();

    if (!create_if_missing || disabled_ || instance_) {
      return instance_.get();
    }

    instance_ = std::make_unique<DiscoveryReporter>();
    instance_->async_run();

    return instance_.get();
  }

  void check_disable_env() {
    if (disable_checked_) {
      return;
    }

    disable_checked_ = true;

    static std::string discovery = Utils::get_env("VLINK_DISCOVER_DISABLE");

    if (!discovery.empty()) {
      int value = 0;
      auto [p, error] = std::from_chars(discovery.data(), discovery.data() + discovery.size(), value);

      if (error == std::errc() && value == 1) {
        VLOG_I("DiscoveryReporter: Global discovery reporter is disabled.");
        disabled_ = true;
        return;
      }
    }
  }

  ~GlobalDiscoveryReporter() = default;

  std::mutex mtx_;
  bool disable_checked_{false};
  bool disabled_{false};
  std::unique_ptr<DiscoveryReporter> instance_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(GlobalDiscoveryReporter)
};

static bool should_report_discovery(const NodeImpl& node) {
  return node.is_discovery_enabled && !node.url.empty() && !node.is_security_type &&
         (!kIgnoreIntraUrl || node.transport_type != TransportType::kIntra);
}

// AbstractNode
std::any AbstractNode::get_native_handle() const { return nullptr; }

AbstractNode::AbstractNode() = default;

AbstractNode::~AbstractNode() = default;

// NodeImpl
struct NodeImplHelper final {
  std::atomic_bool is_interrupted{false};

  Conf::PropertiesMap property_map;
  std::shared_mutex mtx;
  std::shared_mutex status_mtx;
  std::mutex post_mtx;
  NodeImpl::StatusCallback status_callback;
  std::atomic<MessageLoop*> message_loop{nullptr};

  std::shared_ptr<BagWriter> data_recorder;
  std::atomic_bool data_recorder_enabled{false};
};

bool NodeImpl::suspend() {
  // has_suspend = true;

  VLOG_W("Function [suspend] is not supported.");

  return false;
}

bool NodeImpl::resume() {
  // has_suspend = false;

  VLOG_W("Function [resume] is not supported.");

  return false;
}

bool NodeImpl::is_suspend() const {
  VLOG_W("Function [is_suspend] is not supported.");

  return false;
}

void NodeImpl::interrupt() { helper_->is_interrupted.store(true, std::memory_order_release); }

bool NodeImpl::is_support_loan() const { return false; }

Bytes NodeImpl::loan(int64_t size) {
  (void)size;

  return Bytes();
}

bool NodeImpl::return_loan(const Bytes& bytes) {
  (void)bytes;

  return false;
}

const struct Conf* NodeImpl::get_conf() const { return nullptr; }

const AbstractNode* NodeImpl::get_abstract_node() const { return nullptr; }

Status::BasePtr NodeImpl::get_status(Status::Type type) const {
  (void)type;

  VLOG_W("Function [get_status] is not supported.");

  return std::make_shared<Status::Unknown>();
}

bool NodeImpl::check_version(const Version& version) {
  Version runtime_version{VLINK_VERSION_MAJOR, VLINK_VERSION_MINOR, VLINK_VERSION_PATCH};

  if VUNLIKELY (version != runtime_version) {
    static std::atomic_bool print_warn{false};

    if VUNLIKELY (!print_warn.exchange(true, std::memory_order_relaxed)) {
      VLOG_W("The version may be incompatible. [Compiled]: ", version.to_string(),
             " [Runtime]: ", runtime_version.to_string(), ".");
    }

    return false;
  }

  return true;
}

bool NodeImpl::attach(class MessageLoop* message_loop) {
  if VUNLIKELY (!message_loop) {
    return false;
  }

  MessageLoop* expected = nullptr;
  return helper_->message_loop.compare_exchange_strong(expected, message_loop, std::memory_order_release,
                                                       std::memory_order_relaxed);
}

bool NodeImpl::detach() {
  MessageLoop* message_loop = nullptr;

  {
    std::lock_guard lock(helper_->post_mtx);

    message_loop = helper_->message_loop.exchange(nullptr, std::memory_order_acq_rel);
  }

  if (!message_loop) {
    return false;
  }

  if (!message_loop->is_in_same_thread()) {
    message_loop->wait_for_idle();
  }

  return true;
}

class MessageLoop* NodeImpl::get_message_loop() const { return helper_->message_loop.load(std::memory_order_acquire); }

void NodeImpl::register_status_handler(StatusCallback&& callback) {
  if VUNLIKELY (transport_type != TransportType::kDds && transport_type != TransportType::kDdsc &&
                transport_type != TransportType::kDdsr) {
    VLOG_W("Function [register_status_handler] is not supported.");
    return;
  }

  std::lock_guard lock(helper_->status_mtx);
  helper_->status_callback = std::move(callback);
}

bool NodeImpl::has_register_status() const {
  if VUNLIKELY (transport_type != TransportType::kDds && transport_type != TransportType::kDdsc &&
                transport_type != TransportType::kDdsr) {
    VLOG_W("Function [has_register_status] is not supported.");
    return false;
  }

  std::shared_lock lock(helper_->status_mtx);

  return helper_->status_callback != nullptr;
}

void NodeImpl::call_status(Status::BasePtr ptr) {
  if VUNLIKELY (transport_type != TransportType::kDds && transport_type != TransportType::kDdsc &&
                transport_type != TransportType::kDdsr) {
    VLOG_W("Function [call_status] is not supported.");
    return;
  }

  {
    std::lock_guard post_lock(helper_->post_mtx);

    auto* message_loop = helper_->message_loop.load(std::memory_order_acquire);

    if VLIKELY (message_loop) {
      message_loop->post_task([this, ptr]() mutable {
        std::shared_lock lock(helper_->status_mtx);
        if VLIKELY (helper_->status_callback) {
          helper_->status_callback(std::move(ptr));
        }
      });
      return;
    }
  }

  std::shared_lock lock(helper_->status_mtx);

  if VLIKELY (helper_->status_callback) {
    helper_->status_callback(std::move(ptr));
  }
}

void NodeImpl::set_property(const std::string& prop, const std::string& value) {
  std::lock_guard lock(helper_->mtx);
  helper_->property_map[prop] = value;
}

std::string NodeImpl::get_property(const std::string& prop) const {
  std::shared_lock lock(helper_->mtx);

  auto iter = helper_->property_map.find(prop);

  if VLIKELY (iter != helper_->property_map.end()) {
    return iter->second;
  }

  return {};
}

Conf::PropertiesMap NodeImpl::get_all_properties() const {
  std::shared_lock lock(helper_->mtx);
  return helper_->property_map;
}

void NodeImpl::set_discovery_enabled(bool enable) { is_discovery_enabled = enable; }

bool NodeImpl::get_discovery_enabled() const { return is_discovery_enabled; }

void NodeImpl::set_record_path(const std::string& path) {
  std::shared_ptr<BagWriter> new_recorder;

  if (!path.empty()) {
    new_recorder = BagWriter::filter_get(path);
  }

  std::shared_ptr<BagWriter> old_recorder;

  {
    std::lock_guard lock(helper_->mtx);
    old_recorder = std::move(helper_->data_recorder);
    helper_->data_recorder = std::move(new_recorder);
    helper_->data_recorder_enabled.store(helper_->data_recorder != nullptr, std::memory_order_release);
  }

  old_recorder.reset();
}

bool NodeImpl::enable_security(const Security::Config& cfg) {
  auto sec_cfg = cfg;

  return enable_security(std::move(sec_cfg));
}

bool NodeImpl::enable_security(Security::Config&& cfg) {
  if VUNLIKELY (transport_type == TransportType::kIntra || (transport_type == TransportType::kDds && is_cdr_type)) {
    VLOG_W("Security::Config will ignore intra/dds(cdr) transport.");
    return false;
  }

  if VLIKELY (cfg.advanced.aad_context.empty()) {
    cfg.advanced.aad_context = url;
    cfg.advanced.aad_context += "|";
    cfg.advanced.aad_context += ser_type;
    cfg.advanced.aad_context += "|";
    cfg.advanced.aad_context += std::to_string(static_cast<uint32_t>(schema_type));
  }

  auto candidate = std::make_unique<Security>(std::move(cfg));

  if VUNLIKELY (!candidate->is_configured()) {
    VLOG_W("Security::Config has no usable slot.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return false;                                    // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  bool needs_encrypt = (impl_type == kPublisher || impl_type == kSetter);
  bool needs_decrypt = (impl_type == kSubscriber || impl_type == kGetter);

#if defined(NDEBUG) || defined(__ANDROID__)
  if (impl_type == kClient) {
    needs_encrypt = true;
    const auto* client_impl = static_cast<const ClientImpl*>(this);
    needs_decrypt = client_impl != nullptr && client_impl->is_resp_type;
  } else if (impl_type == kServer) {
    needs_decrypt = true;
    const auto* server_impl = static_cast<const ServerImpl*>(this);
    needs_encrypt = server_impl != nullptr && server_impl->is_resp_type;
  }
#else
  if (impl_type == kClient) {
    needs_encrypt = true;
    const auto* client_impl = dynamic_cast<const ClientImpl*>(this);
    needs_decrypt = client_impl != nullptr && client_impl->is_resp_type;
  } else if (impl_type == kServer) {
    needs_decrypt = true;
    const auto* server_impl = dynamic_cast<const ServerImpl*>(this);
    needs_encrypt = server_impl != nullptr && server_impl->is_resp_type;
  }
#endif

  if VUNLIKELY (needs_encrypt && !candidate->can_encrypt()) {
    VLOG_W("Security::Config cannot encrypt for this sender role.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return false;                                                     // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if VUNLIKELY (needs_decrypt && !candidate->can_decrypt()) {
    VLOG_W("Security::Config cannot decrypt for this receiver role.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    return false;                                                       // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  security = std::move(candidate);

  return true;
}

void NodeImpl::set_ssl_options(const SslOptions& options) {
  std::lock_guard lock(helper_->mtx);
  options.parse_to(helper_->property_map);
}

void NodeImpl::try_record(ActionType action_type, const Bytes& data) {
  auto* global_recorder = BagWriter::global_get();
  const bool data_recorder_enabled = helper_->data_recorder_enabled.load(std::memory_order_acquire);

  if VLIKELY (!global_recorder && !data_recorder_enabled) {
    return;
  }

  std::shared_ptr<BagWriter> data_recorder;

  if (data_recorder_enabled) {
    std::shared_lock lock(helper_->mtx);
    data_recorder = helper_->data_recorder;
  }

  if VLIKELY (!global_recorder && !data_recorder) {
    return;
  }

  if constexpr (kIgnoreIntraUrl) {
    if (transport_type == TransportType::kIntra) {
      return;
    }
  }

  Frame frame;
  frame.timestamp = -1;
  frame.url = url;
  frame.ser_type = ser_type;
  frame.schema_type = schema_type;
  frame.action_type = action_type;
  frame.data = Bytes::shallow_copy(data.data(), data.size());

  if VUNLIKELY (global_recorder) {
    global_recorder->push(frame);
  }

  if VUNLIKELY (data_recorder) {
    data_recorder->push(frame);
  }
}

void NodeImpl::reset_interrupted() { helper_->is_interrupted.store(false, std::memory_order_release); }

bool NodeImpl::is_interrupted() const { return helper_->is_interrupted.load(std::memory_order_acquire); }

void NodeImpl::init_ext() {
  if (should_report_discovery(*this)) {
    auto* global_reporter = GlobalDiscoveryReporter::get();

    if (global_reporter) {
      if (CpuProfiler::is_global_enabled() && !profiler) {
        profiler = std::make_unique<CpuProfiler>();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      global_reporter->add(this);
    }
  }

#ifdef VLINK_ENABLE_CHECK_LICENSE
  [[maybe_unused]] static LicenseCheck license;
#endif
}

void NodeImpl::deinit_ext() {
  if (!should_report_discovery(*this)) {
    return;
  }

  auto* global_reporter = GlobalDiscoveryReporter::get(false);

  if (global_reporter) {
    global_reporter->remove(this);

    if (CpuProfiler::is_global_enabled() && profiler) {
      profiler->restart();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
  }
}

void NodeImpl::global_init() {
  Logger::get();

  Bytes::init_memory_pool();

  BagWriter::global_get();

  GlobalDiscoveryReporter::get(false);
}

NodeImpl::NodeImpl(ImplType type) : impl_type(type), helper_(std::make_unique<NodeImplHelper>()) { global_init(); }

NodeImpl::~NodeImpl() = default;

}  // namespace vlink
