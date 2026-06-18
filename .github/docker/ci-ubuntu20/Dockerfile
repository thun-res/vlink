FROM ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential \
      ca-certificates \
      ccache \
      cmake \
      file \
      git \
      libacl1-dev \
      libssl-dev \
      ninja-build \
      patch \
      pkg-config \
      python3 \
      tar \
      zstd \
    && rm -rf /var/lib/apt/lists/*
