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

#include "./ddsc_client_impl.hpp"

#include <memory>
#include <utility>

#include "./base/elapsed_timer.h"
#include "./base/message_loop.h"

namespace vlink {

// WriterListener
DdscClientImpl::WriterListener::WriterListener(NodeImpl* impl) : DdscWriterListener(impl) {}

void DdscClientImpl::WriterListener::on_publication_matched(NodeImpl* impl,
                                                            const dds_publication_matched_status_t& status) {
  auto* instance = static_cast<DdscClientImpl*>(impl);
  auto* message_loop = instance->get_message_loop();

  instance->write_session_count_.store(status.current_count, std::memory_order_release);

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

  DdscWriterListener::on_publication_matched(impl, status);
}

// ReaderListener
DdscClientImpl::ReaderListener::ReaderListener(NodeImpl* impl) : DdscReaderListener(impl) {}

void DdscClientImpl::ReaderListener::on_subscription_matched(NodeImpl* impl,
                                                             const dds_subscription_matched_status_t& status) {
  auto* instance = static_cast<DdscClientImpl*>(impl);
  auto* message_loop = instance->get_message_loop();

  instance->read_session_count_.store(status.current_count, std::memory_order_release);

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

  DdscReaderListener::on_subscription_matched(impl, status);
}

void DdscClientImpl::ReaderListener::on_data_available(NodeImpl* impl, dds_entity_t reader) {
  auto* instance = static_cast<DdscClientImpl*>(impl);
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

// DdscClientImpl
DdscClientImpl::DdscClientImpl(const DdscConf& conf) : conf_(conf) {}

void DdscClientImpl::process_message(dds_entity_t reader) {
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

    NodeImpl::MsgCallback cb;
    {
      std::lock_guard param_lock(param_mtx_);
      auto iter = callbacks_.find(msg.id);

      if VUNLIKELY (iter == callbacks_.end()) {
        DdscFactory::release_data(reader, msg);
        continue;
      }

      cb = std::move(iter->second);
      callbacks_.erase(iter);
    }

    if VLIKELY (cb) {
      cb(msg.bytes);
    }

    DdscFactory::release_data(reader, msg);
  }
}

void DdscClientImpl::init() {
  participant_ = DdscFactory::create_participant(kServer | kClient, conf_, get_all_properties());

  if VUNLIKELY (!participant_) {
    VLOG_E("DdscClientImpl::init(): participant creation failed; client left uninitialised.");

    return;
  }

  if (is_resp_type) {
    std::tie(topic_req_, topic_resp_) = DdscFactory::create_method_topic(kServer | kClient, conf_, participant_.get());

    publisher_ = DdscFactory::create_publisher(kClient, conf_, participant_.get());

    writer_listener_.emplace(this);

    writer_ =
        DdscFactory::create_datawriter(kClient, conf_, publisher_.get(), topic_req_.get(), writer_listener_->get_ptr());

    subscriber_ = DdscFactory::create_subscriber(kClient, conf_, participant_.get());

    reader_listener_.emplace(this);

    reader_ = DdscFactory::create_datareader(kClient, conf_, subscriber_.get(), topic_resp_.get(),
                                             reader_listener_->get_ptr());
  } else {
    topic_req_ = DdscFactory::create_topic(kServer | kClient, conf_, participant_.get());

    publisher_ = DdscFactory::create_publisher(kClient, conf_, participant_.get());

    writer_listener_.emplace(this);

    writer_ =
        DdscFactory::create_datawriter(kClient, conf_, publisher_.get(), topic_req_.get(), writer_listener_->get_ptr());
  }

  quit_flag_.store(false, std::memory_order_release);
}

void DdscClientImpl::deinit() {
  quit_flag_.store(true, std::memory_order_release);

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
  std::lock_guard lock(param_mtx_);
  callbacks_.clear();
  write_session_count_.store(0, std::memory_order_release);
  read_session_count_.store(0, std::memory_order_release);
}

void DdscClientImpl::interrupt() {
  ClientImpl::interrupt();

  ack_manager_.clear();
}

const Conf* DdscClientImpl::get_conf() const { return &conf_; }

const AbstractNode* DdscClientImpl::get_abstract_node() const { return this; }

Status::BasePtr DdscClientImpl::get_status(Status::Type type) const {
  if (Status::is_for_writer(type)) {
    if VUNLIKELY (!writer_) {
      return std::make_shared<Status::Unknown>();
    }

    return WriterListener::get_status(writer_->entity, type);
  }

  if VUNLIKELY (!reader_) {
    return std::make_shared<Status::Unknown>();
  }

  if (reader_listener_) {
    return ReaderListener::get_status(reader_->entity, type);
  }

  return std::make_shared<Status::Unknown>();
}

std::any DdscClientImpl::get_native_handle() const { return publisher_; }

bool DdscClientImpl::is_connected() const {
  if (is_resp_type) {
    return write_session_count_.load(std::memory_order_acquire) > 0 &&
           read_session_count_.load(std::memory_order_acquire) > 0;
  } else {
    return write_session_count_.load(std::memory_order_acquire) > 0;
  }
}

bool DdscClientImpl::call(const Bytes& req_data, MsgCallback&& callback, std::chrono::milliseconds timeout) {
  if (!callback) {
    return DdscFactory::write_data(writer_->entity, req_data, 0);
  }

  dds_guid_t guid;
  dds_get_guid(writer_->entity, &guid);
  uint64_t id = DdscFactory::get_guid(&guid, seq_.fetch_add(1, std::memory_order_relaxed) + 1);

  auto cleanup_callback = [this, &id]() {
    std::lock_guard param_lock(param_mtx_);
    callbacks_.erase(id);
  };

  if (timeout.count() != 0) {
    ack_manager_.reset_interrupted();

    auto ack_request = ack_manager_.create_request();

    {
      std::lock_guard param_lock(param_mtx_);
      callbacks_[id] = [ack_request, callback = std::move(callback)](const Bytes& resp_data) {
        AckManager::notify(ack_request, [&callback, &resp_data]() { callback(resp_data); });
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
      return DdscFactory::write_data(writer_->entity, req_data, id);
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

  bool result = DdscFactory::write_data(writer_->entity, req_data, id);

  if VUNLIKELY (!result) {
    cleanup_callback();
  }

  return result;
}

}  // namespace vlink
