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
 * @file abstract_factory.h
 * @brief Topic-scoped registration store that fans transport callbacks across @c NodeImpl peers.
 *
 * @details
 * This is an internal implementation header used by the public VLink node templates
 * (@c Publisher, @c Subscriber, @c Client, @c Server, @c Setter, @c Getter); it should not
 * be included directly by application code.  The header introduces two co-operating
 * class templates that let several @c NodeImpl instances bound to the same logical
 * topic share one registration record:
 *
 * - @c AbstractObject -- a per-topic record that owns the set of registered
 *   @c NodeImpl pointers together with six callback dictionaries (server connect,
 *   subscriber connect, request/response, serialised message, intra-process message
 *   and transport status).
 * - @c AbstractFactory -- a thread-safe map keyed on @c FilterT (commonly the
 *   topic URL string) that lazily creates an @c AbstractObject for each key and
 *   reuses it via a cached @c std::weak_ptr until the last owner releases it.
 *
 * @par Registration flow
 * @code
 *                    +----------------------+
 *                    | AbstractFactory<K>   |
 *                    |  map<K, weak_ptr<O>> |
 *                    +----------+-----------+
 *                               | get_object<O>(key)
 *                               v
 *                    +----------------------+
 *                    |  AbstractObject<K>   |
 *                    |  ImplList            |
 *                    |  ConnectCallbackMap  |
 *                    |  MsgCallbackMap ...  |
 *                    +----------+-----------+
 *                       ^       |   ^
 *      add_impl(impl)   |       |   | register_msg_callback(impl, cb)
 *      remove_impl(impl)|       |   |
 *                       |       v
 *                +------+--+   +-+--------+   +----------+
 *                | NodeImpl|   | NodeImpl |   | NodeImpl |
 *                +---------+   +----------+   +----------+
 * @endcode
 *
 * @par Registry keys
 * | Map field                       | Callback signature                                |
 * | ------------------------------- | ------------------------------------------------- |
 * | @c server_connect_callback_map_ | @c void(bool) -- server side peer presence change |
 * | @c sub_connect_callback_map_    | @c void(bool) -- subscriber side presence change  |
 * | @c req_resp_callback_map_       | @c void(uint64_t, const Bytes&, Bytes*)           |
 * | @c msg_callback_map_            | @c void(const Bytes&)                             |
 * | @c intra_msg_callback_map_      | @c void(const IntraData&)                         |
 * | @c status_callback_map_         | @c void(const Status::BasePtr&)                   |
 *
 * @par Example
 * @code
 * struct TopicObject final : vlink::AbstractObject<std::string> {
 *   using AbstractObject::AbstractObject;
 * };
 *
 * vlink::AbstractFactory<std::string> factory;
 *
 * auto object = factory.get_object<TopicObject>("dds://my_topic");
 * object->add_impl(impl);
 * object->register_msg_callback(impl, [](const vlink::Bytes& bytes) {
 *   // forward each delivery to the owning node
 * });
 *
 * object->traverse_msg_callback([](vlink::NodeImpl*, const vlink::NodeImpl::MsgCallback& cb) {
 *   cb(payload);
 * });
 * @endcode
 *
 * @note Every public @c AbstractObject method acquires the internal
 *       @c std::recursive_mutex; callbacks invoked through @c traverse_*() execute
 *       under that lock and may re-enter the same @c AbstractObject safely.
 *
 * @tparam FilterT Key type used to identify topics inside the factory map.
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "../base/functional.h"
#include "../base/logger.h"
#include "./node_impl.h"

namespace vlink {

/**
 * @class AbstractObject
 * @brief Topic-scoped fan-out store of @c NodeImpl peers and their callbacks.
 *
 * @details
 * Holds the active @c NodeImpl pointer set together with the six callback
 * dictionaries documented at file scope and serialises mutation, traversal and
 * accounting through a @c std::recursive_mutex.  The traversal helpers honour
 * the @c ignore_called() escape hatch so that individual callbacks can opt out
 * of the "any callback was invoked" accounting tracked by @c has_called().
 *
 * @tparam FilterT Key type used by the owning @c AbstractFactory for this object.
 */
template <typename FilterT>
class AbstractObject : public AbstractNode {
 public:
  using ImplList = std::unordered_set<NodeImpl*>;  ///< Set of currently registered @c NodeImpl peers.

  using ConnectCallbackMap = std::unordered_map<NodeImpl*, NodeImpl::ConnectCallback>;  ///< Connect handlers per impl.
  using ReqRespCallbackMap =
      std::unordered_map<NodeImpl*, NodeImpl::ReqRespCallback>;                 ///< Req/resp callbacks, keyed by impl.
  using MsgCallbackMap = std::unordered_map<NodeImpl*, NodeImpl::MsgCallback>;  ///< Message callbacks, keyed by impl.
  using IntraMsgCallbackMap = std::unordered_map<NodeImpl*, NodeImpl::IntraMsgCallback>;  ///< Intra-message callbacks.
  using StatusCallbackMap = std::unordered_map<NodeImpl*, NodeImpl::StatusCallback>;  ///< Status callbacks per impl.

  using FindConnectCallback =
      Function<void(NodeImpl*, const NodeImpl::ConnectCallback&)>;  ///< Visitor invoked for each connect entry.
  using FindReqRespCallback =
      Function<void(NodeImpl*, const NodeImpl::ReqRespCallback&)>;  ///< Visitor invoked for each req/resp entry.
  using FindMsgCallback =
      Function<void(NodeImpl*, const NodeImpl::MsgCallback&)>;  ///< Visitor invoked for each message entry.
  using FindIntraMsgCallback =
      Function<void(NodeImpl*, const NodeImpl::IntraMsgCallback&)>;  ///< Visitor for each intra-message entry.
  using FindStatusCallback =
      Function<void(NodeImpl*, const NodeImpl::StatusCallback&)>;  ///< Visitor invoked for each status entry.

  /**
   * @brief Registers @p impl as an active peer on this topic.
   *
   * @details
   * Inserts @p impl into @c impl_list_ and refreshes the cached @c first_impl_
   * pointer to the latest registrant.  The operation is serialised against all
   * other public methods.
   *
   * @param impl  Non-owning peer pointer to track.
   * @return @c true when the pointer was newly inserted; @c false when it was
   *         already present.
   */
  bool add_impl(NodeImpl* impl);

  /**
   * @brief Removes @p impl from the peer set and forgets every associated callback.
   *
   * @details
   * Erases the pointer from @c impl_list_, reassigns @c first_impl_ if needed
   * and drops the entry from all six callback dictionaries.  Thread-safe.
   *
   * @param impl  Peer pointer previously passed to @c add_impl().
   * @return @c true if @p impl was found and removed; @c false otherwise.
   */
  bool remove_impl(NodeImpl* impl);

  /**
   * @brief Returns the most recently registered peer.
   *
   * @details
   * The "first" pointer follows the latest successful @c add_impl() call.  After
   * a matching @c remove_impl() the cache is repopulated with an arbitrary
   * remaining peer, or @c nullptr if the set has been drained.
   *
   * @return Pointer to the current cached peer; @c nullptr when no peer is registered.
   */
  [[nodiscard]] NodeImpl* get_first_impl() const;

  /**
   * @brief Tests whether @p impl is currently part of the peer set.
   *
   * @param impl  Pointer to query.
   * @return @c true when @p impl is registered, @c false otherwise.
   */
  [[nodiscard]] bool is_contains_impl(NodeImpl* impl) const;

  /**
   * @brief Indicates whether at least one peer has been registered.
   *
   * @return @c true when @c impl_list_ is non-empty.
   */
  [[nodiscard]] bool has_impl() const;

  /**
   * @brief Stores @p callback as the server-side connect handler for @p impl.
   *
   * @param impl      Peer that owns @p callback.
   * @param callback  Callable @c void(bool) invoked when a remote client comes or goes.
   * @return @c true if the entry was inserted; @c false when one was already present.
   */
  bool register_server_connect_callback(NodeImpl* impl, NodeImpl::ConnectCallback&& callback);

  /**
   * @brief Stores @p callback as the subscriber-side connect handler for @p impl.
   *
   * @param impl      Peer that owns @p callback.
   * @param callback  Callable @c void(bool) invoked when a subscriber appears or disappears.
   * @return @c true if the entry was inserted; @c false when one was already present.
   */
  bool register_sub_connect_callback(NodeImpl* impl, NodeImpl::ConnectCallback&& callback);

  /**
   * @brief Stores @p callback as the request/response handler for @p impl.
   *
   * @param impl      Peer that owns @p callback.
   * @param callback  Callable invoked for every incoming RPC request.
   * @return @c true on insertion; @c false when already registered.
   */
  bool register_req_resp_callback(NodeImpl* impl, NodeImpl::ReqRespCallback&& callback);

  /**
   * @brief Stores @p callback as the serialised-message handler for @p impl.
   *
   * @param impl      Peer that owns @p callback.
   * @param callback  Callable @c void(const Bytes&) invoked for every received message.
   * @return @c true on insertion; @c false when already registered.
   */
  bool register_msg_callback(NodeImpl* impl, NodeImpl::MsgCallback&& callback);

  /**
   * @brief Stores @p callback as the intra-process message handler for @p impl.
   *
   * @param impl      Peer that owns @p callback.
   * @param callback  Callable @c void(const IntraData&) invoked for each in-process delivery.
   * @return @c true on insertion; @c false when already registered.
   */
  bool register_intra_msg_callback(NodeImpl* impl, NodeImpl::IntraMsgCallback&& callback);

  /**
   * @brief Stores @p callback as the transport-status handler for @p impl.
   *
   * @param impl      Peer that owns @p callback.
   * @param callback  Callable invoked when the transport reports a status change.
   * @return @c true on insertion; @c false when already registered.
   */
  bool register_status_callback(NodeImpl* impl, NodeImpl::StatusCallback&& callback);

  /**
   * @brief Reports whether the server-connect dictionary is empty.
   *
   * @return @c true when no server-connect callbacks are registered.
   */
  [[nodiscard]] bool server_connect_map_is_empty() const;

  /**
   * @brief Reports whether the subscriber-connect dictionary is empty.
   *
   * @return @c true when no subscriber-connect callbacks are registered.
   */
  [[nodiscard]] bool sub_connect_map_is_empty() const;

  /**
   * @brief Reports whether the request/response dictionary is empty.
   *
   * @return @c true when no req/resp callbacks are registered.
   */
  [[nodiscard]] bool req_resp_map_is_empty() const;

  /**
   * @brief Reports whether the serialised-message dictionary is empty.
   *
   * @return @c true when no message callbacks are registered.
   */
  [[nodiscard]] bool msg_map_is_empty() const;

  /**
   * @brief Reports whether the intra-process message dictionary is empty.
   *
   * @return @c true when no intra-message callbacks are registered.
   */
  [[nodiscard]] bool intra_msg_map_is_empty() const;

  /**
   * @brief Reports whether the transport-status dictionary is empty.
   *
   * @return @c true when no status callbacks are registered.
   */
  [[nodiscard]] bool status_map_is_empty() const;

  /**
   * @brief Walks the server-connect dictionary and invokes @p callback for every entry.
   *
   * @details
   * Iteration is performed while holding the recursive mutex.  Individual visits
   * may call @c ignore_called() to keep the current entry from setting the
   * @c has_called() flag; iteration carries on regardless.
   *
   * @param callback  Visitor receiving each peer pointer and its stored handler.
   */
  void traverse_server_connect_callback(const FindConnectCallback& callback);

  /**
   * @brief Walks the subscriber-connect dictionary and invokes @p callback for every entry.
   *
   * @param callback  Visitor receiving each peer pointer and its stored handler.
   */
  void traverse_sub_connect_callback(const FindConnectCallback& callback);

  /**
   * @brief Walks the request/response dictionary and invokes @p callback for every entry.
   *
   * @param callback  Visitor receiving each peer pointer and its stored handler.
   */
  void traverse_req_resp_callback(const FindReqRespCallback& callback);

  /**
   * @brief Walks the serialised-message dictionary and invokes @p callback for every entry.
   *
   * @param callback  Visitor receiving each peer pointer and its stored handler.
   */
  void traverse_msg_callback(const FindMsgCallback& callback);

  /**
   * @brief Walks the intra-process dictionary and invokes @p callback for every entry.
   *
   * @param callback  Visitor receiving each peer pointer and its stored handler.
   */
  void traverse_intra_msg_callback(const FindIntraMsgCallback& callback);

  /**
   * @brief Walks the transport-status dictionary and invokes @p callback for every entry.
   *
   * @param callback  Visitor receiving each peer pointer and its stored handler.
   */
  void traverse_status_callback(const FindStatusCallback& callback);

 protected:
  AbstractObject();

  ~AbstractObject() override;

  [[nodiscard]] bool has_called() const;

  void ignore_called();

 private:
  template <typename CallbackMapT, typename CallbackT>
  void traverse_internal_callback(const CallbackMapT& map, const CallbackT& callback);

  bool has_called_{false};
  bool ignore_called_{false};
  ImplList impl_list_;
  mutable std::recursive_mutex mtx_;
  ConnectCallbackMap server_connect_callback_map_;
  ConnectCallbackMap sub_connect_callback_map_;
  ReqRespCallbackMap req_resp_callback_map_;
  MsgCallbackMap msg_callback_map_;
  IntraMsgCallbackMap intra_msg_callback_map_;
  StatusCallbackMap status_callback_map_;
  NodeImpl* first_impl_{nullptr};

  VLINK_DISALLOW_COPY_AND_ASSIGN(AbstractObject)
};

/**
 * @class AbstractFactory
 * @brief Lazily allocates and caches @c AbstractObject instances keyed by @c FilterT.
 *
 * @details
 * Holds a @c std::map<FilterT, std::weak_ptr<Object>> so that multiple node
 * peers requesting the same key reuse the same registration record.  The
 * returned @c std::shared_ptr carries a custom deleter that erases the map
 * entry when the last owner releases the object, which keeps the lookup map
 * free of stale @c weak_ptr slots.
 *
 * @note The factory itself is non-copyable and non-movable; share it through a
 *       singleton or a per-transport static instance.
 *
 * @tparam FilterT Topic-key type (typically @c std::string).
 */
template <typename FilterT>
class AbstractFactory {
  using Object = AbstractObject<FilterT>;
  using Map = std::map<FilterT, std::weak_ptr<Object>>;
  using Set = std::unordered_set<Object*>;

 public:
  /**
   * @brief Tests whether @p ptr corresponds to a live object created by this factory.
   *
   * @param ptr  Raw pointer to validate.
   * @return @c true when the object is still alive in this factory; @c false otherwise.
   */
  [[nodiscard]] bool has_object(Object* ptr) const;

  /**
   * @brief Looks up or creates the @c ObjectT registered against @p filter.
   *
   * @details
   * If the cached @c weak_ptr is still valid, the existing instance is shared
   * with the caller.  Otherwise a new @c ObjectT is allocated (outside the
   * factory lock so its constructor can re-enter VLink safely), the resulting
   * @c shared_ptr is given a deleter that removes the cache entry on
   * destruction, and the value is stored back into the map.
   *
   * @tparam ObjectT Concrete subclass of @c AbstractObject<FilterT> to allocate.
   *
   * @param filter  Key identifying the topic.
   * @return Shared ownership handle for the cached object.
   */
  template <typename ObjectT>
  [[nodiscard]] std::shared_ptr<ObjectT> get_object(const FilterT& filter);

 protected:
  /**
   * @brief Constructs an empty factory.
   */
  AbstractFactory();

  /**
   * @brief Destroys the factory and releases the cache.
   */
  virtual ~AbstractFactory();

 private:
  Set set_;
  Map map_;
  mutable std::mutex mtx_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(AbstractFactory)
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

template <typename FilterT>
inline bool AbstractObject<FilterT>::add_impl(NodeImpl* impl) {
  std::lock_guard lock(mtx_);

  first_impl_ = impl;

  return impl_list_.emplace(impl).second;
}

template <typename FilterT>
inline bool AbstractObject<FilterT>::remove_impl(NodeImpl* impl) {
  std::lock_guard lock(mtx_);

  if VUNLIKELY (impl_list_.erase(impl) == 0) {
    return false;
  }

  if (first_impl_ == impl) {
    first_impl_ = impl_list_.empty() ? nullptr : *impl_list_.begin();
  }

  server_connect_callback_map_.erase(impl);
  sub_connect_callback_map_.erase(impl);
  req_resp_callback_map_.erase(impl);
  msg_callback_map_.erase(impl);
  intra_msg_callback_map_.erase(impl);
  status_callback_map_.erase(impl);
  return true;
}

template <typename FilterT>
NodeImpl* AbstractObject<FilterT>::get_first_impl() const {
  std::lock_guard lock(mtx_);
  return first_impl_;
}

template <typename FilterT>
inline bool AbstractObject<FilterT>::is_contains_impl(NodeImpl* impl) const {
  std::lock_guard lock(mtx_);
  return impl_list_.find(impl) != impl_list_.end();
}

template <typename FilterT>
inline bool AbstractObject<FilterT>::has_impl() const {
  std::lock_guard lock(mtx_);
  return !impl_list_.empty();
}

template <typename FilterT>
inline bool AbstractObject<FilterT>::register_server_connect_callback(NodeImpl* impl,
                                                                      NodeImpl::ConnectCallback&& callback) {
  std::lock_guard lock(this->mtx_);
  return server_connect_callback_map_.try_emplace(impl, std::move(callback)).second;
}

template <typename FilterT>
inline bool AbstractObject<FilterT>::register_sub_connect_callback(NodeImpl* impl,
                                                                   NodeImpl::ConnectCallback&& callback) {
  std::lock_guard lock(this->mtx_);
  return sub_connect_callback_map_.try_emplace(impl, std::move(callback)).second;
}

template <typename FilterT>
inline bool AbstractObject<FilterT>::register_req_resp_callback(NodeImpl* impl, NodeImpl::ReqRespCallback&& callback) {
  std::lock_guard lock(this->mtx_);
  return req_resp_callback_map_.try_emplace(impl, std::move(callback)).second;
}

template <typename FilterT>
inline bool AbstractObject<FilterT>::register_msg_callback(NodeImpl* impl, NodeImpl::MsgCallback&& callback) {
  std::lock_guard lock(this->mtx_);
  return msg_callback_map_.try_emplace(impl, std::move(callback)).second;
}

template <typename FilterT>
inline bool AbstractObject<FilterT>::register_intra_msg_callback(NodeImpl* impl,
                                                                 NodeImpl::IntraMsgCallback&& callback) {
  std::lock_guard lock(this->mtx_);
  return intra_msg_callback_map_.try_emplace(impl, std::move(callback)).second;
}

template <typename FilterT>
inline bool AbstractObject<FilterT>::register_status_callback(NodeImpl* impl, NodeImpl::StatusCallback&& callback) {
  std::lock_guard lock(this->mtx_);
  return status_callback_map_.try_emplace(impl, std::move(callback)).second;
}

template <typename FilterT>
inline bool AbstractObject<FilterT>::server_connect_map_is_empty() const {
  std::lock_guard lock(this->mtx_);
  return server_connect_callback_map_.empty();
}

template <typename FilterT>
inline bool AbstractObject<FilterT>::sub_connect_map_is_empty() const {
  std::lock_guard lock(this->mtx_);
  return sub_connect_callback_map_.empty();
}

template <typename FilterT>
inline bool AbstractObject<FilterT>::req_resp_map_is_empty() const {
  std::lock_guard lock(this->mtx_);
  return req_resp_callback_map_.empty();
}

template <typename FilterT>
inline bool AbstractObject<FilterT>::msg_map_is_empty() const {
  std::lock_guard lock(this->mtx_);
  return msg_callback_map_.empty();
}

template <typename FilterT>
inline bool AbstractObject<FilterT>::intra_msg_map_is_empty() const {
  std::lock_guard lock(this->mtx_);
  return intra_msg_callback_map_.empty();
}

template <typename FilterT>
inline bool AbstractObject<FilterT>::status_map_is_empty() const {
  std::lock_guard lock(this->mtx_);
  return status_callback_map_.empty();
}

template <typename FilterT>
inline void AbstractObject<FilterT>::traverse_server_connect_callback(const FindConnectCallback& callback) {
  this->traverse_internal_callback(server_connect_callback_map_, callback);
}

template <typename FilterT>
inline void AbstractObject<FilterT>::traverse_sub_connect_callback(const FindConnectCallback& callback) {
  this->traverse_internal_callback(sub_connect_callback_map_, callback);
}

template <typename FilterT>
inline void AbstractObject<FilterT>::traverse_req_resp_callback(const FindReqRespCallback& callback) {
  this->traverse_internal_callback(req_resp_callback_map_, callback);
}

template <typename FilterT>
inline void AbstractObject<FilterT>::traverse_msg_callback(const FindMsgCallback& callback) {
  this->traverse_internal_callback(msg_callback_map_, callback);
}

template <typename FilterT>
inline void AbstractObject<FilterT>::traverse_intra_msg_callback(const FindIntraMsgCallback& callback) {
  this->traverse_internal_callback(intra_msg_callback_map_, callback);
}

template <typename FilterT>
inline void AbstractObject<FilterT>::traverse_status_callback(const FindStatusCallback& callback) {
  this->traverse_internal_callback(status_callback_map_, callback);
}

template <typename FilterT>
inline AbstractObject<FilterT>::AbstractObject() = default;

template <typename FilterT>
inline AbstractObject<FilterT>::~AbstractObject() = default;

template <typename FilterT>
inline bool AbstractObject<FilterT>::has_called() const {
  return has_called_;
}

template <typename FilterT>
inline void AbstractObject<FilterT>::ignore_called() {
  ignore_called_ = true;
}

template <typename FilterT>
template <typename CallbackMapT, typename CallbackT>
inline void AbstractObject<FilterT>::traverse_internal_callback(const CallbackMapT& map, const CallbackT& callback) {
  std::lock_guard lock(mtx_);

  this->ignore_called_ = false;
  this->has_called_ = false;

  for (const auto& [impl, target_callback] : map) {
    callback(impl, target_callback);

    if VUNLIKELY (this->ignore_called_) {
      this->ignore_called_ = false;
    } else {
      this->has_called_ = true;
    }
  }
}

template <typename FilterT>
inline bool AbstractFactory<FilterT>::has_object(Object* ptr) const {
  std::lock_guard lock(mtx_);
  return set_.count(ptr) > 0;
}

template <typename FilterT>
template <typename ObjectT>
inline std::shared_ptr<ObjectT> AbstractFactory<FilterT>::get_object(const FilterT& filter) {
  static_assert(std::is_base_of_v<Object, ObjectT>, "ObjectT must be derived from AbstractObject");
  std::shared_ptr<ObjectT> obj;
  {
    std::unique_lock lock(mtx_);

    const auto& deleter = [this, filter](ObjectT* obj) {
      {
        std::lock_guard lock(mtx_);

        set_.erase(obj);

        auto iter = map_.find(filter);

        if VUNLIKELY (iter != map_.end() && iter->second.expired()) {
          map_.erase(iter);
        }
      }

      delete obj;
    };

    auto iter = map_.find(filter);

    if (iter != map_.end()) {
      obj = std::static_pointer_cast<ObjectT>(iter->second.lock());

      if VLIKELY (obj) {
        return obj;
      }

      map_.erase(iter);
    }

    {
      lock.unlock();
      auto* obj_ptr = new ObjectT(filter);
      lock.lock();

      auto [it, inserted] = map_.try_emplace(filter, std::weak_ptr<Object>());

      if (!inserted) {
        obj = std::static_pointer_cast<ObjectT>(it->second.lock());
      }

      if (inserted || !obj) {
        if (!inserted) {
          map_.erase(it);
          it = map_.try_emplace(filter, std::weak_ptr<Object>()).first;
        }

        obj = std::shared_ptr<ObjectT>(obj_ptr, deleter);
        it->second = obj;
        set_.emplace(obj_ptr);
      } else {
        delete obj_ptr;
      }
    }
    return obj;
  }
}

template <typename FilterT>
inline AbstractFactory<FilterT>::AbstractFactory() = default;

template <typename FilterT>
inline AbstractFactory<FilterT>::~AbstractFactory() = default;

}  // namespace vlink
