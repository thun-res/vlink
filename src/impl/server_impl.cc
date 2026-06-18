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

#include "./impl/server_impl.h"

#include "./base/logger.h"

namespace vlink {

// ServerImpl
ServerImpl::~ServerImpl() = default;

bool ServerImpl::has_clients() const { return false; }

bool ServerImpl::reply(uint64_t req_id, const Bytes& resp_data, bool is_sync) {
  (void)req_id;
  (void)resp_data;

  if VUNLIKELY (!is_sync) {
    VLOG_W("Function [reply] is not supported.");
  }

  return false;
}

ServerImpl::ServerImpl() : NodeImpl(kServer) {}

}  // namespace vlink
