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

#include "./qnx_client_impl.h"

#include <utility>

#include "./base/elapsed_timer.h"
#include "./base/helpers.h"

namespace vlink {

// QnxClientImpl
QnxClientImpl::QnxClientImpl(const QnxConf& conf) : conf_(conf) {}

void QnxClientImpl::init() {
  static auto& factory = QnxFactory::get();

  conf_.hash_code = Helpers::get_hash_code(conf_.event);

  object_ = factory.get_object<Object>({kImplType, conf_.address});

  object_->add_impl(this);

  object_->start_timer();

  object_->register_server_connect_callback(this, [this](bool) { ClientImpl::update_connected(); });

  ClientImpl::update_connected();
}

void QnxClientImpl::deinit() { object_->remove_impl(this); }

void QnxClientImpl::interrupt() {
  ClientImpl::interrupt();

  ack_manager_.clear();
}

const Conf* QnxClientImpl::get_conf() const { return &conf_; }

const AbstractNode* QnxClientImpl::get_abstract_node() const { return object_.get(); }

bool QnxClientImpl::attach(class MessageLoop*) {
  VLOG_W("Function [attach] is not supported.");
  return false;
}

bool QnxClientImpl::detach() {
  VLOG_W("Function [detach] is not supported.");
  return false;
}

bool QnxClientImpl::is_connected() const { return object_->is_connected(); }

bool QnxClientImpl::call(const Bytes& req_data, MsgCallback&& callback, std::chrono::milliseconds timeout) {
  if VUNLIKELY (!callback) {
    return object_->call(conf_.hash_code, req_data);
  }

  if (timeout.count() != 0) {
    ack_manager_.reset_interrupted();

    ElapsedTimer timer;
    timer.start();

    if (!wait_for_connected(timeout)) {
      return false;
    }

    auto elapsed = timer.get();

    if (timeout.count() > 0 && elapsed >= timeout.count()) {
      return false;
    }

    auto ack_request = ack_manager_.create_request();

    auto ack_function = [this, ack_request, callback = std::move(callback)](const Bytes& resp_data) mutable {
      ack_manager_.notify(ack_request, [&callback, &resp_data]() { callback(resp_data); });
    };

    return ack_manager_.process(ack_request, timeout.count() - elapsed,
                                [this, &req_data, ack_function = std::move(ack_function)]() mutable {
                                  return object_->call(conf_.hash_code, req_data, std::move(ack_function));
                                });
  }

  return object_->call(conf_.hash_code, req_data, std::move(callback));
}

}  // namespace vlink
