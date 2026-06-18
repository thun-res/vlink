FROM menci/archlinuxarm:base-devel

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

RUN set -eux; \
    pacman-key --init; \
    pacman-key --populate archlinuxarm; \
    pacman -Sy --noconfirm --needed archlinuxarm-keyring; \
    pacman-key --populate archlinuxarm; \
    pacman -Syu --noconfirm --needed \
      acl \
      base-devel \
      ca-certificates \
      ccache \
      cmake \
      git \
      libarchive \
      openssl \
      patch \
      sqlite \
      tar \
      zstd; \
    if command -v update-ca-trust >/dev/null 2>&1; then update-ca-trust; fi; \
    cmake --version; \
    pacman --version; \
    pacman -Scc --noconfirm || true
