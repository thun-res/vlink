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
 * @file launcher_notify.h
 * @brief Process-side notification and watchdog client library for @c LauncherServer
 *        (analogous to systemd @c sd_notify).
 *
 * @details
 * @c LauncherNotify provides a lightweight, dependency-free datagram client that managed
 * @c [[process]] executables can call to report readiness, watchdog heartbeats, health
 * state, status text, reload/stop transitions, and startup timeout extensions back to the
 * supervising @c LauncherServer.
 *
 * When a @c [[process]] unit is launched, @c LauncherServer automatically injects the
 * notification endpoint and per-launch authentication token into the child environment.  Calling
 * @c LauncherNotify methods outside a managed process is safe and returns @c false (or @c 0 /
 * empty string for query helpers) without blocking or throwing.
 *
 * @par Injected Environment Variables
 *
 * | Variable                       | Description                                                          |
 * | ------------------------------ | -------------------------------------------------------------------- |
 * | @c VLINK_LAUNCHER_NOTIFY_ADDR  | Datagram endpoint (@c unix:/path/to/sock or @c udp:127.0.0.1:port). |
 * | @c VLINK_LAUNCHER_NOTIFY_TOKEN | Per-launch 128-bit hex token authenticating the unit and generation. |
 * | @c VLINK_LAUNCHER_UNIT_NAME    | Logical unit name assigned to the managed process.                   |
 * | @c VLINK_LAUNCHER_UNIT_GEN     | Monotonic launch generation assigned to this process instance.       |
 * | @c VLINK_LAUNCHER_WATCHDOG_MS  | Configured watchdog timeout in milliseconds (@c 0 when disabled).    |
 *
 * @par Wire Protocol Keys
 * Payloads are UTF-8 newline-delimited @c KEY=VALUE datagrams (max 4096 bytes):
 * - @c READY=1: Mark a @c ready.type="notify" (or @c "none") process as ready.
 * - @c WATCHDOG=1: Refresh the process watchdog heartbeat timestamp.
 * - @c WATCHDOG=trigger: Immediately trigger watchdog fault recovery.
 * - @c HEALTH=1 / @c HEALTH=0: Report healthy or degraded state in @c UnitStatus.
 * - @c STATUS=<text>: Update human-readable detail text in @c UnitStatus (max 1024 bytes).
 * - @c RELOADING=1: Emit an @c on_reloading lifecycle event.
 * - @c STOPPING=1: Emit an @c on_stopping lifecycle event.
 * - @c EXTEND_TIMEOUT_MS=<ms> (or @c EXTEND_TIMEOUT_USEC=<us>): Extend readiness timeout.
 *
 * @par Example
 * @code
 * #include <vlink/external/launcher_notify.h>
 *
 * int main() {
 *   using vlink::LauncherNotify;
 *   // ... initialise service ...
 *   LauncherNotify::notify_ready("service listening");
 *
 *   const uint32_t wd_ms = LauncherNotify::watchdog_interval_ms();
 *   while (running) {
 *     // ... work ...
 *     if (wd_ms > 0U) {
 *       LauncherNotify::notify_watchdog();
 *     }
 *   }
 *   LauncherNotify::notify_stopping("draining connections");
 *   return 0;
 * }
 * @endcode
 *
 * @note
 * - All @c notify*() functions are non-blocking and safe to call from any thread.
 * - Newlines and NUL bytes inside @p status / @p reason arguments are sanitised before
 *   transmission so free-form text cannot forge additional protocol keys.
 *
 * @see launcher_api.h, launcher_server.h
 */

#pragma once

/// @cond
#undef VLINK_LAUNCHER_API_EXPORT
#ifdef VLINK_LAUNCHER_API_LIBRARY_STATIC
#define VLINK_LAUNCHER_API_EXPORT
#elif defined(_WIN32) || defined(__CYGWIN__)
#ifdef VLINK_LAUNCHER_API_LIBRARY
#define VLINK_LAUNCHER_API_EXPORT __declspec(dllexport)
#else
#define VLINK_LAUNCHER_API_EXPORT __declspec(dllimport)
#endif
#else
#define VLINK_LAUNCHER_API_EXPORT __attribute__((visibility("default")))
#endif
/// @endcond

#include <cstdint>
#include <string>
#include <string_view>

namespace vlink {

/**
 * @class LauncherNotify
 * @brief Static process-to-launcher notification and watchdog client (similar to @c sd_notify).
 */
class VLINK_LAUNCHER_API_EXPORT LauncherNotify final {
 public:
  LauncherNotify() = delete;
  ~LauncherNotify() = delete;
  LauncherNotify(const LauncherNotify&) = delete;
  LauncherNotify& operator=(const LauncherNotify&) = delete;
  LauncherNotify(LauncherNotify&&) = delete;
  LauncherNotify& operator=(LauncherNotify&&) = delete;

  /**
   * @brief Check whether this process was launched with a valid Launcher notify channel.
   * @return @c true when both @c VLINK_LAUNCHER_NOTIFY_ADDR and @c VLINK_LAUNCHER_NOTIFY_TOKEN
   *         are present and non-empty in the process environment.
   */
  [[nodiscard]] static bool is_managed() noexcept;

  /**
   * @brief Return the logical unit name injected via @c VLINK_LAUNCHER_UNIT_NAME.
   * @return Unit name string, or an empty string when not managed.
   */
  [[nodiscard]] static std::string unit_name();

  /**
   * @brief Return the launch generation injected via @c VLINK_LAUNCHER_UNIT_GEN.
   * @return Positive monotonic generation counter, or @c 0 when unavailable/invalid.
   */
  [[nodiscard]] static uint64_t unit_generation() noexcept;

  /**
   * @brief Return the watchdog timeout in milliseconds injected via @c VLINK_LAUNCHER_WATCHDOG_MS.
   * @return Watchdog timeout in milliseconds, or @c 0 when watchdog monitoring is disabled.
   * @note Applications should send @c notify_watchdog() at roughly half of this interval.
   */
  [[nodiscard]] static uint32_t watchdog_interval_ms() noexcept;

  /**
   * @brief Send a raw newline-delimited @c KEY=VALUE notification datagram to @c LauncherServer.
   * @param state One or more @c KEY=VALUE assignments separated by @c '\\n'.
   * @return @c true if the datagram was handed to the OS socket without error; @c false when
   *         unmanaged, payload is empty/oversized, or socket delivery fails.
   */
  static bool notify(std::string_view state);

  /**
   * @brief Report that the process has finished initialisation and is ready to serve traffic.
   * @param status Optional human-readable status line recorded in @c UnitStatus::detail.
   * @return @c true if the notification datagram was sent.
   */
  static bool notify_ready(std::string_view status = "");

  /**
   * @brief Report that the process is reloading its configuration.
   * @param status Optional human-readable status line attached to the @c on_reloading event.
   * @return @c true if the notification datagram was sent.
   */
  static bool notify_reloading(std::string_view status = "");

  /**
   * @brief Report that the process is initiating a graceful shutdown.
   * @param status Optional human-readable status line attached to the @c on_stopping event.
   * @return @c true if the notification datagram was sent.
   */
  static bool notify_stopping(std::string_view status = "");

  /**
   * @brief Update the unit's human-readable detail text in @c UnitStatus::detail.
   * @param status Free-form status message (sanitised to a single line, max 1024 bytes).
   * @return @c true if the notification datagram was sent.
   */
  static bool notify_status(std::string_view status);

  /**
   * @brief Report the process's self-diagnosed health state to @c LauncherServer.
   * @param healthy @c true if healthy, @c false if degraded/unhealthy.
   * @param status Optional detail message recorded alongside the health update.
   * @return @c true if the notification datagram was sent.
   */
  static bool notify_health(bool healthy, std::string_view status = "");

  /**
   * @brief Refresh the process watchdog heartbeat (@c WATCHDOG=1).
   * @return @c true if the heartbeat datagram was sent.
   */
  static bool notify_watchdog();

  /**
   * @brief Immediately trigger watchdog fault recovery (@c WATCHDOG=trigger).
   * @param reason Optional diagnostic reason recorded in @c UnitStatus::detail.
   * @return @c true if the trigger datagram was sent.
   */
  static bool notify_watchdog_trigger(std::string_view reason = "");

  /**
   * @brief Request an extension of the readiness probe timeout (@c EXTEND_TIMEOUT_MS).
   * @param extra_ms Additional milliseconds to wait from the moment the server receives the
   *                 notification (clamped server-side to @c [100, 600000] ms).
   * @return @c true if the extension datagram was sent.
   */
  static bool notify_extend_timeout(uint32_t extra_ms);
};

}  // namespace vlink
