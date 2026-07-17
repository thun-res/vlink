# Fast-DDS — VLink modification notice (Apache License 2.0 §4(b))

> *Upstream*: eProsima Fast-DDS, <https://github.com/eProsima/Fast-DDS>
> *Upstream version*: v2.10.7 (fetched by CPM, see `cmake/cpm_thirdparty.cmake`)
> *Backported fix*: eProsima commit `28e2ce8fafb0cc1ebfdf883896fa39148e91c95c`
> (<https://github.com/eProsima/Fast-DDS/commit/28e2ce8fafb0cc1ebfdf883896fa39148e91c95c>)
> *Upstream license*: Apache License, Version 2.0
> *Modifier*: VLink contributors (<https://github.com/thun-res/vlink>)
> *Modifier license*: Apache License, Version 2.0 (same as upstream)

This document is the §4(b) "prominent notice" required by the Apache License,
Version 2.0 for the modifications VLink applies to Fast-DDS v2.10.7 at build
time, via `packup/patch/fastdds_2.10.x.patch`. The Apache 2.0 license text
covering both the original work and these modifications is shipped at
`licenses/fastdds/LICENSE` in this distribution.

## Files modified

| Path | Purpose of modification |
|------|-------------------------|
| `CMakeLists.txt` | Collapse the upstream QNX-specific `OPENSSL_FOUND=1` shortcut and the duplicated `SECURITY` branches into a single `find_package(OpenSSL [REQUIRED])` call. Reuse the imported targets supplied by `FindOpenSSL`, creating QNX fallback targets only when they are absent. The shortcut hard-codes a non-functional `OPENSSL_FOUND` value on QNX, which prevents VLink's `ENABLE_SECURITY=ON` build from detecting the absence of OpenSSL and emitting a clean diagnostic. |
| `include/fastrtps/utils/RefCountedPointer.hpp`, `include/fastdds/rtps/reader/LocalReaderPointer.hpp` | Backport eProsima's protected local-reader reference mechanism so an RTPS reader cannot be destroyed while an intra-process operation is using it. VLink additionally makes the active-state check, reference-count increment, and pointer acquisition one mutex-protected operation. |
| `include/fastdds/rtps/reader/RTPSReader.h`, `src/cpp/rtps/reader/RTPSReader.cpp` | Associate each reader with a protected local pointer and deactivate it before reader destruction. |
| `src/cpp/rtps/RTPSDomain.cpp`, `src/cpp/rtps/RTPSDomainImpl.hpp`, `src/cpp/rtps/participant/RTPSParticipantImpl.cpp`, `src/cpp/rtps/participant/RTPSParticipantImpl.h` | Return protected local-reader references and wait for active intra-process users before deleting a reader endpoint. |
| `include/fastdds/rtps/writer/ReaderLocator.h`, `include/fastdds/rtps/writer/ReaderProxy.h`, `src/cpp/rtps/writer/ReaderLocator.cpp`, `src/cpp/rtps/writer/StatefulWriter.cpp`, `src/cpp/rtps/writer/StatelessWriter.cpp` | Hold a protected reader instance for the complete intra-process data, GAP, heartbeat, data-sharing, and stateless delivery calls. |

## Why the modification

VLink statically links Fast-DDS into `libvlink-dds.so`. The upstream
QNX `OPENSSL_FOUND=1` short-circuit is incompatible with VLink's
unified detection logic (which falls back gracefully when OpenSSL is
not present). The modification preserves Fast-DDS behaviour on every
platform where OpenSSL is actually installed.

Fast-DDS v2.10.7 also obtains a raw `RTPSReader*` for intra-process delivery.
Participant or reader teardown can invalidate that pointer before the delivery
finishes, causing an intermittent use-after-free in
`StatefulWriter::intraprocess_delivery`. The patch backports eProsima's official
reference-counted lifetime fix, adapts the API differences required by v2.10.7,
and closes the check-before-increment window when a protected pointer is acquired
concurrently with reader deactivation. Upstream tests are not included in the
production dependency patch.

The OpenSSL change and the mutex-protected pointer-acquisition hardening are
authored by VLink contributors. The remaining intra-process lifetime code is
derived from the eProsima commit identified above. Both are offered under the
same Apache License, Version 2.0 as the upstream files, and the original
copyright notices are retained.

## Where to obtain the corresponding source

* Upstream tag: <https://github.com/eProsima/Fast-DDS/archive/refs/tags/v2.10.7.zip>
* VLink modifications applied: `packup/patch/fastdds_2.10.x.patch`
