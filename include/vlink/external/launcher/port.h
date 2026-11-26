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
 * @file port.h
 * @brief Lifecycle-aware @c LauncherPublisher and @c LauncherSubscriber port handles and factories.
 *
 * @details
 * Defines the managed pub/sub wrappers returned by @c LauncherContext::create_publisher() and
 * @c LauncherContext::create_subscriber(), integrating lifecycle active-state gating, unload
 * task-guard protection, callback CPU/log scoping, and runtime active bag recording hooks.
 *
 * @see context.h, launcher_component.h, publisher.h, subscriber.h
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "../../base/bytes.h"
#include "../../base/logger.h"
#include "../../base/macros.h"
#include "../../base/memory_resource.h"
#include "../../publisher.h"
#include "../../subscriber.h"
#include "./context.h"

namespace vlink {

/**
 * @class LauncherPublisher
 * @brief Lifecycle-aware, shared-ownership publisher handle bound to a declared output port.
 *
 * @details
 * Wraps @c vlink::Publisher\<MsgT\> together with a shared view of the owning component's
 * lifecycle active flag.  Components offloading work onto a @c ThreadPool may copy a
 * @c LauncherPublisher handle into compute tasks safely without risking use-after-free on stop,
 * while deactivation immediately suppresses outbound traffic from stale tasks.
 *
 * @tparam MsgT  Published message type.
 */
template <typename MsgT>
class LauncherPublisher final {
 public:
  /**
   * @brief Constructs an empty, invalid publisher handle.
   */
  LauncherPublisher() = default;

  /**
   * @brief Constructs a managed publisher handle bound to an initialised @c Publisher and lifecycle flags.
   *
   * @param pub                 Shared underlying @c Publisher\<MsgT\> instance.
   * @param active_flag         Optional shared atomic active flag from @c LauncherContext::active_flag().
   * @param url                 Resolved VLink URL string.
   * @param force_publish_flag  Optional shared flag enabling forced publish during active recording.
   */
  LauncherPublisher(std::shared_ptr<Publisher<MsgT>> pub, std::shared_ptr<const std::atomic_bool> active_flag,
                    std::string url, std::shared_ptr<const std::atomic_bool> force_publish_flag = nullptr);

  /**
   * @brief Reports whether this handle holds an initialised publisher.
   *
   * @return @c true when usable; @c false when empty.
   */
  [[nodiscard]] bool valid() const noexcept;

  /**
   * @brief Explicit boolean conversion equivalent to @c valid().
   *
   * @return @c true when usable; @c false when empty.
   */
  explicit operator bool() const noexcept;

  /**
   * @brief Returns the resolved VLink URL bound to this publisher.
   *
   * @return Resolved URL string.
   */
  [[nodiscard]] const std::string& url() const noexcept;

  /**
   * @brief Publishes @p msg when the owning component is in the active lifecycle state.
   *
   * @param msg    Payload to publish.
   * @param force  When @c true, forces serialisation/recording even with no subscriber attached.
   * @return       @c true when handed to the transport; @c false if inactive or invalid.
   */
  bool publish(const MsgT& msg, bool force = false) const;

  /**
   * @brief Publishes an owned @c Bytes buffer by move when the owning component is active.
   *
   * @tparam U     SFINAE guard restricting this overload to @c MsgT == @c Bytes.
   * @param msg    Owned payload moved into the transport without deep copy.
   * @param force  When @c true, forces serialisation/recording even with no subscriber attached.
   * @return       @c true when handed to the transport; @c false if inactive or invalid.
   */
  template <typename U = MsgT, std::enable_if_t<std::is_same_v<U, Bytes> && std::is_same_v<MsgT, Bytes>, int> = 0>
  bool publish(Bytes&& msg, bool force = false) const;

  /**
   * @brief Enables or disables per-node active bag recording on this publisher.
   *
   * @details
   * Non-empty @p path delegates to @c Publisher::set_record_path(); an empty path stops
   * recording.  @c intra:// publishers are skipped with a warning because VLink's per-node
   * recorder does not support @c intra://.
   *
   * @param path  Target bag path (@c .vdb / @c .vdbx / @c .vcap / @c .vcapx) or empty string to disable.
   * @return      @c true when applied to the underlying publisher; @c false if invalid or unsupported.
   */
  bool set_record_path(const std::string& path) const;

  /**
   * @brief Reports whether the underlying transport supports zero-copy loaned buffers.
   *
   * @return @c true when @c loan() can allocate transport-managed buffers; @c false otherwise.
   */
  [[nodiscard]] bool is_support_loan() const noexcept;

  /**
   * @brief Allocates a zero-copy loaned buffer from the underlying transport when the component is active.
   *
   * @param size  Requested buffer size in bytes.
   * @return      Loaned @c Bytes buffer, or an empty @c Bytes when invalid, inactive, or unsupported.
   */
  [[nodiscard]] Bytes loan(int64_t size) const;

  /**
   * @brief Returns an unused loaned buffer back to the underlying transport pool.
   *
   * @param bytes  Loaned buffer previously obtained from @c loan().
   * @return       @c true when returned; @c false otherwise.
   */
  bool return_loan(const Bytes& bytes) const;

  /**
   * @brief Returns the underlying @c Publisher pointer for advanced operations (e.g. zero-copy @c loan()).
   *
   * @return Raw publisher pointer, or @c nullptr when invalid.
   */
  [[nodiscard]] Publisher<MsgT>* raw() const noexcept;

  /**
   * @brief Releases this handle's reference to the underlying publisher and flags.
   */
  void reset() noexcept;

 private:
  std::shared_ptr<Publisher<MsgT>> pub_;
  std::shared_ptr<const std::atomic_bool> active_flag_;
  std::shared_ptr<const std::atomic_bool> force_publish_flag_;
  std::string url_;
};

/**
 * @class LauncherSubscriber
 * @brief Lifecycle-aware subscriber handle bound to a declared input port.
 *
 * @details
 * Wraps @c vlink::Subscriber\<MsgT\> with @c set_safety_quit(true), optional @c MessageLoop
 * attachment, @c listen() error propagation, lifecycle active-state gating, unload task-guard
 * tracking, and deterministic @c deinit() before @c detach() teardown.
 *
 * @tparam MsgT  Subscribed message type.
 */
template <typename MsgT>
class LauncherSubscriber final {
 public:
  /**
   * @brief Constructs an empty, invalid subscriber handle.
   */
  LauncherSubscriber() = default;

  /**
   * @brief Constructs a managed subscriber handle taking ownership of @p sub.
   *
   * @param sub       Initialised and listening @c Subscriber\<MsgT\> instance.
   * @param url       Resolved VLink URL string.
   * @param attached  Whether @p sub is attached to a @c MessageLoop.
   */
  LauncherSubscriber(std::unique_ptr<Subscriber<MsgT>> sub, std::string url, bool attached);

  /**
   * @brief Constructs a managed subscriber handle sharing lifetime with runtime record hooks.
   *
   * @param sub       Initialised and listening @c Subscriber\<MsgT\> instance.
   * @param url       Resolved VLink URL string.
   * @param attached  Whether @p sub is attached to a @c MessageLoop.
   */
  LauncherSubscriber(std::shared_ptr<Subscriber<MsgT>> sub, std::string url, bool attached);

  /**
   * @brief Move-constructs a subscriber handle, transferring ownership from @p other.
   *
   * @param other  Source handle left empty after the move.
   */
  LauncherSubscriber(LauncherSubscriber&& other) noexcept;

  /**
   * @brief Move-assigns a subscriber handle, releasing any previously owned subscriber first.
   *
   * @param other  Source handle left empty after the move.
   * @return       Reference to @c *this.
   */
  LauncherSubscriber& operator=(LauncherSubscriber&& other) noexcept;

  /**
   * @brief Stops transport delivery, detaches if attached, and destroys the subscriber.
   */
  ~LauncherSubscriber();

  LauncherSubscriber(const LauncherSubscriber&) = delete;
  LauncherSubscriber& operator=(const LauncherSubscriber&) = delete;

  /**
   * @brief Reports whether the subscriber bound and installed its listener successfully.
   *
   * @return @c true when active and listening; @c false when empty.
   */
  [[nodiscard]] bool valid() const noexcept;

  /**
   * @brief Explicit boolean conversion equivalent to @c valid().
   *
   * @return @c true when active and listening; @c false when empty.
   */
  explicit operator bool() const noexcept;

  /**
   * @brief Returns the resolved VLink URL bound to this subscriber.
   *
   * @return Resolved URL string.
   */
  [[nodiscard]] const std::string& url() const noexcept;

  /**
   * @brief Enables or disables per-node active bag recording on this subscriber.
   *
   * @param path  Target bag path (@c .vdb / @c .vdbx / @c .vcap / @c .vcapx) or empty string to disable.
   * @return      @c true when applied to the underlying subscriber; @c false if invalid or unsupported.
   */
  bool set_record_path(const std::string& path) const;

  /**
   * @brief Returns the underlying @c Subscriber pointer.
   *
   * @return Raw subscriber pointer, or @c nullptr when invalid.
   */
  [[nodiscard]] Subscriber<MsgT>* raw() const noexcept;

  /**
   * @brief Stops transport delivery, detaches if attached, and destroys the underlying subscriber.
   */
  void reset() noexcept;

 private:
  std::shared_ptr<Subscriber<MsgT>> sub_;
  std::string url_;
  bool attached_{false};
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

namespace detail {

template <typename EndpointT>
inline bool apply_endpoint_record_path(EndpointT* endpoint, const std::string& path, const char* kind,
                                       const std::string& url) {
  if VUNLIKELY (endpoint == nullptr) {
    return false;
  }

  if (endpoint->get_transport_type() == TransportType::kIntra) {
    if (!path.empty()) {
      VLOG_W(kind, ": Per-node set_record_path() is not supported on intra:// (", url, ").");
    }

    return false;
  }

  endpoint->set_record_path(path);
  return true;
}

}  // namespace detail

template <typename MsgT>
inline LauncherPublisher<MsgT>::LauncherPublisher(std::shared_ptr<Publisher<MsgT>> pub,
                                                  std::shared_ptr<const std::atomic_bool> active_flag, std::string url,
                                                  std::shared_ptr<const std::atomic_bool> force_publish_flag)
    : pub_(std::move(pub)),
      active_flag_(std::move(active_flag)),
      force_publish_flag_(std::move(force_publish_flag)),
      url_(std::move(url)) {}

template <typename MsgT>
inline bool LauncherPublisher<MsgT>::valid() const noexcept {
  return pub_ != nullptr;
}

template <typename MsgT>
inline LauncherPublisher<MsgT>::operator bool() const noexcept {
  return valid();
}

template <typename MsgT>
inline const std::string& LauncherPublisher<MsgT>::url() const noexcept {
  return url_;
}

template <typename MsgT>
inline bool LauncherPublisher<MsgT>::publish(const MsgT& msg, bool force) const {
  if VUNLIKELY (!pub_ || (active_flag_ != nullptr && !active_flag_->load(std::memory_order_acquire))) {
    return false;
  }

  const bool eff_force =
      force || (force_publish_flag_ != nullptr && force_publish_flag_->load(std::memory_order_acquire));
  return pub_->publish(msg, eff_force);
}

template <typename MsgT>
template <typename U, std::enable_if_t<std::is_same_v<U, Bytes> && std::is_same_v<MsgT, Bytes>, int>>
inline bool LauncherPublisher<MsgT>::publish(Bytes&& msg, bool force) const {
  if VUNLIKELY (!pub_ || (active_flag_ != nullptr && !active_flag_->load(std::memory_order_acquire))) {
    return false;
  }

  const bool eff_force =
      force || (force_publish_flag_ != nullptr && force_publish_flag_->load(std::memory_order_acquire));
  return pub_->publish(std::move(msg), eff_force);
}

template <typename MsgT>
inline bool LauncherPublisher<MsgT>::set_record_path(const std::string& path) const {
  return detail::apply_endpoint_record_path(pub_.get(), path, "LauncherPublisher", url_);
}

template <typename MsgT>
inline bool LauncherPublisher<MsgT>::is_support_loan() const noexcept {
  return pub_ != nullptr && pub_->is_support_loan();
}

template <typename MsgT>
inline Bytes LauncherPublisher<MsgT>::loan(int64_t size) const {
  if VUNLIKELY (!pub_ || (active_flag_ != nullptr && !active_flag_->load(std::memory_order_acquire))) {
    return {};
  }

  return pub_->loan(size);
}

template <typename MsgT>
inline bool LauncherPublisher<MsgT>::return_loan(const Bytes& bytes) const {
  if VUNLIKELY (!pub_) {
    return false;
  }

  return pub_->return_loan(bytes);
}

template <typename MsgT>
inline Publisher<MsgT>* LauncherPublisher<MsgT>::raw() const noexcept {
  return pub_.get();
}

template <typename MsgT>
inline void LauncherPublisher<MsgT>::reset() noexcept {
  pub_.reset();
  active_flag_.reset();
  force_publish_flag_.reset();
  url_.clear();
}

template <typename MsgT>
inline LauncherSubscriber<MsgT>::LauncherSubscriber(std::unique_ptr<Subscriber<MsgT>> sub, std::string url,
                                                    bool attached)
    : sub_(std::move(sub)), url_(std::move(url)), attached_(attached) {}

template <typename MsgT>
inline LauncherSubscriber<MsgT>::LauncherSubscriber(std::shared_ptr<Subscriber<MsgT>> sub, std::string url,
                                                    bool attached)
    : sub_(std::move(sub)), url_(std::move(url)), attached_(attached) {}

template <typename MsgT>
inline LauncherSubscriber<MsgT>::LauncherSubscriber(LauncherSubscriber&& other) noexcept
    : sub_(std::move(other.sub_)), url_(std::move(other.url_)), attached_(other.attached_) {
  other.attached_ = false;
}

template <typename MsgT>
inline LauncherSubscriber<MsgT>& LauncherSubscriber<MsgT>::operator=(LauncherSubscriber&& other) noexcept {
  if (this != &other) {
    reset();
    sub_ = std::move(other.sub_);
    url_ = std::move(other.url_);
    attached_ = other.attached_;
    other.attached_ = false;
  }

  return *this;
}

template <typename MsgT>
inline LauncherSubscriber<MsgT>::~LauncherSubscriber() {
  reset();
}

template <typename MsgT>
inline bool LauncherSubscriber<MsgT>::valid() const noexcept {
  return sub_ != nullptr;
}

template <typename MsgT>
inline LauncherSubscriber<MsgT>::operator bool() const noexcept {
  return valid();
}

template <typename MsgT>
inline const std::string& LauncherSubscriber<MsgT>::url() const noexcept {
  return url_;
}

template <typename MsgT>
inline bool LauncherSubscriber<MsgT>::set_record_path(const std::string& path) const {
  return detail::apply_endpoint_record_path(sub_.get(), path, "LauncherSubscriber", url_);
}

template <typename MsgT>
inline Subscriber<MsgT>* LauncherSubscriber<MsgT>::raw() const noexcept {
  return sub_.get();
}

template <typename MsgT>
inline void LauncherSubscriber<MsgT>::reset() noexcept {
  if (sub_) {
    sub_->deinit();

    if (attached_) {
      (void)sub_->detach();
      attached_ = false;
    }

    sub_.reset();
  }

  url_.clear();
}

template <typename MsgT>
inline LauncherPublisher<MsgT> LauncherContext::create_publisher(std::string_view port_key,
                                                                 std::string_view default_url) const {
  std::string resolved = get_output_url(port_key, default_url);

  if VUNLIKELY (resolved.empty()) {
    return {};
  }

  auto pub = MemoryResource::make_shared<Publisher<MsgT>>(resolved, InitType::kWithoutInit);

  if VUNLIKELY (!pub->init()) {
    return {};
  }

  std::weak_ptr<Publisher<MsgT>> weak_pub(pub);
  LauncherPublisher<MsgT> handle(std::move(pub), active_flag(), resolved, record_force_publish_flag());

  register_record_hook(
      [weak_pub = std::move(weak_pub), resolved = std::move(resolved)](const std::string& path) -> bool {
        auto locked = weak_pub.lock();

        if (!locked) {
          return false;
        }

        (void)detail::apply_endpoint_record_path(locked.get(), path, "LauncherPublisher", resolved);
        return true;
      });

  return handle;
}

template <typename MsgT, typename CallbackT>
inline LauncherSubscriber<MsgT> LauncherContext::create_subscriber(std::string_view port_key, CallbackT&& callback,
                                                                   std::string_view default_url, bool attach_loop) {
  std::string resolved = get_input_url(port_key, default_url);

  if VUNLIKELY (resolved.empty()) {
    return {};
  }

  auto sub = MemoryResource::make_shared<Subscriber<MsgT>>(resolved, InitType::kWithoutInit);
  sub->set_safety_quit(true);
  bool attached = false;

  if (attach_loop) {
    attached = sub->attach(&loop());
  }

  if VUNLIKELY (!sub->init()) {
    if (attached) {
      (void)sub->detach();
    }

    return {};
  }

  auto flag = active_flag();
  auto wrapped = [this, flag = std::move(flag), cb = std::forward<CallbackT>(callback)](const MsgT& msg) mutable {
    if VUNLIKELY (flag != nullptr ? !flag->load(std::memory_order_acquire) : !is_active()) {
      return;
    }

    auto guard = acquire_task_guard();

    if VUNLIKELY (!guard) {
      return;
    }

    CallbackScope scope(this);
    cb(msg);
  };

  if VUNLIKELY (!sub->listen(std::move(wrapped))) {
    sub->deinit();

    if (attached) {
      (void)sub->detach();
    }

    return {};
  }

  std::weak_ptr<Subscriber<MsgT>> weak_sub_cleanup(sub);
  std::weak_ptr<Subscriber<MsgT>> weak_sub(sub);
  LauncherSubscriber<MsgT> handle(std::move(sub), resolved, attached);

  register_cleanup_hook([weak_sub_cleanup = std::move(weak_sub_cleanup), attached]() {
    auto locked = weak_sub_cleanup.lock();

    if (locked) {
      locked->deinit();

      if (attached) {
        (void)locked->detach();
      }
    }
  });

  register_record_hook(
      [weak_sub = std::move(weak_sub), resolved = std::move(resolved)](const std::string& path) -> bool {
        auto locked = weak_sub.lock();

        if (!locked) {
          return false;
        }

        (void)detail::apply_endpoint_record_path(locked.get(), path, "LauncherSubscriber", resolved);
        return true;
      });

  return handle;
}

}  // namespace vlink
