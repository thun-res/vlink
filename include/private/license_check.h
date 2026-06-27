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

/**
 * @file license_check.h
 * @brief Internal license-verification helper used by the VLink runtime bootstrap.
 *
 * @details
 * This is a private, non-installed header.  It is consumed only by the VLink library source
 * and must never be included by application code.  The helper runs an out-of-band
 * @c MessageLoop that periodically validates the active deployment licence; the exact
 * verification protocol is intentionally hidden inside the PImpl @c Impl.
 *
 * Because the file participates in the security perimeter of the runtime, the public API
 * exposed by @c LicenseCheck is deliberately narrow:
 *
 * | Member       | Role                                                       |
 * | ------------ | ---------------------------------------------------------- |
 * | constructor  | Spin up the verification loop and load the licence blob    |
 * | destructor   | Stop the loop and clear any in-memory credential material  |
 * | @c do_check  | Execute one verification pass on the calling thread        |
 */

#pragma once

#include <memory>

#include "./base/message_loop.h"

namespace vlink {

/**
 * @class LicenseCheck
 * @brief Private MessageLoop driver that validates the runtime licence at start-up and on demand.
 */
class LicenseCheck final : protected MessageLoop {
 public:
  LicenseCheck();

  ~LicenseCheck() override;

  /** @brief Run a single, synchronous licence-verification pass on the calling thread. */
  void do_check();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(LicenseCheck)
};

}  // namespace vlink
