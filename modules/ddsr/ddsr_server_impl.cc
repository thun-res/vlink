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

#include "./ddsr_server_impl.hpp"

#include <memory>
#include <utility>

#include "./base/message_loop.h"

namespace vlink {

// WriterListener
DdsrServerImpl::WriterListener::WriterListener(NodeImpl* impl) : DdsrWriterListener(impl) {}

// ReaderListener
DdsrServerImpl::ReaderListener::ReaderListener(NodeImpl* impl) : DdsrReaderListener(impl) {}

void DdsrServerImpl::ReaderListener::on_subscription_matched(NodeImpl* impl,
                                                             const DDS_SubscriptionMatchedStatus& status) {
  auto* instance = static_cast<DdsrServerImpl*>(impl);

  instance->read_session_count_ = status.current_count;

  DdsrReaderListener::on_subscription_matched(impl, status);
}

void DdsrServerImpl::ReaderListener::on_data_available(NodeImpl* impl, DDS_DataReader* reader) {
  auto* instance = static_cast<DdsrServerImpl*>(impl);
  auto* message_loop = instance->get_message_loop();

  if VUNLIKELY (instance->has_suspend) {
    DdsrFactory::ReadMessage msg;

    while (DdsrFactory::take_data(reader, msg)) {
      DdsrFactory::release_data(reader, msg);

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

// DdsrServerImpl
DdsrServerImpl::DdsrServerImpl(const DdsrConf& conf) : conf_(conf) {}

void DdsrServerImpl::process_message(DDS_DataReader* reader) {
  DdsrFactory::ReadMessage msg;

  while (DdsrFactory::take_data(reader, msg)) {
    if VUNLIKELY (quit_flag_) {
      DdsrFactory::release_data(reader, msg);
      break;
    }

    if VUNLIKELY (!msg.info->valid_data) {
      DdsrFactory::release_data(reader, msg);
      continue;
    }

    if (writer_) {
      Bytes resp_data;
      callback_(msg.id, msg.bytes, &resp_data);
    } else {
      callback_(msg.id, msg.bytes, nullptr);
    }

    DdsrFactory::release_data(reader, msg);
  }
}

void DdsrServerImpl::init() {
  participant_ = DdsrFactory::create_participant(kServer | kClient, conf_, get_all_properties());

  if (is_resp_type) {
    std::tie(topic_req_, topic_resp_) = DdsrFactory::create_method_topic(kServer | kClient, conf_, participant_.get());

    publisher_ = DdsrFactory::create_publisher(kServer, conf_, participant_.get());

    writer_listener_.emplace(this);

    writer_ = DdsrFactory::create_datawriter(kServer, conf_, publisher_.get(), topic_resp_.get(),
                                             writer_listener_->get_ptr());
  } else {
    topic_req_ = DdsrFactory::create_topic(kServer | kClient, conf_, participant_.get());
  }

  subscriber_ = DdsrFactory::create_subscriber(kServer, conf_, participant_.get());

  quit_flag_ = false;
}

void DdsrServerImpl::deinit() {
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

bool DdsrServerImpl::suspend() {
  has_suspend = true;

  return true;
}

bool DdsrServerImpl::resume() {
  has_suspend = false;

  return true;
}

bool DdsrServerImpl::is_suspend() const { return has_suspend; }

const Conf* DdsrServerImpl::get_conf() const { return &conf_; }

const AbstractNode* DdsrServerImpl::get_abstract_node() const { return this; }

Status::BasePtr DdsrServerImpl::get_status(Status::Type type) const {
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

std::any DdsrServerImpl::get_native_handle() const { return subscriber_; }

bool DdsrServerImpl::has_clients() const { return read_session_count_ > 0; }

bool DdsrServerImpl::listen(ReqRespCallback&& callback) {
  if VUNLIKELY (callback_) {
    return false;
  }

  callback_ = std::move(callback);

  reader_listener_.emplace(this);

  reader_ =
      DdsrFactory::create_datareader(kServer, conf_, subscriber_.get(), topic_req_.get(), reader_listener_->get_ptr());

  return true;
}

bool DdsrServerImpl::reply(uint64_t req_id, const Bytes& resp_data, bool is_sync) {
  (void)is_sync;

  bool ret = DdsrFactory::write_data(writer_->entity, resp_data, req_id);

  return ret;
}

}  // namespace vlink
