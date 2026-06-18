#!/usr/bin/env bash
#
# Build a Debian/Ubuntu .deb package for vlink via CPack DEB generator.
#
# CPM-fetched third-party libs are static + marked EXCLUDE_FROM_ALL in
# cmake/cpm_thirdparty.cmake, so their install rules do not run during
# cpack's internal `cmake --install`. The resulting .deb contains only
# vlink's own files.
#
# Usage:
#   ./packup/build-deb.sh {project dir}
#
# Output:
#   {project}/build-deb/packup/linux/vlink-<ver>-linux-<arch>.deb
#
# Required system build deps (CPM fetches DDS/iceoryx/etc itself):
#   build-essential cmake git
#   libssl-dev libsqlite3-dev libzstd-dev
#
# Host support:
#   - Debian / Ubuntu : native; dpkg-deb pre-installed
#   - Manjaro / Arch  : sudo pacman -S dpkg   (or: yay -S dpkg)
#   - Fedora / RHEL   : sudo dnf install dpkg

shopt -s extglob
set -e

WORK_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PLATFORM_ARCH=$(uname -m)

GITHUB_URL=""
POSITIONAL=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            echo -e "Usage:\n  build-deb.sh [--github_url <url>] {project dir}\n\nOptions:\n  --github_url <url>  override CPM_GITHUB_URL (default: https://github.com)"
            exit 0
            ;;
        --github_url)
            GITHUB_URL="$2"
            shift 2
            ;;
        *)
            POSITIONAL+=("$1")
            shift
            ;;
    esac
done

if [ ${#POSITIONAL[@]} -eq 0 ]; then
    echo -e "Usage:\n  build-deb.sh [--github_url <url>] {project dir}"
    exit 0
fi

command -v cmake >/dev/null || { echo "Error: cmake not found in PATH"; exit 2; }
command -v dpkg-deb >/dev/null || {
    echo "Error: dpkg-deb not found."
    echo "  Debian/Ubuntu: pre-installed (dpkg)"
    echo "  Manjaro/Arch : sudo pacman -S dpkg   (or: yay -S dpkg)"
    echo "  Fedora/RHEL  : sudo dnf install dpkg"
    exit 2
}

SRC_DIR=$(cd "${POSITIONAL[0]}" && pwd)
BUILD_DIR=$SRC_DIR/build-deb
OUTPUT_DIR=$BUILD_DIR/packup/linux
VERSION=$(tr -d '[:space:]' < "$SRC_DIR/version.txt")
: "${PACKAGE_SPLIT:=OFF}"
case "${PACKAGE_SPLIT^^}" in
    ON|TRUE|YES|Y|1)        PACKAGE_SPLIT=ON ;;
    OFF|FALSE|NO|N|0)       PACKAGE_SPLIT=OFF ;;
    *) echo "Error: PACKAGE_SPLIT='$PACKAGE_SPLIT' is not boolean (use ON/OFF/1/0/TRUE/FALSE/YES/NO)" >&2; exit 2 ;;
esac

DEB_DEPENDS="libssl3 | libssl3t64, libsqlite3-0, libzstd1"

[ -d "$BUILD_DIR" ] && rm -rf "$BUILD_DIR"

echo ""
echo "********************************************************************"
echo "*** [build-deb] cmake configure..."
echo "*** SRC_DIR              = $SRC_DIR"
echo "*** BUILD_DIR            = $BUILD_DIR"
echo "*** VERSION              = $VERSION"
echo "*** ARCH                 = $PLATFORM_ARCH"
echo "*** PACKAGE_SPLIT        = $PACKAGE_SPLIT"
echo "*** CPM_GITHUB_URL       = ${GITHUB_URL:-(default)}"
echo "********************************************************************"
echo ""

CMAKE_EXTRA_ARGS=()
[ -n "$GITHUB_URL" ] && CMAKE_EXTRA_ARGS+=("-DCPM_GITHUB_URL=$GITHUB_URL")

cmake -S "$SRC_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DINSTALL_CONFIG_DIR=share/vlink \
    -DENABLE_SYMLINKS=OFF \
    -DENABLE_COMPLETIONS=ON \
    -DENABLE_CPM=ON \
    -DENABLE_CPM_PROTOBUF=ON \
    -DENABLE_CPM_FLATBUFFERS=ON \
    -DENABLE_IOX_ROUDI=OFF \
    -DENABLE_WEBVIZ=ON \
    -DENABLE_PACKAGE_SPLIT="$PACKAGE_SPLIT" \
    -DCPACK_GENERATOR=DEB \
    -DCPACK_DEBIAN_PACKAGE_DEPENDS="$DEB_DEPENDS" \
    "${CMAKE_EXTRA_ARGS[@]}"

echo ""
echo "********************************************************************"
echo "*** [build-deb] build..."
echo "********************************************************************"
echo ""

cmake --build "$BUILD_DIR" -j"$(nproc)"

echo ""
echo "********************************************************************"
echo "*** [build-deb] cpack -G DEB..."
echo "********************************************************************"
echo ""

(cd "$BUILD_DIR" && cpack -G DEB)

mkdir -p "$OUTPUT_DIR"
mv -f "$BUILD_DIR"/vlink*.deb "$OUTPUT_DIR"/ 2>/dev/null || true

echo ""
echo "********************************************************************"
echo "*** Generated:"
echo "********************************************************************"
ls -la "$OUTPUT_DIR"/vlink*.deb 2>/dev/null || echo "(none)"

echo ""
echo "Install:    sudo apt install $OUTPUT_DIR/vlink*.deb"
echo "Inspect:    dpkg-deb -I  $OUTPUT_DIR/vlink*.deb"
echo "List files: dpkg-deb -c  $OUTPUT_DIR/vlink*.deb"
echo "Done."
exit 0
