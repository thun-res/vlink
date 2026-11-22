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

#include "./dds_getter_impl.h"

#include <memory>
#include <utility>

#include "./base/elapsed_timer.h"
#include "./base/message_loop.h"

namespace vlink {

// ReaderListener
DdsGetterImpl::ReaderListener::ReaderListener(NodeImpl* impl) : DdsReaderListener(impl) {}

void DdsGetterImpl::ReaderListener::on_data_available(dds::DataReader* reader) {
  auto* instance = static_cast<DdsGetterImpl*>(get_impl());
  auto* message_loop = instance->get_message_loop();

  if VUNLIKELY (instance->has_suspend.load(std::memory_order_acquire)) {
    if (instance->is_cdr_type) {
      DdsFactory::ReadCdrMessage msg;

      while (DdsFactory::take_cdr_data(reader, msg) > 0) {
        const bool should_quit = instance->quit_flag_.load(std::memory_order_acquire);
        DdsFactory::return_cdr_loan(reader, msg);

        if VUNLIKELY (should_quit) {
          break;
        }
      }
    } else {
      DdsFactory::ReadMessage msg;

      while (DdsFactory::take_data(reader, msg) > 0) {
        DdsFactory::return_data_loan(reader, msg);

        if VUNLIKELY (instance->quit_flag_.load(std::memory_order_acquire)) {
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

// DdsGetterImpl
DdsGetterImpl::DdsGetterImpl(const DdsTransport& transport) : transport_(transport) {}

void DdsGetterImpl::process_message(dds::DataReader* reader) {
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

        if VUNLIKELY (is_latency_and_lost_enabled_.load(std::memory_order_acquire)) {
          last_latency_.store(
              ElapsedTimer::get_sys_timestamp(ElapsedTimer::kNano, false) - info.source_timestamp.to_ns(),
              std::memory_order_relaxed);

          uint64_t part1 = 0;
          uint64_t part2 = 0;

          std::memcpy(&part1, info.publication_handle.value, sizeof(uint64_t));
          std::memcpy(&part2, info.publication_handle.value + 8, sizeof(uint64_t));

          calc_sample_.update(info.sample_identity.sequence_number().to64long(), part1 ^ part2);
        }

        callback_(msg.samples[i]);
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

        if VUNLIKELY (is_latency_and_lost_enabled_.load(std::memory_order_acquire)) {
          last_latency_.store(
              ElapsedTimer::get_sys_timestamp(ElapsedTimer::kNano, false) - info.source_timestamp.to_ns(),
              std::memory_order_relaxed);

          uint64_t part1 = 0;
          uint64_t part2 = 0;

          std::memcpy(&part1, info.publication_handle.value, sizeof(uint64_t));
          std::memcpy(&part2, info.publication_handle.value + 8, sizeof(uint64_t));

          calc_sample_.update(msg.samples[i].id(), part1 ^ part2);
        }

        callback_(msg.samples[i].data());
      }

      if VUNLIKELY (should_quit) {
        break;
      }
    }
  }
}

void DdsGetterImpl::init() {
  if VUNLIKELY (is_cdr_type && is_security_type) {
    VLOG_F("Cdr type does not support security.");
  }

  participant_ = DdsFactory::create_participant(kPublisher | kSubscriber, transport_, get_all_properties());

  if VUNLIKELY (!participant_) {
    VLOG_F("Create participant failed.");
  }

  topic_ =
      DdsFactory::create_topic(kPublisher | kSubscriber, transport_, participant_.get(), is_cdr_type, {}, ser_type);

  if VUNLIKELY (!topic_) {
    VLOG_F("Create topic failed: ", transport_.topic, ".");
  }

  subscriber_ = DdsFactory::create_subscriber(kSubscriber, transport_, participant_.get());

  if VUNLIKELY (!subscriber_) {
    VLOG_F("Create subscriber failed.");
  }

  if (is_cdr_type) {
    ser_type = topic_->get_type_name();
  }

  quit_flag_.store(false, std::memory_order_release);
}

void DdsGetterImpl::deinit() {
  quit_flag_.store(true, std::memory_order_release);

  if (reader_) {
    reader_->set_listener(nullptr, dds::StatusMask::none());
  }

  detach();

  reader_.reset();

  listener_.reset();
  subscriber_.reset();
  topic_.reset();
  participant_.reset();
  callback_ = {};
}

bool DdsGetterImpl::suspend() {
  has_suspend.store(true, std::memory_order_release);

  return true;
}

bool DdsGetterImpl::resume() {
  has_suspend.store(false, std::memory_order_release);

  return true;
}

bool DdsGetterImpl::is_suspend() const { return has_suspend.load(std::memory_order_acquire); }

const Transport* DdsGetterImpl::get_transport() const { return &transport_; }

const AbstractNode* DdsGetterImpl::get_abstract_node() const { return this; }

Status::BasePtr DdsGetterImpl::get_status(Status::Type type) const {
  if VUNLIKELY (!reader_) {
    return std::make_shared<Status::Unknown>();
  }

  return ReaderListener::get_status(reader_.get(), type);
}

std::any DdsGetterImpl::get_native_handle() const { return subscriber_; }

bool DdsGetterImpl::listen(MsgCallback&& callback) {
  if VUNLIKELY (callback_) {
    return false;
  }

  callback_ = std::move(callback);

  listener_.emplace(this);

  reader_ = DdsFactory::create_datareader(kGetter, transport_, subscriber_.get(), topic_.get(), &listener_.value(),
                                          is_cdr_type);

  if VUNLIKELY (!reader_) {
    listener_.reset();
    callback_ = {};
    return false;
  }

  return true;
}

void DdsGetterImpl::set_latency_and_lost_enabled(bool enable) {
  is_latency_and_lost_enabled_.store(enable, std::memory_order_release);
}

bool DdsGetterImpl::is_latency_and_lost_enabled() const {
  return is_latency_and_lost_enabled_.load(std::memory_order_acquire);
}

int64_t DdsGetterImpl::get_latency() const {
  if (!is_latency_and_lost_enabled_.load(std::memory_order_acquire)) {
    return 0;
  }

  return last_latency_.load(std::memory_order_relaxed);
}

SampleLostInfo DdsGetterImpl::get_lost() const {
  if (!is_latency_and_lost_enabled_.load(std::memory_order_acquire)) {
    return SampleLostInfo();
  }

  return SampleLostInfo{calc_sample_.get_total(), calc_sample_.get_lost()};
}

}  // namespace vlink
