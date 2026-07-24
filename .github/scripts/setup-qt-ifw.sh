#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE:-$0}")" && pwd)"
. "$script_dir/github-progress.sh"

qt_host="${1:?Qt host is required}"
qt_arch="${2:?Qt arch is required}"

if [ "$qt_host:$qt_arch" != "mac:clang_64" ]; then
    echo "Unsupported Qt package: $qt_host $qt_arch" >&2
    exit 1
fi

qt_version="6.8.3"
setup_root="${VLINK_SETUP_ROOT:-$HOME/.vlink-ci}"
qt_root="$setup_root/qt"
download_root="$setup_root/downloads"
qt_dir="$qt_root/$qt_version/macos"
ifw_dir="$qt_root/Tools/QtInstallerFramework/4.11"
qt_repo="https://download.qt.io/online/qtsdkrepository/mac_x64/desktop/qt6_683/qt6_683/qt.qt6.683.clang_64"
ifw_repo="https://download.qt.io/online/qtsdkrepository/mac_x64/ifw/tools_ifw_411/qt.tools.ifw.411"
qt_prefix="6.8.3-0-202503201723"
ifw_prefix="4.11.0-0-202603311245"

function download() {
    local url="$1"
    local output="$2"
    local attempt

    for attempt in 1 2 3 4 5; do
        if curl -fsSL --connect-timeout 30 --speed-limit 1024 --speed-time 60 \
            "$url" -o "$output"; then
            return 0
        fi
        rm -f "$output"
        sleep "$((attempt * 5))"
    done
    return 1
}

function extract_archive() {
    local repo="$1"
    local prefix="$2"
    local archive="$3"
    local destination="$4"
    local file="$prefix$archive"
    local path="$download_root/$file"
    download "$repo/$file" "$path" || return 1
    download "$repo/$file.sha1" "$path.sha1" || return 1
    echo "$(cat "$path.sha1")  $path" | shasum -a 1 --status -c - || return 1
    mkdir -p "$destination" || return 1
    (cd "$destination" && cmake -E tar xf "$path") || return 1
    rm -f "$path" "$path.sha1"
}

progress_steps=("Prepare download cache")
if [ ! -x "$qt_dir/bin/qmake" ]; then
    progress_steps+=(
        "Reset Qt cache"
        "Install Qt base archive"
        "Install Qt SVG archive"
    )
else
    progress_steps+=("Use cached Qt")
fi
if [ ! -x "$ifw_dir/bin/binarycreator" ]; then
    progress_steps+=(
        "Reset QtIFW cache"
        "Install QtIFW archive"
    )
else
    progress_steps+=("Use cached QtIFW")
fi
progress_steps+=(
    "Verify Qt tools"
    "Export Qt environment"
)

vlink_progress_init "Setup Qt and QtIFW" "${progress_steps[@]}"
vlink_progress_run "Prepare download cache" mkdir -p "$download_root"

if [ ! -x "$qt_dir/bin/qmake" ]; then
    vlink_progress_run "Reset Qt cache" rm -rf "${qt_root:?}/$qt_version"
    vlink_progress_run "Install Qt base archive" \
        extract_archive "$qt_repo" "$qt_prefix" \
        "qtbase-MacOS-MacOS_14-Clang-MacOS-MacOS_14-X86_64-ARM64.7z" "$qt_dir"
    vlink_progress_run "Install Qt SVG archive" \
        extract_archive "$qt_repo" "$qt_prefix" \
        "qtsvg-MacOS-MacOS_14-Clang-MacOS-MacOS_14-X86_64-ARM64.7z" "$qt_dir"
else
    vlink_progress_run "Use cached Qt" test -x "$qt_dir/bin/qmake"
fi

if [ ! -x "$ifw_dir/bin/binarycreator" ]; then
    vlink_progress_run "Reset QtIFW cache" rm -rf "$ifw_dir"
    vlink_progress_run "Install QtIFW archive" \
        extract_archive "$ifw_repo" "$ifw_prefix" "ifw-mac-universal.7z" "$ifw_dir"
else
    vlink_progress_run "Use cached QtIFW" test -x "$ifw_dir/bin/binarycreator"
fi

vlink_progress_start "Verify Qt tools"
if [ -x "$qt_dir/bin/qmake" ] &&
    [ -x "$ifw_dir/bin/binarycreator" ]; then
    vlink_progress_complete "Verify Qt tools"
else
    status=$?
    vlink_progress_fail "Verify Qt tools"
    exit "$status"
fi

vlink_progress_start "Export Qt environment"
if echo "QT_DIR=$qt_dir" >> "$GITHUB_ENV" &&
    echo "QTIFW_DIR=$ifw_dir" >> "$GITHUB_ENV"; then
    vlink_progress_complete "Export Qt environment"
else
    status=$?
    vlink_progress_fail "Export Qt environment"
    exit "$status"
fi
