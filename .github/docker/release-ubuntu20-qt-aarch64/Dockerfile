# syntax=docker/dockerfile:1.7

FROM ubuntu:20.04

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

ARG QT_VERSION=6.8.3
ARG QTIFW_VERSION=4.8.1

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
      libtool \
      libva-dev \
      libvdpau-dev \
      libwayland-bin \
      libwayland-dev \
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
      libxkbcommon-dev \
      libxkbcommon-x11-dev \
      libxrender-dev \
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
      wayland-protocols \
      xz-utils \
      yasm \
      zlib1g-dev \
      zstd

RUN python3 -m pip install --no-cache-dir --upgrade \
      conan \
      pip \
      setuptools \
      wheel

WORKDIR /tmp/qt-build

RUN set -eux; \
    qt_base_url="https://download.qt.io/official_releases/qt/6.8/${QT_VERSION}/submodules"; \
    curl -fsSLO "${qt_base_url}/qtbase-everywhere-src-${QT_VERSION}.tar.xz"; \
    curl -fsSLO "${qt_base_url}/qtsvg-everywhere-src-${QT_VERSION}.tar.xz"; \
    curl -fsSLO "${qt_base_url}/qtwayland-everywhere-src-${QT_VERSION}.tar.xz"; \
    echo "540e9665dbfd9a2cf98e6e4b4aa4bb98  qtbase-everywhere-src-${QT_VERSION}.tar.xz" | md5sum -c -; \
    echo "4748ec76af552d21f2bf2acce45b39af  qtsvg-everywhere-src-${QT_VERSION}.tar.xz" | md5sum -c -; \
    echo "5215ab60ea5f8fcafb35c7709b4eb386  qtwayland-everywhere-src-${QT_VERSION}.tar.xz" | md5sum -c -; \
    tar -xf "qtbase-everywhere-src-${QT_VERSION}.tar.xz"; \
    tar -xf "qtsvg-everywhere-src-${QT_VERSION}.tar.xz"; \
    tar -xf "qtwayland-everywhere-src-${QT_VERSION}.tar.xz"; \
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
    cmake --build . --parallel "$(nproc)"; \
    cmake --install .; \
    popd; \
    for module in qtsvg qtwayland; do \
      cmake -S "${module}-everywhere-src-${QT_VERSION}" -B "${module}-build" -GNinja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${QT_DIR}" \
        -DCMAKE_PREFIX_PATH="${QT_DIR}" \
        -DQT_BUILD_EXAMPLES=OFF \
        -DQT_BUILD_TESTS=OFF; \
      cmake --build "${module}-build" --parallel "$(nproc)"; \
      cmake --install "${module}-build"; \
    done; \
    "${QT_DIR}/bin/qmake" -query QT_VERSION; \
    test -f "${QT_DIR}/lib/libQt6Core.so"; \
    test -f "${QT_DIR}/lib/libQt6Widgets.so"; \
    test -f "${QT_DIR}/lib/libQt6OpenGLWidgets.so"; \
    test -f "${QT_DIR}/lib/libQt6Svg.so"; \
    test -f "${QT_DIR}/lib/libQt6WaylandClient.so"; \
    test -f "${QT_DIR}/plugins/platforms/libqxcb.so"; \
    test -f "${QT_DIR}/plugins/platforms/libqwayland.so" || \
      test -f "${QT_DIR}/plugins/platforms/libqwayland-egl.so" || \
      test -f "${QT_DIR}/plugins/platforms/libqwayland-generic.so"; \
    test -f "${QT_DIR}/plugins/imageformats/libqsvg.so"; \
    rm -rf /tmp/qt-build

WORKDIR /tmp

RUN set -eux; \
    ifw_url="https://download.qt.io/official_releases/qt-installer-framework/${QTIFW_VERSION}/QtInstallerFramework-linux-arm64-${QTIFW_VERSION}.run"; \
    curl -fsSL "${ifw_url}" -o qtifw.run; \
    chmod +x qtifw.run; \
    ./qtifw.run --root "${QTIFW_DIR}" --accept-licenses --default-answer --confirm-command install; \
    "${QTIFW_DIR}/bin/binarycreator" --version; \
    rm -f qtifw.run

WORKDIR /work
