# syntax=docker/dockerfile:1.7

FROM ubuntu:20.04

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

ARG QT_VERSION=6.8.3
ARG QTIFW_VERSION=4.7.0
ARG QT_BUILD_PARALLEL=2

ENV DEBIAN_FRONTEND=noninteractive \
    PIP_DISABLE_PIP_VERSION_CHECK=1 \
    QT_DIR=/opt/Qt/${QT_VERSION}/gcc_arm64 \
    QTIFW_DIR=/opt/Qt/Tools/QtInstallerFramework/${QTIFW_VERSION}

ENV PATH=${QT_DIR}/bin:${QTIFW_DIR}/bin:${PATH}

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    apt-get update && apt-get install -y --no-install-recommends \
      autoconf \
      automake \
      bison \
      build-essential \
      ca-certificates \
      ccache \
      cmake \
      curl \
      dpkg-dev \
      file \
      flex \
      gperf \
      git \
      git-lfs \
      libacl1-dev \
      libdbus-1-dev \
      libegl1-mesa-dev \
      libfontconfig1-dev \
      libfreetype6-dev \
      libgl1-mesa-dev \
      libgles2-mesa-dev \
      libjpeg-dev \
      libopengl-dev \
      libpng-dev \
      libsqlite3-dev \
      libssl-dev \
      libtiff5 \
      libtool \
      libva-dev \
      libvdpau-dev \
      libx11-dev \
      libx11-xcb-dev \
      libxcb-cursor-dev \
      libxcb-dri2-0-dev \
      libxcb-dri3-dev \
      libxcb-glx0-dev \
      libxcb-icccm4-dev \
      libxcb-image0-dev \
      libxcb-keysyms1-dev \
      libxcb-present-dev \
      libxcb-randr0-dev \
      libxcb-render-util0-dev \
      libxcb-render0-dev \
      libxcb-shape0-dev \
      libxcb-shm0-dev \
      libxcb-sync-dev \
      libxcb-util-dev \
      libxcb-xfixes0-dev \
      libxcb-xinerama0-dev \
      libxcb-xkb-dev \
      libxcb1-dev \
      libxext-dev \
      libxinerama-dev \
      libxkbcommon-dev \
      libxkbcommon-x11-dev \
      libxrandr-dev \
      libxrender-dev \
      libzstd-dev \
      nasm \
      ninja-build \
      patch \
      perl \
      patchelf \
      pkg-config \
      python3 \
      python3-dev \
      python3-pip \
      python3-venv \
      tar \
      xz-utils \
      yasm \
      zlib1g-dev \
      zstd

RUN python3 -m pip install --no-cache-dir --upgrade \
      cmake==3.31.6 \
      conan \
      pip \
      setuptools \
      wheel \
    && cmake --version \
    && conan --version

WORKDIR /tmp/qt-build

RUN set -eux; \
    download() { \
      local url="$1"; \
      local output="$2"; \
      local attempt; \
      for attempt in 1 2 3 4 5; do \
        if curl -fL --connect-timeout 30 --speed-limit 1024 --speed-time 60 "${url}" -o "${output}"; then \
          return 0; \
        fi; \
        rm -f "${output}"; \
        sleep "$((attempt * 5))"; \
      done; \
      return 1; \
    }; \
    qt_base_url="https://download.qt.io/official_releases/qt/6.8/${QT_VERSION}/submodules"; \
    download "${qt_base_url}/qtbase-everywhere-src-${QT_VERSION}.tar.xz" "qtbase-everywhere-src-${QT_VERSION}.tar.xz"; \
    download "${qt_base_url}/qtsvg-everywhere-src-${QT_VERSION}.tar.xz" "qtsvg-everywhere-src-${QT_VERSION}.tar.xz"; \
    echo "540e9665dbfd9a2cf98e6e4b4aa4bb98  qtbase-everywhere-src-${QT_VERSION}.tar.xz" | md5sum -c -; \
    echo "4748ec76af552d21f2bf2acce45b39af  qtsvg-everywhere-src-${QT_VERSION}.tar.xz" | md5sum -c -; \
    tar -xf "qtbase-everywhere-src-${QT_VERSION}.tar.xz"; \
    tar -xf "qtsvg-everywhere-src-${QT_VERSION}.tar.xz"

RUN set -eux; \
    cmake -E make_directory qtbase-build; \
    pushd qtbase-build; \
    "../qtbase-everywhere-src-${QT_VERSION}/configure" \
      -prefix "${QT_DIR}" \
      -opensource \
      -confirm-license \
      -release \
      -nomake examples \
      -nomake tests \
      -no-feature-cups \
      -no-feature-gtk3 \
      -opengl desktop; \
    cmake --build . --parallel "${QT_BUILD_PARALLEL}"; \
    cmake --install .; \
    popd; \
    "${QT_DIR}/bin/qmake" -query QT_VERSION

RUN set -eux; \
    cmake -S "qtsvg-everywhere-src-${QT_VERSION}" -B qtsvg-build -GNinja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="${QT_DIR}" \
      -DCMAKE_PREFIX_PATH="${QT_DIR}" \
      -DQT_BUILD_EXAMPLES=OFF \
      -DQT_BUILD_TESTS=OFF; \
    cmake --build qtsvg-build --parallel "${QT_BUILD_PARALLEL}"; \
    cmake --install qtsvg-build

RUN set -eux; \
    "${QT_DIR}/bin/qmake" -query QT_VERSION; \
    test -f "${QT_DIR}/lib/libQt6Core.so"; \
    test -f "${QT_DIR}/lib/libQt6Widgets.so"; \
    test -f "${QT_DIR}/lib/libQt6OpenGLWidgets.so"; \
    test -f "${QT_DIR}/lib/libQt6Svg.so"; \
    test -f "${QT_DIR}/plugins/platforms/libqxcb.so"; \
    test -f "${QT_DIR}/plugins/imageformats/libqsvg.so"; \
    rm -rf /tmp/qt-build

WORKDIR /tmp

RUN set -eux; \
    download() { \
      local url="$1"; \
      local output="$2"; \
      local attempt; \
      for attempt in 1 2 3 4 5; do \
        if curl -fL --connect-timeout 30 --speed-limit 1024 --speed-time 60 "${url}" -o "${output}"; then \
          return 0; \
        fi; \
        rm -f "${output}"; \
        sleep "$((attempt * 5))"; \
      done; \
      return 1; \
    }; \
    ifw_url="https://download.qt.io/official_releases/qt-installer-framework/${QTIFW_VERSION}/QtInstallerFramework-linux-arm64-${QTIFW_VERSION}.run"; \
    download "${ifw_url}" qtifw.run; \
    chmod +x qtifw.run; \
    ./qtifw.run --root "${QTIFW_DIR}" --accept-licenses --default-answer --confirm-command install; \
    test -x "${QTIFW_DIR}/bin/binarycreator"; \
    echo "QtIFW binarycreator: ${QTIFW_DIR}/bin/binarycreator"; \
    rm -f qtifw.run

WORKDIR /work
