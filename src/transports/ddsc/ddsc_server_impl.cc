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

  instance->read_session_count_.store(status.current_count, std::memory_order_relaxed);

  DdscReaderListener::on_subscription_matched(impl, status);
}

void DdscServerImpl::ReaderListener::on_data_available(NodeImpl* impl, dds_entity_t reader) {
  auto* instance = static_cast<DdscServerImpl*>(impl);
  auto* message_loop = instance->get_message_loop();

  if VUNLIKELY (instance->has_suspend.load(std::memory_order_acquire)) {
    DdscFactory::ReadMessage msg;

    while (DdscFactory::take_data(reader, msg)) {
      DdscFactory::release_data(reader, msg);

      if VUNLIKELY (instance->quit_flag_.load(std::memory_order_acquire)) {
        break;
      }
    }

    return;
  }

  if VUNLIKELY (!instance->callback_) {
    return;
  }

  if (message_loop) {
    instance->post_to_message_loop([instance, reader]() {
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
DdscServerImpl::DdscServerImpl(const DdscTransport& transport) : transport_(transport) {}

void DdscServerImpl::process_message(dds_entity_t reader) {
  if VUNLIKELY (quit_flag_.load(std::memory_order_acquire)) {
    return;
  }

  DdscFactory::ReadMessage msg;

  while (DdscFactory::take_data(reader, msg)) {
    if VUNLIKELY (quit_flag_.load(std::memory_order_acquire)) {
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
  participant_ = DdscFactory::create_participant(kServer | kClient, transport_, get_all_properties());

  if VUNLIKELY (!participant_) {
    VLOG_F("Create participant failed.");
  }

  if (is_resp_type) {
    std::tie(topic_req_, topic_resp_) =
        DdscFactory::create_method_topic(kServer | kClient, transport_, participant_.get());

    if VUNLIKELY (!topic_req_ || !topic_resp_) {
      VLOG_F("Create method topic failed: ", transport_.topic, ".");
    }

    publisher_ = DdscFactory::create_publisher(kServer, transport_, participant_.get());

    if VUNLIKELY (!publisher_) {
      VLOG_F("Create publisher failed.");
    }

    writer_listener_.emplace(this);

    writer_ = DdscFactory::create_datawriter(kServer, transport_, publisher_.get(), topic_resp_.get(),
                                             writer_listener_->get_ptr());

    if VUNLIKELY (!writer_) {
      VLOG_F("Create datawriter failed.");
    }
  } else {
    topic_req_ = DdscFactory::create_topic(kServer | kClient, transport_, participant_.get());

    if VUNLIKELY (!topic_req_) {
      VLOG_F("Create topic failed: ", transport_.topic, ".");
    }
  }

  subscriber_ = DdscFactory::create_subscriber(kServer, transport_, participant_.get());

  if VUNLIKELY (!subscriber_) {
    VLOG_F("Create subscriber failed.");
  }

  quit_flag_.store(false, std::memory_order_release);
}

void DdscServerImpl::deinit() {
  quit_flag_.store(true, std::memory_order_release);

  if (reader_) {
    dds_set_listener(reader_->entity, nullptr);
  }

  if (writer_) {
    dds_set_listener(writer_->entity, nullptr);
  }

  detach();

  reader_.reset();
  writer_.reset();

  reader_listener_.reset();
  writer_listener_.reset();
  subscriber_.reset();
  publisher_.reset();
  topic_resp_.reset();
  topic_req_.reset();
  participant_.reset();
  callback_ = {};
  is_listened.store(false, std::memory_order_release);
  read_session_count_.store(0, std::memory_order_relaxed);
}

bool DdscServerImpl::suspend() {
  has_suspend.store(true, std::memory_order_release);

  return true;
}

bool DdscServerImpl::resume() {
  has_suspend.store(false, std::memory_order_release);

  return true;
}

bool DdscServerImpl::is_suspend() const { return has_suspend.load(std::memory_order_acquire); }

const Transport* DdscServerImpl::get_transport() const { return &transport_; }

const AbstractNode* DdscServerImpl::get_abstract_node() const { return this; }

Status::BasePtr DdscServerImpl::get_status(Status::Type type) const {
  if (Status::is_for_writer(type)) {
    if (writer_) {
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

bool DdscServerImpl::has_clients() const { return read_session_count_.load(std::memory_order_relaxed) > 0; }

bool DdscServerImpl::listen(ReqRespCallback&& callback) {
  if VUNLIKELY (callback_ || (is_resp_type && !writer_)) {
    return false;
  }

  callback_ = std::move(callback);

  reader_listener_.emplace(this);

  reader_ = DdscFactory::create_datareader(kServer, transport_, subscriber_.get(), topic_req_.get(),
                                           reader_listener_->get_ptr());

  if VUNLIKELY (!reader_) {
    reader_listener_.reset();
    callback_ = {};
    return false;
  }

  return true;
}

bool DdscServerImpl::reply(uint64_t req_id, const Bytes& resp_data, bool is_sync) {
  if VUNLIKELY (!writer_) {
    return false;
  }

  (void)is_sync;

  bool ret = DdscFactory::write_data(writer_->entity, resp_data, req_id);

  return ret;
}

}  // namespace vlink
