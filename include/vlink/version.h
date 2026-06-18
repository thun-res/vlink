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
 * @file version.h
 * @brief VLink library version constants, build-time feature flags, and compile-time comparison helpers.
 *
 * @details
 * This header is the single source of truth for the VLink library version
 * triple and for the set of optional subsystems compiled into the build.
 * When the CMake-generated @c version_config.h is present in the include
 * tree it is preferred; otherwise the fallback @c #define block below
 * supplies the default values used by header-only or out-of-tree builds.
 *
 * @par Version Macros
 * | Macro                         | Type             | Meaning                                             |
 * | ----------------------------- | ---------------- | --------------------------------------------------- |
 * | @c VLINK_VERSION_MAJOR        | integer literal  | Major version component.                            |
 * | @c VLINK_VERSION_MINOR        | integer literal  | Minor version component.                            |
 * | @c VLINK_VERSION_PATCH        | integer literal  | Patch version component.                            |
 * | @c VLINK_VERSION              | string literal   | Dot-separated version string, e.g. @c "2.0.0".      |
 * | @c VLINK_VERSION_TIMESTAMP    | string literal   | Build timestamp (empty in fallback builds).         |
 * | @c VLINK_VERSION_TAG          | string literal   | Git tag (empty in fallback builds).                 |
 * | @c VLINK_VERSION_COMMIT_ID    | string literal   | Git commit hash (empty in fallback builds).         |
 * | @c VLINK_VERSION_CHECK(a,b,c) | 24-bit integer   | Packs a version triple for compile-time compare.    |
 * | @c VLINK_VERSION_VALUE        | 24-bit integer   | Encoded current version for use with the check.     |
 *
 * @par Feature-flag Macros (selected subset)
 * | Macro                       | Subsystem                                            |
 * | --------------------------- | ---------------------------------------------------- |
 * | @c VLINK_ENABLE_CXX_STD_20  | Compile-time opt-in to C++20 facilities.             |
 * | @c VLINK_ENABLE_C_API       | Pure C wrapper API (see @c external/c_api.h).        |
 * | @c VLINK_ENABLE_SECURITY    | Built-in security backends and @c Security* nodes.   |
 * | @c VLINK_ENABLE_SQLITE      | SQLite-backed bag storage.                           |
 * | @c VLINK_ENABLE_ZSTD        | Zstd compression for bag files (off by default).     |
 * | @c VLINK_ENABLE_PROXY       | Proxy monitoring API (see @c external/proxy_api.h).  |
 * | @c VLINK_ENABLE_CLI_*       | Individual CLI sub-commands (info, bag, monitor...). |
 * | @c VLINK_ENABLE_LOG_*       | Optional logging back-ends (NAT, SPD, DLT, QUI).     |
 * | @c VLINK_ENABLE_TEST        | Unit-test build artefacts.                           |
 *
 * @par Compile-time Version Check
 * @code
 * #include <vlink/version.h>
 *
 * #if VLINK_VERSION_VALUE >= VLINK_VERSION_CHECK(2, 0, 0)
 *   // Safe to use APIs introduced in VLink 2.0.0
 * #endif
 * @endcode
 *
 * @par Runtime Version Read
 * @code
 * #include <vlink/version.h>
 * #include <iostream>
 *
 * void print_vlink_version() {
 *   std::cout << "VLink " << VLINK_VERSION << "\n"
 *             << "  major = " << VLINK_VERSION_MAJOR << "\n"
 *             << "  minor = " << VLINK_VERSION_MINOR << "\n"
 *             << "  patch = " << VLINK_VERSION_PATCH << "\n";
 * }
 * @endcode
 */

#pragma once

#if __has_include("./vlink/version_config.h")

#include "./vlink/version_config.h"

#else

#define VLINK_VERSION_MAJOR 2
#define VLINK_VERSION_MINOR 0
#define VLINK_VERSION_PATCH 0
#define VLINK_VERSION "2.0.0"
#define VLINK_VERSION_TIMESTAMP ""
#define VLINK_VERSION_TAG ""
#define VLINK_VERSION_COMMIT_ID ""

#ifndef VLINK_ENABLE_CXX_STD_20
#if __cplusplus >= 202002L
#define VLINK_ENABLE_CXX_STD_20
#endif
#endif

#ifndef VLINK_ENABLE_C_API
#define VLINK_ENABLE_C_API
#endif

#ifndef VLINK_ENABLE_SECURITY
#define VLINK_ENABLE_SECURITY
#endif

#ifndef VLINK_ENABLE_ZSTD
// #define VLINK_ENABLE_ZSTD
#endif

#ifndef VLINK_ENABLE_SQLITE
#define VLINK_ENABLE_SQLITE
#endif

#ifndef VLINK_ENABLE_CLI_INFO
#define VLINK_ENABLE_CLI_INFO
#endif

#ifndef VLINK_ENABLE_CLI_BAG
#define VLINK_ENABLE_CLI_BAG
#endif

#ifndef VLINK_ENABLE_CLI_BENCH
#define VLINK_ENABLE_CLI_BENCH
#endif

#ifndef VLINK_ENABLE_CLI_EPROTO
#define VLINK_ENABLE_CLI_EPROTO
#endif

#ifndef VLINK_ENABLE_CLI_EFBS
#define VLINK_ENABLE_CLI_EFBS
#endif

#ifndef VLINK_ENABLE_CLI_LIST
#define VLINK_ENABLE_CLI_LIST
#endif

#ifndef VLINK_ENABLE_CLI_MONITOR
#define VLINK_ENABLE_CLI_MONITOR
#endif

#ifndef VLINK_ENABLE_CLI_DUMP
#define VLINK_ENABLE_CLI_DUMP
#endif

#ifndef VLINK_ENABLE_CLI_CHECK
#define VLINK_ENABLE_CLI_CHECK
#endif

#ifndef VLINK_ENABLE_LOG_QUI
// #define VLINK_ENABLE_LOG_QUI
#endif

#ifndef VLINK_ENABLE_LOG_SPD
// #define VLINK_ENABLE_LOG_SPD
#endif

#ifndef VLINK_ENABLE_LOG_DLT
// #define VLINK_ENABLE_LOG_DLT
#endif

#ifndef VLINK_ENABLE_LOG_NAT
#define VLINK_ENABLE_LOG_NAT
#endif

#ifndef VLINK_ENABLE_EXPRTK
#define VLINK_ENABLE_EXPRTK
#endif

#ifndef VLINK_ENABLE_PROXY
#define VLINK_ENABLE_PROXY
#endif

#ifndef VLINK_ENABLE_VIEWER
// #define VLINK_ENABLE_VIEWER
#endif

#ifndef VLINK_ENABLE_WEBVIZ
// #define VLINK_ENABLE_WEBVIZ
#endif

#ifndef VLINK_ENABLE_SYMLINKS
// #define VLINK_ENABLE_SYMLINKS
#endif

#ifndef VLINK_ENABLE_COMPLETIONS
// #define VLINK_ENABLE_COMPLETIONS
#endif

#ifndef VLINK_ENABLE_EXAMPLES
// #define VLINK_ENABLE_EXAMPLES
#endif

#ifndef VLINK_ENABLE_EXAMPLES_ALL
// #define VLINK_ENABLE_EXAMPLES_ALL
#endif

#ifndef VLINK_ENABLE_TEST
#define VLINK_ENABLE_TEST
#endif

#endif

/**
 * @def VLINK_VERSION_CHECK
 * @brief Packs a (major, minor, patch) triple into a single 24-bit integer.
 *
 * @details
 * Stores @p major in bits 23-16, @p minor in bits 15-8, and @p patch in
 * bits 7-0.  Use together with @c VLINK_VERSION_VALUE to write compile-time
 * version guards.
 *
 * @param major  Major component (0-255).
 * @param minor  Minor component (0-255).
 * @param patch  Patch component (0-255).
 * @return       Encoded 24-bit integer.
 */
#define VLINK_VERSION_CHECK(major, minor, patch) (((major) << 16) | ((minor) << 8) | (patch))

/**
 * @def VLINK_VERSION_VALUE
 * @brief The encoded 24-bit value for the current library version.
 *
 * @details
 * Equivalent to
 * @c VLINK_VERSION_CHECK(VLINK_VERSION_MAJOR, VLINK_VERSION_MINOR, VLINK_VERSION_PATCH).
 * Compare against the output of @c VLINK_VERSION_CHECK to gate code paths
 * on a minimum library version at compile time.
 */
#define VLINK_VERSION_VALUE VLINK_VERSION_CHECK(VLINK_VERSION_MAJOR, VLINK_VERSION_MINOR, VLINK_VERSION_PATCH)
