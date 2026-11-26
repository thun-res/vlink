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
 * @file timer_component.h
 * @brief Periodic timer-driven component base class built on @c LauncherComponent.
 *
 * @details
 * Defines @c LauncherTimerComponent, which configures a periodic @c vlink::Timer from
 * @c interval_ms / @c period_ms parameters, refreshes the component watchdog on each tick,
 * and suspends/resumes cleanly across @c on_deactivate() / @c on_activate().
 *
 * @see component.h, context.h, launcher_component.h
 */

#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>
#include <variant>

#include "../../base/macros.h"
#include "../../base/timer.h"
#include "./component.h"
#include "./context.h"

namespace vlink {

/**
 * @class LauncherTimerComponent
 * @brief Periodic timer-driven component base class.
 *
 * @details
 * Reads the @c interval_ms (or @c period_ms) parameter during @c on_configure(), binds a
 * @c vlink::Timer to the component's assigned @c MessageLoop during @c on_start(), calls
 * @c on_tick() on each period while refreshing the watchdog, supports live interval updates
 * via @c on_param_changed(), and stops/detaches the timer in @c on_deactivate() and
 * @c on_stop().
 */
class LauncherTimerComponent : public LauncherComponent {
 public:
  /**
   * @brief Constructs a periodic component with a fallback default interval.
   *
   * @param default_interval_ms  Default tick interval in milliseconds when not configured.
   */
  explicit LauncherTimerComponent(uint32_t default_interval_ms = 1000U) noexcept;

  /**
   * @brief Virtual destructor.
   */
  ~LauncherTimerComponent() override = default;

  /**
   * @brief Parses @c interval_ms / @c period_ms and delegates to @c on_timer_configure().
   *
   * @param ctx  Component execution context.
   * @return     @c true when the interval is in @c [1, UINT32_MAX] and @c on_timer_configure() succeeded.
   */
  bool on_configure(LauncherContext& ctx) override;

  /**
   * @brief Runs @c on_timer_start(), arms the periodic timer, pings the watchdog, and marks ready.
   *
   * @param ctx  Component execution context.
   * @return     @c true on success; @c false if @c on_timer_start() fails.
   */
  bool on_start(LauncherContext& ctx) override;

  /**
   * @brief Re-arms the periodic timer when resuming from @c kInactive standby and marks ready.
   *
   * @param ctx  Component execution context.
   * @return     @c true on success.
   */
  bool on_activate(LauncherContext& ctx) override;

  /**
   * @brief Stops and releases the periodic timer while entering @c kInactive standby.
   */
  void on_deactivate() override;

  /**
   * @brief Stops the periodic timer and invokes @c on_timer_stop().
   */
  void on_stop() override;

  /**
   * @brief Applies live updates to @c interval_ms / @c period_ms and forwards other keys.
   *
   * @param key    Parameter key.
   * @param value  Candidate parameter value.
   * @return       @c true when accepted; @c false when rejected.
   */
  bool on_param_changed(std::string_view key, const LauncherParamValue& value) override;

  /**
   * @brief Returns the active tick interval in milliseconds.
   *
   * @return Current timer interval.
   */
  [[nodiscard]] uint32_t interval_ms() const noexcept;

 protected:
  /**
   * @brief Optional configuration hook invoked during @c on_configure() after interval parsing.
   *
   * @param ctx  Component execution context.
   * @return     @c true on success.
   */
  virtual bool on_timer_configure(LauncherContext& ctx);

  /**
   * @brief Optional startup hook invoked during @c on_start() before the periodic timer is armed.
   *
   * @param ctx  Component execution context.
   * @return     @c true on success.
   */
  virtual bool on_timer_start(LauncherContext& ctx);

  /**
   * @brief Periodic business tick invoked on the component's @c MessageLoop while active.
   *
   * @param ctx  Component execution context.
   * @return     @c true if healthy; @c false to report degraded health via @c report_health().
   */
  virtual bool on_tick(LauncherContext& ctx) = 0;

  /**
   * @brief Optional stop hook invoked during @c on_stop() after the periodic timer has stopped.
   */
  virtual void on_timer_stop();

  /**
   * @brief Optional dynamic parameter hook invoked for keys other than @c interval_ms / @c period_ms.
   *
   * @param key    Parameter key.
   * @param value  Candidate parameter value.
   * @return       @c true to accept the update; @c false to reject it.
   */
  virtual bool on_timer_param_changed(std::string_view key, const LauncherParamValue& value);

 private:
  void start_timer_internal(LauncherContext& ctx);

  void stop_timer_internal();

  uint32_t default_interval_ms_{1000U};
  uint32_t interval_ms_{1000U};
  LauncherContext* ctx_{nullptr};
  std::unique_ptr<Timer> timer_;
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

inline LauncherTimerComponent::LauncherTimerComponent(uint32_t default_interval_ms) noexcept
    : default_interval_ms_(default_interval_ms), interval_ms_(default_interval_ms) {}

inline bool LauncherTimerComponent::on_configure(LauncherContext& ctx) {
  auto configured = static_cast<int64_t>(default_interval_ms_);

  if (ctx.has_param("interval_ms")) {
    configured = ctx.get_param_int("interval_ms", -1);
  } else if (ctx.has_param("period_ms")) {
    configured = ctx.get_param_int("period_ms", -1);
  }

  if (configured <= 0 || configured > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
    return false;
  }

  interval_ms_ = static_cast<uint32_t>(configured);
  return on_timer_configure(ctx);
}

inline bool LauncherTimerComponent::on_start(LauncherContext& ctx) {
  ctx_ = &ctx;

  if (!on_timer_start(ctx)) {
    ctx_ = nullptr;
    return false;
  }

  start_timer_internal(ctx);
  ctx.watchdog_ping();
  ctx.mark_ready();
  return true;
}

inline bool LauncherTimerComponent::on_activate(LauncherContext& ctx) {
  ctx_ = &ctx;

  if (!timer_) {
    start_timer_internal(ctx);
  }

  ctx.watchdog_ping();
  ctx.mark_ready();
  return true;
}

inline void LauncherTimerComponent::on_deactivate() { stop_timer_internal(); }

inline void LauncherTimerComponent::on_stop() {
  stop_timer_internal();
  on_timer_stop();
  ctx_ = nullptr;
}

inline bool LauncherTimerComponent::on_param_changed(std::string_view key, const LauncherParamValue& value) {
  if (key == "interval_ms" || key == "period_ms") {
    const auto* iptr = std::get_if<int64_t>(&value);

    if (iptr == nullptr || *iptr <= 0 || *iptr > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
      return false;
    }

    interval_ms_ = static_cast<uint32_t>(*iptr);

    if (timer_) {
      timer_->set_interval(interval_ms_);
    }

    return true;
  }

  return on_timer_param_changed(key, value);
}

inline uint32_t LauncherTimerComponent::interval_ms() const noexcept { return interval_ms_; }

inline bool LauncherTimerComponent::on_timer_configure(LauncherContext& ctx) {
  (void)ctx;
  return true;
}

inline bool LauncherTimerComponent::on_timer_start(LauncherContext& ctx) {
  (void)ctx;
  return true;
}

inline void LauncherTimerComponent::on_timer_stop() {}

inline bool LauncherTimerComponent::on_timer_param_changed(std::string_view key, const LauncherParamValue& value) {
  (void)key;
  (void)value;
  return true;
}

inline void LauncherTimerComponent::start_timer_internal(LauncherContext& ctx) {
  timer_ = ctx.create_timer(interval_ms_, [this] {
    if VUNLIKELY (ctx_ == nullptr) {
      return;
    }

    ctx_->watchdog_ping();

    if (!on_tick(*ctx_)) {
      ctx_->report_health(false, "timer tick returned false");
    }
  });
  timer_->start();
}

inline void LauncherTimerComponent::stop_timer_internal() {
  if (timer_) {
    timer_->stop();
    timer_.reset();
  }
}

}  // namespace vlink
