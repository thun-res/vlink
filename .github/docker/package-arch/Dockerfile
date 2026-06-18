FROM archlinux:base-devel

RUN pacman -Syu --noconfirm --needed \
      acl \
      base-devel \
      ca-certificates \
      ccache \
      cmake \
      file \
      git \
      libarchive \
      openssl \
      patch \
      sqlite \
      tar \
      zstd \
    && pacman -Scc --noconfirm
