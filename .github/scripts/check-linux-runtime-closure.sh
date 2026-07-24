#!/usr/bin/env bash
set -euo pipefail

export LD_LIBRARY_PATH="$PWD/build-conan/packup/linux/vlink/lib:${LD_LIBRARY_PATH:-}"
find build-conan/packup/linux/vlink/bin build-conan/packup/linux/vlink/lib -type f -print0 |
    xargs -0 -r file |
    awk -F: '/ELF/ && $1 !~ /\/platformthemes\/libqgtk3\.so$/ {print $1}' |
    xargs -r .github/scripts/check-elf-deps.sh
