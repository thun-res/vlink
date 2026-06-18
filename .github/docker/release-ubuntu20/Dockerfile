FROM ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive \
    PIP_DISABLE_PIP_VERSION_CHECK=1

RUN apt-get update && apt-get install -y --no-install-recommends \
      autoconf \
      automake \
      build-essential \
      ca-certificates \
      cmake \
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
      python3-pip \
      python3-venv \
      tar \
      xz-utils \
      yasm \
      zstd \
    && rm -rf /var/lib/apt/lists/*
