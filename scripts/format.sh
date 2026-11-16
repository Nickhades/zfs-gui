#!/bin/sh
# format.sh - 按 .clang-format 统一格式化全部 C 源码
set -eu
cd "$(dirname "$0")/.."
command -v clang-format >/dev/null 2>&1 || {
    echo "缺少 clang-format（pkg install llvm）" >&2
    exit 1
}
clang-format -i common/*.c common/*.h daemon/*.c daemon/*.h src/*.c src/*.h
echo "格式化完成"
