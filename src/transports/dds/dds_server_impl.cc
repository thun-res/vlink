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

      while (DdsFactory::take_cdr_data(reader, msg) > 0) {
        const bool should_quit = instance->quit_flag_.load(std::memory_order_relaxed);
        DdsFactory::return_cdr_loan(reader, msg);

        if VUNLIKELY (should_quit) {
          break;
        }
      }
    } else {
      DdsFactory::ReadMessage msg;

      while (DdsFactory::take_data(reader, msg) > 0) {
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

// DdsServerImpl
DdsServerImpl::DdsServerImpl(const DdsTransport& transport) : transport_(transport) {}

void DdsServerImpl::process_message(dds::DataReader* reader) {
  if VUNLIKELY (quit_flag_.load(std::memory_order_acquire)) {
    return;
  }

  if (is_cdr_type) {
    DdsFactory::ReadCdrMessage msg;

    while (true) {
      const int32_t count = DdsFactory::take_cdr_data(reader, msg);

      if (count <= 0) {
        break;
      }

      DdsFactory::CdrLoanGuard loan_guard(reader, msg);
      bool should_quit = false;

      for (int32_t i = 0; i < count; ++i) {
        if VUNLIKELY (quit_flag_.load(std::memory_order_acquire)) {
          should_quit = true;
          break;
        }

        const auto& info = msg.infos[i];

        if VUNLIKELY (!info.valid_data) {
          continue;
        }

        if (writer_) {
          const bool sync_mode = is_sync_type.load(std::memory_order_acquire);

          struct CdrEntryGuard final {
            DdsServerImpl& self;
            uint64_t id{0};
            bool active{false};

            ~CdrEntryGuard() {
              if VUNLIKELY (active) {
                std::lock_guard lock(self.param_mtx_);
                self.cdr_id_map_.erase(id);
              }
            }
          } guard{*this, 0, sync_mode};

          uint64_t req_id = 0;

          {
            std::lock_guard lock(param_mtx_);
            req_id = cdr_seq_.fetch_add(1, std::memory_order_relaxed) + 1;
            guard.id = req_id;
            cdr_id_map_.try_emplace(req_id, info.sample_identity, sync_mode ? &guard.active : nullptr);
          }

          Bytes resp_data;

          callback_(req_id, msg.samples[i], &resp_data);
        } else {
          callback_(info.sample_identity.sequence_number().to64long(), msg.samples[i], nullptr);
        }
      }

      if VUNLIKELY (should_quit) {
        break;
      }
    }
  } else {
    DdsFactory::ReadMessage msg;

    while (true) {
      const int32_t count = DdsFactory::take_data(reader, msg);

      if (count <= 0) {
        break;
      }

      DdsFactory::DataLoanGuard loan_guard(reader, msg);
      bool should_quit = false;

      for (int32_t i = 0; i < count; ++i) {
        if VUNLIKELY (quit_flag_.load(std::memory_order_acquire)) {
          should_quit = true;
          break;
        }

        const auto& info = msg.infos[i];

        if VUNLIKELY (!info.valid_data) {
          continue;
        }

        const uint64_t req_id = msg.samples[i].id();

        if (writer_) {
          Bytes resp_data;
          callback_(req_id, msg.samples[i].data(), &resp_data);
        } else {
          callback_(req_id, msg.samples[i].data(), nullptr);
        }
      }

      if VUNLIKELY (should_quit) {
        break;
      }
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

  participant_ = DdsFactory::create_participant(kServer | kClient, transport_, get_all_properties());

  if VUNLIKELY (!participant_) {
    VLOG_F("Create participant failed.");
  }

  if (is_resp_type) {
    std::tie(topic_req_, topic_resp_) =
        DdsFactory::create_method_topic(kServer | kClient, transport_, participant_.get(), is_cdr_type, ser_type);

    if VUNLIKELY (!topic_req_ || !topic_resp_) {
      VLOG_F("Create method topic failed: ", transport_.topic, ".");
    }
  } else {
    topic_req_ = DdsFactory::create_topic(kServer | kClient, transport_, participant_.get(), is_cdr_type, {}, ser_type);

    if VUNLIKELY (!topic_req_) {
      VLOG_F("Create topic failed: ", transport_.topic, ".");
    }
  }

  if (is_cdr_type) {
    ser_type = topic_req_->get_type_name();

    if (is_resp_type) {
      ser_type += "|" + topic_resp_->get_type_name();
    }
  }

  if (is_resp_type) {
    publisher_ = DdsFactory::create_publisher(kServer, transport_, participant_.get());

    if VUNLIKELY (!publisher_) {
      VLOG_F("Create publisher failed.");
    }

    writer_listener_.emplace(this);

    writer_ = DdsFactory::create_datawriter(kServer, transport_, publisher_.get(), topic_resp_.get(),
                                            &writer_listener_.value(), is_cdr_type);

    if VUNLIKELY (!writer_) {
      VLOG_F("Create datawriter failed.");
    }
  }

  subscriber_ = DdsFactory::create_subscriber(kServer, transport_, participant_.get());

  if VUNLIKELY (!subscriber_) {
    VLOG_F("Create subscriber failed.");
  }

  quit_flag_.store(false, std::memory_order_release);
}

void DdsServerImpl::deinit() {
  quit_flag_.store(true, std::memory_order_release);

  if (reader_) {
    reader_->set_listener(nullptr, dds::StatusMask::none());
  }

  if (writer_) {
    writer_->set_listener(nullptr, dds::StatusMask::none());
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
  std::lock_guard lock(param_mtx_);
  cdr_id_map_.clear();
  callback_ = {};
  is_listened.store(false, std::memory_order_release);
  read_session_count_.store(0, std::memory_order_release);
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

const Transport* DdsServerImpl::get_transport() const { return &transport_; }

const AbstractNode* DdsServerImpl::get_abstract_node() const { return this; }

Status::BasePtr DdsServerImpl::get_status(Status::Type type) const {
  if (Status::is_for_writer(type)) {
    if (writer_) {
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
  if VUNLIKELY (callback_ || (is_resp_type && !writer_)) {
    return false;
  }

  callback_ = std::move(callback);

  reader_listener_.emplace(this);

  reader_ = DdsFactory::create_datareader(kServer, transport_, subscriber_.get(), topic_req_.get(),
                                          &reader_listener_.value(), is_cdr_type);

  if VUNLIKELY (!reader_) {
    reader_listener_.reset();
    callback_ = {};
    return false;
  }

  return true;
}

bool DdsServerImpl::reply(uint64_t req_id, const Bytes& resp_data, bool is_sync) {
  if VUNLIKELY (!writer_) {
    return false;
  }

  (void)is_sync;

  if (!is_cdr_type) {
    return DdsFactory::write_data(writer_.get(), resp_data, req_id);
  }

  rtps::WriteParams params;
  {
    std::unique_lock lock(param_mtx_);
    auto iter = cdr_id_map_.find(req_id);

    if VUNLIKELY (iter == cdr_id_map_.end() || iter->second.replying) {
      lock.unlock();
      VLOG_E("DdsServer: Request id is unknown or a reply is in progress.");
      return false;
    }

    params.related_sample_identity(iter->second.sample_identity);
    iter->second.replying = true;
  }

  auto finish_request = [this, req_id](bool sent) {
    std::lock_guard lock(param_mtx_);
    auto iter = cdr_id_map_.find(req_id);

    if VUNLIKELY (iter == cdr_id_map_.end()) {
      return;
    }

    if VLIKELY (sent) {
      if (iter->second.active_guard) {
        *iter->second.active_guard = false;
      }

      cdr_id_map_.erase(iter);
    } else {
      iter->second.replying = false;
    }
  };

  bool sent = false;

  try {
    sent = DdsFactory::write_cdr_data(writer_.get(), resp_data, &params);
  } catch (...) {
    finish_request(false);
    throw;
  }

  finish_request(sent);
  return sent;
}

}  // namespace vlink
