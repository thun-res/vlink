FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    PIP_DISABLE_PIP_VERSION_CHECK=1

RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
      build-essential \
      ca-certificates \
      ccache \
      cmake \
      file \
      git \
      lcov \
      libacl1-dev \
      libssl-dev \
      ninja-build \
      patch \
      pkg-config \
      python3 \
      python3-dev \
      python3-venv \
      tar \
      zstd \
    && rm -rf /var/lib/apt/lists/*

RUN python3 -m venv /opt/vlink/python-tools \
    && /opt/vlink/python-tools/bin/python -m pip install -q --upgrade pip \
    && /opt/vlink/python-tools/bin/python -m pip install -q \
      clang-format==21.1.8 \
      clang-tidy==21.1.6 \
      cmake-format==0.6.13 \
      gcovr==8.4 \
      nanobind==2.13.0 \
      PyYAML==6.0.2 \
      cpplint==2.0.2 \
    && ln -s /opt/vlink/python-tools/bin/clang-format /usr/local/bin/clang-format \
    && ln -s /opt/vlink/python-tools/bin/clang-tidy /usr/local/bin/clang-tidy \
    && ln -s /opt/vlink/python-tools/bin/cmake-format /usr/local/bin/cmake-format \
    && ln -s /opt/vlink/python-tools/bin/gcovr /usr/local/bin/gcovr \
    && ln -s /opt/vlink/python-tools/bin/cpplint /usr/local/bin/cpplint
