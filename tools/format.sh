#!/usr/bin/env bash

([ -z "$1" ] || [ "$1" = "-h" ] || [ "$1" = "--help" ]) && echo -e "Usage: \n  format.sh {project dir}" && exit 0

command -v clang-format &> /dev/null || pip install clang-format --user
command -v cmake-format &> /dev/null || pip install cmake-format --user

SRCS=$(find "$1" -type f -not -path "*/thirdparty/*" -not -path "*/build/*" -not -path "*/build-*" -not -path "*/builtin/*" -not -path "*/prebuilt/*" -not -path "*/android-bp/*" \
    \( -name "*.h" -o -name "*.hpp" -o -name "*.cc" -o -name "*.cpp" \))

CONFS=$(find "$1" -type f -not -path "*/thirdparty/*" -not -path "*/build/*" -not -path "*/build-*" -not -path "*/builtin/*" -not -path "*/prebuilt/*" -not -path "*/android-bp/*" \
    \( -name "CMakeLists.txt" -o -name "*.cmake" -o -name "*.cmake.in" \))

echo -e "\n=== CLANG-FORMAT ===\n"
clang-format -i $SRCS
([ $? -ne 0 ] || [ -z "$SRCS" ]) && exit 1

echo -e "\n=== CMAKE-FORMAT ===\n"
cmake-format -i $CONFS
([ $? -ne 0 ] || [ -z "$CONFS" ]) && exit 1

cd "$1" && git diff --quiet
[ $? -eq 0 ] && exit 0

echo -e "\nFiles have changed.\n"

exit 1
