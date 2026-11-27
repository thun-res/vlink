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

// NOLINTBEGIN

#include "reusable_test_node.h"

VLINK_LAUNCHER_MODULE_BEGIN(1, 0)
VLINK_LAUNCHER_EXPORT_COMPONENT("ReusableTestNode", ReusableTestNode)
VLINK_LAUNCHER_EXPORT_COMPONENT("MinimalTestNode", MinimalTestNode)
VLINK_LAUNCHER_EXPORT_COMPONENT("TimerTestNode", TimerTestNode)
#ifdef VLINK_ENABLE_COROUTINE
VLINK_LAUNCHER_EXPORT_COMPONENT("CoroTestNode", CoroTestNode)
#endif
VLINK_LAUNCHER_MODULE_END()

// NOLINTEND
