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

#include "./base/memory_resource.h"

#ifdef VLINK_ENABLE_BASE_MEMORY_RESOURCE

#define MEMORY_RESOURCE_NERVER_DELETE 0

#include <new>

#include "./base/macros.h"

namespace vlink {

MemoryResource::MemoryResource() : pool_(new MemoryPool()), owns_pool_(true) {}

MemoryResource::MemoryResource(int level, bool prealloc) : pool_(new MemoryPool(level, prealloc)), owns_pool_(true) {}

MemoryResource::MemoryResource(const MemoryPool::Config& config) : pool_(new MemoryPool(config)), owns_pool_(true) {}

MemoryResource::~MemoryResource() {
  if (owns_pool_) {
    delete pool_;
  }
}

MemoryPool& MemoryResource::get_memory_pool() noexcept { return *pool_; }

void MemoryResource::trim() noexcept { pool_->trim(); }

MemoryResource& MemoryResource::global_instance(bool use_env_level) {
#if MEMORY_RESOURCE_NERVER_DELETE
  alignas(MemoryResource) static char buf[sizeof(MemoryResource)];

  static auto* instance = new (buf) MemoryResource(MemoryPool::global_instance(use_env_level));

  return *instance;
#else
  static MemoryResource instance(MemoryPool::global_instance(use_env_level));

  return instance;
#endif
}

void* MemoryResource::do_allocate(size_t bytes, size_t alignment) {
  void* p = pool_->allocate(bytes, alignment);

  if VUNLIKELY (p == nullptr) {
    throw std::bad_alloc();
  }

  return p;
}

void MemoryResource::do_deallocate(void* p, size_t bytes, size_t alignment) { pool_->deallocate(p, bytes, alignment); }

bool MemoryResource::do_is_equal(const std::pmr::memory_resource& other) const noexcept {
  if (this == &other) {
    return true;
  }

#if defined(NDEBUG) || defined(__ANDROID__)
  const auto* rhs = static_cast<const MemoryResource*>(&other);
#else
  const auto* rhs = dynamic_cast<const MemoryResource*>(&other);
#endif

  return rhs != nullptr && pool_ == rhs->pool_;
}

// NOLINTNEXTLINE(modernize-use-default-member-init)
MemoryResource::MemoryResource(MemoryPool& global_pool) noexcept : pool_(&global_pool), owns_pool_(false) {}

}  // namespace vlink

#endif
