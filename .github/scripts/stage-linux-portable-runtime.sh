#!/usr/bin/env bash
set -euo pipefail

root="${1:-build-conan/packup/linux/vlink}"
libdir="${root}/lib"

test -d "${libdir}"

copy_soname() {
  local soname="$1"
  local src
  src="$(ldconfig -p | awk -v soname="${soname}" '$1 == soname {print $NF; exit}')"
  if [ -z "${src}" ] || [ ! -f "${src}" ]; then
    echo "::error::Missing runtime library: ${soname}"
    exit 1
  fi
  cp -L "${src}" "${libdir}/${soname}"
  echo "Staged runtime library: ${soname}"
}

copy_soname libwayland-client.so.0
copy_soname libwayland-cursor.so.0
copy_soname libwayland-egl.so.1
