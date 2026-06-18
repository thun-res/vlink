FROM ubuntu:20.04

ARG PYTHON_STANDALONE_RELEASE=20260610
ARG PYTHON_STANDALONE_VERSION=3.10.20
ARG TARGETARCH

ENV DEBIAN_FRONTEND=noninteractive \
    PIP_DISABLE_PIP_VERSION_CHECK=1 \
    VLINK_AQT_PYTHON=/opt/python/cp310/bin/python3.10

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
      libgl1-mesa-dev \
      libsqlite3-dev \
      libssl-dev \
      libtool \
      libva-dev \
      libvdpau-dev \
      libxkbcommon-x11-0 \
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
    && case "${TARGETARCH:-amd64}" in \
         amd64) python_arch=x86_64 ;; \
         arm64) python_arch=aarch64 ;; \
         *) echo "Unsupported TARGETARCH: ${TARGETARCH}" >&2; exit 1 ;; \
       esac \
    && python_url="https://github.com/astral-sh/python-build-standalone/releases/download/${PYTHON_STANDALONE_RELEASE}/cpython-${PYTHON_STANDALONE_VERSION}%2B${PYTHON_STANDALONE_RELEASE}-${python_arch}-unknown-linux-gnu-install_only_stripped.tar.gz" \
    && mkdir -p /opt/python/cp310 \
    && curl -fsSL "${python_url}" -o /tmp/python-standalone.tar.gz \
    && tar -xzf /tmp/python-standalone.tar.gz -C /opt/python/cp310 --strip-components=1 \
    && "${VLINK_AQT_PYTHON}" -V \
    && "${VLINK_AQT_PYTHON}" -m venv /tmp/vlink-python-check \
    && /tmp/vlink-python-check/bin/python -m pip --version \
    && rm -rf /tmp/vlink-python-check /tmp/python-standalone.tar.gz \
    && rm -rf /var/lib/apt/lists/*
