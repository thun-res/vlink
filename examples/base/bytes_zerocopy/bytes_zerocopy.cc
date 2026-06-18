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

#include <vlink/base/bytes.h>
#include <vlink/base/logger.h>

#include <cstring>

// Dumps the three ownership flags so each scenario below makes its
// shallow / owning / loaned / opaque-pointer status explicit.
static void print_ownership(const char* label, const vlink::Bytes& b) {
  VLOG_I(label, ": size=", b.size(), " is_owner=", b.is_owner(), " is_loaned=", b.is_loaned(), " is_ptr=", b.is_ptr(),
         " empty=", b.empty());
}
// -----------------------------------------------------------------------------
// Bytes zero-copy / ownership example
//
// Module:   vlink/base/bytes.h
// Scenario: Walk through every ownership flavour Bytes can carry. This is the
//           foundation of VLink's zero-copy story: the same Bytes type is used
//           whether the payload is owned (heap/SBO), aliasing an external
//           buffer (shallow_copy), owning a clone of an external buffer
//           (deep_copy), loaned from a transport (loan_internal, e.g.
//           iceoryx shm), or wrapping an opaque pointer (shallow_copy_ptr).
// CAUTION:  shallow_copy / loan_internal DO NOT extend the lifetime of the
//           underlying memory -- the caller is responsible for keeping the
//           source alive for at least as long as the Bytes alias is used.
//           Use deep_copy_self() to promote an alias into a real owner before
//           crossing thread / loop / process boundaries.
// -----------------------------------------------------------------------------
int main() {
  VLOG_I("=== Bytes Zero-Copy / Ownership Example ===");

  // shallow_copy(mutable): non-owning alias to an external writable buffer.
  // The Bytes does NOT free the memory; writing through the alias mutates
  // the original buffer in place (true zero-copy view).
  {
    uint8_t external_buf[32];
    std::memset(external_buf, 0x42, sizeof(external_buf));

    auto alias = vlink::Bytes::shallow_copy(external_buf, sizeof(external_buf));
    print_ownership("shallow_copy mut", alias);
    alias[0] = 0xFF;
    VLOG_I("external_buf[0] after alias write: 0x", std::hex, static_cast<int>(external_buf[0]));
  }

  // shallow_copy(const): same as above but read-only -- attempting to write
  // via operator[] would be UB; the Bytes treats the buffer as immutable.
  {
    const uint8_t read_only[] = {0x10, 0x20, 0x30};
    auto alias = vlink::Bytes::shallow_copy(read_only, sizeof(read_only));
    print_ownership("shallow_copy const", alias);
  }

  // deep_copy from an external buffer: allocates fresh storage and copies
  // the bytes in. The resulting Bytes is a real owner -- safe to outlive
  // the source buffer and safe to pass to another thread.
  {
    uint8_t external_buf[] = {0xAA, 0xBB, 0xCC, 0xDD};
    auto owned = vlink::Bytes::deep_copy(external_buf, sizeof(external_buf));
    print_ownership("deep_copy ext", owned);
    VLOG_I("distinct memory=", (owned.data() != external_buf), " content ok=", (owned[0] == 0xAA && owned[3] == 0xDD));
  }

  // deep_copy with reserved offset: copies the payload but leaves an extra
  // header slot in front (see bytes_basic for the offset rationale).
  {
    uint8_t payload[] = {0x01, 0x02, 0x03};
    auto buf = vlink::Bytes::deep_copy(payload, sizeof(payload), /*offset=*/4);
    print_ownership("deep_copy offset=4", buf);
    VLOG_I("offset=", static_cast<int>(buf.offset()), " size=", buf.size(), " real_size=", buf.real_size());
  }

  // Instance methods mirror the static factories. deep_copy(other) yields a
  // distinct owning buffer; shallow_copy(other) shares the same memory and
  // marks the new Bytes as a non-owning alias of the source.
  {
    auto original = vlink::Bytes::from_string("original data");

    vlink::Bytes deep;
    deep.deep_copy(original);
    print_ownership("after deep_copy", deep);
    VLOG_I("distinct memory=", (deep.data() != original.data()));

    vlink::Bytes alias;
    alias.shallow_copy(original);
    print_ownership("after shallow_copy", alias);
    VLOG_I("same memory=", (alias.data() == original.data()));
  }

  // deep_copy_self promotes an aliasing Bytes into a real owner by allocating
  // fresh storage and copying its current view. Critical when the underlying
  // source is about to be mutated/destroyed (e.g. before queueing across a
  // MessageLoop boundary).
  {
    uint8_t ext[] = {0x11, 0x22, 0x33};
    auto alias = vlink::Bytes::shallow_copy(ext, sizeof(ext));
    VLOG_I("before deep_copy_self is_owner=", alias.is_owner());

    alias.deep_copy_self();
    VLOG_I("after deep_copy_self is_owner=", alias.is_owner());
    // Mutate the original buffer after the promotion: the alias-now-owner
    // must remain stable because it has its own backing storage.
    ext[0] = 0xFF;
    VLOG_I("ext mutated, alias[0]=0x", std::hex, static_cast<int>(alias[0]));
  }

  // shallow_copy_ptr/to_ptr wrap an opaque pointer inside Bytes. Used by the
  // zerocopy::CameraFrame / PointCloud helpers and by user code that wants
  // to ship a non-trivially-copyable object through a pub/sub channel that
  // accepts Bytes payloads. No memory is owned -- the caller manages it.
  {
    int my_object = 42;
    auto ptr_bytes = vlink::Bytes::shallow_copy_ptr(&my_object);
    print_ownership("shallow_copy_ptr", ptr_bytes);

    int* recovered = ptr_bytes.to_ptr<int>();
    VLOG_I("to_ptr<int>=", *recovered, " same address=", (recovered == &my_object));
  }

  // loan_internal marks the Bytes as a buffer borrowed from a transport
  // layer (typical: iceoryx shm publish chunk). The Bytes does not free the
  // memory; the transport reclaims it when the publish call returns.
  {
    uint8_t simulated_shm[64];
    std::memset(simulated_shm, 0x99, sizeof(simulated_shm));
    auto loaned = vlink::Bytes::loan_internal(simulated_shm, sizeof(simulated_shm));
    print_ownership("loan_internal", loaned);
  }

  // Copy constructor preserves the source flavour: copying an owner clones
  // the storage, copying an alias produces another alias to the same memory.
  // This is intentional: copies of a loan are still loans.
  {
    auto owner = vlink::Bytes::from_string("owned");
    vlink::Bytes copy_of_owner(owner);
    VLOG_I("copy of owner: is_owner=", copy_of_owner.is_owner(),
           " distinct memory=", (copy_of_owner.data() != owner.data()));

    uint8_t ext[] = {1, 2, 3};
    auto alias = vlink::Bytes::shallow_copy(ext, 3);
    vlink::Bytes copy_of_alias(alias);
    VLOG_I("copy of alias: is_owner=", copy_of_alias.is_owner(),
           " same memory=", (copy_of_alias.data() == alias.data()));
  }

  // Move constructor transfers state without allocation; the source becomes
  // empty. Prefer std::move when handing a Bytes off to a callee.
  {
    auto source = vlink::Bytes::from_string("moveable");
    vlink::Bytes dest(std::move(source));
    VLOG_I("after move: dest.size=", dest.size(), " source.empty=", source.empty(), " dest=\"", dest.to_string(), "\"");
  }

  VLOG_I("=== Bytes Zero-Copy Example Complete ===");
  return 0;
}
