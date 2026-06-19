FROM ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
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
