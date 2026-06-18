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

#include "./ddst_subscriber_impl.h"

#include <memory>
#include <utility>

#include "./base/elapsed_timer.h"
#include "./base/message_loop.h"

namespace vlink {

// ReaderListener
DdstSubscriberImpl::ReaderListener::ReaderListener(NodeImpl* impl) : DdstReaderListener(impl) {}

void DdstSubscriberImpl::ReaderListener::on_data_available(ddst::DataReader* reader) {
  auto* instance = static_cast<DdstSubscriberImpl*>(get_impl());
  auto* message_loop = instance->get_message_loop();

  if VUNLIKELY (instance->has_suspend) {
    DdstFactory::ReadMessage msg;

    while (DdstFactory::take_data(reader, msg)) {
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

// DdstSubscriberImpl
DdstSubscriberImpl::DdstSubscriberImpl(const DdstConf& conf) : conf_(conf) {}

void DdstSubscriberImpl::process_message(ddst::DataReader* reader) {
  DdstFactory::ReadMessage msg;

  while (DdstFactory::take_data(reader, msg)) {
    if VUNLIKELY (quit_flag_) {
      break;
    }

    if VUNLIKELY (!msg.info.valid_data) {
      continue;
    }

    if VUNLIKELY (is_latency_and_lost_enabled_.load(std::memory_order_acquire)) {
      last_latency_.store(ElapsedTimer::get_sys_timestamp(ElapsedTimer::kNano, false) - msg.timestamp,
                          std::memory_order_relaxed);

      uint64_t part1 = 0;
      uint64_t part2 = 0;

      std::memcpy(&part1, msg.info.publication_handle.keyHash.value, sizeof(uint64_t));
      std::memcpy(&part2, msg.info.publication_handle.keyHash.value + 8, sizeof(uint64_t));

      calc_sample_.update(msg.id, part1 ^ part2);
    }

    callback_(msg.bytes);
  }
}

void DdstSubscriberImpl::init() {
  if VUNLIKELY (is_cdr_type && is_security_type) {
    VLOG_F("Cdr type does not support security.");
  }

  participant_ = DdstFactory::create_participant(kPublisher | kSubscriber, conf_, get_all_properties());

  topic_ = DdstFactory::create_topic(kPublisher | kSubscriber, conf_, participant_.get());

  subscriber_ = DdstFactory::create_subscriber(kSubscriber, conf_, participant_.get());

  quit_flag_ = false;
}

void DdstSubscriberImpl::deinit() {
  detach();

  quit_flag_ = true;

  reader_.reset();
  listener_.reset();
  subscriber_.reset();
  topic_.reset();
  participant_.reset();
}

bool DdstSubscriberImpl::suspend() {
  has_suspend = true;

  return true;
}

bool DdstSubscriberImpl::resume() {
  has_suspend = false;

  return true;
}

bool DdstSubscriberImpl::is_suspend() const { return has_suspend; }

const Conf* DdstSubscriberImpl::get_conf() const { return &conf_; }

const AbstractNode* DdstSubscriberImpl::get_abstract_node() const { return this; }

Status::BasePtr DdstSubscriberImpl::get_status(Status::Type type) const {
  if VUNLIKELY (!reader_) {
    return std::make_shared<Status::Unknown>();
  }

  return ReaderListener::get_status(reader_.get(), type);
}

std::any DdstSubscriberImpl::get_native_handle() const { return subscriber_; }

bool DdstSubscriberImpl::listen(MsgCallback&& callback) {
  if VUNLIKELY (callback_) {
    return false;
  }

  callback_ = std::move(callback);

  listener_.emplace(this);

  reader_ = DdstFactory::create_datareader(kSubscriber, conf_, subscriber_.get(), topic_.get(), &listener_.value());

  return true;
}

void DdstSubscriberImpl::set_latency_and_lost_enabled(bool enable) {
  is_latency_and_lost_enabled_.store(enable, std::memory_order_release);
}

bool DdstSubscriberImpl::is_latency_and_lost_enabled() const {
  return is_latency_and_lost_enabled_.load(std::memory_order_acquire);
}

int64_t DdstSubscriberImpl::get_latency() const {
  if (!is_latency_and_lost_enabled_.load(std::memory_order_acquire)) {
    return 0;
  }

  return last_latency_.load(std::memory_order_relaxed);
}

SampleLostInfo DdstSubscriberImpl::get_lost() const {
  if (!is_latency_and_lost_enabled_.load(std::memory_order_acquire)) {
    return SampleLostInfo();
  }

  return SampleLostInfo{calc_sample_.get_total(), calc_sample_.get_lost()};
}

}  // namespace vlink
