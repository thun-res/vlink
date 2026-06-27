#!/usr/bin/env bash
#
# Build an Arch/Manjaro pacman package (.pkg.tar.zst) for vlink.
#
# CPack has no native pacman generator, so this script:
#   1) Builds with cmake + DESTDIR install to a per-component staging dir
#   2) Hard-links it under a /usr-wrapper + emits .PKGINFO + .MTREE
#   3) Packs the wrapper into a .pkg.tar.zst with bsdtar
#
# CPM-fetched third-party libs are static + marked EXCLUDE_FROM_ALL in
# cmake/cpm_thirdparty.cmake, so the install tree contains only vlink's
# own files.
#
# Usage:
#   ./packup/build-arch.sh {project dir}
#
# Output:
#   {project}/build-arch/packup/linux/vlink-dev-<version>-<distro>-<arch>.pkg.tar.zst
#   (or vlink-runtime-<version>-... plus vlink-dev-<version>-... if PACKAGE_SPLIT=ON)
#
# Env:
#   PACKAGE_SPLIT  ON  -> emit two packages: vlink-runtime + vlink-dev
#                        OFF -> emit one fat package (default for Arch)
#   SOURCE_DATE_EPOCH    epoch seconds for reproducible builds
#
# Required system build deps (CPM fetches DDS/iceoryx/etc itself):
#   base-devel cmake git
#   openssl sqlite zstd
#
# Host support:
#   - Manjaro / Arch: native; bsdtar is part of base (libarchive).
#   - Other distros: install `libarchive-tools` (Debian) / `bsdtar` (Fedora).

shopt -s extglob
set -e

WORK_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PLATFORM_ARCH=$(uname -m)

GITHUB_URL=""
POSITIONAL=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            echo -e "Usage:\n  build-arch.sh [--github_url <url>] {project dir}\n\nOptions:\n  --github_url <url>  override CPM_GITHUB_URL (default: https://github.com)\n\nEnv:\n  PACKAGE_SPLIT=ON  emit vlink-runtime + vlink-dev\n  SOURCE_DATE_EPOCH=<sec> reproducible BUILDDATE"
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
    echo -e "Usage:\n  build-arch.sh [--github_url <url>] {project dir}"
    exit 0
fi

command -v cmake >/dev/null || { echo "Error: cmake not found in PATH"; exit 2; }
command -v bsdtar >/dev/null || {
    echo "Error: bsdtar (libarchive) not found."
    echo "  Manjaro/Arch : pre-installed (libarchive)"
    echo "  Debian/Ubuntu: sudo apt install libarchive-tools"
    echo "  Fedora/RHEL  : sudo dnf install bsdtar"
    exit 2
}

SRC_DIR=$(cd "${POSITIONAL[0]}" && pwd)
BUILD_DIR=$SRC_DIR/build-arch
OUTPUT_DIR=$BUILD_DIR/packup/linux
VERSION=$(tr -d '[:space:]' < "$SRC_DIR/version.txt")
DISTRO=linux
if [ -r /etc/os-release ]; then
    DISTRO="$(. /etc/os-release && printf '%s%s' "$ID" "${VERSION_ID%%.*}")"
fi
PKGREL=1
: "${PACKAGE_SPLIT:=OFF}"
case "${PACKAGE_SPLIT^^}" in
    ON|TRUE|YES|Y|1)        PACKAGE_SPLIT=ON ;;
    OFF|FALSE|NO|N|0)       PACKAGE_SPLIT=OFF ;;
    *) echo "Error: PACKAGE_SPLIT='$PACKAGE_SPLIT' is not boolean (use ON/OFF/1/0/TRUE/FALSE/YES/NO)" >&2; exit 2 ;;
esac
PKGINFO_TEMPLATE="$WORK_DIR/PKGINFO.in"

if [ -n "$SOURCE_DATE_EPOCH" ]; then
    BUILDDATE="$SOURCE_DATE_EPOCH"
elif _git_ts=$(git -C "$SRC_DIR" log -1 --format=%ct 2>/dev/null) && [ -n "$_git_ts" ]; then
    BUILDDATE="$_git_ts"
else
    BUILDDATE=$(date +%s)
fi

[ -f "$PKGINFO_TEMPLATE" ] || { echo "Error: PKGINFO template not found: $PKGINFO_TEMPLATE"; exit 2; }
[ -d "$BUILD_DIR" ] && rm -rf "$BUILD_DIR"

echo ""
echo "********************************************************************"
echo "*** [build-arch] cmake configure..."
echo "*** SRC_DIR              = $SRC_DIR"
echo "*** BUILD_DIR            = $BUILD_DIR"
echo "*** VERSION              = $VERSION"
echo "*** ARCH                 = $PLATFORM_ARCH"
echo "*** PACKAGE_SPLIT        = $PACKAGE_SPLIT"
echo "*** BUILDDATE            = $BUILDDATE"
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
    "${CMAKE_EXTRA_ARGS[@]}"

echo ""
echo "********************************************************************"
echo "*** [build-arch] build..."
echo "********************************************************************"
echo ""

cmake --build "$BUILD_DIR" -j"$(nproc)"

mkdir -p "$OUTPUT_DIR"

# _pack_arch <pkgname> <pkgdesc> <component-or-empty> [extra-PKGINFO-line ...]
_pack_arch() {
    local pkgname="$1"
    local pkgdesc="$2"
    local component="$3"
    shift 3
    local wrap_dir="$BUILD_DIR/packup/linux/arch-${pkgname}"
    local stage_dir="$BUILD_DIR/install-${pkgname}"

    echo ""
    echo "********************************************************************"
    echo "*** [build-arch] installing component='${component:-all}' -> ${pkgname}"
    echo "********************************************************************"
    echo ""

    rm -rf "$wrap_dir" "$stage_dir"
    if [ -z "$component" ]; then
        DESTDIR="$stage_dir" cmake --install "$BUILD_DIR" --strip
    else
        DESTDIR="$stage_dir" cmake --install "$BUILD_DIR" --strip --component "$component"
    fi
    if [ ! -d "$stage_dir/usr" ]; then
        echo "warning: nothing installed for component '${component:-all}' -- skipping ${pkgname}" >&2
        return 0
    fi

    mkdir -p "$wrap_dir"
    cp -al "$stage_dir/usr" "$wrap_dir/usr"

    local pkg_size
    pkg_size=$(du -sb "$wrap_dir" | cut -f1)

    sed -e "s|@PKGNAME@|${pkgname}|g" \
        -e "s|@PKGDESC@|${pkgdesc}|g" \
        -e "s|@VERSION@|${VERSION}|g" \
        -e "s|@PKGREL@|${PKGREL}|g" \
        -e "s|@BUILDDATE@|${BUILDDATE}|g" \
        -e "s|@PKG_SIZE@|${pkg_size}|g" \
        -e "s|@PLATFORM_ARCH@|${PLATFORM_ARCH}|g" \
        "$PKGINFO_TEMPLATE" > "$wrap_dir/.PKGINFO"

    local line
    for line in "$@"; do
        printf '%s\n' "$line" >> "$wrap_dir/.PKGINFO"
    done

    (cd "$wrap_dir" && bsdtar -czf .MTREE --format=mtree \
        --options='!all,use-set,type,uid,gid,mode,time,size,md5,sha256,link' \
        .PKGINFO usr) \
        || { echo "warning: .MTREE generation failed for ${pkgname}" >&2; rm -f "$wrap_dir/.MTREE"; }

    local pack_args=(.PKGINFO)
    [ -f "$wrap_dir/.MTREE" ] && pack_args+=(.MTREE)
    pack_args+=(usr)

    local pkg_file="$OUTPUT_DIR/${pkgname}-${VERSION}-${DISTRO}-${PLATFORM_ARCH}.pkg.tar.zst"
    (cd "$wrap_dir" && bsdtar --zstd -cf "$pkg_file" "${pack_args[@]}")

    ls -la "$pkg_file"
}

echo ""
echo "********************************************************************"
echo "*** [build-arch] packing .pkg.tar.zst (split=${PACKAGE_SPLIT})..."
echo "********************************************************************"

VLINK_RUNTIME_LICENSES=(
    "license = Apache-2.0"
    "license = MIT"
    "license = BSD-3-Clause"
    "license = BSL-1.0"
    "license = Zlib"
    "license = EPL-2.0"
)
VLINK_RUNTIME_DEPS=(
    "depend = openssl"
    "depend = sqlite"
    "depend = zstd"
)
VLINK_DEVEL_LICENSES=("license = Apache-2.0")
VLINK_DEVEL_DEPS=("depend = vlink-runtime=${VERSION}-${PKGREL}")

if [ "$PACKAGE_SPLIT" = "ON" ]; then
    _pack_arch "vlink-runtime" "Runtime files for vlink"                                  runtime \
        "${VLINK_RUNTIME_LICENSES[@]}" "${VLINK_RUNTIME_DEPS[@]}"
    _pack_arch "vlink-dev"     "Headers and CMake configs for vlink"                      devel \
        "${VLINK_DEVEL_LICENSES[@]}"   "${VLINK_DEVEL_DEPS[@]}"
else
    _pack_arch "vlink-dev"     "Runtime and development files for vlink"              "" \
        "${VLINK_RUNTIME_LICENSES[@]}" "${VLINK_RUNTIME_DEPS[@]}"
fi

echo ""
echo "********************************************************************"
echo "*** Generated:"
echo "********************************************************************"
ls -la "$OUTPUT_DIR"/vlink*.pkg.tar.zst

echo ""
echo "Install:    sudo pacman -U <pkg>.pkg.tar.zst"
echo "Inspect:    pacman -Qpi  <pkg>.pkg.tar.zst"
echo "List files: bsdtar -tf   <pkg>.pkg.tar.zst"
echo "Done."
exit 0
