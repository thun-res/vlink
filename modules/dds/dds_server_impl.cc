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

#include "./dds_server_impl.h"

#include <memory>
#include <utility>

#include "./base/message_loop.h"

namespace vlink {

// WriterListener
DdsServerImpl::WriterListener::WriterListener(NodeImpl* impl) : DdsWriterListener(impl) {}

// ReaderListener
DdsServerImpl::ReaderListener::ReaderListener(NodeImpl* impl) : DdsReaderListener(impl) {}

void DdsServerImpl::ReaderListener::on_subscription_matched(dds::DataReader* reader,
                                                            const dds::SubscriptionMatchedStatus& status) {
  auto* instance = static_cast<DdsServerImpl*>(get_impl());

  instance->read_session_count_.store(status.current_count, std::memory_order_relaxed);

  DdsReaderListener::on_subscription_matched(reader, status);
}

void DdsServerImpl::ReaderListener::on_data_available(dds::DataReader* reader) {
  auto* instance = static_cast<DdsServerImpl*>(get_impl());
  auto* message_loop = instance->get_message_loop();

  if VUNLIKELY (instance->has_suspend.load(std::memory_order_acquire)) {
    if (instance->is_cdr_type) {
      DdsFactory::ReadCdrMessage msg;

      while (DdsFactory::take_cdr_data(reader, msg)) {
        const bool should_quit = instance->quit_flag_.load(std::memory_order_relaxed);
        DdsFactory::return_cdr_loan(reader, msg);

        if VUNLIKELY (should_quit) {
          break;
        }
      }
    } else {
      DdsFactory::ReadMessage msg;

      while (DdsFactory::take_data(reader, msg)) {
        DdsFactory::return_data_loan(reader, msg);

        if VUNLIKELY (instance->quit_flag_.load(std::memory_order_relaxed)) {
          break;
        }
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

// DdsServerImpl
DdsServerImpl::DdsServerImpl(const DdsConf& conf) : conf_(conf) {}

void DdsServerImpl::process_message(dds::DataReader* reader) {
  if (is_cdr_type) {
    DdsFactory::ReadCdrMessage msg;

    while (DdsFactory::take_cdr_data(reader, msg)) {
      if VUNLIKELY (quit_flag_.load(std::memory_order_relaxed)) {
        DdsFactory::return_cdr_loan(reader, msg);
        break;
      }

      const auto& info = msg.infos[0];

      if VUNLIKELY (!info.valid_data) {
        DdsFactory::return_cdr_loan(reader, msg);
        continue;
      }

      if (writer_) {
        {
          std::lock_guard lock(param_mtx_);
          rtps::WriteParams param;
          param.related_sample_identity() = info.sample_identity;
          msg.id = cdr_seq_.fetch_add(1, std::memory_order_relaxed) + 1;
          cdr_id_map_.emplace(msg.id, std::move(param));
        }

        Bytes resp_data;

        callback_(msg.id, msg.samples[0], &resp_data);
      } else {
        callback_(msg.id, msg.samples[0], nullptr);
      }

      DdsFactory::return_cdr_loan(reader, msg);
    }
  } else {
    DdsFactory::ReadMessage msg;

    while (DdsFactory::take_data(reader, msg)) {
      if VUNLIKELY (quit_flag_.load(std::memory_order_relaxed)) {
        DdsFactory::return_data_loan(reader, msg);
        break;
      }

      const auto& info = msg.infos[0];

      if VUNLIKELY (!info.valid_data) {
        DdsFactory::return_data_loan(reader, msg);
        continue;
      }

      if (writer_) {
        Bytes resp_data;
        callback_(msg.id, msg.samples[0].data(), &resp_data);
      } else {
        callback_(msg.id, msg.samples[0].data(), nullptr);
      }

      DdsFactory::return_data_loan(reader, msg);
    }
  }
}

void DdsServerImpl::init() {
  if VUNLIKELY (is_resp_type && is_cdr_type != is_resp_cdr_type) {
    VLOG_F("DdsServer: Request and response must both use raw or CDR serialization.");
  }

  if VUNLIKELY (is_cdr_type && is_security_type) {
    VLOG_F("Cdr type does not support security.");
  }

  participant_ = DdsFactory::create_participant(kServer | kClient, conf_, get_all_properties());

  if (is_resp_type) {
    std::tie(topic_req_, topic_resp_) =
        DdsFactory::create_method_topic(kServer | kClient, conf_, participant_.get(), is_cdr_type, ser_type);
  } else {
    topic_req_ = DdsFactory::create_topic(kServer | kClient, conf_, participant_.get(), is_cdr_type, {}, ser_type);
  }

  if VUNLIKELY (!participant_ || !topic_req_ || (is_resp_type && !topic_resp_)) {
    VLOG_E("DdsServerImpl::init(): participant/topic creation failed; server left uninitialised.");
    return;
  }

  if (is_cdr_type) {
    ser_type = topic_req_->get_type_name();
    if (is_resp_type) {
      ser_type += "|" + topic_resp_->get_type_name();
    }
  }

  if (is_resp_type) {
    publisher_ = DdsFactory::create_publisher(kServer, conf_, participant_.get());

    writer_listener_.emplace(this);

    writer_ = DdsFactory::create_datawriter(kServer, conf_, publisher_.get(), topic_resp_.get(),
                                            &writer_listener_.value(), is_cdr_type);
  }

  subscriber_ = DdsFactory::create_subscriber(kServer, conf_, participant_.get());

  quit_flag_.store(false, std::memory_order_relaxed);
}

void DdsServerImpl::deinit() {
  quit_flag_.store(true, std::memory_order_relaxed);

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
  cdr_id_map_.clear();
  callback_ = {};
  is_listened = false;
  read_session_count_.store(0, std::memory_order_relaxed);
}

bool DdsServerImpl::suspend() {
  has_suspend.store(true, std::memory_order_release);

  return true;
}

bool DdsServerImpl::resume() {
  has_suspend.store(false, std::memory_order_release);

  return true;
}

bool DdsServerImpl::is_suspend() const { return has_suspend.load(std::memory_order_acquire); }

const Conf* DdsServerImpl::get_conf() const { return &conf_; }

const AbstractNode* DdsServerImpl::get_abstract_node() const { return this; }

Status::BasePtr DdsServerImpl::get_status(Status::Type type) const {
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

std::any DdsServerImpl::get_native_handle() const { return subscriber_; }

bool DdsServerImpl::has_clients() const { return read_session_count_.load(std::memory_order_relaxed) > 0; }

bool DdsServerImpl::listen(ReqRespCallback&& callback) {
  if VUNLIKELY (callback_) {
    return false;
  }

  callback_ = std::move(callback);

  reader_listener_.emplace(this);

  reader_ = DdsFactory::create_datareader(kServer, conf_, subscriber_.get(), topic_req_.get(),
                                          &reader_listener_.value(), is_cdr_type);

  return true;
}

bool DdsServerImpl::reply(uint64_t req_id, const Bytes& resp_data, bool is_sync) {
  (void)is_sync;

  bool ret = false;

  if (is_cdr_type) {
    std::lock_guard lock(param_mtx_);
    auto iter = cdr_id_map_.find(req_id);

    if VUNLIKELY (iter == cdr_id_map_.end()) {
      VLOG_E("DdsServer: Cannot find request id.");
      return false;
    }

    ret = DdsFactory::write_cdr_data(writer_.get(), resp_data, &iter->second);

    if VLIKELY (ret) {
      cdr_id_map_.erase(iter);
    }
  } else {
    ret = DdsFactory::write_data(writer_.get(), resp_data, req_id);
  }

  return ret;
}

}  // namespace vlink
