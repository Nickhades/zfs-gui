#!/bin/sh
#
# check-strict-c.sh - 严格 C17 规范检查
#
# 使用 -std=c17 -pedantic-errors 编译全部自有源码：
#   · 任何 GNU 扩展（语句表达式、typeof、零长数组等）都会直接报错
#   · 隐式函数声明、指针类型不兼容、return 类型不匹配均为错误
#   · GLib / GTK 头文件通过 -isystem 引入，不参与本项目的规范判定
#
# 用法：sh scripts/check-strict-c.sh
#

set -e

cd "$(dirname "$0")/.."

PKG_CFLAGS=$(pkg-config --cflags glib-2.0 gtk4 2>/dev/null || pkg-config --cflags glib-2.0)
SYS_CFLAGS=$(echo "$PKG_CFLAGS" | sed 's/-I\([^ ]*\)/-isystem \1/g')

CC=${CC:-cc}

echo "[strict-c] 使用 $CC，标准 -std=c17 -pedantic-errors"
echo "[strict-c] 检查 common / daemon / src ..."

$CC -std=c17 -pedantic-errors \
    -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
    -Wpointer-arith -Wwrite-strings -Wformat=2 \
    -Wno-unused-parameter -Wno-deprecated-declarations \
    -Icommon -Isrc $SYS_CFLAGS \
    -fsyntax-only \
    common/*.c daemon/*.c src/*.c

echo "[strict-c] 通过：未发现非标准 C 用法"
