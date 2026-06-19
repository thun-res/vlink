FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
      build-essential \
      ca-certificates \
      ccache \
      cmake \
      dpkg-dev \
      file \
      git \
      libacl1-dev \
      libsqlite3-dev \
      libssl-dev \
      libzstd-dev \
      patch \
      pkg-config \
	      python3 \
	      tar \
	    zstd \
	    && rm -rf /var/lib/apt/lists/*
