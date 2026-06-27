#!/usr/bin/env bash

SHELL_DIR=$(cd "$(dirname "${BASH_SOURCE:-$0}")" && pwd)

([ -z "$1" ] || [ "$1" = "-h" ] || [ "$1" = "--help" ]) && echo -e "Usage: \n  update_version.sh {version}" && exit 0

NEW_VER="$1"
ROOT_DIR=$(cd "$SHELL_DIR/.." && pwd)

README_CN="$ROOT_DIR/README.md"
README_EN="$ROOT_DIR/README.en.md"
VERSION_TXT="$ROOT_DIR/version.txt"
VERSION_H="$ROOT_DIR/include/vlink/version.h"
CONANFILE="$ROOT_DIR/conanfile.py"
VCPKG_JSON="$ROOT_DIR/tools/vcpkg/vcpkg.json"
CHANGELOG="$ROOT_DIR/CHANGELOG.md"

function _ver_to_num() {
    local IFS=.
    local _a _b _c
    read -r _a _b _c <<< "$1"
    echo $((10#$_a * 1000000 + 10#$_b * 1000 + 10#$_c))
}

echo "$NEW_VER" | grep -qE "^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$"
[ $? -ne 0 ] && echo -e "\nError: invalid version '$NEW_VER', expected X.Y.Z (no leading 'v', no leading zeros)\n" && exit 1

IFS=. read -r NEW_MAJOR NEW_MINOR NEW_PATCH <<< "$NEW_VER"
for n in "$NEW_MAJOR" "$NEW_MINOR" "$NEW_PATCH"; do
    [ "$n" -gt 255 ] && echo -e "\nError: version component '$n' out of range, each part must be 0-255\n" && exit 1
done

for f in "$README_CN" "$README_EN" "$VERSION_TXT" "$VERSION_H" "$CONANFILE" "$CHANGELOG"; do
    [ -f "$f" ] && continue
    echo -e "\nError: file not found: $f\n" && exit 1
done

CUR_VER=$(tr -d '[:space:]' < "$VERSION_TXT")
echo "$CUR_VER" | grep -qE "^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$"
[ $? -ne 0 ] && echo -e "\nError: malformed current version '$CUR_VER' in $VERSION_TXT\n" && exit 1

CUR_NUM=$(_ver_to_num "$CUR_VER")
NEW_NUM=$(_ver_to_num "$NEW_VER")

if [ "$NEW_NUM" -le "$CUR_NUM" ]; then
    echo -e "\n=== WARNING ===\n"
    [ "$NEW_NUM" -eq "$CUR_NUM" ] && echo "new version $NEW_VER is the same as current version $CUR_VER"
    [ "$NEW_NUM" -lt "$CUR_NUM" ] && echo "new version $NEW_VER is lower than current version $CUR_VER (downgrade)"
    read -r -p "continue anyway? [y/N] " ans
    case "$ans" in
        [yY] | [yY][eE][sS]) ;;
        *) echo -e "\nAborted, no files modified.\n" && exit 1 ;;
    esac
fi

TODAY=$(date +%Y/%m/%d)

echo -e "\n=== UPDATE VERSION ($CUR_VER -> $NEW_VER) ===\n"

for r in "$README_CN" "$README_EN"; do
    sed -i -E "s#(badge/version-v)[0-9]+\.[0-9]+\.[0-9]+(-[a-z]+\.svg)#\1${NEW_VER}\2#g" "$r"
    grep -qE "badge/version-v${NEW_VER//./\\.}-[a-z]+\.svg" "$r"
    [ $? -ne 0 ] && echo -e "\nError: failed to update badge in $r\n" && exit 1
    echo "  $r"
done

printf '%s\n' "$NEW_VER" > "$VERSION_TXT"
[ "$(tr -d '[:space:]' < "$VERSION_TXT")" = "$NEW_VER" ]
[ $? -ne 0 ] && echo -e "\nError: failed to update $VERSION_TXT\n" && exit 1
echo "  $VERSION_TXT"

sed -i -E \
    -e "s/(#define[[:space:]]+VLINK_VERSION_MAJOR[[:space:]]+)[0-9]+/\1${NEW_MAJOR}/" \
    -e "s/(#define[[:space:]]+VLINK_VERSION_MINOR[[:space:]]+)[0-9]+/\1${NEW_MINOR}/" \
    -e "s/(#define[[:space:]]+VLINK_VERSION_PATCH[[:space:]]+)[0-9]+/\1${NEW_PATCH}/" \
    -e "s/(#define[[:space:]]+VLINK_VERSION[[:space:]]+)\"[0-9]+\.[0-9]+\.[0-9]+\"/\1\"${NEW_VER}\"/" \
    "$VERSION_H"
grep -qF "#define VLINK_VERSION \"${NEW_VER}\"" "$VERSION_H"
[ $? -ne 0 ] && echo -e "\nError: failed to update $VERSION_H\n" && exit 1
echo "  $VERSION_H"

sed -i -E "s/^([[:space:]]*version[[:space:]]*=[[:space:]]*)\"[0-9]+\.[0-9]+\.[0-9]+\"/\1\"${NEW_VER}\"/" "$CONANFILE"
grep -qF "version = \"${NEW_VER}\"" "$CONANFILE"
[ $? -ne 0 ] && echo -e "\nError: failed to update $CONANFILE\n" && exit 1
echo "  $CONANFILE"

if [ -f "$VCPKG_JSON" ]; then
    sed -i -E "s/^([[:space:]]*\"version\"[[:space:]]*:[[:space:]]*)\"[0-9]+\.[0-9]+\.[0-9]+\"/\1\"${NEW_VER}\"/" "$VCPKG_JSON"
    grep -qF "\"version\": \"${NEW_VER}\"" "$VCPKG_JSON"
    [ $? -ne 0 ] && echo -e "\nError: failed to update $VCPKG_JSON\n" && exit 1
    echo "  $VCPKG_JSON"
else
    echo "  skip  $VCPKG_JSON (not present)"
fi

if grep -qE "^## v${NEW_VER//./\\.}( |\$)" "$CHANGELOG"; then
    echo "  skip  $CHANGELOG (v$NEW_VER already present)"
else
    if [ -s "$CHANGELOG" ]; then
        [ -n "$(tail -c 1 "$CHANGELOG")" ] && printf '\n' >> "$CHANGELOG"
        printf '\n## v%s (%s)\n' "$NEW_VER" "$TODAY" >> "$CHANGELOG"
    else
        printf '## v%s (%s)\n' "$NEW_VER" "$TODAY" >> "$CHANGELOG"
    fi
    echo "  $CHANGELOG"
fi

echo -e "\nVersion updated: $CUR_VER -> $NEW_VER\n"
