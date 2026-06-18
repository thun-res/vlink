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

#include "./ddst_publisher_impl.h"

#include "./base/message_loop.h"

namespace vlink {

// WriterListener
DdstPublisherImpl::WriterListener::WriterListener(NodeImpl* impl) : DdstWriterListener(impl) {}

void DdstPublisherImpl::WriterListener::on_publication_matched(ddst::DataWriter* writer,
                                                               const ddst::PublicationMatchedStatus& status) {
  auto* instance = static_cast<DdstPublisherImpl*>(get_impl());
  auto* message_loop = instance->get_message_loop();

  instance->session_count_ = status.current_count;

  if (message_loop) {
    message_loop->post_task([instance]() {
      if VUNLIKELY (!instance->get_message_loop()) {
        return;
      }

      instance->update_subscribers();
    });
  } else {
    instance->update_subscribers();
  }

  DdstWriterListener::on_publication_matched(writer, status);
}

// DdstPublisherImpl
DdstPublisherImpl::DdstPublisherImpl(const DdstConf& conf) : conf_(conf) {}

void DdstPublisherImpl::init() {
  if VUNLIKELY (is_cdr_type && is_security_type) {
    VLOG_F("Cdr type does not support security.");
  }

  participant_ = DdstFactory::create_participant(kPublisher | kSubscriber, conf_, get_all_properties());

  topic_ = DdstFactory::create_topic(kPublisher | kSubscriber, conf_, participant_.get());

  publisher_ = DdstFactory::create_publisher(kPublisher, conf_, participant_.get());

  listener_.emplace(this);

  writer_ = DdstFactory::create_datawriter(kPublisher, conf_, publisher_.get(), topic_.get(), &listener_.value());
}

void DdstPublisherImpl::deinit() {
  detach();

  writer_.reset();
  listener_.reset();
  publisher_.reset();
  topic_.reset();
  participant_.reset();
}

const Conf* DdstPublisherImpl::get_conf() const { return &conf_; }

const AbstractNode* DdstPublisherImpl::get_abstract_node() const { return this; }

Status::BasePtr DdstPublisherImpl::get_status(Status::Type type) const {
  if VUNLIKELY (!writer_) {
    return std::make_shared<Status::Unknown>();
  }

  return WriterListener::get_status(writer_.get(), type);
}

std::any DdstPublisherImpl::get_native_handle() const { return publisher_; }

bool DdstPublisherImpl::has_subscribers() const { return session_count_ > 0; }

bool DdstPublisherImpl::write(const Bytes& msg_data) {
  return DdstFactory::write_data(writer_.get(), msg_data, seq_++);
}

}  // namespace vlink
