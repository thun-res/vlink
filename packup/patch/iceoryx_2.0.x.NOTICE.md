# iceoryx — VLink modification notice (Apache License 2.0 §4(b))

> *Upstream*: Eclipse iceoryx, <https://github.com/eclipse-iceoryx/iceoryx>
> *Upstream version*: v2.0.8 (fetched by CPM, see `cmake/cpm_thirdparty.cmake`)
> *Upstream license*: Apache License, Version 2.0
> *Modifier*: VLink contributors (<https://github.com/thun-res/vlink>)
> *Modifier license*: Apache License, Version 2.0 (same as upstream)

This document is the §4(b) "prominent notice" required by the Apache License,
Version 2.0 for the modifications VLink applies to iceoryx v2.0.8 source at
build time, via `packup/patch/iceoryx_2.0.x.patch`. The Apache 2.0 license
text covering both the original work and these modifications is shipped at
`licenses/iceoryx/LICENSE` in this distribution.

## Files added

| Path | Purpose of modification |
|------|-------------------------|
| `CMakeLists.txt` (top-level) | Add a top-level CMakeLists that forwards to `iceoryx_meta` so CPM can build iceoryx as a sub-project without using the upstream `iceoryx_meta` path directly. |
| `iceoryx_hoofs/include/iceoryx_hoofs/internal/concurrent/condition_variable.hpp` | A `CLOCK_MONOTONIC`-backed `pthread_cond_t` wrapper used by VLink to keep wait timeouts unaffected by wall-clock jumps. Required because the upstream `std::condition_variable_any::wait_until` is bound to `CLOCK_REALTIME` on some libstdc++ implementations. |

## Files modified

| Path | Purpose of modification |
|------|-------------------------|
| `.gitignore` | Allow our top-level `CMakeLists.txt` to be tracked. |
| `iceoryx_hoofs/CMakeLists.txt` | Replace bare `acl`/`atomic` link items with `find_library`-resolved paths so iceoryx links cleanly on hosts where `libacl` lives outside the default lib path; make `libatomic` linkage conditional on its presence (musl/macOS). |
| `iceoryx_hoofs/cmake/Config.cmake.in` | Use `${PACKAGE_PREFIX_DIR}` instead of the install-time `@CMAKE_INSTALL_PREFIX@` so the exported config is relocatable when iceoryx is built as a CPM sub-project. |
| `iceoryx_hoofs/cmake/IceoryxPlatform.cmake` | Promote `ICEORYX_CXX_STANDARD` from 14 to 17 on Linux/QNX to match VLink's C++17 baseline. |
| `iceoryx_hoofs/include/iceoryx_hoofs/internal/concurrent/periodic_task.hpp` | Switch the periodic-task wait primitive from `std::condition_variable_any` to the new `condition_variable.hpp` wrapper (CLOCK_MONOTONIC). |
| `iceoryx_hoofs/include/iceoryx_hoofs/internal/concurrent/periodic_task.inl` | Adapt the inline implementation to the new wait primitive. |
| `iceoryx_hoofs/include/iceoryx_hoofs/internal/concurrent/sofi.inl` | Annotate a benign data race observed by ThreadSanitizer when sofi is shared by multiple readers (atomic ordering tighten). |
| `iceoryx_hoofs/platform/linux/include/iceoryx_hoofs/platform/semaphore.hpp` | Use the new wrapper instead of `sem_timedwait` for predictable timeout behaviour under suspend/resume. |
| `iceoryx_hoofs/platform/qnx/include/iceoryx_hoofs/platform/semaphore.hpp` | Mirror the Linux change on QNX. |
| `iceoryx_hoofs/source/posix_wrapper/message_queue.cpp` | Same wait-primitive switch on the message-queue receive path. |
| `iceoryx_hoofs/source/posix_wrapper/mutex.cpp` | When `_POSIX_CLOCK_SELECTION` is supported, force the mutex internal clock to `CLOCK_MONOTONIC` so `timed_lock` cannot be skewed by wall-clock jumps. |
| `iceoryx_hoofs/source/posix_wrapper/semaphore.cpp` | Use the new wait primitive on the timed-wait path. |
| `iceoryx_meta/build_options.cmake` | Disable upstream feature toggles that VLink does not consume (introspection, C bindings, doc/test/CI helpers). |
| `iceoryx_posh/CMakeLists.txt` | Skip the `cmake_minimum_required` policy adjustment that conflicts with our CPM toolchain pinning. |
| `iceoryx_posh/cmake/Config.cmake.in` | Same `${PACKAGE_PREFIX_DIR}` relocation fix as `iceoryx_hoofs`. |
| `iceoryx_posh/cmake/iceoryx_versions.hpp.in` | Strip dev-only `git describe` invocations that fail when iceoryx is built outside its own git tree. |

## Why the modifications

VLink statically links iceoryx into `libvlink-shm.so` and ships the
`iox-roudi` broker binary as `bin/iox-roudi`. The above changes:

* keep the build self-contained when iceoryx is consumed via CPM
  (`packup/patch/iceoryx_2.0.x.patch` is applied at configure time);
* tighten the underlying wait primitives so VLink's real-time deadlines
  are not exposed to wall-clock adjustments;
* turn off upstream features that are not consumed by VLink.

No change introduces additional third-party code; every modification is
authored by VLink contributors and is offered to downstream consumers
under the same Apache License, Version 2.0 as the upstream files.

## Where to obtain the corresponding source

* Upstream tag for the modified base: <https://github.com/eclipse-iceoryx/iceoryx/archive/refs/tags/v2.0.8.zip>
* VLink modifications applied: `packup/patch/iceoryx_2.0.x.patch`

Both are required to reproduce the exact source that compiles into the
shipped `libvlink-shm.so` / `iox-roudi`.
