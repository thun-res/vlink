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

#include "./private/license_check.h"

#include <cmath>

#include "./base/elapsed_timer.h"
#include "./base/logger.h"

namespace vlink {

// LicenseCheck::Impl
struct LicenseCheck::Impl final {
  Timer check_timer;
  ElapsedTimer elapsed_timer;
};

// LicenseCheck
LicenseCheck::LicenseCheck() : impl_(std::make_unique<Impl>()) {
  set_name("LicenseCheck");

  impl_->check_timer.set_interval(1000 * 10);
  impl_->check_timer.set_loop_count(Timer::kInfinite);

  impl_->check_timer.attach(this);
  impl_->elapsed_timer.start();
  impl_->check_timer.start([this]() { do_check(); });

  async_run();

  do_check();
}

LicenseCheck::~LicenseCheck() {
  quit();

  wait_for_quit();
}

void LicenseCheck::do_check() {
  static constexpr int kTimeoutInterval = 1000 * 600;

  auto pass_time = impl_->elapsed_timer.get();

  if VUNLIKELY (pass_time >= kTimeoutInterval) {
    impl_->check_timer.set_interval(1);
    abort();
  }

  CLOG_W("VLink trial will end in %.0f seconds.", std::round((kTimeoutInterval - pass_time) / 1000.0F));
}

}  // namespace vlink
