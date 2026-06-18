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

#include "./ddsr_getter_impl.hpp"

#include <memory>
#include <utility>

#include "./base/elapsed_timer.h"
#include "./base/message_loop.h"

namespace vlink {

// ReaderListener
DdsrGetterImpl::ReaderListener::ReaderListener(NodeImpl* impl) : DdsrReaderListener(impl) {}

void DdsrGetterImpl::ReaderListener::on_data_available(NodeImpl* impl, DDS_DataReader* reader) {
  auto* instance = static_cast<DdsrGetterImpl*>(impl);
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

// DdsrGetterImpl
DdsrGetterImpl::DdsrGetterImpl(const DdsrConf& conf) : conf_(conf) {}

void DdsrGetterImpl::process_message(DDS_DataReader* reader) {
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

    if VUNLIKELY (is_latency_and_lost_enabled_.load(std::memory_order_acquire)) {
      last_latency_.store(ElapsedTimer::get_sys_timestamp(ElapsedTimer::kNano, false) - msg.timestamp,
                          std::memory_order_relaxed);

      uint64_t part1 = 0;
      uint64_t part2 = 0;

      std::memcpy(&part1, msg.info->publication_handle.keyHash.value, sizeof(uint64_t));
      std::memcpy(&part2, msg.info->publication_handle.keyHash.value + 8, sizeof(uint64_t));

      calc_sample_.update(msg.id, part1 ^ part2);
    }

    callback_(msg.bytes);

    DdsrFactory::release_data(reader, msg);
  }
}

void DdsrGetterImpl::init() {
  participant_ = DdsrFactory::create_participant(kPublisher | kSubscriber, conf_, get_all_properties());

  topic_ = DdsrFactory::create_topic(kPublisher | kSubscriber, conf_, participant_.get());

  subscriber_ = DdsrFactory::create_subscriber(kSubscriber, conf_, participant_.get());

  quit_flag_ = false;
}

void DdsrGetterImpl::deinit() {
  detach();

  quit_flag_ = true;

  reader_.reset();
  listener_.reset();
  subscriber_.reset();
  topic_.reset();
  participant_.reset();
}

bool DdsrGetterImpl::suspend() {
  has_suspend = true;

  return true;
}

bool DdsrGetterImpl::resume() {
  has_suspend = false;

  return true;
}

bool DdsrGetterImpl::is_suspend() const { return has_suspend; }

const Conf* DdsrGetterImpl::get_conf() const { return &conf_; }

const AbstractNode* DdsrGetterImpl::get_abstract_node() const { return this; }

Status::BasePtr DdsrGetterImpl::get_status(Status::Type type) const {
  if VUNLIKELY (!reader_) {
    return std::make_shared<Status::Unknown>();
  }

  return ReaderListener::get_status(reader_->entity, type);
}

std::any DdsrGetterImpl::get_native_handle() const { return subscriber_; }

bool DdsrGetterImpl::listen(MsgCallback&& callback) {
  if VUNLIKELY (callback_) {
    return false;
  }

  callback_ = std::move(callback);

  listener_.emplace(this);

  reader_ = DdsrFactory::create_datareader(kGetter, conf_, subscriber_.get(), topic_.get(), listener_->get_ptr());

  return true;
}

void DdsrGetterImpl::set_latency_and_lost_enabled(bool enable) {
  is_latency_and_lost_enabled_.store(enable, std::memory_order_release);
}

bool DdsrGetterImpl::is_latency_and_lost_enabled() const {
  return is_latency_and_lost_enabled_.load(std::memory_order_acquire);
}

int64_t DdsrGetterImpl::get_latency() const {
  if (!is_latency_and_lost_enabled_.load(std::memory_order_acquire)) {
    return 0;
  }

  return last_latency_.load(std::memory_order_relaxed);
}

SampleLostInfo DdsrGetterImpl::get_lost() const {
  if (!is_latency_and_lost_enabled_.load(std::memory_order_acquire)) {
    return SampleLostInfo();
  }

  return SampleLostInfo{calc_sample_.get_total(), calc_sample_.get_lost()};
}

}  // namespace vlink
