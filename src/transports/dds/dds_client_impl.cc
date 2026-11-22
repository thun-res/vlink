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

#include <algorithm>
#include <limits>
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
    instance->post_to_message_loop([instance]() {
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
DdsClientImpl::ReaderListener::ReaderListener(NodeImpl* impl) : DdsReaderListener(impl) {}

void DdsClientImpl::ReaderListener::on_subscription_matched(dds::DataReader* reader,
                                                            const dds::SubscriptionMatchedStatus& status) {
  auto* instance = static_cast<DdsClientImpl*>(get_impl());
  auto* message_loop = instance->get_message_loop();

  instance->read_session_count_.store(status.current_count, std::memory_order_release);

  // std::this_thread::sleep_for(std::chrono::microseconds(1));  //?

  if (message_loop) {
    instance->post_to_message_loop([instance]() {
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

// DdsClientImpl
DdsClientImpl::DdsClientImpl(const DdsTransport& transport) : transport_(transport) {}

void DdsClientImpl::process_message(dds::DataReader* reader) {
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

        NodeImpl::MsgCallback cb;
        {
          std::lock_guard param_lock(param_mtx_);
          auto iter = cdr_callbacks_.find(info.related_sample_identity);

          if VLIKELY (iter != cdr_callbacks_.end()) {
            cb = std::move(iter->second);
            cdr_callbacks_.erase(iter);
          } else if (cdr_writes_ != 0 && info.related_sample_identity.writer_guid() == writer_->guid()) {
            early_cdr_replies_.try_emplace(info.related_sample_identity, msg.samples[i], get_message_loop());
          }
        }

        if VLIKELY (cb) {
          cb(msg.samples[i]);
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

        const uint64_t resp_id = msg.samples[i].id();
        NodeImpl::MsgCallback cb;
        {
          std::lock_guard param_lock(param_mtx_);
          auto iter = callbacks_.find(resp_id);

          if VLIKELY (iter != callbacks_.end()) {
            cb = std::move(iter->second);
            callbacks_.erase(iter);
          }
        }

        if VLIKELY (cb) {
          cb(msg.samples[i].data());
        }
      }

      if VUNLIKELY (should_quit) {
        break;
      }
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

    if (is_cdr_type) {
      ser_type = topic_req_->get_type_name() + "|" + topic_resp_->get_type_name();
    }

    publisher_ = DdsFactory::create_publisher(kClient, transport_, participant_.get());

    if VUNLIKELY (!publisher_) {
      VLOG_F("Create publisher failed.");
    }

    writer_listener_.emplace(this);

    writer_ = DdsFactory::create_datawriter(kClient, transport_, publisher_.get(), topic_req_.get(),
                                            &writer_listener_.value(), is_cdr_type);

    if VUNLIKELY (!writer_) {
      VLOG_F("Create datawriter failed.");
    }

    subscriber_ = DdsFactory::create_subscriber(kClient, transport_, participant_.get());

    if VUNLIKELY (!subscriber_) {
      VLOG_F("Create subscriber failed.");
    }

    reader_listener_.emplace(this);

    reader_ = DdsFactory::create_datareader(kClient, transport_, subscriber_.get(), topic_resp_.get(),
                                            &reader_listener_.value(), is_cdr_type);

    if VUNLIKELY (!reader_) {
      VLOG_F("Create datareader failed.");
    }
  } else {
    topic_req_ = DdsFactory::create_topic(kServer | kClient, transport_, participant_.get(), is_cdr_type, {}, ser_type);

    if VUNLIKELY (!topic_req_) {
      VLOG_F("Create topic failed: ", transport_.topic, ".");
    }

    if (is_cdr_type) {
      ser_type = topic_req_->get_type_name();
    }

    publisher_ = DdsFactory::create_publisher(kClient, transport_, participant_.get());

    if VUNLIKELY (!publisher_) {
      VLOG_F("Create publisher failed.");
    }

    writer_listener_.emplace(this);

    writer_ = DdsFactory::create_datawriter(kClient, transport_, publisher_.get(), topic_req_.get(),
                                            &writer_listener_.value(), is_cdr_type);

    if VUNLIKELY (!writer_) {
      VLOG_F("Create datawriter failed.");
    }
  }

  quit_flag_.store(false, std::memory_order_release);
}

void DdsClientImpl::deinit() {
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
  callbacks_.clear();
  cdr_callbacks_.clear();
  write_session_count_.store(0, std::memory_order_release);
  read_session_count_.store(0, std::memory_order_release);
}

void DdsClientImpl::interrupt() {
  ClientImpl::interrupt();
  ack_manager_.clear();
}

const Transport* DdsClientImpl::get_transport() const { return &transport_; }

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
  if VUNLIKELY (!writer_ || (callback && !reader_)) {
    return false;
  }

  if (!callback) {
    if (is_cdr_type) {
      return DdsFactory::write_cdr_data(writer_.get(), req_data);
    } else {
      return DdsFactory::write_data(writer_.get(), req_data, 0);
    }
  }

  const uint64_t id =
      is_cdr_type ? 0 : DdsFactory::get_guid(writer_->guid(), seq_.fetch_add(1, std::memory_order_relaxed) + 1);
  rtps::WriteParams params;

  auto send = [&]() {
    if (!is_cdr_type) {
      {
        std::lock_guard lock(param_mtx_);
        callbacks_[id] = std::move(callback);
      }

      return DdsFactory::write_data(writer_.get(), req_data, id);
    }

    {
      std::lock_guard lock(param_mtx_);
      ++cdr_writes_;
    }

    std::pair<Bytes, MessageLoop*> early_reply;
    {
      struct WriteGuard final {
        DdsClientImpl& self;

        ~WriteGuard() {
          std::lock_guard lock(self.param_mtx_);

          if (--self.cdr_writes_ == 0) {
            self.early_cdr_replies_.clear();
          }
        }
      } guard{*this};

      if VUNLIKELY (!DdsFactory::write_cdr_data(writer_.get(), req_data, &params)) {
        return false;
      }

      std::lock_guard lock(param_mtx_);
      auto iter = early_cdr_replies_.find(params.sample_identity());

      if VLIKELY (iter == early_cdr_replies_.end()) {
        cdr_callbacks_.emplace(params.sample_identity(), std::move(callback));
        return true;
      }

      early_reply = std::move(iter->second);
      early_cdr_replies_.erase(iter);
    }

    if (early_reply.second) {
      const auto gate = get_loop_gate();
      auto* loop = early_reply.second;
      return post_to_loop_gate(gate, loop, [gate, loop, cb = std::move(callback), data = std::move(early_reply.first)] {
        if (is_loop_gate_in_same_thread(gate, loop)) {
          cb(data);
        }
      });
    }

    callback(early_reply.first);
    return true;
  };

  auto cleanup = [&]() {
    std::lock_guard lock(param_mtx_);

    if (is_cdr_type) {
      cdr_callbacks_.erase(params.sample_identity());
    } else {
      callbacks_.erase(id);
    }
  };

  bool result = false;

  try {
    if (timeout.count() == 0) {
      result = send();
    } else {
      ack_manager_.reset_interrupted();
      auto request = ack_manager_.create_request();
      ElapsedTimer timer;
      timer.start();

      if VUNLIKELY (!wait_for_connected(timeout)) {
        return false;
      }

      const auto elapsed = timer.get();

      if VUNLIKELY (timeout.count() > 0 && elapsed >= timeout.count()) {
        return false;
      }

      const int remaining_ms =
          timeout.count() < 0
              ? -1
              : static_cast<int>(std::min<int64_t>(timeout.count() - elapsed, std::numeric_limits<int>::max()));

      callback = [request, cb = std::move(callback)](const Bytes& data) {
        AckManager::notify(request, [&]() { cb(data); });
      };

      result = ack_manager_.process(std::move(request), remaining_ms, send);
    }
  } catch (...) {
    cleanup();
    throw;
  }

  if VUNLIKELY (!result) {
    cleanup();
  }

  return result;
}

}  // namespace vlink
