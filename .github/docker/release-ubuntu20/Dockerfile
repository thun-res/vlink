FROM ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive \
    PIP_DISABLE_PIP_VERSION_CHECK=1 \
    VLINK_AQT_PYTHON=/usr/bin/python3.10

RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates \
      gnupg \
      software-properties-common \
    && add-apt-repository -y ppa:deadsnakes/ppa \
    && apt-get update && apt-get install -y --no-install-recommends \
      autoconf \
      automake \
      build-essential \
      ccache \
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
      python3.10 \
      python3.10-dev \
      python3.10-venv \
      tar \
      xz-utils \
      yasm \
      zstd \
    && rm -rf /var/lib/apt/lists/*
