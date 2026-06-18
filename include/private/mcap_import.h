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
 * @file mcap_import.h
 * @brief Private aggregation header that pulls in the MCAP reader / writer headers.
 *
 * @details
 * The MCAP C++ library is heavyweight and exposes a wide surface of templates that we do not
 * want to leak into public VLink headers.  This private, non-installed file centralises the
 * @c <mcap/reader.hpp> and @c <mcap/writer.hpp> inclusions and applies compile-time toggles
 * that match the VLink build configuration:
 *
 * | Macro                       | Behaviour                                                     |
 * | --------------------------- | ------------------------------------------------------------- |
 * | @c VLINK_ENABLE_ZSTD        | When unset, defines @c MCAP_COMPRESSION_NO_ZSTD               |
 * | @c MCAP_COMPRESSION_NO_LZ4  | Always defined; LZ4 is unsupported across VLink platforms     |
 *
 * Only the bag implementation files (@c bag_writer.cc, @c bag_reader.cc and friends) should
 * include this header; do not add it to any public VLink header.
 */

#pragma once

#ifndef VLINK_ENABLE_ZSTD
#ifndef MCAP_COMPRESSION_NO_ZSTD
#define MCAP_COMPRESSION_NO_ZSTD
#endif
#endif

#ifndef MCAP_COMPRESSION_NO_LZ4
#define MCAP_COMPRESSION_NO_LZ4
#endif

#include <mcap/reader.hpp>
#include <mcap/writer.hpp>
