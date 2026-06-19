#!/usr/bin/env bash
set -euo pipefail

root="${1:-build-conan/packup/linux/vlink}"
libdir="${root}/lib"

test -d "${libdir}"

copy_soname() {
  local soname="$1"
  local src
  src="$(
    ldconfig -p | awk -v soname="${soname}" '
      $1 == soname && $NF ~ "^/usr/local/" {print $NF; found=1; exit}
      $1 == soname && first == "" {first=$NF}
      END {if (!found && first != "") print first}
    '
  )"
  if [ -z "${src}" ] || [ ! -f "${src}" ]; then
    echo "::error::Missing runtime library: ${soname}"
    exit 1
  fi
  cp -L "${src}" "${libdir}/${soname}"
}

copy_soname libwayland-client.so.0
copy_soname libwayland-cursor.so.0
copy_soname libwayland-egl.so.1

if [ -n "${QT_DIR:-}" ] && [ -f "${QT_DIR}/lib/libQt6WlShellIntegration.so.6" ]; then
  cp -L "${QT_DIR}/lib/libQt6WlShellIntegration.so.6" "${libdir}/libQt6WlShellIntegration.so.6"
fi
