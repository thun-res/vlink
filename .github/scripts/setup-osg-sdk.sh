#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE:-$0}")" && pwd)"
. "$script_dir/github-progress.sh"

platform_dir="${1:?OSG platform directory is required}"
platform_root="${platform_dir%%/*}"
setup_root="${VLINK_SETUP_ROOT:-$HOME/.vlink-ci}"
osg_root="$setup_root/osg_sdk"
log="${RUNNER_TEMP:-/tmp}/vlink-osg-sdk.log"

if [ -d "$osg_root/.git" ]; then
    repo_step="Update OSG SDK repository"
else
    repo_step="Clone OSG SDK repository"
fi

vlink_progress_init "Setup OSG SDK" \
    "Prepare SDK cache" \
    "$repo_step" \
    "Install Git LFS hooks" \
    "Pull $platform_root SDK files" \
    "Export OSG environment"

vlink_progress_run "Prepare SDK cache" mkdir -p "$setup_root"

vlink_progress_start "$repo_step"
if [ -d "$osg_root/.git" ]; then
    git -C "$osg_root" remote set-url origin https://github.com/thun-res/osg_sdk.git
    git -C "$osg_root" fetch --quiet --depth 1 origin master > "$log" 2>&1 || {
        status=$?
        tail -100 "$log" >&2
        vlink_progress_fail "$repo_step"
        exit "$status"
    }
    git -C "$osg_root" reset --quiet --hard FETCH_HEAD || {
        status=$?
        vlink_progress_fail "$repo_step"
        exit "$status"
    }
else
    rm -rf "$osg_root"
    GIT_LFS_SKIP_SMUDGE=1 git clone --quiet --depth 1 --branch master \
        https://github.com/thun-res/osg_sdk.git "$osg_root" > "$log" 2>&1 || {
        status=$?
        tail -100 "$log" >&2
        vlink_progress_fail "$repo_step"
        exit "$status"
    }
fi
vlink_progress_complete "$repo_step"

vlink_progress_start "Install Git LFS hooks"
if git -C "$osg_root" lfs install --local > /dev/null; then
    vlink_progress_complete "Install Git LFS hooks"
else
    status=$?
    vlink_progress_fail "Install Git LFS hooks"
    exit "$status"
fi

vlink_progress_start "Pull $platform_root SDK files"
git -C "$osg_root" lfs pull --include="$platform_root/**" --exclude="" > "$log" 2>&1 || {
    status=$?
    tail -100 "$log" >&2
    vlink_progress_fail "Pull $platform_root SDK files"
    exit "$status"
}
vlink_progress_complete "Pull $platform_root SDK files"

vlink_progress_start "Export OSG environment"
if echo "OSG_DIR=$osg_root/$platform_dir" >> "$GITHUB_ENV"; then
    vlink_progress_complete "Export OSG environment"
else
    status=$?
    vlink_progress_fail "Export OSG environment"
    exit "$status"
fi
