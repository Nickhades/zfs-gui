#!/bin/sh
# build-pkg.sh - 生成 FreeBSD .pkg 安装包
#
# 用法：sh packaging/build-pkg.sh [--no-build]
# 产物：dist/zfs-gui-<版本>.pkg
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

PKG=/usr/sbin/pkg
BUILD_DIR="$ROOT/build"
STAGE="$BUILD_DIR/pkgstage"
META="$BUILD_DIR/pkgmeta"
DIST="$ROOT/dist"
DO_BUILD=1

for arg in "$@"; do
    case "$arg" in
        --no-build) DO_BUILD=0 ;;
        *) echo "未知参数：$arg" >&2; exit 1 ;;
    esac
done

if [ ! -x "$PKG" ]; then
    echo "找不到 $PKG（FreeBSD 包管理器）" >&2
    exit 1
fi

VERSION=$(sed -n "s/^[[:space:]]*version:[[:space:]]*'\([^']*\)'.*/\1/p" meson.build | head -1)
if [ -z "$VERSION" ]; then
    echo "无法从 meson.build 读取版本号" >&2
    exit 1
fi

ABI=$("$PKG" config abi 2>/dev/null || echo "FreeBSD:$(freebsd-version -u | cut -d. -f1):amd64")
GTK4_VERSION=$("$PKG" query '%v' gtk4 2>/dev/null || echo "0")
GLIB_VERSION=$("$PKG" query '%v' glib 2>/dev/null || echo "0")

if [ "$DO_BUILD" -eq 1 ]; then
    if [ ! -f "$BUILD_DIR/build.ninja" ]; then
        echo "未找到构建目录，正在配置…"
        meson setup "$BUILD_DIR"
    fi
    ninja -C "$BUILD_DIR"
fi

if [ ! -f "$BUILD_DIR/build.ninja" ]; then
    echo "构建目录不存在，请先运行 meson setup build" >&2
    exit 1
fi

echo "==> 安装到暂存目录 $STAGE"
rm -rf "$STAGE"
DESTDIR="$STAGE" meson install -C "$BUILD_DIR" --no-rebuild >/dev/null

echo "==> 生成元数据"
rm -rf "$META"
mkdir -p "$META"
sed -e "s|@VERSION@|$VERSION|g" \
    -e "s|@ABI@|$ABI|g" \
    -e "s|@GTK4_VERSION@|$GTK4_VERSION|g" \
    -e "s|@GLIB_VERSION@|$GLIB_VERSION|g" \
    packaging/+MANIFEST.in > "$META/+MANIFEST"
cp packaging/+POST_INSTALL "$META/+POST_INSTALL"
cp packaging/+PRE_DEINSTALL "$META/+PRE_DEINSTALL"
chmod 755 "$META/+POST_INSTALL" "$META/+PRE_DEINSTALL"

PLIST="$META/plist"
( cd "$STAGE" && find . \( -type f -o -type l \) -print | sed 's|^\.||' | sort ) > "$PLIST"

mkdir -p "$DIST"
rm -f "$DIST/zfs-gui-$VERSION.pkg"

echo "==> 打包"
"$PKG" create -v -r "$STAGE" -m "$META" -p "$PLIST" -o "$DIST"

echo
echo "完成：$DIST/zfs-gui-$VERSION.pkg"
"$PKG" info -l "$DIST/zfs-gui-$VERSION.pkg" 2>/dev/null | head -5 || true
