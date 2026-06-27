#!/usr/bin/env bash
set -euo pipefail

root="${1:-build-conan/packup/linux/vlink}"
libdir="${root}/lib"

test -d "${libdir}"

rm -f "${libdir}"/libQt5Wayland*.so* \
      "${libdir}"/libQt6Wayland*.so* \
      "${libdir}"/libQt6WlShellIntegration.so* \
      "${libdir}"/libwayland-*.so* \
      "${libdir}"/platforms/libqwayland*.so
rm -rf "${libdir}/wayland-decoration-client" \
       "${libdir}/wayland-graphics-integration-client" \
       "${libdir}/wayland-shell-integration"
