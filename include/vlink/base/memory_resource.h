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
 * @file memory_resource.h
 * @brief PMR adapter that lets standard pmr-aware containers allocate through @c vlink::MemoryPool.
 *
 * @details
 * @c vlink::MemoryResource derives from @c std::pmr::memory_resource and forwards each request
 * onto an owned (or shared) @c vlink::MemoryPool.  This bridges the standard polymorphic
 * allocator machinery with VLink's tiered pool.
 *
 * @par PMR vs VLink resources
 *
 * | Aspect                | @c std::pmr::new_delete_resource      | @c vlink::MemoryResource                    |
 * | --------------------- | ------------------------------------- | ------------------------------------------- |
 * | Backing allocator     | global @c operator @c new / @c delete | @c vlink::MemoryPool (per-tier free lists)  |
 * | Allocation footprint  | unpredictable per-call                | bounded per-tier; pooled                    |
 * | Concurrent allocs     | global allocator scaling              | per-tier locking, low contention            |
 * | Bypass mode           | not applicable                        | empty tier list yields pass-through         |
 * | Maintenance           | not applicable                        | @c trim releases idle chunks                |
 *
 * @par Lifetime
 *  - @c MemoryResource(), @c MemoryResource(int) and @c MemoryResource(const @c Config&) each
 *    heap-allocate a private @c MemoryPool and own it for the lifetime of the resource.
 *  - @c MemoryResource::global_instance returns a process-wide singleton that wraps
 *    @c MemoryPool::global_instance; the resource is not deleted by the destructor.
 *
 * @par Example
 * @code
 *   // Shared process-wide resource.
 *   std::pmr::vector<int> v(&vlink::MemoryResource::global_instance());
 *   v.reserve(1024);
 *
 *   // Private level-3 pyramid:
 *   vlink::MemoryResource res(3);
 *   std::pmr::polymorphic_allocator<char> alloc(&res);
 *   std::pmr::string s(alloc);
 *
 *   // Private resource with a custom tier list and full-quota preallocation.
 *   vlink::MemoryPool::Config cfg;
 *   cfg.tiers    = {{64, 16}, {1024, 4}};
 *   cfg.prealloc = true;
 *   vlink::MemoryResource custom(cfg);
 *   std::pmr::vector<double> w(&custom);
 * @endcode
 *
 * @note @c do_allocate throws @c std::bad_alloc when the underlying pool returns @c nullptr,
 *       satisfying the pmr contract.  Equality is identity over the bound @c MemoryPool object.
 */

#pragma once

#include <memory>

#if defined(__linux__) && __has_include(<memory_resource>)
#include <memory_resource>
#if !defined(VLINK_ENABLE_BASE_MEMORY_RESOURCE) && defined(__cpp_lib_memory_resource)
#define VLINK_ENABLE_BASE_MEMORY_RESOURCE
#endif
#endif

#ifdef VLINK_ENABLE_BASE_MEMORY_RESOURCE

#include <cstddef>

#include "./macros.h"
#include "./memory_pool.h"

namespace vlink {

/**
 * @class MemoryResource
 * @brief Adapter satisfying @c std::pmr::memory_resource that delegates to a @c vlink::MemoryPool.
 *
 * @details
 * Non-copyable, non-movable -- matching the standard interface.  The bound pool may be either
 * owned by the resource or shared with @c MemoryPool::global_instance, depending on the
 * constructor that produced the instance.
 */
class VLINK_EXPORT MemoryResource : public std::pmr::memory_resource {
 public:
  /**
   * @brief Deleter type used by @c make_unique to return objects back through the pmr allocator.
   *
   * @details
   * Carries one @c polymorphic_allocator copy so @c sizeof(Deleter<T>) equals @c sizeof(void*).
   */
  template <typename T>
  struct Deleter final {
    std::pmr::polymorphic_allocator<T> alloc;

    void operator()(T* p) const noexcept {
      if VLIKELY (p) {
        alloc.delete_object(p);
      }
    }
  };

  /**
   * @brief Constructs a resource that owns a private bypass-mode pool.
   *
   * @details
   * Equivalent to @c MemoryResource(MemoryPool::Config{}); every allocation goes through
   * @c ::operator @c new.
   */
  MemoryResource();

  /**
   * @brief Constructs a resource owning a private pool built from the built-in pyramid @p level.
   *
   * @details
   * Forwards to @c MemoryPool(int, @c bool).  In bypass mode @p prealloc is ignored.
   *
   * @param level     Built-in pyramid level in @c [0, @c 9].
   * @param prealloc  When @c true, fills every tier to its quota on construction.  Default: @c false.
   */
  explicit MemoryResource(int level, bool prealloc = false);

  /**
   * @brief Constructs a resource owning a private pool built from @p config.
   *
   * @param config  Tier configuration forwarded to the @c MemoryPool constructor.
   */
  explicit MemoryResource(const MemoryPool::Config& config);

  /**
   * @brief Destructor; releases the owned pool when the resource owns one.
   *
   * @details
   * The destructor does not delete the pool of @c global_instance.  The caller must guarantee
   * no live block obtained from this resource is still outstanding at destruction.
   */
  ~MemoryResource() override;

  /**
   * @brief Returns the underlying @c MemoryPool used by this resource.
   *
   * @return Reference to the bound pool (private or shared depending on the constructor).
   */
  [[nodiscard]] MemoryPool& get_memory_pool() noexcept;

  /**
   * @brief Trims idle chunks from the underlying pool.
   *
   * @details
   * Forwards to @c MemoryPool::trim on the bound pool.
   */
  void trim() noexcept;

  /**
   * @brief Returns the process-wide @c MemoryResource that wraps @c MemoryPool::global_instance.
   *
   * @details
   * Lazy singleton.  The first call's @p use_env_level value is forwarded to
   * @c MemoryPool::global_instance and decides the underlying pool's configuration; later calls
   * ignore the argument and return the same resource.
   *
   * @param use_env_level  Forwarded to @c MemoryPool::global_instance.  Default: @c true.
   * @return Reference to the shared resource.
   */
  static MemoryResource& global_instance(bool use_env_level = true);

  /**
   * @brief @c std::allocate_shared backed by @c global_instance.
   *
   * @tparam T     Object type.
   * @tparam Args  Constructor argument types.
   * @param args   Constructor arguments forwarded into the new object.
   * @return Shared pointer whose control block and object share one pool allocation.
   */
  template <typename T, typename... Args>
  [[maybe_unused]] static std::shared_ptr<T> make_shared(Args&&... args);

  /**
   * @brief Pool-backed analogue of @c std::make_unique.
   *
   * @details
   * Returns @c std::unique_ptr<T, @c Deleter<T>>.  Storage is returned to the pool when @c T 's
   * constructor throws.
   *
   * @tparam T     Object type.
   * @tparam Args  Constructor argument types.
   * @param args   Constructor arguments forwarded into the new object.
   * @return Owning pointer that returns memory to the pool on destruction.
   */
  template <typename T, typename... Args>
  [[maybe_unused]] static std::unique_ptr<T, MemoryResource::Deleter<T>> make_unique(Args&&... args);

 protected:
  void* do_allocate(size_t bytes, size_t alignment) override;

  void do_deallocate(void* p, size_t bytes, size_t alignment) override;

  bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

 private:
  explicit MemoryResource(MemoryPool& global_pool) noexcept;

  MemoryPool* pool_{nullptr};
  bool owns_pool_{false};

  VLINK_DISALLOW_COPY_AND_ASSIGN(MemoryResource)
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

template <typename T, typename... Args>
std::shared_ptr<T> MemoryResource::make_shared(Args&&... args) {
  std::pmr::polymorphic_allocator<T> alloc(&MemoryResource::global_instance());

  return std::allocate_shared<T>(alloc, std::forward<Args>(args)...);
}

template <typename T, typename... Args>
std::unique_ptr<T, MemoryResource::Deleter<T>> MemoryResource::make_unique(Args&&... args) {
  std::pmr::polymorphic_allocator<T> alloc(&MemoryResource::global_instance());

  T* p = alloc.allocate(1);

  try {
    std::allocator_traits<decltype(alloc)>::construct(alloc, p, std::forward<Args>(args)...);
  } catch (...) {
    alloc.deallocate(p, 1);
    throw;
  }

  return std::unique_ptr<T, MemoryResource::Deleter<T>>{p, MemoryResource::Deleter<T>{alloc}};
}

}  // namespace vlink

#else

namespace vlink {

/**
 * @namespace vlink::MemoryResource
 * @brief Fallback shim when @c <memory_resource> is unavailable.
 *
 * @details
 * Only @c make_shared and @c make_unique are emulated by forwarding to the corresponding
 * @c std versions.  The @c MemoryResource class, the @c Deleter alias and
 * @c global_instance are not provided in this mode.
 */
namespace MemoryResource {  // NOLINT(readability-identifier-naming)

/**
 * @brief Fallback @c make_shared forwarding to @c std::make_shared.
 */
template <typename T, typename... Args>
[[maybe_unused]] std::shared_ptr<T> make_shared(Args&&... args) {
  return std::make_shared<T>(std::forward<Args>(args)...);
}

/**
 * @brief Fallback @c make_unique forwarding to @c std::make_unique.
 */
template <typename T, typename... Args>
[[maybe_unused]] std::unique_ptr<T> make_unique(Args&&... args) {
  return std::make_unique<T>(std::forward<Args>(args)...);
}

}  // namespace MemoryResource

}  // namespace vlink

#endif
