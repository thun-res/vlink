# syntax=docker/dockerfile:1.7

# Single source for both CI/release images. docker-images.yml builds it four
# times via build-args:
#   * UBUNTU_REF          - digest-pinned base image (ubuntu@sha256:...)
#   * CI_EXTRAS           - "1" adds lint pip tools and actionlint
#   * PIP/SETUPTOOLS/WHEEL - per-image versions (ubuntu 20.04 ships Python 3.8,
#                            which caps pip/setuptools/wheel below their latest)
# Defaults below target ubuntu:22.04 (Python 3.10) for ad-hoc local builds.
ARG UBUNTU_REF=ubuntu:22.04
FROM ${UBUNTU_REF}

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

ARG TARGETARCH
ARG CI_EXTRAS=0
ARG PIP_VERSION=26.1.2
ARG SETUPTOOLS_VERSION=82.0.1
ARG WHEEL_VERSION=0.47.0

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    TZ=Asia/Shanghai \
    OSG_DIR=/opt/vlink/osg_sdk/current

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    apt-get update -qq && apt-get install -y -qq --no-install-recommends \
      autoconf \
      automake \
      bison \
      build-essential \
      ca-certificates \
      ccache \
      curl \
      dpkg-dev \
      doxygen \
      file \
      flex \
      gawk \
      graphviz \
      gperf \
      git \
      git-lfs \
      libacl1-dev \
      libarchive-tools \
      libavcodec-dev \
      libavformat-dev \
      libavutil-dev \
      libdbus-1-dev \
      libegl1-mesa-dev \
      libexpat1-dev \
      libffi-dev \
      libfontconfig1-dev \
      libfreetype6-dev \
      libgl1-mesa-dev \
      libgles2-mesa-dev \
      libjpeg-dev \
      libopengl-dev \
      libpng-dev \
      libsqlite3-dev \
      libssl-dev \
      libswscale-dev \
      libsystemd-dev \
      libtiff5 \
      libtool \
      libva-dev \
      libvdpau-dev \
      libwebpdemux2 \
      libwebpmux3 \
      libxau-dev \
      libx11-dev \
      libx11-xcb-dev \
      libxcomposite-dev \
      libxcb-cursor-dev \
      libxcb-composite0-dev \
      libxcb-dri2-0-dev \
      libxcb-dri3-dev \
      libxcb-ewmh-dev \
      libxcb-glx0-dev \
      libxcb-icccm4-dev \
      libxcb-image0-dev \
      libxcb-keysyms1-dev \
      libxcb-present-dev \
      libxcb-randr0-dev \
      libxcb-render-util0-dev \
      libxcb-render0-dev \
      libxcb-res0-dev \
      libxcb-shape0-dev \
      libxcb-shm0-dev \
      libxcb-sync-dev \
      libxcb-util-dev \
      libxcb-xfixes0-dev \
      libxcb-xinerama0-dev \
      libxcb-xkb-dev \
      libxcb1-dev \
      libxcursor-dev \
      libxdamage-dev \
      libxext-dev \
      libxfixes-dev \
      libxi-dev \
      libxinerama-dev \
      libxkbfile-dev \
      libxkbcommon-dev \
      libxkbcommon-x11-0 \
      libxkbcommon-x11-dev \
      libxrandr-dev \
      libxrender-dev \
      libxml2-dev \
      libxss-dev \
      libxtst-dev \
      libxv-dev \
      libxxf86vm-dev \
      libzstd-dev \
      lcov \
      nasm \
      ninja-build \
      patch \
      patchelf \
      perl \
      pkg-config \
      python3 \
      python3-dev \
      python3-pip \
      rpm \
      tar \
      tzdata \
      unzip \
      uuid-dev \
      xz-utils \
      yasm \
      zip \
      zlib1g-dev \
      zstd \
      sqlite3 \
      libsqlite3-dev \
      zstd \
      libzstd-dev \
      qtbase5-dev

RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone

RUN python3 -m pip install --no-cache-dir "pip==${PIP_VERSION}" && \
    python3 -m pip install --no-cache-dir \
      "setuptools==${SETUPTOOLS_VERSION}" \
      "wheel==${WHEEL_VERSION}"
RUN python3 -m pip install --no-cache-dir \
      cmake==3.31.6 \
      conan==2.29.1
RUN if [ "${CI_EXTRAS}" = "1" ]; then \
      python3 -m pip install --no-cache-dir \
        clang-format==22.1.5 \
        clang-tidy==21.1.6 \
        cmake-format==0.6.13 \
        cpplint==2.0.2 \
        nanobind==2.13.0 \
        PyYAML==6.0.3; \
    fi

RUN if [ "${CI_EXTRAS}" = "1" ]; then set -eu; \
      case "$(dpkg --print-architecture)" in \
        amd64) actionlint_arch=amd64; actionlint_sha256=8aca8db96f1b94770f1b0d72b6dddcb1ebb8123cb3712530b08cc387b349a3d8 ;; \
        arm64) actionlint_arch=arm64; actionlint_sha256=325e971b6ba9bfa504672e29be93c24981eeb1c07576d730e9f7c8805afff0c6 ;; \
        *) echo "Unsupported actionlint architecture: $(dpkg --print-architecture)" >&2; exit 1 ;; \
      esac; \
      curl -fsSL \
        "https://github.com/rhysd/actionlint/releases/download/v1.7.12/actionlint_1.7.12_linux_${actionlint_arch}.tar.gz" \
        -o /tmp/actionlint.tar.gz; \
      echo "${actionlint_sha256}  /tmp/actionlint.tar.gz" | sha256sum -c -; \
      tar -xzf /tmp/actionlint.tar.gz -C /usr/local/bin actionlint; \
      rm -f /tmp/actionlint.tar.gz; \
      chmod +x /usr/local/bin/actionlint; \
    fi

RUN set -eu; \
    case "${TARGETARCH:-}" in \
      amd64) osg_sdk_arch=linux_x86_64 ;; \
      arm64) osg_sdk_arch=linux_aarch64 ;; \
      *) echo "Unsupported OSG SDK architecture: ${TARGETARCH:-unknown}" >&2; exit 1 ;; \
    esac; \
    GIT_LFS_SKIP_SMUDGE=1 git clone --quiet --depth 1 --branch master https://github.com/thun-res/osg_sdk.git /opt/vlink/osg_sdk; \
    git -C /opt/vlink/osg_sdk lfs install --local >/dev/null; \
    git -C /opt/vlink/osg_sdk lfs pull --include="${osg_sdk_arch}/**" --exclude="" >/dev/null; \
    ln -sfn "/opt/vlink/osg_sdk/${osg_sdk_arch}" /opt/vlink/osg_sdk/current

WORKDIR /tmp/qt-build

RUN set -eu; \
    if [ "${TARGETARCH:-}" != "amd64" ]; then exit 0; fi; \
    qt_version=6.8.3; \
    qt_build=202503201345; \
    download() { \
      local url="$1"; \
      local output="$2"; \
      local attempt; \
      for attempt in 1 2 3 4 5; do \
        if curl -fsSL --connect-timeout 30 --speed-limit 1024 --speed-time 60 "${url}" -o "${output}"; then return 0; fi; \
        rm -f "${output}"; \
        sleep "$((attempt * 5))"; \
      done; \
      return 1; \
    }; \
    extract_qt() { \
      local archive="$1"; \
      local out_dir="$2"; \
      local file="${qt_version}-0-${qt_build}${archive}"; \
      download "${qt_repo}/${file}" "${file}"; \
      download "${qt_repo}/${file}.sha1" "${file}.sha1"; \
      echo "$(cat "${file}.sha1")  ${file}" | sha1sum --status -c -; \
      mkdir -p "${out_dir}"; \
      (cd "${out_dir}" && cmake -E tar xf "/tmp/qt-build/${file}"); \
      rm -f "${file}" "${file}.sha1"; \
    }; \
    qt_repo="https://download.qt.io/online/qtsdkrepository/linux_x64/desktop/qt6_683/qt6_683/qt.qt6.683.linux_gcc_64"; \
    qt_dir="/opt/Qt/${qt_version}/gcc_64"; \
    qt_link="/opt/Qt/current"; \
    for archive in \
      qtbase-Linux-RHEL_8_10-GCC-Linux-RHEL_8_10-X86_64.7z \
      qtsvg-Linux-RHEL_8_10-GCC-Linux-RHEL_8_10-X86_64.7z \
      qtwayland-Linux-RHEL_8_10-GCC-Linux-RHEL_8_10-X86_64.7z; do \
      extract_qt "${archive}" "${qt_dir}"; \
    done; \
    extract_qt icu-linux-Rhel8.6-x86_64.7z "${qt_dir}/lib"; \
    ln -sfn "${qt_dir}" "${qt_link}"; \
    test -x "${qt_link}/bin/qmake"; \
    "${qt_link}/bin/qmake" -query QT_VERSION >/dev/null; \
    test -f "${qt_link}/lib/libQt6Core.so"; \
    test -f "${qt_link}/lib/libQt6Widgets.so"; \
    test -f "${qt_link}/lib/libQt6OpenGLWidgets.so"; \
    test -f "${qt_link}/lib/libQt6Svg.so"; \
    test -f "${qt_link}/plugins/platforms/libqxcb.so"; \
    test -f "${qt_link}/plugins/imageformats/libqsvg.so"; \
    rm -rf /tmp/qt-build/*

WORKDIR /tmp

RUN set -eu; \
    if [ "${TARGETARCH:-}" != "amd64" ]; then exit 0; fi; \
    case "$(dpkg --print-architecture)" in \
      amd64) ifw_arch=x64; ifw_sha1="47f3199133a347c27a5063cbcdf526753e0d1abe" ;; \
      *) echo "Unsupported Qt IFW architecture: ${TARGETARCH:-unknown}" >&2; exit 1 ;; \
    esac; \
    ifw_version=4.11.0; \
    ifw_build=202603231357; \
    ifw_package="${ifw_version}-0-${ifw_build}ifw-linux-${ifw_arch}.7z"; \
    ifw_url="https://download.qt.io/online/qtsdkrepository/linux_${ifw_arch}/ifw/tools_ifw_411/qt.tools.ifw.411/${ifw_package}"; \
    qtifw_dir="/opt/Qt/Tools/QtInstallerFramework/current"; \
    curl -fsSL --connect-timeout 30 --speed-limit 1024 --speed-time 60 "${ifw_url}" -o "${ifw_package}"; \
    echo "${ifw_sha1}  ${ifw_package}" | sha1sum --status -c -; \
    mkdir -p "${qtifw_dir}"; \
    (cd "${qtifw_dir}" && cmake -E tar xf "/tmp/${ifw_package}"); \
    test -x "${qtifw_dir}/bin/binarycreator"; \
    "${qtifw_dir}/bin/binarycreator" --help >/dev/null; \
    binarycreator_deps="$(ldd -r "${qtifw_dir}/bin/binarycreator" 2>&1 || true)"; \
    if grep -E "not found|undefined symbol:" <<< "${binarycreator_deps}"; then exit 1; fi; \
    rm -f "${ifw_package}"

WORKDIR /work
