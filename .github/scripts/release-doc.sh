#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE:-$0}")" && pwd)"
. "$script_dir/github-progress.sh"

vlink_progress_init "Release documentation" \
    "Configure CMake" \
    "Build documentation targets" \
    "Generate Doxygen" \
    "Publish documentation home"

vlink_progress_start "Configure CMake"
if cmake -S . -B build-doc \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_DOC=ON \
    -DENABLE_CCACHE_BUILD=ON \
    -DBUILD_SHARED_LIBS=ON \
    -DENABLE_CPM=ON \
    -DENABLE_CPM_SQLITE3=ON \
    -DENABLE_CPM_ZSTD=ON \
    -DENABLE_CPM_PROTOBUF=ON \
    -DENABLE_CPM_FLATBUFFERS=ON \
    -DENABLE_CXX_STD_20=OFF \
    -DENABLE_VIEWER=OFF \
    -DENABLE_WEBVIZ=OFF \
    -DENABLE_IOX_ROUDI=OFF \
    -DENABLE_EXAMPLES=OFF \
    -DENABLE_PYTHON_API=OFF \
    -DENABLE_TEST=OFF \
    -DENABLE_SYMLINKS=OFF \
    -DENABLE_COMPLETIONS=OFF \
    -DPython_EXECUTABLE="$(command -v python3)"; then
    vlink_progress_complete "Configure CMake"
else
    status=$?
    vlink_progress_fail "Configure CMake"
    exit "$status"
fi

vlink_progress_run "Build documentation targets" cmake --build build-doc --parallel
vlink_progress_run "Generate Doxygen" cmake --build build-doc --target doc

vlink_progress_start "Publish documentation home"
doc_dir=build-doc/output/doc
wiki_dir=.github/wiki
if [ -f "$doc_dir/en_us/index.html" ] &&
    [ -f "$doc_dir/zh_cn/index.html" ] &&
    [ -f "$wiki_dir/index.html" ] &&
    cp -a "$wiki_dir/." "$doc_dir/"; then
    vlink_progress_complete "Publish documentation home"
else
    status=$?
    vlink_progress_fail "Publish documentation home"
    exit "$status"
fi
