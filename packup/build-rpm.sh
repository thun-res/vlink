#!/usr/bin/env bash
#
# Build a RHEL/Fedora/openEuler .rpm package for vlink via CPack RPM generator.
#
# CPM-fetched third-party libs are static + marked EXCLUDE_FROM_ALL in
# cmake/cpm_thirdparty.cmake, so their install rules do not run during
# cpack's internal `cmake --install`. The resulting .rpm contains only
# vlink's own files.
#
# Usage:
#   ./packup/build-rpm.sh {project dir}
#
# Output:
#   {project}/build-rpm/packup/linux/vlink-dev-<version>-<distro>-<arch>.rpm
#
# Required system build deps (CPM fetches DDS/iceoryx/etc itself):
#   gcc-c++ cmake git
#   openssl-devel sqlite-devel libzstd-devel
#
# Host support:
#   - Fedora / RHEL / openEuler / Anolis: native; rpmbuild from rpm-build
#       sudo dnf install rpm-build
#   - Manjaro / Arch : sudo pacman -S rpm-tools
#   - Debian / Ubuntu: sudo apt install rpm

shopt -s extglob
set -e

WORK_DIR=$(cd "$(dirname "${BASH_SOURCE:-$0}")" && pwd)
PLATFORM_ARCH=$(uname -m)

GITHUB_URL=""
POSITIONAL=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            echo -e "Usage:\n  build-rpm.sh [--github_url <url>] {project dir}\n\nOptions:\n  --github_url <url>  override CPM_GITHUB_URL (default: https://github.com)"
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
    echo -e "Usage:\n  build-rpm.sh [--github_url <url>] {project dir}"
    exit 0
fi

command -v cmake >/dev/null || { echo "Error: cmake not found in PATH"; exit 2; }
command -v rpmbuild >/dev/null || {
    echo "Error: rpmbuild not found."
    echo "  Fedora/RHEL  : sudo dnf install rpm-build"
    echo "  Manjaro/Arch : sudo pacman -S rpm-tools"
    echo "  Debian/Ubuntu: sudo apt install rpm"
    exit 2
}

SRC_DIR=$(cd "${POSITIONAL[0]}" && pwd)
VERSION=$(tr -d '[:space:]' < "$SRC_DIR/version.txt")
BUILD_DIR=$SRC_DIR/build-rpm
OUTPUT_DIR=$BUILD_DIR/packup/linux
DISTRO=linux
if [ -r /etc/os-release ]; then
    DISTRO="$(. /etc/os-release && printf '%s%s' "$ID" "${VERSION_ID%%.*}")"
fi
: "${PACKAGE_SPLIT:=OFF}"
case "${PACKAGE_SPLIT^^}" in
    ON|TRUE|YES|Y|1)        PACKAGE_SPLIT=ON ;;
    OFF|FALSE|NO|N|0)       PACKAGE_SPLIT=OFF ;;
    *) echo "Error: PACKAGE_SPLIT='$PACKAGE_SPLIT' is not boolean (use ON/OFF/1/0/TRUE/FALSE/YES/NO)" >&2; exit 2 ;;
esac

case "$PLATFORM_ARCH" in
    x86_64|aarch64|ppc64le|s390x) LIBDIR=lib64 ;;
    *) LIBDIR=lib ;;
esac

RPM_REQUIRES="openssl-libs, sqlite-libs, libzstd"

[ -d "$BUILD_DIR" ] && rm -rf "$BUILD_DIR"

echo ""
echo "********************************************************************"
echo "*** [build-rpm] cmake configure..."
echo "*** SRC_DIR              = $SRC_DIR"
echo "*** BUILD_DIR            = $BUILD_DIR"
echo "*** ARCH                 = $PLATFORM_ARCH"
echo "*** LIBDIR               = $LIBDIR"
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
    -DCMAKE_INSTALL_LIBDIR="$LIBDIR" \
    -DINSTALL_CONFIG_DIR=share/vlink \
    -DENABLE_SYMLINKS=OFF \
    -DENABLE_COMPLETIONS=ON \
    -DENABLE_CPM=ON \
    -DENABLE_CPM_PROTOBUF=ON \
    -DENABLE_CPM_FLATBUFFERS=ON \
    -DENABLE_IOX_ROUDI=OFF \
    -DENABLE_WEBVIZ=ON \
    -DENABLE_PACKAGE_SPLIT="$PACKAGE_SPLIT" \
    -DCPACK_GENERATOR=RPM \
    -DCPACK_RPM_FILE_NAME="vlink-dev-${VERSION}-${DISTRO}-${PLATFORM_ARCH}.rpm" \
    -DCPACK_RPM_RUNTIME_FILE_NAME="vlink-runtime-${VERSION}-${DISTRO}-${PLATFORM_ARCH}.rpm" \
    -DCPACK_RPM_DEVEL_FILE_NAME="vlink-dev-${VERSION}-${DISTRO}-${PLATFORM_ARCH}.rpm" \
    -DCPACK_RPM_PACKAGE_REQUIRES="$RPM_REQUIRES" \
    "${CMAKE_EXTRA_ARGS[@]}"

echo ""
echo "********************************************************************"
echo "*** [build-rpm] build..."
echo "********************************************************************"
echo ""

cmake --build "$BUILD_DIR" -j"$(nproc)"

echo ""
echo "********************************************************************"
echo "*** [build-rpm] cpack -G RPM..."
echo "********************************************************************"
echo ""

(cd "$BUILD_DIR" && cpack -G RPM)

mkdir -p "$OUTPUT_DIR"
for pkg in "$BUILD_DIR"/vlink-runtime-*.rpm "$BUILD_DIR"/vlink-dev-*.rpm; do
    [ -f "$pkg" ] || continue
    mv -f "$pkg" "$OUTPUT_DIR"/
done

echo ""
echo "********************************************************************"
echo "*** Generated:"
echo "********************************************************************"
ls -la "$OUTPUT_DIR"/vlink*.rpm 2>/dev/null || echo "(none)"

echo ""
echo "Install:    sudo dnf install $OUTPUT_DIR/vlink*.rpm"
echo "Inspect:    rpm -qpi  $OUTPUT_DIR/vlink*.rpm"
echo "List files: rpm -qpl  $OUTPUT_DIR/vlink*.rpm"
echo "Requires:   rpm -qpR  $OUTPUT_DIR/vlink*.rpm"
echo "Done."
exit 0
