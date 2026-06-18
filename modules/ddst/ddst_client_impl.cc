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

#include "./ddst_client_impl.h"

#include <memory>
#include <utility>

#include "./base/elapsed_timer.h"
#include "./base/message_loop.h"

namespace vlink {

// WriterListener
DdstClientImpl::WriterListener::WriterListener(NodeImpl* impl) : DdstWriterListener(impl) {}

void DdstClientImpl::WriterListener::on_publication_matched(ddst::DataWriter* writer,
                                                            const ddst::PublicationMatchedStatus& status) {
  auto* instance = static_cast<DdstClientImpl*>(get_impl());
  auto* message_loop = instance->get_message_loop();

  instance->write_session_count_ = status.current_count;

  // std::this_thread::sleep_for(std::chrono::microseconds(1));  //?

  if (message_loop) {
    message_loop->post_task([instance]() {
      if VUNLIKELY (!instance->get_message_loop()) {
        return;
      }

      instance->update_connected();
    });
  } else {
    instance->update_connected();
  }

  DdstWriterListener::on_publication_matched(writer, status);
}

// ReaderListener
DdstClientImpl::ReaderListener::ReaderListener(NodeImpl* impl) : DdstReaderListener(impl) {}

void DdstClientImpl::ReaderListener::on_subscription_matched(ddst::DataReader* reader,
                                                             const ddst::SubscriptionMatchedStatus& status) {
  auto* instance = static_cast<DdstClientImpl*>(get_impl());
  auto* message_loop = instance->get_message_loop();

  instance->read_session_count_ = status.current_count;

  // std::this_thread::sleep_for(std::chrono::microseconds(1));  //?

  if (message_loop) {
    message_loop->post_task([instance]() {
      if VUNLIKELY (!instance->get_message_loop()) {
        return;
      }

      instance->update_connected();
    });
  } else {
    instance->update_connected();
  }

  DdstReaderListener::on_subscription_matched(reader, status);
}

void DdstClientImpl::ReaderListener::on_data_available(ddst::DataReader* reader) {
  auto* instance = static_cast<DdstClientImpl*>(get_impl());
  auto* message_loop = instance->get_message_loop();

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

// DdstClientImpl
DdstClientImpl::DdstClientImpl(const DdstConf& conf) : conf_(conf) {}

void DdstClientImpl::process_message(ddst::DataReader* reader) {
  DdstFactory::ReadMessage msg;

  while (DdstFactory::take_data(reader, msg)) {
    if VUNLIKELY (quit_flag_) {
      break;
    }

    if VUNLIKELY (!msg.info.valid_data) {
      continue;
    }

    NodeImpl::MsgCallback cb;
    {
      std::lock_guard param_lock(param_mtx_);
      auto iter = callbacks_.find(msg.id);

      if VUNLIKELY (iter == callbacks_.end()) {
        continue;
      }

      cb = std::move(iter->second);
      callbacks_.erase(iter);
    }

    if VLIKELY (cb) {
      cb(msg.bytes);
    }
  }
}

void DdstClientImpl::init() {
  if VUNLIKELY (is_cdr_type && is_security_type) {
    VLOG_F("Cdr type does not support security.");
  }

  participant_ = DdstFactory::create_participant(kServer | kClient, conf_, get_all_properties());

  if (is_resp_type) {
    std::tie(topic_req_, topic_resp_) = DdstFactory::create_method_topic(kServer | kClient, conf_, participant_.get());

    publisher_ = DdstFactory::create_publisher(kClient, conf_, participant_.get());

    writer_listener_.emplace(this);

    writer_ =
        DdstFactory::create_datawriter(kClient, conf_, publisher_.get(), topic_req_.get(), &writer_listener_.value());

    subscriber_ = DdstFactory::create_subscriber(kClient, conf_, participant_.get());

    reader_listener_.emplace(this);

    reader_ =
        DdstFactory::create_datareader(kClient, conf_, subscriber_.get(), topic_resp_.get(), &reader_listener_.value());
  } else {
    topic_req_ = DdstFactory::create_topic(kServer | kClient, conf_, participant_.get());

    publisher_ = DdstFactory::create_publisher(kClient, conf_, participant_.get());

    writer_listener_.emplace(this);

    writer_ =
        DdstFactory::create_datawriter(kClient, conf_, publisher_.get(), topic_req_.get(), &writer_listener_.value());
  }

  quit_flag_ = false;
}

void DdstClientImpl::deinit() {
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

void DdstClientImpl::interrupt() {
  ClientImpl::interrupt();
  ack_manager_.clear();
}

const Conf* DdstClientImpl::get_conf() const { return &conf_; }

const AbstractNode* DdstClientImpl::get_abstract_node() const { return this; }

Status::BasePtr DdstClientImpl::get_status(Status::Type type) const {
  if (Status::is_for_writer(type)) {
    if VUNLIKELY (!writer_) {
      return std::make_shared<Status::Unknown>();
    }

    return WriterListener::get_status(writer_.get(), type);
  }

  if VUNLIKELY (!reader_) {
    return std::make_shared<Status::Unknown>();
  }

  if (reader_listener_) {
    return ReaderListener::get_status(reader_.get(), type);
  }

  return std::make_shared<Status::Unknown>();
}

std::any DdstClientImpl::get_native_handle() const { return publisher_; }

bool DdstClientImpl::is_connected() const {
  if (is_resp_type) {
    return write_session_count_ > 0 && read_session_count_ > 0;
  } else {
    return write_session_count_ > 0;
  }
}

bool DdstClientImpl::call(const Bytes& req_data, MsgCallback&& callback, std::chrono::milliseconds timeout) {
  if (!callback) {
    return DdstFactory::write_data(writer_.get(), req_data, 0);
  }

  uint64_t id = DdstFactory::get_guid(writer_->get_guid(), ++seq_);

  auto cleanup_callback = [this, &id]() {
    std::lock_guard param_lock(param_mtx_);
    callbacks_.erase(id);
  };

  if (timeout.count() != 0) {
    ack_manager_.reset_interrupted();

    auto ack_request = ack_manager_.create_request();

    {
      std::lock_guard param_lock(param_mtx_);
      callbacks_[id] = [this, ack_request, callback = std::move(callback)](const Bytes& resp_data) {
        ack_manager_.notify(ack_request, [&callback, &resp_data]() { callback(resp_data); });
      };
    }

    ElapsedTimer timer;
    timer.start();

    if VUNLIKELY (!wait_for_connected(timeout)) {
      cleanup_callback();
      return false;
    }

    auto elapsed = timer.get();

    if VUNLIKELY (timeout.count() > 0 && elapsed >= timeout.count()) {
      cleanup_callback();
      return false;
    }

    bool result = ack_manager_.process(ack_request, timeout.count() - elapsed, [this, &req_data, &id]() {
      return DdstFactory::write_data(writer_.get(), req_data, id);
    });

    if VUNLIKELY (!result) {
      cleanup_callback();
    }

    return result;
  }

  {
    std::lock_guard param_lock(param_mtx_);
    callbacks_[id] = [callback = std::move(callback)](const Bytes& resp_data) { callback(resp_data); };
  }

  bool result = DdstFactory::write_data(writer_.get(), req_data, id);

  if VUNLIKELY (!result) {
    cleanup_callback();
  }

  return result;
}

}  // namespace vlink
