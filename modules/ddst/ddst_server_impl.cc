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

#include "./ddst_server_impl.h"

#include <memory>
#include <utility>

#include "./base/message_loop.h"

namespace vlink {

// WriterListener
DdstServerImpl::WriterListener::WriterListener(NodeImpl* impl) : DdstWriterListener(impl) {}

// ReaderListener
DdstServerImpl::ReaderListener::ReaderListener(NodeImpl* impl) : DdstReaderListener(impl) {}

void DdstServerImpl::ReaderListener::on_subscription_matched(ddst::DataReader* reader,
                                                             const ddst::SubscriptionMatchedStatus& status) {
  auto* instance = static_cast<DdstServerImpl*>(get_impl());

  instance->read_session_count_ = status.current_count;

  DdstReaderListener::on_subscription_matched(reader, status);
}

void DdstServerImpl::ReaderListener::on_data_available(ddst::DataReader* reader) {
  auto* instance = static_cast<DdstServerImpl*>(get_impl());
  auto* message_loop = instance->get_message_loop();

  if VUNLIKELY (instance->has_suspend) {
    DdstFactory::ReadMessage msg;

    while (DdstFactory::take_data(reader, msg)) {
      if VUNLIKELY (instance->quit_flag_) {
        break;
      }
    }

    return;
  }

  if VUNLIKELY (!instance->callback_) {
    return;
  }

  if (message_loop) {
    message_loop->post_task([instance, reader]() {
      if VUNLIKELY (!instance->get_message_loop()) {
        return;
      }

      instance->process_message(reader);
    });
  } else {
    instance->process_message(reader);
  }
}

// DdstServerImpl
DdstServerImpl::DdstServerImpl(const DdstConf& conf) : conf_(conf) {}

void DdstServerImpl::process_message(ddst::DataReader* reader) {
  DdstFactory::ReadMessage msg;

  while (DdstFactory::take_data(reader, msg)) {
    if VUNLIKELY (quit_flag_) {
      break;
    }

    if VUNLIKELY (!msg.info.valid_data) {
      continue;
    }

    if (writer_) {
      Bytes resp_data;
      callback_(msg.id, msg.bytes, &resp_data);
    } else {
      callback_(msg.id, msg.bytes, nullptr);
    }
  }
}

void DdstServerImpl::init() {
  if VUNLIKELY (is_cdr_type && is_security_type) {
    VLOG_F("Cdr type does not support security.");
  }

  participant_ = DdstFactory::create_participant(kServer | kClient, conf_, get_all_properties());

  if (is_resp_type) {
    std::tie(topic_req_, topic_resp_) = DdstFactory::create_method_topic(kServer | kClient, conf_, participant_.get());

    publisher_ = DdstFactory::create_publisher(kServer, conf_, participant_.get());

    writer_listener_.emplace(this);

    writer_ =
        DdstFactory::create_datawriter(kServer, conf_, publisher_.get(), topic_resp_.get(), &writer_listener_.value());
  } else {
    topic_req_ = DdstFactory::create_topic(kServer | kClient, conf_, participant_.get());
  }

  subscriber_ = DdstFactory::create_subscriber(kServer, conf_, participant_.get());

  quit_flag_ = false;
}

void DdstServerImpl::deinit() {
  detach();

  quit_flag_ = true;

  reader_.reset();
  writer_.reset();
  reader_listener_.reset();
  writer_listener_.reset();
  subscriber_.reset();
  publisher_.reset();
  topic_resp_.reset();
  topic_req_.reset();
  participant_.reset();
}

bool DdstServerImpl::suspend() {
  has_suspend = true;

  return true;
}

bool DdstServerImpl::resume() {
  has_suspend = false;

  return true;
}

bool DdstServerImpl::is_suspend() const { return has_suspend; }

const Conf* DdstServerImpl::get_conf() const { return &conf_; }

const AbstractNode* DdstServerImpl::get_abstract_node() const { return this; }

Status::BasePtr DdstServerImpl::get_status(Status::Type type) const {
  if (Status::is_for_writer(type)) {
    if (writer_listener_) {
      return WriterListener::get_status(writer_.get(), type);
    }
    return std::make_shared<Status::Unknown>();
  }

  if VUNLIKELY (!reader_) {
    return std::make_shared<Status::Unknown>();
  }

  return ReaderListener::get_status(reader_.get(), type);
}

std::any DdstServerImpl::get_native_handle() const { return subscriber_; }

bool DdstServerImpl::has_clients() const { return read_session_count_ > 0; }

bool DdstServerImpl::listen(ReqRespCallback&& callback) {
  if VUNLIKELY (callback_) {
    return false;
  }

  callback_ = std::move(callback);

  reader_listener_.emplace(this);

  reader_ =
      DdstFactory::create_datareader(kServer, conf_, subscriber_.get(), topic_req_.get(), &reader_listener_.value());

  return true;
}

bool DdstServerImpl::reply(uint64_t req_id, const Bytes& resp_data, bool is_sync) {
  (void)is_sync;

  return DdstFactory::write_data(writer_.get(), resp_data, req_id);
}

}  // namespace vlink
