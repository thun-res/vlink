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

#pragma once

#include <memory>
#include <utility>

#include "./fdbus_factory.h"
#include "./impl/getter_impl.h"

namespace vlink {

// FdbusGetterImpl
class FdbusGetterImpl final : public GetterImpl {
 public:
  using Object = FdbusClient;
  static constexpr uint8_t kImplType = kClient | kSubscriber | kGetter;

  explicit FdbusGetterImpl(const FdbusConf& conf);

 private:
  void init() override;

  void deinit() override;

  bool suspend() override;

  bool resume() override;

  bool is_suspend() const override;

  const Conf* get_conf() const override;

  const AbstractNode* get_abstract_node() const override;

  bool attach(class MessageLoop* message_loop) override;

  bool detach() override;

  bool listen(MsgCallback&& callback) override;

  void subscribe();

  FdbusConf conf_;
  std::shared_ptr<Object> object_;
  bool has_subscribed_{false};

  VLINK_DISALLOW_COPY_AND_ASSIGN(FdbusGetterImpl)
};

}  // namespace vlink
