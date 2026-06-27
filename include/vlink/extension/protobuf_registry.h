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
 * @file protobuf_registry.h
 * @brief Header probe that enables Protobuf-backed schema-plugin code when available.
 *
 * @details
 * Unlike FlatBuffers, Protobuf already provides a process-wide registry of generated
 * descriptors through @c google::protobuf::DescriptorPool::generated_pool(), so VLink
 * does not need its own table for Protobuf schemas.  This header exists purely to:
 *
 * - Centralise the protobuf reflection includes used by the schema plugin.
 * - Probe for protobuf availability and expose a feature macro to the rest of the
 *   schema-plugin code.
 *
 * Registration API surface:
 *
 * | Source         | API used                                            | Notes                            |
 * | -------------- | --------------------------------------------------- | -------------------------------- |
 * | Generated code | @c DescriptorPool::generated_pool()                 | Populated by Protobuf at startup |
 * | Schema plugin  | @c VLINK_HAS_SCHEMA_PLUGIN_PROTOBUF (defined below) | Enables protobuf code paths      |
 *
 * @par Example
 * @code
 * #include <vlink/extension/protobuf_registry.h>
 *
 * #ifdef VLINK_HAS_SCHEMA_PLUGIN_PROTOBUF
 * auto* pool = google::protobuf::DescriptorPool::generated_pool();
 * if (auto* desc = pool->FindMessageTypeByName("demo.proto.PointCloud")) {
 *   // ... reflectively walk the message ...
 *   (void)desc;
 * }
 * #endif
 * @endcode
 */

#pragma once

/**
 * @brief Defined when the Protobuf reflection headers are visible in the build.
 *
 * @details
 * The probe relies on @c __has_include to detect the protobuf installation.  When the
 * macro is defined, dependent schema-plugin code may safely use protobuf descriptors,
 * reflection and dynamic messages.
 */
#if __has_include(<google/protobuf/dynamic_message.h>)
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>
#define VLINK_HAS_SCHEMA_PLUGIN_PROTOBUF
#endif
