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

#include "./ddsr_setter_impl.hpp"

namespace vlink {

// WriterListener
DdsrSetterImpl::WriterListener::WriterListener(NodeImpl* impl) : DdsrWriterListener(impl) {}

// DdsrSetterImpl
DdsrSetterImpl::DdsrSetterImpl(const DdsrTransport& transport) : transport_(transport) {}

void DdsrSetterImpl::init() {
  participant_ = DdsrFactory::create_participant(kPublisher | kSubscriber, transport_, get_all_properties());

  topic_ = DdsrFactory::create_topic(kPublisher | kSubscriber, transport_, participant_.get());

  publisher_ = DdsrFactory::create_publisher(kPublisher, transport_, participant_.get());

  if VUNLIKELY (!participant_ || !topic_) {
    VLOG_E("DdsrSetterImpl::init(): participant/topic creation failed; setter left uninitialised.");

    return;
  }

  listener_.emplace(this);

  writer_ = DdsrFactory::create_datawriter(kSetter, transport_, publisher_.get(), topic_.get(), listener_->get_ptr());
}

void DdsrSetterImpl::deinit() {
  if (writer_) {
    DDS_DataWriter_set_listener(writer_->entity, nullptr, DDS_STATUS_MASK_NONE);
  }

  detach();

  writer_.reset();
  listener_.reset();
  publisher_.reset();
  topic_.reset();
  participant_.reset();
}

const Transport* DdsrSetterImpl::get_transport() const { return &transport_; }

const AbstractNode* DdsrSetterImpl::get_abstract_node() const { return this; }

Status::BasePtr DdsrSetterImpl::get_status(Status::Type type) const {
  if VUNLIKELY (!writer_) {
    return std::make_shared<Status::Unknown>();
  }

  return WriterListener::get_status(writer_->entity, type);
}

std::any DdsrSetterImpl::get_native_handle() const { return publisher_; }

void DdsrSetterImpl::write(const Bytes& msg_data) {
  if VUNLIKELY (!writer_) {
    return;
  }

  DdsrFactory::write_data(writer_->entity, msg_data, seq_.fetch_add(1, std::memory_order_relaxed));
}

void DdsrSetterImpl::sync(SyncCallback&& callback) { callback(); }

}  // namespace vlink
