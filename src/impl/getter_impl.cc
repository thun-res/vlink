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

#include "./impl/getter_impl.h"

namespace vlink {

// GetterImpl
GetterImpl::~GetterImpl() = default;

GetterImpl::GetterImpl() : NodeImpl(kGetter) {}

void GetterImpl::set_latency_and_lost_enabled(bool enable) { (void)enable; }

bool GetterImpl::is_latency_and_lost_enabled() const { return false; }

int64_t GetterImpl::get_latency() const { return 0; }

SampleLostInfo GetterImpl::get_lost() const { return SampleLostInfo(); }

}  // namespace vlink
