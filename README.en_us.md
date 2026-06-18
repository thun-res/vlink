# VLink

![](doc/images/vlink.svg)

**One minimalist API, 12 transports, zero-cost switching**

**[Official website: https://vlink.work](https://vlink.work)**

**[Github: https://github.com/thun-res/vlink](https://github.com/thun-res/vlink)**

![](https://img.shields.io/badge/version-v2.0.0-blue.svg) ![](https://img.shields.io/badge/language-C++17-yellow.svg) ![](https://img.shields.io/badge/license-Apache%202.0-green.svg) ![](https://img.shields.io/badge/platform-linux%20|%20qnx%20|%20macos%20|%20windows%20|%20android-brightgreen.svg)

English | [中文](README.md)

VLink is a lightweight C++ communication middleware for **autonomous driving and embodied intelligence**, positioned as a minimalist alternative to ROS 2.

A minimalist API (3–5 lines to get communication going), zero-cost transport switching, and compile-time serialization deduction. Ships with **12 transports**, **14 serialization formats**, **3 communication models**, **9 CLI tools**, and optional Foxglove / Rerun visualization bridges.

---

## 📚 Documentation

### Part I — Getting Started

| Documentation                                    | Content                                |
| ------------------------------------------------ | -------------------------------------- |
| [Whitepaper](doc/00-whitepaper.md)               | Background, positioning, architecture & technical deep-dive |
| [Build Guide](doc/01-build.md)                   | CMake / Conan / Integration / Cross-platform |
| [Examples](doc/22-examples.md)                   | Runnable samples (fastest on-ramp)     |

### Part II — Core Communication Models

| Documentation                                    | Content                                |
| ------------------------------------------------ | -------------------------------------- |
| [Event Model](doc/03-event-model.md)             | Publisher / Subscriber                 |
| [Method Model](doc/04-method-model.md)           | Client / Server                        |
| [Field Model](doc/05-field-model.md)             | Setter / Getter                        |
| [Serialization](doc/06-serialization.md)         | 14 serialization types & auto-deduction |

### Part III — Transport & Common Features

| Documentation                                    | Content                                |
| ------------------------------------------------ | -------------------------------------- |
| [Transport Backends](doc/07-transport.md)        | 12 backends & URL format               |
| [QoS](doc/08-qos.md)                             | Quality of Service configuration       |
| [Zero-Copy](doc/10-zerocopy.md)                  | CameraFrame / PointCloud / OccupancyGrid / Tensor / ObjectArray / AudioFrame / RawData (large-payload essentials) |
| [Base Library](doc/11-base-library.md)           | Logger / MessageLoop / Timer / ThreadPool / Concurrency / IPC / Profiler |
| [Bag Recording](doc/12-bag-recording.md)         | Bag / MCAP file format & API           |
| [CLI Tools](doc/13-cli-tools.md)                 | 9 command-line tools reference         |
| [Environment Variables](doc/21-environment-vars.md) | `VLINK_*` configuration reference   |

### Part IV — Tools & Visualization

| Documentation                                    | Content                                |
| ------------------------------------------------ | -------------------------------------- |
| [Viewer](doc/14-viewer.md)                       | Qt desktop: Viewer / Player / Analyzer |
| [WebViz](doc/15-webviz.md)                       | vlink-foxglove / vlink-rerun bridges   |
| [Proxy](doc/16-proxy.md)                         | ProxyServer / ProxyAPI                 |
| [Discovery](doc/17-discovery.md)                 | UDP multicast discovery & topology     |

### Part V — Advanced Topics

| Documentation                                    | Content                                |
| ------------------------------------------------ | -------------------------------------- |
| [Node Base](doc/02-node-lifecycle.md)            | Node base template shared interface & lifecycle (init/deinit/interrupt) |
| [Security](doc/09-security.md)                   | AES-128-GCM AEAD, RSA hybrid handshake & custom encryption callbacks |

### Part VI — Extension & Contribution

| Documentation                                    | Content                                |
| ------------------------------------------------ | -------------------------------------- |
| [C API](doc/18-c-api.md)                         | C wrapper & multi-language FFI (six primitives, data plane) |
| [Extensions](doc/19-extensions.md)               | Plugin system & custom transports      |
| [Testing](doc/20-testing.md)                     | doctest framework & gcov/lcov          |
| [PR Conventions](doc/92-pr-conventions.md)       | Branches, commits, code style, CI gates |

### Quick Reference

| Documentation                                    | Content                                |
| ------------------------------------------------ | -------------------------------------- |
| [Cheatsheet](doc/90-cheatsheet.md)               | Single-page API / URL / QoS / CLI / env-var reference |
| [Troubleshooting](doc/91-troubleshooting.md)     | Symptom-indexed problem solving        |
| [CHANGELOG](CHANGELOG.md)                        | Release notes                          |

---

## 🚀 30-Second Tour

![Quick Start](doc/images/quickstart-workflow.gif)

```cpp
vlink::Publisher<Imu> pub("dds://sensor/imu");
vlink::Publisher<Imu> pub("shm://sensor/imu");
vlink::Publisher<Imu> pub("intra://sensor/imu");

vlink::Publisher<Imu> pub("dds://sensor/lidar?qos=sensor");
vlink::Publisher<Imu> pub("shm://sensor/image?depth=10");
```

URL syntax: [Transport Backends](doc/07-transport.md).

---

## 🏛️ Architecture

![VLink Architecture](doc/images/readme-architecture.png)

---

## 🚌 Transport Backends

| Scheme | Underlying | Scope | Zero-copy | Status |
| --- | --- | --- | :---: | :---: |
| `intra://` | Lock-free queue | In-process | ✅ | ✅ Stable |
| `shm://` | Iceoryx | Same-host IPC | ✅ | ✅ Stable |
| `dds://` | Fast-DDS | Cross-machine | — | ✅ Stable |
| `ddsc://` | CycloneDDS | Cross-machine | — | ✅ Stable |
| `shm2://` | Iceoryx2 | Same-host | ✅ | 🟡 Beta |
| `ddsr://` | RTI Connext | Cross-machine | — | 🟡 Beta |
| `ddst://` | TravoDDS (domestic DDS) | Cross-machine | — | 🟡 Beta |
| `zenoh://` | Zenoh | Cross-machine / cloud-edge | — | 🟡 Beta |
| `someip://` | vsomeip | Automotive Ethernet | — | 🟡 Beta |
| `mqtt://` | Paho MQTT | Cloud | — | 🟡 Beta |
| `fdbus://` | FDBus | Same-host | — | 🟡 Beta |
| `qnx://` | QNX IPC | Same-host (QNX) | — | 🟡 Beta |

---

## 📡 Communication Models

![Three Communication Models](doc/images/readme-communication-models.png)

**Event — Publish/Subscribe**

```cpp
vlink::Publisher<Imu> publisher("dds://sensor/imu");
publisher.publish(msg);

vlink::Subscriber<Imu> subscriber("dds://sensor/imu");
subscriber.listen([](const Imu& msg) { process(msg); });
```

**Method — Request/Response**

```cpp
vlink::Server<Req, Resp> server("dds://calc/add");
server.listen([](const Req& req, Resp& resp) {
  resp.set_sum(req.left() + req.right());
});

vlink::Client<Req, Resp> client("dds://calc/add");
if (auto r = client.invoke(req, 3s)) { use(*r); }
client.invoke(req, [](const Resp& r) { use(r); });
auto future = client.async_invoke(req);
vlink::Client<Req> fire("dds://event/notify");
fire.send(req);
```

**Field — State Synchronization**

```cpp
vlink::Setter<Status> setter("shm://vehicle/status");
setter.set(status);

vlink::Getter<Status> getter("shm://vehicle/status");
getter.listen([](auto& s) { use(s); });
getter.set_change_reporting(true);
```

---

## 🔧 Getting Started (Build)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
sudo cmake --install build
```

CMake integration:

```cmake
# Specific modules
find_package(vlink REQUIRED COMPONENTS shm dds)
target_link_libraries(my_app PRIVATE vlink::vlink vlink::shm vlink::dds)

# All modules
find_package(vlink REQUIRED COMPONENTS all)
target_link_libraries(my_app PRIVATE vlink::all)

# Generated proto targets
vlink_generate_cpp(TARGET example_gen PROTO example1.proto example2.proto)
target_link_libraries(my_app PRIVATE example_gen)
```

See the [Build Guide](doc/01-build.md) — section 1.5 covers the full CMake target list and `vlink_generate_cpp()` reference.

---

## 💻 Platform Support

| Platform | Architecture | Compiler | Status |
| --- | --- | --- | :---: |
| Linux | x86_64 / aarch64 | GCC 9+ / Clang 10+ | ✅ Stable |
| Windows 10+ | x86_64 | MSVC 2019+ / MinGW | ✅ Stable |
| macOS 10.15+ | x86_64 / arm64 | AppleClang 12+ | 🟡 Beta |
| Android | aarch64 / x86_64 | NDK Clang r25+ | ✅ Stable |
| QNX 7.x/8.x | aarch64 / x86_64 | QCC (QNX SDP) | ✅ Stable |

---

## 📁 Project Structure

```
vlink/
├── include/vlink/      Public headers (6 primitives + base + extensions + zerocopy)
├── src/                Core library implementation
├── modules/            12 transport backend implementations
├── cli/                9 command-line tools
├── proxy/              vlink-proxy executable + ProxyServer / ProxyAPI libraries
├── viewer/             Qt desktop visualization tool (default OFF, requires ENABLE_VIEWER=ON)
├── webviz/             vlink-foxglove / vlink-rerun bridge executables (default OFF)
├── c_api/              C API (data plane only, for Python/Rust/etc. FFI)
├── python_api/         nanobind Python bindings (default OFF, requires ENABLE_PYTHON_API=ON)
├── examples/           Usage examples (default OFF, 14 categories)
├── test/               doctest main test suite (vlink-test)
├── doc/                Documentation
├── tools/              Cross-compilation toolchains, Docker, formatting & check scripts
├── cmake/              CMake toolchains and Find modules
├── thirdparty/         Third-party dependencies and patches
├── packup/             Release packaging
├── CMakeLists.txt      Top-level CMake entry
├── conanfile.py        Conan recipe
└── Android.bp          Android.bp (Soong build rules)
```

---

## 🤝 Contributing

vlink is currently maintained by Thun Lu as a solo effort. This is a long-term commitment, but response times and roadmap velocity are bounded by one person's bandwidth. PRs, issues, and documentation improvements are all welcome.

Please read the [PR Conventions](doc/92-pr-conventions.md) before opening a PR.

---

## 📜 License

[Apache License 2.0](LICENSE) — free for commercial use.

Copyright (C) 2026 Thun Lu. All rights reserved.
