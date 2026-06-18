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

#include "./extension/dynamic_data.h"

#include <cstring>
#include <string_view>
#include <utility>

namespace vlink {

// DynamicData
DynamicData::DynamicData() = default;

DynamicData::DynamicData(const DynamicData& target) : data_(target.data_) { refresh_type_view(target.type_.size()); }

DynamicData::DynamicData(DynamicData&& target) noexcept : data_(std::move(target.data_)) {
  refresh_type_view(target.type_.size());
  target.type_ = std::string_view();
}

DynamicData& DynamicData::operator=(const DynamicData& target) {
  if VUNLIKELY (this == &target) {
    return *this;
  }

  data_ = target.data_;
  refresh_type_view(target.type_.size());
  return *this;
}

DynamicData& DynamicData::operator=(DynamicData&& target) noexcept {
  if VUNLIKELY (this == &target) {
    return *this;
  }

  const auto type_size = target.type_.size();
  data_ = std::move(target.data_);
  refresh_type_view(type_size);
  target.type_ = std::string_view();
  return *this;
}

const Bytes& DynamicData::get_data() const { return data_; }

const std::string_view& DynamicData::get_type() const { return type_; }

bool DynamicData::is_empty() const { return data_.empty(); }

bool DynamicData::operator==(const DynamicData& target) const { return data_ == target.data_ && type_ == target.type_; }

bool DynamicData::operator!=(const DynamicData& target) const { return !(*this == target); }

bool DynamicData::operator<<(const Bytes& bytes) noexcept {
  if VUNLIKELY (bytes.size() < get_offset() || bytes.data() == nullptr) {
    return false;
  }

  auto next_data = Bytes::deep_copy(bytes.data() + get_offset(), bytes.size() - get_offset(), get_offset());

  if VUNLIKELY (next_data.real_data() == nullptr) {
    return false;
  }

  std::memcpy(next_data.real_data(), bytes.data(), get_offset());
  const char* type_s = reinterpret_cast<const char*>(next_data.real_data());
  const auto* type_end = static_cast<const char*>(std::memchr(type_s, '\0', get_offset()));

  if VUNLIKELY (type_end == nullptr) {
    return false;
  }

  const auto type_size = static_cast<size_t>(type_end - type_s);
  data_ = std::move(next_data);
  refresh_type_view(type_size);

  return true;
}

bool DynamicData::operator>>(Bytes& bytes) const noexcept {
  if VUNLIKELY (data_.empty() || data_.offset() == 0 || !data_.is_owner()) {
    return false;
  }

  bytes = Bytes::shallow_copy(data_.real_data(), data_.real_size());

  return true;
}

void DynamicData::refresh_type_view() noexcept {
  if VUNLIKELY (data_.real_data() == nullptr || data_.offset() < get_offset()) {
    type_ = std::string_view();
    return;
  }

  const char* type_s = reinterpret_cast<const char*>(data_.real_data());
  const auto* type_end = static_cast<const char*>(std::memchr(type_s, '\0', get_offset()));

  if VUNLIKELY (type_end == nullptr) {
    type_ = std::string_view();
    return;
  }

  type_ = std::string_view(type_s, static_cast<size_t>(type_end - type_s));
}

void DynamicData::refresh_type_view(size_t type_size) noexcept {
  if VUNLIKELY (type_size == 0 || type_size >= get_offset() || data_.real_data() == nullptr ||
                data_.offset() < get_offset()) {
    type_ = std::string_view();
    return;
  }

  type_ = std::string_view(reinterpret_cast<const char*>(data_.real_data()), type_size);
}

}  // namespace vlink
