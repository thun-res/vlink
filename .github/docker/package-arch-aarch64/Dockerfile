FROM menci/archlinuxarm:base-devel

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

RUN set -eux; \
    retry_pacman() { \
      local attempt; \
      for attempt in 1 2 3; do \
        if pacman "$@"; then \
          return 0; \
        fi; \
        rm -f /var/lib/pacman/db.lck; \
        sleep "$((attempt * 10))"; \
      done; \
      pacman "$@"; \
    }; \
    test "$(uname -m)" = "aarch64"; \
    rm -f /var/lib/pacman/db.lck; \
    pacman-key --init; \
    pacman-key --populate archlinuxarm || pacman-key --populate || true; \
    retry_pacman -Syy --noconfirm; \
    if ! retry_pacman -S --noconfirm --needed archlinuxarm-keyring; then \
      tmp_config="$(mktemp)"; \
      cp /etc/pacman.conf "${tmp_config}"; \
      if grep -q '^SigLevel[[:space:]]*=' "${tmp_config}"; then \
        sed -i 's/^SigLevel[[:space:]]*=.*/SigLevel = Never/g' "${tmp_config}"; \
      else \
        sed -i '/^\[options\]/a SigLevel = Never' "${tmp_config}"; \
      fi; \
      retry_pacman -S --noconfirm --needed --config "${tmp_config}" archlinuxarm-keyring; \
      rm -f "${tmp_config}"; \
    fi; \
    pacman-key --populate archlinuxarm || pacman-key --populate; \
    retry_pacman -Syu --noconfirm --needed \
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
