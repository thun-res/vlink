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

#include "./dds_client_impl.h"

#include <memory>
#include <utility>

#include "./base/elapsed_timer.h"
#include "./base/message_loop.h"

namespace vlink {

// WriterListener
DdsClientImpl::WriterListener::WriterListener(NodeImpl* impl) : DdsWriterListener(impl) {}

void DdsClientImpl::WriterListener::on_publication_matched(dds::DataWriter* writer,
                                                           const dds::PublicationMatchedStatus& status) {
  auto* instance = static_cast<DdsClientImpl*>(get_impl());
  auto* message_loop = instance->get_message_loop();

  instance->write_session_count_.store(status.current_count, std::memory_order_release);

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

  DdsWriterListener::on_publication_matched(writer, status);
}

// ReaderListener
DdsClientImpl::ReaderListener::ReaderListener(NodeImpl* impl) : DdsReaderListener(impl) {
  write_params_.sample_identity().sequence_number() = rtps::SequenceNumber_t(0, 0);
}

void DdsClientImpl::ReaderListener::on_subscription_matched(dds::DataReader* reader,
                                                            const dds::SubscriptionMatchedStatus& status) {
  auto* instance = static_cast<DdsClientImpl*>(get_impl());
  auto* message_loop = instance->get_message_loop();

  instance->read_session_count_.store(status.current_count, std::memory_order_release);

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

  DdsReaderListener::on_subscription_matched(reader, status);
}

void DdsClientImpl::ReaderListener::on_data_available(dds::DataReader* reader) {
  auto* instance = static_cast<DdsClientImpl*>(get_impl());
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

// DdsClientImpl
DdsClientImpl::DdsClientImpl(const DdsConf& conf) : conf_(conf) {}

void DdsClientImpl::process_message(dds::DataReader* reader) {
  if (is_cdr_type) {
    DdsFactory::ReadCdrMessage msg;

    while (DdsFactory::take_cdr_data(reader, msg)) {
      if VUNLIKELY (quit_flag_.load(std::memory_order_acquire)) {
        DdsFactory::return_cdr_loan(reader, msg);
        break;
      }

      const auto& info = msg.infos[0];

      if VUNLIKELY (!info.valid_data) {
        DdsFactory::return_cdr_loan(reader, msg);
        continue;
      }

      NodeImpl::MsgCallback cb;
      {
        std::lock_guard param_lock(param_mtx_);
        auto iter = cdr_callbacks_.find(info.related_sample_identity);

        if VLIKELY (iter != cdr_callbacks_.end()) {
          cb = std::move(iter->second);
          cdr_callbacks_.erase(iter);
        }
      }

      if VLIKELY (cb) {
        cb(msg.samples[0]);
      }

      DdsFactory::return_cdr_loan(reader, msg);
    }
  } else {
    DdsFactory::ReadMessage msg;

    while (DdsFactory::take_data(reader, msg)) {
      if VUNLIKELY (quit_flag_.load(std::memory_order_acquire)) {
        DdsFactory::return_data_loan(reader, msg);
        break;
      }

      const auto& info = msg.infos[0];

      if VUNLIKELY (!info.valid_data) {
        DdsFactory::return_data_loan(reader, msg);
        continue;
      }

      NodeImpl::MsgCallback cb;
      {
        std::lock_guard param_lock(param_mtx_);
        auto iter = callbacks_.find(msg.id);

        if VLIKELY (iter != callbacks_.end()) {
          cb = std::move(iter->second);
          callbacks_.erase(iter);
        }
      }

      if VLIKELY (cb) {
        cb(msg.samples[0].data());
      }

      DdsFactory::return_data_loan(reader, msg);
    }
  }
}

void DdsClientImpl::init() {
  if VUNLIKELY (is_resp_type && is_cdr_type != is_resp_cdr_type) {
    VLOG_F("DdsClient: Request and response must both use raw or CDR serialization.");
  }

  if VUNLIKELY (is_cdr_type && is_security_type) {
    VLOG_F("Cdr type does not support security.");
  }

  participant_ = DdsFactory::create_participant(kServer | kClient, conf_, get_all_properties());

  if VUNLIKELY (!participant_) {
    VLOG_E("DdsClientImpl::init(): participant creation failed; client left uninitialised.");

    return;
  }

  if (is_resp_type) {
    std::tie(topic_req_, topic_resp_) =
        DdsFactory::create_method_topic(kServer | kClient, conf_, participant_.get(), is_cdr_type, ser_type);

    if VUNLIKELY (!topic_req_ || !topic_resp_) {
      VLOG_E("DdsClientImpl::init(): method topic creation failed; client left uninitialised.");
      return;
    }

    if (is_cdr_type) {
      ser_type = topic_req_->get_type_name() + "|" + topic_resp_->get_type_name();
    }

    publisher_ = DdsFactory::create_publisher(kClient, conf_, participant_.get());

    writer_listener_.emplace(this);

    writer_ = DdsFactory::create_datawriter(kClient, conf_, publisher_.get(), topic_req_.get(),
                                            &writer_listener_.value(), is_cdr_type);

    subscriber_ = DdsFactory::create_subscriber(kClient, conf_, participant_.get());

    reader_listener_.emplace(this);

    reader_ = DdsFactory::create_datareader(kClient, conf_, subscriber_.get(), topic_resp_.get(),
                                            &reader_listener_.value(), is_cdr_type);
  } else {
    topic_req_ = DdsFactory::create_topic(kServer | kClient, conf_, participant_.get(), is_cdr_type, {}, ser_type);

    if VUNLIKELY (!topic_req_) {
      VLOG_E("DdsClientImpl::init(): request topic creation failed; client left uninitialised.");
      return;
    }

    if (is_cdr_type) {
      ser_type = topic_req_->get_type_name();
    }

    publisher_ = DdsFactory::create_publisher(kClient, conf_, participant_.get());

    writer_listener_.emplace(this);

    writer_ = DdsFactory::create_datawriter(kClient, conf_, publisher_.get(), topic_req_.get(),
                                            &writer_listener_.value(), is_cdr_type);
  }

  quit_flag_.store(false, std::memory_order_release);
}

void DdsClientImpl::deinit() {
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
  cdr_callbacks_.clear();
  write_session_count_.store(0, std::memory_order_release);
  read_session_count_.store(0, std::memory_order_release);
}

void DdsClientImpl::interrupt() {
  ClientImpl::interrupt();
  ack_manager_.clear();
}

const Conf* DdsClientImpl::get_conf() const { return &conf_; }

const AbstractNode* DdsClientImpl::get_abstract_node() const { return this; }

Status::BasePtr DdsClientImpl::get_status(Status::Type type) const {
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

std::any DdsClientImpl::get_native_handle() const { return publisher_; }

bool DdsClientImpl::is_connected() const {
  if (is_resp_type) {
    return write_session_count_.load(std::memory_order_acquire) > 0 &&
           read_session_count_.load(std::memory_order_acquire) > 0;
  } else {
    return write_session_count_.load(std::memory_order_acquire) > 0;
  }
}

bool DdsClientImpl::call(const Bytes& req_data, MsgCallback&& callback, std::chrono::milliseconds timeout) {
  if (!callback) {
    if (is_cdr_type) {
      return DdsFactory::write_cdr_data(writer_.get(), req_data);
    } else {
      return DdsFactory::write_data(writer_.get(), req_data, 0);
    }
  }

  uint64_t id = DdsFactory::get_guid(writer_->guid(), seq_.fetch_add(1, std::memory_order_relaxed) + 1);

  rtps::WriteParams write_params;
  rtps::SampleIdentity sample_identity;

  if (is_cdr_type) {
    std::lock_guard param_lock(param_mtx_);
    write_params = reader_listener_->write_params_;

    auto& write_identity = write_params.sample_identity();
    write_identity.writer_guid() = writer_->guid();
    ++write_identity.sequence_number().low;

    if VUNLIKELY (write_identity.sequence_number().low == 0) {
      write_identity.sequence_number() = rtps::SequenceNumber_t(0, 1);
    }

    sample_identity = write_identity;
    reader_listener_->write_params_.sample_identity(sample_identity);
  }

  auto cleanup_callback = [this, &id, &sample_identity]() {
    std::lock_guard param_lock(param_mtx_);

    if (is_cdr_type) {
      cdr_callbacks_.erase(sample_identity);
    } else {
      callbacks_.erase(id);
    }
  };

  if (timeout.count() != 0) {
    ack_manager_.reset_interrupted();

    auto ack_request = ack_manager_.create_request();

    if (is_cdr_type) {
      std::lock_guard param_lock(param_mtx_);
      cdr_callbacks_[sample_identity] = [this, ack_request, callback = std::move(callback)](const Bytes& resp_data) {
        ack_manager_.notify(ack_request, [&callback, &resp_data]() { callback(resp_data); });
      };
    } else {
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

    bool result = ack_manager_.process(ack_request, timeout.count() - elapsed, [this, &req_data, &id, &write_params]() {
      if (is_cdr_type) {
        std::lock_guard param_lock(param_mtx_);
        return DdsFactory::write_cdr_data(writer_.get(), req_data, &write_params);
      } else {
        return DdsFactory::write_data(writer_.get(), req_data, id);
      }
    });

    if VUNLIKELY (!result) {
      cleanup_callback();
    }

    return result;
  }

  bool result = false;

  if (is_cdr_type) {
    {
      std::lock_guard param_lock(param_mtx_);

      cdr_callbacks_[sample_identity] = [callback = std::move(callback)](const Bytes& resp_data) {
        callback(resp_data);
      };

      result = DdsFactory::write_cdr_data(writer_.get(), req_data, &write_params);
    }
  } else {
    {
      std::lock_guard param_lock(param_mtx_);
      callbacks_[id] = [callback = std::move(callback)](const Bytes& resp_data) { callback(resp_data); };
    }

    result = DdsFactory::write_data(writer_.get(), req_data, id);
  }

  if VUNLIKELY (!result) {
    cleanup_callback();
  }

  return result;
}

}  // namespace vlink
