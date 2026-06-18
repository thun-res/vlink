# Fast-DDS — VLink modification notice (Apache License 2.0 §4(b))

> *Upstream*: eProsima Fast-DDS, <https://github.com/eProsima/Fast-DDS>
> *Upstream version*: v2.10.7 (fetched by CPM, see `cmake/cpm_thirdparty.cmake`)
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
| `CMakeLists.txt` | Collapse the upstream QNX-specific `OPENSSL_FOUND=1` shortcut and the duplicated `SECURITY` branches into a single `find_package(OpenSSL [REQUIRED])` call. The shortcut hard-codes a non-functional `OPENSSL_FOUND` value on QNX, which prevents VLink's `ENABLE_SECURITY=ON` build from detecting the absence of OpenSSL and emitting a clean diagnostic. |

## Why the modification

VLink statically links Fast-DDS into `libvlink-dds.so`. The upstream
QNX `OPENSSL_FOUND=1` short-circuit is incompatible with VLink's
unified detection logic (which falls back gracefully when OpenSSL is
not present). The modification preserves Fast-DDS behaviour on every
platform where OpenSSL is actually installed.

No third-party code is introduced; the change is authored by VLink
contributors and offered under the same Apache License, Version 2.0
as the upstream file.

## Where to obtain the corresponding source

* Upstream tag: <https://github.com/eProsima/Fast-DDS/archive/refs/tags/v2.10.7.zip>
* VLink modifications applied: `packup/patch/fastdds_2.10.x.patch`
