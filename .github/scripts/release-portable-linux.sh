#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE:-$0}")" && pwd)"
. "$script_dir/github-progress.sh"

arch="${1:?Linux portable arch is required}"
if [ "$arch" = x86_64 ]; then
    export QT_DIR=/opt/Qt/current
    export QTIFW_DIR=/opt/Qt/Tools/QtInstallerFramework/current
else
    unset QT_DIR QTIFW_DIR
fi

progress_steps=()
if [ -n "${CMAKE_GENERATOR:-}" ]; then
    progress_steps+=("Configure Conan generator")
fi
progress_steps+=(
    "Build Conan package"
    "Stage portable runtime"
    "Create portable archive"
)
if [ "$arch" = x86_64 ]; then
    progress_steps+=("Create QtIFW installer")
fi
progress_steps+=("Check runtime closure")

vlink_progress_init "Release portable Linux $arch" "${progress_steps[@]}"

if [ -n "${CMAKE_GENERATOR:-}" ]; then
    vlink_progress_start "Configure Conan generator"
    if conan_home="${CONAN_HOME:-$HOME/.conan2}" &&
        mkdir -p "$conan_home" &&
        printf '&:tools.cmake.cmaketoolchain:generator=%s\n' "$CMAKE_GENERATOR" >> "$conan_home/global.conf"; then
        vlink_progress_complete "Configure Conan generator"
    else
        status=$?
        vlink_progress_fail "Configure Conan generator"
        exit "$status"
    fi
fi

vlink_progress_run "Build Conan package" bash packup/build-conan.sh . "$arch"
vlink_progress_run "Stage portable runtime" \
    bash "$script_dir/stage-linux-portable-runtime.sh" build-conan/packup/linux/vlink

vlink_progress_start "Create portable archive"
if version="$(tr -d '[:space:]' < version.txt)" &&
    archive_path="build-conan/packup/linux/vlink-desktop-$version-linux-$arch.tgz" &&
    tar -czf "$archive_path" -C build-conan/packup/linux vlink; then
    vlink_progress_complete "Create portable archive"
else
    status=$?
    vlink_progress_fail "Create portable archive"
    exit "$status"
fi

if [ "$arch" = x86_64 ]; then
    vlink_progress_start "Create QtIFW installer"
    if installer_data="build-conan/installer/packages/com.vlink/data" &&
        installer_path="build-conan/packup/linux/vlink-desktop-$version-linux-$arch.run" &&
        rm -rf "${installer_data:?}/"* &&
        cp -rf build-conan/packup/linux/vlink/. "$installer_data/" &&
        rm -f "$installer_path" &&
        "$QTIFW_DIR/bin/binarycreator" \
            --offline-only \
            -c build-conan/installer/config/config.xml \
            -p build-conan/installer/packages \
            "$installer_path"; then
        vlink_progress_complete "Create QtIFW installer"
    else
        status=$?
        vlink_progress_fail "Create QtIFW installer"
        exit "$status"
    fi
fi

vlink_progress_run "Check runtime closure" \
    bash "$script_dir/check-linux-runtime-closure.sh"
