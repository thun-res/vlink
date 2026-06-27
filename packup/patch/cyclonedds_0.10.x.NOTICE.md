# Cyclone DDS — VLink modification notice

> *Upstream*: Eclipse Cyclone DDS, <https://github.com/eclipse-cyclonedds/cyclonedds>
> *Upstream version*: v0.10.5 (fetched by CPM, see `cmake/cpm_thirdparty.cmake`)
> *Upstream license*: Eclipse Public License 2.0 (EPL-2.0) OR Eclipse Distribution License 1.0 (EDL-1.0)
> *Modifier*: VLink contributors (<https://github.com/thun-res/vlink>)
> *Modifier license*: Apache License, Version 2.0 (the modifications themselves);
> the resulting derivative is distributed under the original EPL-2.0/EDL-1.0
> terms as required by EPL §3.

This document records the VLink-side modifications applied at build time to
Cyclone DDS v0.10.5, via `packup/patch/cyclonedds_0.10.x.patch`. The full
EPL-2.0 license text is shipped at `licenses/cyclonedds/LICENSE` in this
distribution. Source code corresponding to the exact statically-linked binary
shipped inside `libvlink-ddsc.so` consists of the upstream tag below plus
this patch.

## Files modified

| Path | Purpose of modification |
|------|-------------------------|
| `src/CMakeLists.txt` | Add a `CYCLONEDDS_DISABLE_SSL` escape hatch so VLink can build Cyclone DDS without OpenSSL in fully-static configurations; force `ENABLE_SSL=OFF` when the gate is set. |
| `src/core/CMakeLists.txt` | Wrap the `OpenSSL::SSL` link target in `$<BUILD_INTERFACE:...>` so the dependency does not leak into the exported Cyclone DDS CMake config consumed by downstream targets in the same build tree. |

## Why the modifications

VLink statically links Cyclone DDS into `libvlink-ddsc.so` with
`-DCYCLONEDDS_DISABLE_SSL=ON` and `-DENABLE_SSL=OFF`. The upstream gate
unconditionally probes for OpenSSL and emits a transitive link
dependency, which conflicts with VLink's static-link strategy.

## Where to obtain the corresponding source

* Upstream tag: <https://github.com/eclipse-cyclonedds/cyclonedds/archive/refs/tags/0.10.5.zip>
* VLink modifications applied: `packup/patch/cyclonedds_0.10.x.patch`
