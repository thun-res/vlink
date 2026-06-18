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

#include "./modules/qnx_conf.h"

#include <memory>

#include "./base/helpers.h"
#include "./base/logger.h"
#include "./impl/conf_plugin_interface.h"
#include "./impl/url.h"
#include "./qnx_client_impl.h"
#include "./qnx_getter_impl.h"
#include "./qnx_publisher_impl.h"
#include "./qnx_server_impl.h"
#include "./qnx_setter_impl.h"
#include "./qnx_subscriber_impl.h"

namespace vlink {

VLINK_DEFINE_GLOBAL_PROPERTY(QnxConf)

// QnxConf
void QnxConf::global_init() { QnxFactory::get(); }

bool QnxConf::parse_protocol(struct Protocol* protocol) {
  if VUNLIKELY (get_impl_type() == kUnknownImplType) {
    VLOG_F("QnxConf: Unknown node type, cannot parse protocol.");

    return false;
  }

  if VUNLIKELY (protocol->host.empty()) {
    return false;
  }

  address = protocol->host + (protocol->path.empty() ? "" : "/" + protocol->path);
  event = protocol->dictionary["event"];

  return true;
}

bool QnxConf::is_valid() const {
  if VUNLIKELY (address.empty()) {
    return false;
  }

  return true;
}

std::unique_ptr<ServerImpl> QnxConf::create_server() const { return std::make_unique<QnxServerImpl>(*this); }

std::unique_ptr<ClientImpl> QnxConf::create_client() const { return std::make_unique<QnxClientImpl>(*this); }

std::unique_ptr<PublisherImpl> QnxConf::create_publisher() const { return std::make_unique<QnxPublisherImpl>(*this); }

std::unique_ptr<SubscriberImpl> QnxConf::create_subscriber() const {
  return std::make_unique<QnxSubscriberImpl>(*this);
}

std::unique_ptr<SetterImpl> QnxConf::create_setter() const { return std::make_unique<QnxSetterImpl>(*this); }

std::unique_ptr<GetterImpl> QnxConf::create_getter() const { return std::make_unique<QnxGetterImpl>(*this); }

std::ostream& operator<<(std::ostream& ostream, const QnxConf& conf) noexcept {
  std::ios_base::fmtflags f = ostream.flags();

  ostream << "QnxConf:"
          << "[type]" << +conf.get_impl_type() << "[address]" << conf.address << "[event]" << conf.event;

  ostream.flags(f);

  return ostream;
}

class ConfPluginQnx : public ConfPluginInterface {
 protected:
  TransportType get_transport_type() const override { return TransportType::kQnx; }

  std::unique_ptr<Conf> create() const override { return std::make_unique<QnxConf>(); }
};

#ifdef VLINK_LIBRARY
VLINK_PLUGIN_DECLARE(ConfPluginQnx, 1, 0)
#endif

}  // namespace vlink
