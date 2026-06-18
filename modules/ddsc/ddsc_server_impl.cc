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

#include "./ddsc_server_impl.hpp"

#include <memory>
#include <utility>

#include "./base/message_loop.h"

namespace vlink {

// WriterListener
DdscServerImpl::WriterListener::WriterListener(NodeImpl* impl) : DdscWriterListener(impl) {}

// ReaderListener
DdscServerImpl::ReaderListener::ReaderListener(NodeImpl* impl) : DdscReaderListener(impl) {}

void DdscServerImpl::ReaderListener::on_subscription_matched(NodeImpl* impl,
                                                             const dds_subscription_matched_status_t& status) {
  auto* instance = static_cast<DdscServerImpl*>(impl);

  instance->read_session_count_ = status.current_count;

  DdscReaderListener::on_subscription_matched(impl, status);
}

void DdscServerImpl::ReaderListener::on_data_available(NodeImpl* impl, dds_entity_t reader) {
  auto* instance = static_cast<DdscServerImpl*>(impl);
  auto* message_loop = instance->get_message_loop();

  if VUNLIKELY (instance->has_suspend) {
    DdscFactory::ReadMessage msg;

    while (DdscFactory::take_data(reader, msg)) {
      DdscFactory::release_data(reader, msg);

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

// DdscServerImpl
DdscServerImpl::DdscServerImpl(const DdscConf& conf) : conf_(conf) {}

void DdscServerImpl::process_message(dds_entity_t reader) {
  DdscFactory::ReadMessage msg;

  while (DdscFactory::take_data(reader, msg)) {
    if VUNLIKELY (quit_flag_) {
      DdscFactory::release_data(reader, msg);
      break;
    }

    if VUNLIKELY (!msg.info.valid_data) {
      DdscFactory::release_data(reader, msg);
      continue;
    }

    if (writer_) {
      Bytes resp_data;
      callback_(msg.id, msg.bytes, &resp_data);
    } else {
      callback_(msg.id, msg.bytes, nullptr);
    }

    DdscFactory::release_data(reader, msg);
  }
}

void DdscServerImpl::init() {
  participant_ = DdscFactory::create_participant(kServer | kClient, conf_, get_all_properties());

  if (is_resp_type) {
    std::tie(topic_req_, topic_resp_) = DdscFactory::create_method_topic(kServer | kClient, conf_, participant_.get());

    publisher_ = DdscFactory::create_publisher(kServer, conf_, participant_.get());

    writer_listener_.emplace(this);

    writer_ = DdscFactory::create_datawriter(kServer, conf_, publisher_.get(), topic_resp_.get(),
                                             writer_listener_->get_ptr());
  } else {
    topic_req_ = DdscFactory::create_topic(kServer | kClient, conf_, participant_.get());
  }

  subscriber_ = DdscFactory::create_subscriber(kServer, conf_, participant_.get());

  quit_flag_ = false;
}

void DdscServerImpl::deinit() {
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

bool DdscServerImpl::suspend() {
  has_suspend = true;

  return true;
}

bool DdscServerImpl::resume() {
  has_suspend = false;

  return true;
}

bool DdscServerImpl::is_suspend() const { return has_suspend; }

const Conf* DdscServerImpl::get_conf() const { return &conf_; }

const AbstractNode* DdscServerImpl::get_abstract_node() const { return this; }

Status::BasePtr DdscServerImpl::get_status(Status::Type type) const {
  if (Status::is_for_writer(type)) {
    if (writer_listener_) {
      return WriterListener::get_status(writer_->entity, type);
    }

    return std::make_shared<Status::Unknown>();
  }

  if VUNLIKELY (!reader_) {
    return std::make_shared<Status::Unknown>();
  }

  return ReaderListener::get_status(reader_->entity, type);
}

std::any DdscServerImpl::get_native_handle() const { return subscriber_; }

bool DdscServerImpl::has_clients() const { return read_session_count_ > 0; }

bool DdscServerImpl::listen(ReqRespCallback&& callback) {
  if VUNLIKELY (callback_) {
    return false;
  }

  callback_ = std::move(callback);

  reader_listener_.emplace(this);

  reader_ =
      DdscFactory::create_datareader(kServer, conf_, subscriber_.get(), topic_req_.get(), reader_listener_->get_ptr());

  return true;
}

bool DdscServerImpl::reply(uint64_t req_id, const Bytes& resp_data, bool is_sync) {
  (void)is_sync;

  bool ret = DdscFactory::write_data(writer_->entity, resp_data, req_id);

  return ret;
}

}  // namespace vlink
