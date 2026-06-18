#!/usr/bin/env bash

SHELL_DIR=$(cd "$(dirname "${BASH_SOURCE:-$0}")" && pwd)

([ -z "$1" ] || [ "$1" = "-h" ] || [ "$1" = "--help" ]) && echo -e "Usage: \n  check.sh {project dir}" && exit 0

command -v cpplint &> /dev/null || pip install cpplint --user

SRCS=$(find "$1" -type f -not -path "*/thirdparty/*" -not -path "*/build/*" -not -path "*/build-*" -not -path "*/builtin/*" -not -path "*/prebuilt/*" -not -path "*/android-bp/*" \
    \( -name "*.h" -o -name "*.hpp" -o -name "*.cc" -o -name "*.cpp" \))

FILTER=\
-build/c++17,\
-build/include,\
-build/include_order,\
-build/include_subdir,\
-runtime/references,\
-readability/check,\
-readability/braces,\
-whitespace/indent_namespace

echo -e "\n=== CPPLINT ===\n"
cpplint --counting=detailed --linelength=120 --filter=$FILTER $SRCS
([ $? -ne 0 ] || [ -z "$SRCS" ]) && exit 1

ROWS=$(wc -l $SRCS | tail -n 1 | awk '{print $1}')
[ $? -ne 0 ] && exit 1
echo -e "\nTotal lines of code: $ROWS\n"
