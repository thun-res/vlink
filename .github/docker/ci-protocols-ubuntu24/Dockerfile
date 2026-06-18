# syntax=docker/dockerfile:1.7
FROM ubuntu:24.04

ARG ZENOH_PICO_VERSION=1.9.0
ARG VSOMEIP_VERSION=3.7.3
ARG PAHO_MQTT_C_VERSION=v1.3.16
ARG ICEORYX2_VERSION=v0.9.1

ENV DEBIAN_FRONTEND=noninteractive \
    CARGO_HOME=/opt/cargo \
    PATH=/opt/cargo/bin:/usr/local/bin:$PATH

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential \
      ca-certificates \
      ccache \
      cmake \
      curl \
      git \
      libacl1-dev \
      libboost-all-dev \
      libssl-dev \
      libsystemd-dev \
      ninja-build \
      patch \
      pkg-config \
      python3 \
      tar \
      uuid-dev \
      zstd \
    && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain stable \
    && rustc --version \
    && cargo --version

RUN set -eux; \
    mkdir -p /tmp/protocol-src; \
    git clone --depth 1 --branch "${PAHO_MQTT_C_VERSION}" https://github.com/eclipse-paho/paho.mqtt.c.git /tmp/protocol-src/paho.mqtt.c; \
    cmake -S /tmp/protocol-src/paho.mqtt.c -B /tmp/protocol-src/paho.mqtt.c/build -GNinja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr/local \
      -DPAHO_WITH_SSL=ON \
      -DPAHO_BUILD_SHARED=ON \
      -DPAHO_BUILD_STATIC=OFF \
      -DPAHO_BUILD_SAMPLES=OFF \
      -DPAHO_BUILD_DOCUMENTATION=OFF \
      -DPAHO_ENABLE_TESTING=OFF \
      -DPAHO_ENABLE_CPACK=OFF \
      -DPAHO_HIGH_PERFORMANCE=ON; \
    cmake --build /tmp/protocol-src/paho.mqtt.c/build --parallel 2; \
    cmake --install /tmp/protocol-src/paho.mqtt.c/build; \
    rm -rf /tmp/protocol-src/paho.mqtt.c

RUN set -eux; \
    mkdir -p /tmp/protocol-src; \
    git clone --depth 1 --branch "${VSOMEIP_VERSION}" https://github.com/COVESA/vsomeip.git /tmp/protocol-src/vsomeip; \
    sed -i \
      -e 's/ -Wall//g' \
      -e 's/ -Wextra//g' \
      -e 's/ -Wformat-security//g' \
      -e 's/ -Wformat//g' \
      -e 's/ -Wconversion//g' \
      -e 's/ -Wpedantic//g' \
      -e 's/ -Werror//g' \
      -e 's/ -Wno-inconsistent-missing-override//g' \
      /tmp/protocol-src/vsomeip/CMakeLists.txt; \
    cmake -S /tmp/protocol-src/vsomeip -B /tmp/protocol-src/vsomeip/build -GNinja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr/local \
      -DENABLE_SIGNAL_HANDLING=ON \
      -DDISABLE_SECURITY=ON; \
    cmake --build /tmp/protocol-src/vsomeip/build --parallel 2; \
    cmake --install /tmp/protocol-src/vsomeip/build; \
    rm -rf /tmp/protocol-src/vsomeip

RUN set -eux; \
    mkdir -p /tmp/protocol-src; \
    git clone --depth 1 --branch "${ZENOH_PICO_VERSION}" https://github.com/eclipse-zenoh/zenoh-pico.git /tmp/protocol-src/zenoh-pico; \
    cmake -S /tmp/protocol-src/zenoh-pico -B /tmp/protocol-src/zenoh-pico/build -GNinja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr/local \
      -DBUILD_SHARED_LIBS=ON; \
    cmake --build /tmp/protocol-src/zenoh-pico/build --parallel 2; \
    cmake --install /tmp/protocol-src/zenoh-pico/build; \
    rm -rf /tmp/protocol-src/zenoh-pico

RUN set -eux; \
    mkdir -p /tmp/protocol-src; \
    git clone --depth 1 --branch "${ICEORYX2_VERSION}" https://github.com/eclipse-iceoryx/iceoryx2.git /tmp/protocol-src/iceoryx2; \
    cmake -S /tmp/protocol-src/iceoryx2 -B /tmp/protocol-src/iceoryx2/build -GNinja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr/local \
      -DBUILD_CXX=OFF \
      -DBUILD_EXAMPLES=OFF \
      -DBUILD_TESTING=OFF; \
    cmake --build /tmp/protocol-src/iceoryx2/build --parallel 2; \
    cmake --install /tmp/protocol-src/iceoryx2/build; \
    rm -rf /tmp/protocol-src/iceoryx2 /opt/cargo/registry /opt/cargo/git

RUN ldconfig && mkdir -p /tmp/protocol-check

RUN cat > /tmp/protocol-check/CMakeLists.txt <<'EOF'
cmake_minimum_required(VERSION 3.22)
project(vlink-protocol-deps-check LANGUAGES C CXX)
find_package(eclipse-paho-mqtt-c CONFIG REQUIRED)
find_package(vsomeip3 CONFIG REQUIRED)
find_package(zenohpico CONFIG REQUIRED)
find_package(iceoryx2-c CONFIG REQUIRED)
if(NOT TARGET eclipse-paho-mqtt-c::paho-mqtt3c AND NOT TARGET eclipse-paho-mqtt-c::paho-mqtt3cs)
  message(FATAL_ERROR "Missing Eclipse Paho MQTT C target")
endif()
if(NOT TARGET vsomeip3)
  message(FATAL_ERROR "Missing vsomeip3 target")
endif()
if(NOT TARGET zenohpico::zenohpico_shared AND NOT TARGET zenohpico::zenohpico_static)
  message(FATAL_ERROR "Missing zenoh-pico target")
endif()
if(NOT TARGET iceoryx2-c::shared-lib AND NOT TARGET iceoryx2-c::static-lib)
  message(FATAL_ERROR "Missing iceoryx2-c target")
endif()
EOF

RUN cmake -S /tmp/protocol-check -B /tmp/protocol-check/build -GNinja -DCMAKE_PREFIX_PATH=/usr/local \
    && rm -rf /tmp/protocol-check
