FROM ubuntu:20.04

ARG TARGETARCH

ENV DEBIAN_FRONTEND=noninteractive \
    PIP_DISABLE_PIP_VERSION_CHECK=1

RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates \
      autoconf \
      automake \
      build-essential \
      ccache \
      cmake \
      curl \
      dpkg-dev \
      file \
      git \
      git-lfs \
      libacl1-dev \
      libdbus-1-dev \
      libfontconfig1-dev \
      libfreetype6-dev \
      libgl1-mesa-dev \
      libsqlite3-dev \
      libssl-dev \
      libtiff5 \
      libtool \
      libva-dev \
      libvdpau-dev \
      libx11-dev \
      libx11-xcb-dev \
      libxcb-cursor-dev \
      libxcb-icccm4-dev \
      libxcb-image0-dev \
      libxcb-keysyms1-dev \
      libxcb-randr0-dev \
      libxcb-render-util0-dev \
      libxcb-render0-dev \
      libxcb-shape0-dev \
      libxcb-shm0-dev \
      libxcb-sync-dev \
      libxcb-xfixes0-dev \
      libxcb-xinerama0-dev \
      libxcb-xkb-dev \
      libxcb1-dev \
      libxext-dev \
      libxinerama-dev \
      libxkbcommon-dev \
      libxkbcommon-x11-0 \
      libxkbcommon-x11-dev \
      libxrandr-dev \
      libxrender-dev \
      libzstd-dev \
      nasm \
      ninja-build \
      patch \
      patchelf \
      pkg-config \
      python3 \
      python3-dev \
      python3-venv \
      tar \
      xz-utils \
      yasm \
      zstd \
    && build_arch="${TARGETARCH:-}" \
    && if [ -z "${build_arch}" ]; then \
         case "$(uname -m)" in \
           x86_64) build_arch=amd64 ;; \
           aarch64|arm64) build_arch=arm64 ;; \
           *) echo "Unsupported machine architecture: $(uname -m)" >&2; exit 1 ;; \
         esac; \
       fi \
    && case "${build_arch}" in \
         amd64) expected_machine=x86_64 ;; \
         arm64) expected_machine=aarch64 ;; \
         *) echo "Unsupported TARGETARCH: ${build_arch}" >&2; exit 1 ;; \
       esac \
    && test "$(uname -m)" = "${expected_machine}" \
    && python3 -m venv /opt/vlink-python \
    && /opt/vlink-python/bin/python -m pip install --no-cache-dir --upgrade pip setuptools wheel conan \
    && ln -sf /opt/vlink-python/bin/conan /usr/local/bin/conan \
    && /opt/vlink-python/bin/python -V \
    && conan --version \
    && rm -rf /var/lib/apt/lists/*
