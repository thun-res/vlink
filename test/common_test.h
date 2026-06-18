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

#ifndef DOCTEST_CONFIG_USE_STD_HEADERS
#define DOCTEST_CONFIG_USE_STD_HEADERS
#endif

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

#include "./extension/dynamic_data.h"
#include "./extension/qos_profile.h"
#include "./extension/status_detail.h"
#include "./vlink.h"

#ifdef VLINK_TEST_SUPPORT_FASTDDS_GEN
#if __has_include("test.hpp")
#include "test.hpp"
#elif __has_include("test.h")
#include "test.h"
#endif
#if __has_include("testPubSubTypes.hpp")
#include "testPubSubTypes.hpp"
#elif __has_include("testPubSubTypes.h")
#include "testPubSubTypes.h"
#endif

#endif

#ifdef VLINK_TEST_SUPPORT_PROTOBUF

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcpp"
#endif

#if defined(__ANDROID__) && __has_include("test/idl/test.pb.h")
#include "test/idl/test.pb.h"
#else
#include "test.pb.h"
#endif

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#endif

#ifdef VLINK_TEST_SUPPORT_FLATBUFFERS
#if __has_include("test.fbs.hpp")
#include "test.fbs.hpp"
#else
#include "test_generated.h"
#endif
#endif

// NOLINTNEXTLINE(build/namespaces, google-build-using-namespace, google-global-names-in-headers)
using namespace std::chrono_literals;
// NOLINTNEXTLINE(build/namespaces, google-build-using-namespace, google-global-names-in-headers)
using namespace vlink;

namespace common_test {}  // namespace common_test
