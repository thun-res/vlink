#!/usr/bin/env bash
set -euo pipefail

# Verify the staged macOS portable tree is self-contained. Every Mach-O image
# under bin/ and lib/ must resolve each dependency either to a system location
# (/usr/lib, /System/Library) or to a library bundled inside the tree (via an
# @rpath/@executable_path/@loader_path reference). An absolute path anywhere
# else (Homebrew, the Conan cache, a build-only prefix) means the binary was
# not relocated and will fail on a user's machine. Mirrors
# check-linux-runtime-closure.sh.

root="${1:-build-conan/packup/darwin/vlink}"
bindir="${root}/bin"
libdir="${root}/lib"

test -d "${bindir}"
test -d "${libdir}"

status=0
while IFS= read -r -d '' image; do
  if ! file -b "${image}" | grep -q "Mach-O"; then
    continue
  fi
  while IFS= read -r dep; do
    [ -z "${dep}" ] && continue
    case "${dep}" in
      /usr/lib/*|/System/Library/*)
        ;;
      @rpath/*)
        target="${libdir}/${dep#@rpath/}"
        if [ ! -e "${target}" ]; then
          echo "::error::${image}: unresolved bundled dependency ${dep}"
          status=1
        fi
        ;;
      @executable_path/*)
        target="${bindir}/${dep#@executable_path/}"
        if [ ! -e "${target}" ]; then
          echo "::error::${image}: unresolved bundled dependency ${dep}"
          status=1
        fi
        ;;
      @loader_path/*)
        target="$(dirname "${image}")/${dep#@loader_path/}"
        if [ ! -e "${target}" ]; then
          echo "::error::${image}: unresolved bundled dependency ${dep}"
          status=1
        fi
        ;;
      /*)
        echo "::error::${image}: non-relocatable absolute dependency ${dep}"
        status=1
        ;;
      *)
        echo "::error::${image}: unexpected dependency reference ${dep}"
        status=1
        ;;
    esac
  done < <(otool -L "${image}" | tail -n +2 | awk '{print $1}')
done < <(find "${bindir}" "${libdir}" -type f -print0)

exit "${status}"
