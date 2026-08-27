#!/usr/bin/env bash

([ -z "$1" ] || [ "$1" = "-h" ] || [ "$1" = "--help" ]) && echo -e "Usage: \n  format.sh {project dir}" && exit 0

command -v clang-format &> /dev/null || pip install clang-format --user
command -v cmake-format &> /dev/null || pip install cmake-format --user

VLINK_ROOT=$(cd "$1" && pwd)

PRUNE=(-not -path "$VLINK_ROOT/tools/*" -not -path "*/thirdparty/*" -not -path "*/build/*" -not -path "*/build-*/*" -not -path "*/builtin/*" -not -path "*/prebuilt/*" -not -path "*/android-bp/*")

SRCS=$(find "$VLINK_ROOT" -type f "${PRUNE[@]}" \
    \( -name "*.h" -o -name "*.hpp" -o -name "*.cc" -o -name "*.cpp" \))

CONFS=$(find "$VLINK_ROOT" -type f "${PRUNE[@]}" \
    \( -name "CMakeLists.txt" -o -name "*.cmake" -o -name "*.cmake.in" \))

SCRIPTS=$(find "$VLINK_ROOT" -type f "${PRUNE[@]}" \
    \( -name "*.bat" -o -name "*.bat.in" -o -name "*.cmd" -o -name "*.cmd.in" -o -name "*.ps1" -o -name "*.ps1.in" \
       -o -name "*.sh" -o -name "*.sh.in" -o -name "*.bash" -o -name "*.bash.in" \))

echo -e "\n=== CLANG-FORMAT ===\n"
clang-format -i $SRCS
([ $? -ne 0 ] || [ -z "$SRCS" ]) && exit 1

echo -e "\n=== CMAKE-FORMAT ===\n"
cmake-format -i $CONFS
([ $? -ne 0 ] || [ -z "$CONFS" ]) && exit 1

echo -e "\n=== EOL-FORMAT ===\n"
for f in $SCRIPTS; do
    EOL=$(git -C "$VLINK_ROOT" check-attr eol -- "$f" | sed 's/.*: //')
    case "$EOL" in
        crlf)
            sed -i 's/\r\{0,1\}$/\r/' "$f" && echo "  crlf  $f"
            ;;
        lf)
            sed -i 's/\r$//' "$f" && echo "  lf    $f"
            ;;
        *)
            echo "  skip  $f (eol=$EOL)"
            ;;
    esac
done

cd "$VLINK_ROOT" && git diff --quiet
[ $? -eq 0 ] && exit 0

echo -e "\nFiles have changed.\n"

git --no-pager diff

exit 1
