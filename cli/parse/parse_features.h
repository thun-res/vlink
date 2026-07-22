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

#if __has_include(<google/protobuf/compiler/importer.h>) && __has_include(<google/protobuf/text_format.h>)

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcpp"
#endif

#include <google/protobuf/stubs/common.h>

#if GOOGLE_PROTOBUF_VERSION >= 3004000
#define VLINK_HAS_PROTOBUF_COMPILER
#endif

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#endif

#if __has_include(<flatbuffers/idl.h>)
#include <flatbuffers/base.h>

#if FLATBUFFERS_VERSION_MAJOR >= 22
#define VLINK_HAS_FBS_COMPILER
#endif

#endif

#ifdef _WIN32
#undef min
#undef max
#undef GetMessage
#undef ERROR
#endif
