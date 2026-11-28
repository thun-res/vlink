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

#include "./ddsc_proxy.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <string>

#include "./base/bytes.h"
#include "./base/logger.h"
#include "./builtin/BuiltInRaw.h"

#if VLINK_DDSC_USE_CUSTOM_SERTYPE
#include <dds/ddsi/q_radmin.h>
#include <dds/ddsrt/heap.h>
#endif

namespace ddsc {

#if VLINK_DDSC_USE_CUSTOM_SERTYPE
struct BuiltInRawSertype final {
  struct ddsi_sertype c;
  BuiltInRawSerdata* untyped_key{nullptr};
  BuiltInRawSertype* xcdr2_variant{nullptr};
  uint8_t enc_le{0x01U};
  uint8_t enc_be{0x00U};
};

static uint8_t built_in_raw_enc_id(const struct ddsi_sertype* type) noexcept {
  const bool le = vlink::Bytes::is_little_endian();

  if VLIKELY (type != nullptr) {
    const auto* st = reinterpret_cast<const BuiltInRawSertype*>(type);
    return le ? st->enc_le : st->enc_be;
  }

  return le ? 0x01U : 0x00U;
}

static uint32_t bswap32(uint32_t value) noexcept {
  return ((value & 0x000000FFU) << 24U) | ((value & 0x0000FF00U) << 8U) | ((value & 0x00FF0000U) >> 8U) |
         ((value & 0xFF000000U) >> 24U);
}

static uint64_t bswap64(uint64_t value) noexcept {
  return ((value & 0x00000000000000FFULL) << 56U) | ((value & 0x000000000000FF00ULL) << 40U) |
         ((value & 0x0000000000FF0000ULL) << 24U) | ((value & 0x00000000FF000000ULL) << 8U) |
         ((value & 0x000000FF00000000ULL) >> 8U) | ((value & 0x0000FF0000000000ULL) >> 24U) |
         ((value & 0x00FF000000000000ULL) >> 40U) | ((value & 0xFF00000000000000ULL) >> 56U);
}

static void built_in_raw_serdata_free(struct ddsi_serdata* d) {
  auto* sd = reinterpret_cast<BuiltInRawSerdata*>(d);

  if (sd->wire_buf != nullptr) {
    vlink::Bytes::bytes_free(sd->wire_buf, sd->wire_cap);
    sd->wire_buf = nullptr;
    sd->wire_cap = 0U;
  }

  delete sd;
}

static BuiltInRawSerdata* alloc_built_in_raw_serdata(const struct ddsi_sertype* type, enum ddsi_serdata_kind kind,
                                                     size_t wire_size) {
  if VUNLIKELY (wire_size > static_cast<size_t>(std::numeric_limits<uint32_t>::max() - 3U)) {
    return nullptr;
  }

  auto* sd = new (std::nothrow) BuiltInRawSerdata{};

  if VUNLIKELY (!sd) {
    return nullptr;
  }

  ddsi_serdata_init(&sd->c, type, kind);

  if (wire_size > 0U) {
    const size_t cap = (wire_size + 3U) & ~size_t{3U};
    uint8_t* buf = vlink::Bytes::bytes_malloc(cap);

    if VUNLIKELY (!buf) {
      delete sd;
      return nullptr;
    }

    if (cap > wire_size) {
      std::memset(buf + wire_size, 0, cap - wire_size);
    }

    sd->wire_cap = cap;
    sd->wire_size = static_cast<uint32_t>(wire_size);
    sd->wire_buf = buf;
  }

  return sd;
}

static BuiltInRawSerdata* alloc_built_in_raw_key_serdata(const struct ddsi_sertype* type, enum ddsi_serdata_kind kind) {
  auto* sd = alloc_built_in_raw_serdata(type, kind, 4U);

  if VUNLIKELY (!sd) {
    return nullptr;
  }

  sd->wire_buf[0] = 0x00U;
  sd->wire_buf[1] = built_in_raw_enc_id(type);
  sd->wire_buf[2] = 0x00U;
  sd->wire_buf[3] = 0x00U;
  return sd;
}

static bool parse_built_in_raw_header(BuiltInRawSerdata* sd) noexcept {
  uint8_t* buf = sd != nullptr ? sd->wire_buf : nullptr;

  if VUNLIKELY (!buf || sd->wire_size < kRawWireHeaderSize) {
    return false;
  }

  const uint8_t enc_id = buf[1];
  const bool valid_enc = (enc_id == 0x00U || enc_id == 0x01U || enc_id == 0x06U || enc_id == 0x07U);

  if VUNLIKELY (buf[0] != 0x00U || !valid_enc) {
    return false;
  }

  const uint32_t pad_bytes = buf[3] & 0x03U;

  if VUNLIKELY (sd->wire_size < kRawWireHeaderSize + pad_bytes) {
    return false;
  }

  uint64_t raw_id = 0U;
  uint32_t raw_len = 0U;
  std::memcpy(&raw_id, buf + 4U, sizeof(raw_id));
  std::memcpy(&raw_len, buf + 12U, sizeof(raw_len));

  const bool wire_le = (enc_id == 0x01U || enc_id == 0x07U);

  if VUNLIKELY (wire_le != vlink::Bytes::is_little_endian()) {
    raw_id = bswap64(raw_id);
    raw_len = bswap32(raw_len);
  }

  if VUNLIKELY (raw_len > sd->wire_size - kRawWireHeaderSize - pad_bytes) {
    return false;
  }

  sd->id = raw_id;
  sd->payload_size = raw_len;
  return true;
}

static bool built_in_raw_eqkey(const struct ddsi_serdata* a, const struct ddsi_serdata* b) {
  (void)a;
  (void)b;
  return true;
}

static uint32_t built_in_raw_get_size(const struct ddsi_serdata* d) {
  const auto* sd = reinterpret_cast<const BuiltInRawSerdata*>(d);
  return static_cast<uint32_t>(sd->wire_cap);
}

static struct ddsi_serdata* built_in_raw_from_ser(const struct ddsi_sertype* type, enum ddsi_serdata_kind kind,
                                                  const struct nn_rdata* fragchain, size_t size) {
  if VUNLIKELY (kind != SDK_DATA) {
    auto* sd = alloc_built_in_raw_key_serdata(type, kind);
    return sd ? &sd->c : nullptr;
  }

  if VUNLIKELY (size < kRawWireHeaderSize || size > static_cast<size_t>(std::numeric_limits<uint32_t>::max() - 3U)) {
    return nullptr;
  }

  auto* sd = alloc_built_in_raw_serdata(type, kind, size);

  if VUNLIKELY (!sd) {
    return nullptr;
  }

  uint8_t* wire_buf = sd->wire_buf;
  size_t off = 0U;

  for (const struct nn_rdata* frag = fragchain; frag != nullptr && off < size; frag = frag->nextfrag) {
    if (frag->maxp1 <= off) {
      continue;
    }

    if VUNLIKELY (frag->min > off) {
      built_in_raw_serdata_free(&sd->c);
      return nullptr;
    }

    const size_t frag_skip = off - static_cast<size_t>(frag->min);
    const size_t avail = static_cast<size_t>(frag->maxp1) - off;
    const size_t copy_len = std::min(avail, size - off);
    const uint8_t* src = NN_RMSG_PAYLOADOFF(frag->rmsg, NN_RDATA_PAYLOAD_OFF(frag)) + frag_skip;
    std::memcpy(wire_buf + off, src, copy_len);
    off += copy_len;
  }

  if VUNLIKELY (off != size || !parse_built_in_raw_header(sd)) {
    built_in_raw_serdata_free(&sd->c);
    return nullptr;
  }

  return &sd->c;
}

static struct ddsi_serdata* built_in_raw_from_ser_iov(const struct ddsi_sertype* type, enum ddsi_serdata_kind kind,
                                                      ddsrt_msg_iovlen_t niov, const ddsrt_iovec_t* iov, size_t size) {
  if VUNLIKELY (kind != SDK_DATA) {
    auto* sd = alloc_built_in_raw_key_serdata(type, kind);
    return sd ? &sd->c : nullptr;
  }

  if VUNLIKELY (size < kRawWireHeaderSize || size > static_cast<size_t>(std::numeric_limits<uint32_t>::max() - 3U)) {
    return nullptr;
  }

  auto* sd = alloc_built_in_raw_serdata(type, kind, size);

  if VUNLIKELY (!sd) {
    return nullptr;
  }

  uint8_t* wire_buf = sd->wire_buf;
  size_t off = 0U;

  for (ddsrt_msg_iovlen_t i = 0; i < niov && off < size; ++i) {
    const size_t copy_len = std::min(static_cast<size_t>(iov[i].iov_len), size - off);

    if (copy_len > 0U) {
      std::memcpy(wire_buf + off, iov[i].iov_base, copy_len);
      off += copy_len;
    }
  }

  if VUNLIKELY (off != size || !parse_built_in_raw_header(sd)) {
    built_in_raw_serdata_free(&sd->c);
    return nullptr;
  }

  return &sd->c;
}

static struct ddsi_serdata* built_in_raw_from_keyhash(const struct ddsi_sertype* type,
                                                      const struct ddsi_keyhash* keyhash) {
  (void)keyhash;
  auto* sd = alloc_built_in_raw_key_serdata(type, SDK_KEY);
  return sd ? &sd->c : nullptr;
}

static struct ddsi_serdata* built_in_raw_from_sample(const struct ddsi_sertype* type, enum ddsi_serdata_kind kind,
                                                     const void* sample) {
  if VUNLIKELY (kind != SDK_DATA) {
    auto* sd = alloc_built_in_raw_key_serdata(type, kind);
    return sd ? &sd->c : nullptr;
  }

  const auto* msg = static_cast<const vlink_dds__BuiltInRaw_*>(sample);

  if VUNLIKELY (!msg || (msg->data._length > 0U && msg->data._buffer == nullptr)) {
    return nullptr;
  }

  if VUNLIKELY (msg->data._length > std::numeric_limits<uint32_t>::max() - kRawWireHeaderSize - 3U) {
    return nullptr;
  }

  const size_t wire_size = static_cast<size_t>(kRawWireHeaderSize) + static_cast<size_t>(msg->data._length);
  auto* sd = alloc_built_in_raw_serdata(type, kind, wire_size);

  if VUNLIKELY (!sd) {
    return nullptr;
  }

  uint8_t* wire_buf = sd->wire_buf;
  wire_buf[0] = 0x00U;
  wire_buf[1] = built_in_raw_enc_id(type);
  wire_buf[2] = 0x00U;
  wire_buf[3] = static_cast<uint8_t>(sd->wire_cap - wire_size) & 0x03U;
  std::memcpy(wire_buf + 4U, &msg->id, sizeof(msg->id));
  std::memcpy(wire_buf + 12U, &msg->data._length, sizeof(msg->data._length));

  if (msg->data._length > 0U) {
    std::memcpy(wire_buf + kRawWireHeaderSize, msg->data._buffer, msg->data._length);
  }

  sd->id = msg->id;
  sd->payload_size = msg->data._length;
  return &sd->c;
}

static void built_in_raw_to_ser(const struct ddsi_serdata* d, size_t off, size_t sz, void* buf) {
  const auto* sd = reinterpret_cast<const BuiltInRawSerdata*>(d);

  if VUNLIKELY (sz == 0U || !buf) {
    return;
  }

  if VLIKELY (sd->wire_buf != nullptr && off <= sd->wire_cap && sz <= sd->wire_cap - off) {
    std::memcpy(buf, sd->wire_buf + off, sz);
    return;
  }

  std::memset(buf, 0, sz);
}

static struct ddsi_serdata* built_in_raw_to_ser_ref(const struct ddsi_serdata* d, size_t off, size_t sz,
                                                    ddsrt_iovec_t* ref) {
  const auto* sd = reinterpret_cast<const BuiltInRawSerdata*>(d);

  if VLIKELY (sd->wire_buf != nullptr && off <= sd->wire_cap && sz <= sd->wire_cap - off) {
    ref->iov_base = sd->wire_buf + off;
    ref->iov_len = static_cast<ddsrt_iov_len_t>(sz);
  } else {
    ref->iov_base = nullptr;
    ref->iov_len = 0U;
  }

  return ddsi_serdata_ref(d);
}

static void built_in_raw_to_ser_unref(struct ddsi_serdata* d, const ddsrt_iovec_t* ref) {
  (void)ref;
  ddsi_serdata_unref(d);
}

static bool built_in_raw_to_sample(const struct ddsi_serdata* d, void* sample, void** bufptr, void* buflim) {
  const auto* sd = reinterpret_cast<const BuiltInRawSerdata*>(d);
  auto* dst = static_cast<vlink_dds__BuiltInRaw_*>(sample);

  if VUNLIKELY (!dst) {
    return false;
  }

  if VUNLIKELY (sd->c.kind != SDK_DATA) {
    dst->id = 0U;
    dst->data._length = 0U;
    return true;
  }

  dst->id = sd->id;
  const uint32_t len = sd->payload_size;
  const uint8_t* src_payload = sd->payload_data();

  if (len == 0U || src_payload == nullptr) {
    dst->data._length = 0U;
    return true;
  }

  if (bufptr != nullptr && *bufptr != nullptr) {
    auto* cur_ptr = static_cast<uint8_t*>(*bufptr);
    auto* lim_ptr = static_cast<uint8_t*>(buflim);

    if VUNLIKELY (lim_ptr < cur_ptr || static_cast<size_t>(lim_ptr - cur_ptr) < len) {
      return false;
    }

    dst->data._buffer = cur_ptr;
    dst->data._maximum = len;
    dst->data._length = len;
    dst->data._release = false;
    *bufptr = cur_ptr + len;
  } else {
    if (dst->data._maximum < len || !dst->data._buffer) {
      auto* next_buf = static_cast<uint8_t*>(dds_realloc(dst->data._release ? dst->data._buffer : nullptr, len));

      if VUNLIKELY (!next_buf) {
        return false;
      }

      dst->data._buffer = next_buf;
      dst->data._maximum = len;
      dst->data._release = true;
    }

    dst->data._length = len;
  }

  std::memcpy(dst->data._buffer, src_payload, len);
  return true;
}

static struct ddsi_serdata* built_in_raw_to_untyped(const struct ddsi_serdata* d) {
  if VUNLIKELY (!d->type) {
    return ddsi_serdata_ref(d);
  }

  const auto* st = reinterpret_cast<const BuiltInRawSertype*>(d->type);
  return ddsi_serdata_ref(&st->untyped_key->c);
}

static bool built_in_raw_untyped_to_sample(const struct ddsi_sertype* type, const struct ddsi_serdata* d, void* sample,
                                           void** bufptr, void* buflim) {
  (void)type;
  (void)d;
  (void)bufptr;
  (void)buflim;

  if (sample != nullptr) {
    auto* dst = static_cast<vlink_dds__BuiltInRaw_*>(sample);
    dst->id = 0U;
    dst->data._length = 0U;
  }

  return true;
}

static size_t built_in_raw_print(const struct ddsi_sertype* type, const struct ddsi_serdata* d, char* buf,
                                 size_t size) {
  (void)type;
  (void)d;

  if (buf != nullptr && size > 0U) {
    buf[0] = '\0';
  }

  return 0U;
}

static void built_in_raw_get_keyhash(const struct ddsi_serdata* d, struct ddsi_keyhash* buf, bool force_md5) {
  (void)d;
  (void)force_md5;

  if (buf != nullptr) {
    std::memset(buf->value, 0, sizeof(buf->value));
  }
}

static const struct ddsi_serdata_ops kBuiltInRawSerdataOps = {
    built_in_raw_eqkey,
    built_in_raw_get_size,
    built_in_raw_from_ser,
    built_in_raw_from_ser_iov,
    built_in_raw_from_keyhash,
    built_in_raw_from_sample,
    built_in_raw_to_ser,
    built_in_raw_to_ser_ref,
    built_in_raw_to_ser_unref,
    built_in_raw_to_sample,
    built_in_raw_to_untyped,
    built_in_raw_untyped_to_sample,
    built_in_raw_serdata_free,
    built_in_raw_print,
    built_in_raw_get_keyhash,
#ifdef DDS_HAS_SHM
    nullptr,
    nullptr,
#endif
};

static void built_in_raw_sertype_free(struct ddsi_sertype* tp) {
  auto* st = reinterpret_cast<BuiltInRawSertype*>(tp);

  if (st->xcdr2_variant != nullptr) {
    ddsi_sertype_unref(&st->xcdr2_variant->c);
    st->xcdr2_variant = nullptr;
  }

  if (st->untyped_key != nullptr) {
    ddsi_serdata_unref(&st->untyped_key->c);
    st->untyped_key = nullptr;
  }

  ddsi_sertype_fini(&st->c);
  delete st;
}

static void built_in_raw_zero_samples(const struct ddsi_sertype* d, void* samples, size_t count) {
  (void)d;

  if (samples != nullptr && count > 0U) {
    std::memset(samples, 0, count * sizeof(vlink_dds__BuiltInRaw_));
  }
}

static void built_in_raw_realloc_samples(void** ptrs, const struct ddsi_sertype* d, void* old, size_t oldcount,
                                         size_t count) {
  (void)d;
  auto* new_arr = static_cast<vlink_dds__BuiltInRaw_*>(
      count > 0U ? dds_realloc(old, count * sizeof(vlink_dds__BuiltInRaw_)) : nullptr);

  if (count == 0U && old != nullptr) {
    dds_free(old);
  }

  if (new_arr != nullptr && count > oldcount) {
    std::memset(new_arr + oldcount, 0, (count - oldcount) * sizeof(vlink_dds__BuiltInRaw_));
  }

  for (size_t i = 0U; i < count; ++i) {
    ptrs[i] = new_arr ? static_cast<void*>(new_arr + i) : nullptr;
  }
}

static void built_in_raw_free_samples(const struct ddsi_sertype* d, void** ptrs, size_t count, dds_free_op_t op) {
  (void)d;

  if (count == 0U || ptrs == nullptr) {
    return;
  }

  if ((op & DDS_FREE_CONTENTS_BIT) != 0U) {
    for (size_t i = 0U; i < count; ++i) {
      if (ptrs[i] != nullptr) {
        vlink_dds__BuiltInRaw__free(static_cast<vlink_dds__BuiltInRaw_*>(ptrs[i]), DDS_FREE_CONTENTS);
      }
    }
  }

  if ((op & DDS_FREE_ALL_BIT) != 0U) {
    dds_free(ptrs[0]);
  }
}

static bool built_in_raw_sertype_equal(const struct ddsi_sertype* a, const struct ddsi_sertype* b) {
  const auto* sa = reinterpret_cast<const BuiltInRawSertype*>(a);
  const auto* sb = reinterpret_cast<const BuiltInRawSertype*>(b);
  return sa->enc_le == sb->enc_le && sa->enc_be == sb->enc_be;
}

static uint32_t built_in_raw_sertype_hash(const struct ddsi_sertype* tp) {
  const auto* st = reinterpret_cast<const BuiltInRawSertype*>(tp);
  return (static_cast<uint32_t>(st->enc_le) << 8U) | static_cast<uint32_t>(st->enc_be);
}

static struct ddsi_sertype* built_in_raw_derive_sertype(const struct ddsi_sertype* base_sertype,
                                                        dds_data_representation_id_t data_representation,
                                                        dds_type_consistency_enforcement_qospolicy_t tce_qos) {
  (void)tce_qos;

  if VUNLIKELY (!base_sertype) {
    return nullptr;
  }

  const auto* base_st = reinterpret_cast<const BuiltInRawSertype*>(base_sertype);

  if (data_representation == DDS_DATA_REPRESENTATION_XCDR1) {
    return const_cast<struct ddsi_sertype*>(base_sertype);
  }

  if (data_representation == DDS_DATA_REPRESENTATION_XCDR2 && base_st->xcdr2_variant != nullptr) {
    return &base_st->xcdr2_variant->c;
  }

  return const_cast<struct ddsi_sertype*>(base_sertype);
}

static const struct ddsi_sertype_ops kBuiltInRawSertypeOps = {
    ddsi_sertype_v0,
    nullptr,
    built_in_raw_sertype_free,
    built_in_raw_zero_samples,
    built_in_raw_realloc_samples,
    built_in_raw_free_samples,
    built_in_raw_sertype_equal,
    built_in_raw_sertype_hash,
    nullptr,
    nullptr,
    nullptr,
    built_in_raw_derive_sertype,
    nullptr,
    nullptr,
};

static struct ddsi_sertype* create_built_in_raw_sertype() {
  auto* st = new (std::nothrow) BuiltInRawSertype{};

  if VUNLIKELY (!st) {
    return nullptr;
  }

  ddsi_sertype_init_flags(&st->c, vlink_dds__BuiltInRaw__desc.m_typename, &kBuiltInRawSertypeOps,
                          &kBuiltInRawSerdataOps, DDSI_SERTYPE_FLAG_TOPICKIND_NO_KEY);

  if VUNLIKELY (!st->c.type_name) {
    ddsi_sertype_unref(&st->c);
    return nullptr;
  }

  st->c.allowed_data_representation = DDS_DATA_REPRESENTATION_RESTRICT_DEFAULT;
  st->enc_le = 0x01U;
  st->enc_be = 0x00U;
  st->untyped_key = alloc_built_in_raw_key_serdata(&st->c, SDK_KEY);

  if VUNLIKELY (!st->untyped_key) {
    ddsi_sertype_unref(&st->c);
    return nullptr;
  }

  st->untyped_key->c.type = nullptr;

  auto* xcdr2 = new (std::nothrow) BuiltInRawSertype{};

  if VUNLIKELY (!xcdr2) {
    ddsi_sertype_unref(&st->c);
    return nullptr;
  }

  ddsi_sertype_init_flags(&xcdr2->c, vlink_dds__BuiltInRaw__desc.m_typename, &kBuiltInRawSertypeOps,
                          &kBuiltInRawSerdataOps, DDSI_SERTYPE_FLAG_TOPICKIND_NO_KEY);

  if VUNLIKELY (!xcdr2->c.type_name) {
    ddsi_sertype_unref(&xcdr2->c);
    ddsi_sertype_unref(&st->c);
    return nullptr;
  }

  xcdr2->c.allowed_data_representation = DDS_DATA_REPRESENTATION_RESTRICT_DEFAULT;
  xcdr2->enc_le = 0x07U;
  xcdr2->enc_be = 0x06U;
  xcdr2->untyped_key = reinterpret_cast<BuiltInRawSerdata*>(ddsi_serdata_ref(&st->untyped_key->c));
  st->xcdr2_variant = xcdr2;
  return &st->c;
}

const struct ddsi_serdata_ops* built_in_raw_serdata_ops() noexcept { return &kBuiltInRawSerdataOps; }
#endif

// DomainParticipant
DomainParticipant::DomainParticipant(int32_t domain, const dds_qos_t* qos) {
  entity = dds_create_participant(domain, qos, nullptr);

  if VUNLIKELY (entity <= 0) {
    CLOG_E("dds_create_participant: %s.", dds_strretcode(-entity));
  }
}

DomainParticipant::~DomainParticipant() {
  if VUNLIKELY (entity <= 0) {
    return;
  }

  dds_delete(entity);

  entity = 0;
}

// Topic
Topic::Topic(dds_entity_t part, const std::string& topic, const std::string& type_name, bool use_default_sertype) {
  (void)type_name;

#if VLINK_DDSC_USE_CUSTOM_SERTYPE
  if (!use_default_sertype) {
    struct ddsi_sertype* sertype = create_built_in_raw_sertype();

    if VUNLIKELY (!sertype) {
      entity = DDS_RETCODE_OUT_OF_RESOURCES;
      CLOG_E("dds_create_topic_sertype: out of resources.");
      return;
    }

    entity = dds_create_topic_sertype(part, topic.c_str(), &sertype, nullptr, nullptr, nullptr);

    if VUNLIKELY (entity <= 0) {
      ddsi_sertype_unref(sertype);
      CLOG_E("dds_create_topic_sertype: %s.", dds_strretcode(-entity));
    }

    return;
  }
#else
  (void)use_default_sertype;
#endif

  entity = dds_create_topic(part, &vlink_dds__BuiltInRaw__desc, topic.c_str(), nullptr, nullptr);

  if VUNLIKELY (entity <= 0) {
    CLOG_E("dds_create_topic: %s.", dds_strretcode(-entity));
  }
}

Topic::~Topic() {
  if VUNLIKELY (entity <= 0) {
    return;
  }

  dds_delete(entity);

  entity = 0;
}

// Publisher
Publisher::Publisher(dds_entity_t part) {
  entity = dds_create_publisher(part, nullptr, nullptr);

  if VUNLIKELY (entity <= 0) {
    CLOG_E("dds_create_publisher: %s.", dds_strretcode(-entity));
  }
}

Publisher::~Publisher() {
  if VUNLIKELY (entity <= 0) {
    return;
  }

  dds_delete(entity);

  entity = 0;
}

// Subscriber
Subscriber::Subscriber(dds_entity_t part) {
  entity = dds_create_subscriber(part, nullptr, nullptr);

  if VUNLIKELY (entity <= 0) {
    CLOG_E("dds_create_subscriber: %s.", dds_strretcode(-entity));
  }
}

Subscriber::~Subscriber() {
  if VUNLIKELY (entity <= 0) {
    return;
  }

  dds_delete(entity);

  entity = 0;
}

// DataWriter
DataWriter::DataWriter(dds_entity_t pub, dds_entity_t topic, const dds_qos_t* qos, const dds_listener_t* listener) {
  entity = dds_create_writer(pub, topic, qos, listener);

  if VUNLIKELY (entity <= 0) {
    CLOG_E("dds_create_writer: %s.", dds_strretcode(-entity));
  }
}

DataWriter::~DataWriter() {
  if VUNLIKELY (entity <= 0) {
    return;
  }

  dds_set_listener(entity, nullptr);
  dds_delete(entity);

  entity = 0;
}

// DataReader
DataReader::DataReader(dds_entity_t sub, dds_entity_t topic, const dds_qos_t* qos, const dds_listener_t* listener) {
  entity = dds_create_reader(sub, topic, qos, listener);

  if VUNLIKELY (entity <= 0) {
    CLOG_E("dds_create_reader: %s.", dds_strretcode(-entity));
  }
}

DataReader::~DataReader() {
  if VUNLIKELY (entity <= 0) {
    return;
  }

  dds_set_listener(entity, nullptr);
  dds_delete(entity);

  entity = 0;
}

}  // namespace ddsc
