FROM fedora:43

RUN dnf install -y \
      cmake \
      ccache \
      file \
      gcc-c++ \
      git \
      glibc-static \
      libacl-devel \
      libstdc++-static \
      libzstd-devel \
      make \
      openssl-devel \
      patch \
      pkgconf-pkg-config \
      rpm-build \
      sqlite-devel \
      tar \
	      zstd \
	    && printf '%s\n' 'int main() { return 0; }' > /tmp/static-check.cc \
	    && g++ -static /tmp/static-check.cc -o /tmp/static-check \
	    && /tmp/static-check \
	    && rm -f /tmp/static-check.cc /tmp/static-check \
	    && dnf clean all
