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

#include "./qnx_factory.h"

#include <charconv>
#include <set>
#include <string>
#include <utility>

#include "./impl/server_impl.h"

#define _PULSE_CODE_CUSTOM_CONNECT -101
#define _PULSE_CODE_CUSTOM_HEARTBEAT -102

namespace vlink {

constexpr size_t kMaxTaskSize = 10000U;

static void wake_channel(name_attach_t* fd) {
  if VUNLIKELY (!fd) {
    return;
  }

  int coid = ::ConnectAttach(0, 0, fd->chid, _NTO_SIDE_CHANNEL, 0);

  if VLIKELY (coid >= 0) {
    (void)::MsgSendPulse(coid, SIGEV_PULSE_PRIO_INHERIT, _PULSE_CODE_CUSTOM_HEARTBEAT, 0);
    ::ConnectDetach(coid);
  }
}

size_t QnxLoop::get_max_task_count() const { return kMaxTaskSize; }

// QnxMsg
struct alignas(8) QnxMsg {
  static constexpr size_t kBufferSize{240};

  enum Type : uint8_t {
    kSend = 0,
    kInvoke = 1,
    kResponse = 2,
    kGetResult = 3,
    kSubscribe = 4,
    kPublish = 5,
  };

  union Data {
    uint32_t id;
    uint32_t token;
  } other;

  uint16_t type;
  uint32_t size;
  uint32_t channel;
  uint8_t buffer[kBufferSize];
};

// QnxHeader
union QnxHeader {
  struct _pulse pulse;
  QnxMsg msg;
};

// QnxFactory
QnxFactory::QnxFactory() {
  Bytes::init_memory_pool();

  if VUNLIKELY (QnxConf::get_thread_count() == 0) {
    VLOG_W("QnxFactory: Qnx does not support zero thread count.");
    QnxConf::set_thread_count(1);
  }

  message_loop_.emplace(QnxConf::get_thread_count());

  message_loop_->set_name("QNX-FACTORY");
  message_loop_->async_run();
}

QnxFactory::~QnxFactory() {
  message_loop_->quit();
  message_loop_->wait_for_quit();
}

std::string QnxFactory::get_back_address(const std::string& address, int id) {
  return address + "#" + std::to_string(id);
}

QnxLoop& QnxFactory::get_message_loop() { return QnxFactory::get().message_loop_.value(); }

int32_t QnxFactory::get_pid() {
  static auto pid = Utils::get_pid();

  return static_cast<int32_t>(pid);
}

// QnxServer
QnxServer::QnxServer(const QnxID& id) {
  std::tie(std::ignore, address_) = id;

  int retry_cnt = 0;

  do {
    fd_ = ::name_attach(nullptr, address_.c_str(), 0);

    if VUNLIKELY (!fd_) {
      std::this_thread::sleep_for(std::chrono::microseconds(10));
      if (++retry_cnt > 3) {
        break;
      }
    }
  } while (!fd_ && !quit_flag_);

  if VUNLIKELY (!fd_ && !quit_flag_) {
    VLOG_E("QnxFactory: Server name_attach failed.");
    return;
  }

  thread_ = std::thread([this] { process_message(); });
}

QnxServer::~QnxServer() {
  quit_flag_ = true;

  if VUNLIKELY (is_busy_) {
    QnxFactory::get_message_loop().wait_for_idle();
  }

  auto* fd = fd_.load();
  wake_channel(fd);

  if VLIKELY (thread_.joinable()) {
    thread_.join();
  }

  fd = fd_.exchange(nullptr);

  if VLIKELY (fd) {
    ::name_detach(fd, 0);
  }

  std::lock_guard lock(mtx_);
  for (auto [coid, back_coid] : clients_) {
    ::name_close(back_coid);
  }

  clients_.clear();
  channels_.clear();
}

bool QnxServer::publish(uint32_t channel, const Bytes& msg_data) {
  is_busy_ = true;

  QnxFactory::get_message_loop().post_task([weak_self = weak_from_this(), channel, msg_data] {
    auto self = weak_self.lock();

    if VUNLIKELY (!self) {
      return;
    }

    if VUNLIKELY (self->quit_flag_) {
      self->is_busy_ = false;
      return;
    }

    std::lock_guard lock(self->mtx_);

    if VUNLIKELY (self->clients_.empty()) {
      self->is_busy_ = false;
      return;
    }

    auto iter = self->channels_.find(channel);

    if VUNLIKELY (iter == self->channels_.end()) {
      self->is_busy_ = false;
      return;
    }

    const auto& list = iter->second;

    Bytes msg_data_buf;
    {
      QnxHeader header;

      std::memset(&header, 0, sizeof(QnxHeader));

      header.msg.type = QnxMsg::kPublish;
      header.msg.size = msg_data.size();
      header.msg.channel = channel;

      std::memset(header.msg.buffer, 0, QnxMsg::kBufferSize);

      msg_data_buf = Bytes::create(sizeof(header) + msg_data.size());

      std::memcpy(msg_data_buf.data(), &header, sizeof(header));
      std::memcpy(msg_data_buf.data() + sizeof(header), msg_data.data(), msg_data.size());
    }

    int ret = -1;

    for (auto id : list) {
      auto back_coid = self->clients_[id];

      ret = ::MsgSend(back_coid, msg_data_buf.data(), msg_data_buf.size(), 0, 0);

      if VUNLIKELY (ret < 0) {
        // VLOG_E("QnxServer MsgSend failed.");
      }
    }

    self->is_busy_ = false;
  });
  return true;
}

std::any QnxServer::get_native_handle() const { return this; }

int QnxServer::get_session_count() {
  std::lock_guard lock(mtx_);
  return clients_.size();
}

void QnxServer::process_message() {
  QnxHeader header;

  std::memset(&header, 0, sizeof(QnxHeader));

  _msg_info info;

  std::memset(&info, 0, sizeof(_msg_info));

  int rcvid = -1;

  while (!quit_flag_) {
    auto* fd = fd_.load();

    if VUNLIKELY (!fd) {
      break;
    }

    rcvid = ::MsgReceive(fd->chid, &header, sizeof(header), &info);

    if VUNLIKELY (rcvid < 0) {
      if VUNLIKELY (!quit_flag_) {
        VLOG_E("QnxFactory: Server MsgReceive failed.");
      }

      continue;
    }

    if (rcvid == 0) {
      if (header.pulse.code == _PULSE_CODE_CUSTOM_CONNECT) {
        std::lock_guard lock(mtx_);

        int id = header.pulse.value.sival_int;
        scoid_to_id_[header.pulse.scoid] = id;
        auto iter = clients_.find(id);

        if VUNLIKELY (iter != clients_.end()) {
          continue;
        }

        int back_coid = -1;
        int retry_cnt = 0;

        do {
          back_coid = ::name_open(QnxFactory::get_back_address(address_, id).c_str(), 0);

          if VUNLIKELY (back_coid < 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(10));

            if (++retry_cnt > 3) {
              break;
            }
          }
        } while (back_coid < 0 && !quit_flag_);

        if VUNLIKELY (back_coid < 0 && !quit_flag_) {
          VLOG_E("QnxFactory: Server name_open failed.");
          continue;
        }

        clients_.emplace(id, back_coid);
      } else if (header.pulse.code == _PULSE_CODE_DISCONNECT) {
        std::lock_guard lock(mtx_);
        int id = scoid_to_id_[header.pulse.scoid];

        auto iter = clients_.find(id);

        if VUNLIKELY (iter == clients_.end()) {
          continue;
        }

        ::name_close(iter->second);
        ::ConnectDetach(header.pulse.scoid);

        auto erased_id = iter->first;
        clients_.erase(iter);

        for (auto& [id, list] : channels_) {
          list.erase(erased_id);
        }

        traverse_sub_connect_callback([this](NodeImpl*, const auto& callback) { callback(!clients_.empty()); });
      } else if (header.pulse.code == _PULSE_CODE_CUSTOM_HEARTBEAT) {
        // Ignore.
      }
      continue;
    }

    if (header.msg.type == QnxMsg::kSend || header.msg.type == QnxMsg::kInvoke) {
      Bytes req_data;

      if (header.msg.size <= QnxMsg::kBufferSize) {
        req_data = Bytes::shallow_copy(header.msg.buffer, header.msg.size);
      } else {
        req_data = Bytes::create(header.msg.size);

        auto read_size = ::MsgRead(rcvid, req_data.data(), req_data.size(), sizeof(header));

        if VUNLIKELY (read_size != static_cast<ssize_t>(req_data.size())) {
          VLOG_E("QnxFactory: Server MsgRead failed.");

          ::MsgReply(rcvid, 0, nullptr, 0);
          continue;
        }
      }

      bool replied = false;

      traverse_req_resp_callback([this, &header, &req_data, &rcvid, &replied](NodeImpl* impl, const auto& callback) {
        const auto* conf_ptr = impl->get_target_conf<QnxConf>();

        if (conf_ptr->hash_code != header.msg.channel || impl->has_suspend) {
          ignore_called();
          return;
        }

        if VUNLIKELY (has_called()) {
          VLOG_F(*conf_ptr, "Two identical service requests.");
          return;
        }

        if (header.msg.type == QnxMsg::kSend) {
          ::MsgReply(rcvid, 0, nullptr, 0);
          replied = true;

          callback(0, req_data, nullptr);
        } else {
          Bytes resp_data;

          callback(0, req_data, &resp_data);

          QnxMsg msg;

          std::memset(&msg, 0, sizeof(QnxMsg));

          msg.size = resp_data.size();
          msg.type = QnxMsg::kResponse;

          if (resp_data.size() <= QnxMsg::kBufferSize) {
            std::memcpy(msg.buffer, resp_data.data(), resp_data.size());

            ::MsgReply(rcvid, 0, &msg, sizeof(msg));
            replied = true;
          } else {
            msg.other.token = resp_token;
            resp_cache_map_.emplace(resp_token, std::move(resp_data));

            ::MsgReply(rcvid, 0, &msg, sizeof(msg));
            replied = true;

            ++resp_token;
          }
        }
      });

      if VUNLIKELY (!replied) {
        ::MsgReply(rcvid, 0, nullptr, 0);
      }
    } else if (header.msg.type == QnxMsg::kGetResult) {
      auto iter = resp_cache_map_.find(header.msg.other.token);

      if VUNLIKELY (iter == resp_cache_map_.end()) {
        VLOG_E("QnxFactory: Server GetResult failed.");

        ::MsgReply(rcvid, 0, nullptr, 0);
        continue;
      }

      ::MsgReply(rcvid, 0, iter->second.data(), iter->second.size());

      resp_cache_map_.erase(iter);
    } else if (header.msg.type == QnxMsg::kSubscribe) {
      std::lock_guard lock(mtx_);

      auto iter = channels_.find(header.msg.channel);

      if VUNLIKELY (iter == channels_.end()) {
        iter = channels_.emplace(header.msg.channel, std::set<int>{}).first;
      }

      iter->second.emplace(header.msg.other.id);

      ::MsgReply(rcvid, 0, nullptr, 0);

      traverse_sub_connect_callback([this](NodeImpl*, const auto& callback) { callback(!clients_.empty()); });
    }
  }
}

// QnxClient
QnxClient::QnxClient(const QnxID& id) {
  static auto& factory = QnxFactory::get();

  std::tie(std::ignore, address_) = id;

  timer_.set_interval(10);
  timer_.set_loop_count(Timer::kInfinite);
  timer_.attach(&QnxFactory::get_message_loop());
}

QnxClient::~QnxClient() {
  quit_flag_ = true;

  if VUNLIKELY (is_busy_) {
    QnxFactory::get_message_loop().wait_for_idle();
  }

  timer_.stop();
  timer_.detach();

  disconnect();
}

std::any QnxClient::get_native_handle() const { return this; }

bool QnxClient::call(uint32_t channel, const Bytes& req_data, NodeImpl::MsgCallback&& callback) {
  if VUNLIKELY (coid_ < 0 || !back_fd_) {
    return false;
  }

  is_busy_ = true;

  QnxFactory::get_message_loop().post_task(
      [weak_self = weak_from_this(), channel, callback = std::move(callback), req_data] {
        auto self = weak_self.lock();

        if VUNLIKELY (!self) {
          return;
        }

        if VUNLIKELY (self->quit_flag_) {
          self->is_busy_ = false;
          return;
        }

        if VUNLIKELY (self->coid_ < 0 || !self->back_fd_) {
          self->is_busy_ = false;
          return;
        }

        int ret = -1;

        QnxHeader header;

        std::memset(&header, 0, sizeof(QnxHeader));

        header.msg.type = callback ? QnxMsg::kInvoke : QnxMsg::kSend;
        header.msg.size = req_data.size();
        header.msg.channel = channel;

        Bytes req_data_buf;

        if (req_data.size() <= QnxMsg::kBufferSize) {
          std::memcpy(header.msg.buffer, req_data.data(), req_data.size());
          req_data_buf = Bytes::shallow_copy(reinterpret_cast<uint8_t*>(&header), sizeof(header));
        } else {
          req_data_buf = Bytes::create(sizeof(header) + req_data.size());
          std::memcpy(req_data_buf.data(), &header, sizeof(header));
          std::memcpy(req_data_buf.data() + sizeof(header), req_data.data(), req_data.size());
        }

        if VLIKELY (callback) {
          QnxMsg resp_msg;

          std::memset(&resp_msg, 0, sizeof(QnxMsg));

          ret = ::MsgSend(self->coid_, req_data_buf.data(), req_data_buf.size(), &resp_msg, sizeof(resp_msg));

          if VUNLIKELY (ret < 0) {
            VLOG_E("QnxFactory: Client MsgSend failed.");
            self->is_busy_ = false;
            return;
          }

          if (resp_msg.size <= QnxMsg::kBufferSize) {
            Bytes resp_data = Bytes::shallow_copy(resp_msg.buffer, resp_msg.size);

            callback(resp_data);
          } else {
            std::memset(&header, 0, sizeof(QnxHeader));

            header.msg.type = QnxMsg::kGetResult;
            header.msg.size = 0;
            header.msg.other.token = resp_msg.other.token;

            Bytes resp_data = Bytes::create(resp_msg.size);

            ::MsgSend(self->coid_, &header, sizeof(header), resp_data.data(), resp_data.size());

            callback(resp_data);
          }
        } else {
          ret = ::MsgSend(self->coid_, req_data_buf.data(), req_data_buf.size(), 0, 0);

          if VUNLIKELY (ret < 0) {
            VLOG_E("QnxFactory: Client MsgSend failed.");

            self->is_busy_ = false;

            return;
          }
        }

        self->is_busy_ = false;
      });

  return true;
}

bool QnxClient::subscribe(uint32_t channel) {
  is_busy_ = true;

  QnxFactory::get_message_loop().post_task([weak_self = weak_from_this(), channel] {
    auto self = weak_self.lock();

    if VUNLIKELY (!self) {
      return;
    }

    if VUNLIKELY (self->quit_flag_) {
      self->is_busy_ = false;
      return;
    }

    if VUNLIKELY (self->coid_ < 0 || !self->back_fd_) {
      self->is_busy_ = false;
      return;
    }

    int ret = -1;

    QnxHeader header;

    std::memset(&header, 0, sizeof(QnxHeader));

    header.msg.type = QnxMsg::kSubscribe;
    header.msg.size = 0;
    header.msg.channel = channel;

    std::memset(header.msg.buffer, 0, QnxMsg::kBufferSize);

    header.msg.other.id = self->coid_ | QnxFactory::get_pid();

    ret = ::MsgSend(self->coid_, &header, sizeof(header), 0, 0);

    if VUNLIKELY (ret < 0) {
      VLOG_E("QnxFactory: Client MsgSend subscribe failed.");
      self->disconnect();
    }

    self->is_busy_ = false;
  });

  return true;
}

bool QnxClient::is_connected() const { return coid_ > 0; }

bool QnxClient::listen() {
  back_fd_ = ::name_attach(nullptr, QnxFactory::get_back_address(address_, coid_ | QnxFactory::get_pid()).c_str(), 0);

  if VUNLIKELY (!back_fd_) {
    VLOG_E("QnxFactory: Client name_attach failed.");
    return false;
  }

  thread_ = std::thread([this] {
    QnxHeader header;

    std::memset(&header, 0, sizeof(QnxHeader));

    _msg_info info;

    std::memset(&info, 0, sizeof(_msg_info));

    int rcvid = -1;

    while (!quit_flag_ && coid_ >= 0) {
      auto* back_fd = back_fd_.load();

      if VUNLIKELY (!back_fd) {
        break;
      }

      rcvid = ::MsgReceive(back_fd->chid, &header, sizeof(header), &info);

      if VUNLIKELY (rcvid < 0) {
        //          if (!quit_flag_ && coid_ >= 0) {
        //            VLOG_E("QnxClient MsgReceive failed.");
        //          }
        continue;
      }

      if (rcvid == 0) {
        if (header.pulse.code == _PULSE_CODE_DISCONNECT) {
          if VLIKELY (!quit_flag_) {
            is_busy_ = true;

            QnxFactory::get_message_loop().post_task([weak_self = weak_from_this()]() {
              auto self = weak_self.lock();

              if VUNLIKELY (!self) {
                return;
              }

              if VUNLIKELY (self->quit_flag_) {
                self->is_busy_ = false;
                return;
              }

              self->try_detect();

              self->is_busy_ = false;
            });
          }
        }

        continue;
      }

      if (header.msg.type == QnxMsg::kPublish) {
        Bytes msg_data = Bytes::create(header.msg.size);

        auto size = ::MsgRead(rcvid, msg_data.data(), msg_data.size(), sizeof(header));

        ::MsgReply(rcvid, 0, nullptr, 0);

        if VUNLIKELY (size != static_cast<ssize_t>(msg_data.size()) && !quit_flag_) {
          VLOG_E("QnxFactory: Client MsgRead failed.");
          continue;
        }

        traverse_msg_callback([this, &header, &msg_data, &rcvid](NodeImpl* impl, const auto& callback) {
          const auto* conf_ptr = impl->get_target_conf<QnxConf>();

          if (conf_ptr->hash_code != header.msg.channel || impl->has_suspend) {
            return;
          }

          callback(msg_data);
        });
      }
    }
  });

  return true;
}

void QnxClient::start_timer() {
  bool expected = false;

  if (!timer_started_.compare_exchange_strong(expected, true)) {
    return;
  }

  try_connect();

  std::weak_ptr<QnxClient> weak_self = weak_from_this();

  timer_.start([weak_self] {
    auto self = weak_self.lock();

    if VUNLIKELY (!self) {
      return;
    }

    if VUNLIKELY (self->quit_flag_) {
      return;
    }

    if (self->coid_ < 0 || !self->back_fd_) {
      self->timer_.set_interval(50);
      self->try_connect();
    } else {
      self->timer_.set_interval(100);
      self->try_detect();
    }
  });
}

void QnxClient::try_connect() {
  if VUNLIKELY (quit_flag_) {
    return;
  }

  if VLIKELY (coid_ < 0) {
    coid_ = ::name_open(address_.c_str(), 0);
    if VUNLIKELY (coid_ < 0) {
      return;
    }
  }

  if (!back_fd_) {
    if VUNLIKELY (!listen()) {
      disconnect();
      return;
    }
  }

  std::this_thread::sleep_for(std::chrono::microseconds(10));

  int ret = -1;
  int retry_cnt = 0;

  do {
    ret = ::MsgSendPulse(coid_, SIGEV_PULSE_PRIO_INHERIT, _PULSE_CODE_CUSTOM_CONNECT, coid_ | QnxFactory::get_pid());

    if VUNLIKELY (ret < 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(10));

      if (++retry_cnt > 3) {
        break;
      }
    }
  } while (ret < 0 && !quit_flag_);

  if VUNLIKELY (ret < 0 && !quit_flag_) {
    VLOG_E("QnxFactory: Client MsgSendPulse failed.");

    disconnect();

    return;
  }

  traverse_server_connect_callback([](NodeImpl*, const auto& callback) { callback(true); });
}

void QnxClient::try_detect() {
  if VUNLIKELY (quit_flag_) {
    return;
  }

  if VUNLIKELY (coid_ < 0) {
    return;
  }

  int ret = -1;
  int retry_cnt = 0;

  do {
    ret = ::MsgSendPulse(coid_, SIGEV_PULSE_PRIO_INHERIT, _PULSE_CODE_CUSTOM_HEARTBEAT, 0);

    if VUNLIKELY (ret < 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(10));

      if (++retry_cnt > 3) {
        break;
      }
    }
  } while (ret < 0 && !quit_flag_);

  if VUNLIKELY (ret < 0 && !quit_flag_) {
    disconnect();

    traverse_server_connect_callback([](NodeImpl*, const auto& callback) { callback(false); });
  }
}

void QnxClient::disconnect() {
  int coid = coid_.exchange(-1);

  if (coid > 0) {
    ::name_close(coid);
  }

  auto* back_fd = back_fd_.load();
  wake_channel(back_fd);

  if VLIKELY (thread_.joinable()) {
    thread_.join();
  }

  back_fd = back_fd_.exchange(nullptr);

  if (back_fd) {
    ::name_detach(back_fd, 0);
  }
}

}  // namespace vlink
