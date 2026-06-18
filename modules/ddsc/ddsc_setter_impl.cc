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

#include "./ddsc_setter_impl.hpp"

namespace vlink {

// WriterListener
DdscSetterImpl::WriterListener::WriterListener(NodeImpl* impl) : DdscWriterListener(impl) {}

// DdscSetterImpl
DdscSetterImpl::DdscSetterImpl(const DdscConf& conf) : conf_(conf) {}

void DdscSetterImpl::init() {
  participant_ = DdscFactory::create_participant(kPublisher | kSubscriber, conf_, get_all_properties());

  topic_ = DdscFactory::create_topic(kPublisher | kSubscriber, conf_, participant_.get());

  publisher_ = DdscFactory::create_publisher(kPublisher, conf_, participant_.get());

  listener_.emplace(this);

  writer_ = DdscFactory::create_datawriter(kSetter, conf_, publisher_.get(), topic_.get(), listener_->get_ptr());
}

void DdscSetterImpl::deinit() {
  detach();

  writer_.reset();
  listener_.reset();
  publisher_.reset();
  topic_.reset();
  participant_.reset();
}

const Conf* DdscSetterImpl::get_conf() const { return &conf_; }

const AbstractNode* DdscSetterImpl::get_abstract_node() const { return this; }

Status::BasePtr DdscSetterImpl::get_status(Status::Type type) const {
  if VUNLIKELY (!writer_) {
    return std::make_shared<Status::Unknown>();
  }

  return WriterListener::get_status(writer_->entity, type);
}

std::any DdscSetterImpl::get_native_handle() const { return publisher_; }

void DdscSetterImpl::write(const Bytes& msg_data) { DdscFactory::write_data(writer_->entity, msg_data, seq_++); }

void DdscSetterImpl::sync(SyncCallback&& callback) { (void)callback; }

}  // namespace vlink
