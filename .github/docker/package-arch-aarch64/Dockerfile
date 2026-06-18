FROM menci/archlinuxarm:base-devel

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

RUN set -eux; \
    test "$(uname -m)" = "aarch64"; \
    pacman-key --init; \
    pacman-key --populate || true; \
    pacman -Syy --noconfirm; \
    if ! pacman -S --noconfirm --needed archlinuxarm-keyring; then \
      tmp_config="$(mktemp)"; \
      cp /etc/pacman.conf "${tmp_config}"; \
      if grep -q '^SigLevel[[:space:]]*=' "${tmp_config}"; then \
        sed -i 's/^SigLevel[[:space:]]*=.*/SigLevel = Never/g' "${tmp_config}"; \
      else \
        sed -i '/^\[options\]/a SigLevel = Never' "${tmp_config}"; \
      fi; \
      pacman -S --noconfirm --needed --config "${tmp_config}" archlinuxarm-keyring; \
      rm -f "${tmp_config}"; \
    fi; \
    pacman-key --populate archlinuxarm

RUN set -eux; \
    pacman -S --noconfirm --needed \
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
