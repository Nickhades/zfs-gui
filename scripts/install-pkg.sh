#!/bin/sh
#
# install-pkg.sh - ZFS-GUI 一键安装（FreeBSD pkg 包）
#
# 用法：
#   sh scripts/install-pkg.sh              # 安装 dist/ 中最新包
#   sh scripts/install-pkg.sh --rebuild    # 先重新打包，再安装
#   sh scripts/install-pkg.sh --dry-run    # 只打印将执行的操作
#   sh scripts/install-pkg.sh --uninstall  # 卸载并停止服务
#
# 需要 root；脚本会自动通过 doas/sudo 提权。
#
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DIST="$ROOT/dist"
PKG_BIN=/usr/sbin/pkg
DO_REBUILD=0
DRY_RUN=0
DO_UNINSTALL=0
ROOT_INSTALL=0
PKG_FILE=""

say() { printf '%s\n' "$*"; }
die() { printf '错误: %s\n' "$*" >&2; exit 1; }

# 内部提权分支：只做安装，不再构建
if [ "${1:-}" = "--install-root" ]; then
    ROOT_INSTALL=1
    PKG_FILE=${2:-}
    shift 2 2>/dev/null || shift $#
fi

for arg in "$@"; do
    case "$arg" in
    --rebuild) DO_REBUILD=1 ;;
    --dry-run) DRY_RUN=1 ;;
    --uninstall) DO_UNINSTALL=1 ;;
    -h | --help)
        sed -n '2,14p' "$0"
        exit 0
        ;;
    *) die "未知参数：$arg" ;;
    esac
done

[ "$(uname -s)" = "FreeBSD" ] || die "此脚本仅支持 FreeBSD"
[ -x "$PKG_BIN" ] || die "找不到 $PKG_BIN"

need_root() {
    if [ "$(id -u)" -eq 0 ]; then
        return 0
    fi

    if command -v doas >/dev/null 2>&1; then
        exec doas sh "$0" "$@"
    elif command -v sudo >/dev/null 2>&1; then
        exec sudo sh "$0" "$@"
    fi

    die "需要 root 权限，请使用：su -c 'sh $0'"
}

find_pkg() {
    ls -t "$DIST"/zfs-gui-*.pkg 2>/dev/null | head -1
}

install_now() {
    [ -n "$PKG_FILE" ] || die "没有可安装的 pkg 文件"
    [ -f "$PKG_FILE" ] || die "安装包不存在：$PKG_FILE"

    say "==> 安装运行依赖（gtk4 / glib）"
    ASSUME_ALWAYS_YES=yes "$PKG_BIN" install -y gtk4 glib >/dev/null

    say "==> 安装 $PKG_FILE"
    ASSUME_ALWAYS_YES=yes "$PKG_BIN" install -y "$PKG_FILE"

    say "==> 启用并启动后台服务"
    sysrc zfsguid_enable=YES >/dev/null
    service zfsguid stop >/dev/null 2>&1 || true
    rm -f /var/run/zfs-gui.pid
    rm -f /var/run/zfs-gui/zfs-gui.sock
    service zfsguid start >/dev/null 2>&1 || true

    i=0
    while [ ! -S /var/run/zfs-gui/zfs-gui.sock ]; do
        i=$((i + 1))
        if [ "$i" -gt 50 ]; then
            say "警告: 后台服务未在 5 秒内创建 socket"
            say "  service zfsguid status"
            say "  tail -n 30 /var/log/zfs-guid.log"
            break
        fi
        sleep 0.1
    done

    say ""
    say "安装完成"
    say "  程序：zfs-gui"
    say "  服务：service zfsguid status"
    say "  配置：/usr/local/etc/zfs-gui.conf"
    say "  卸载：sh $ROOT/scripts/install-pkg.sh --uninstall"
}

# 提权后的安装路径
if [ "$ROOT_INSTALL" -eq 1 ]; then
    install_now
    exit 0
fi

# 卸载
if [ "$DO_UNINSTALL" -eq 1 ]; then
    need_root "$@"

    say "==> 停止并禁用 zfsguid 服务"
    service zfsguid stop >/dev/null 2>&1 || true
    sysrc -x zfsguid_enable >/dev/null 2>&1 || true

    say "==> 卸载 zfs-gui"
    if [ "$DRY_RUN" -eq 1 ]; then
        say "    pkg delete -y zfs-gui"
    else
        "$PKG_BIN" delete -y zfs-gui || true
    fi

    say "卸载完成（/usr/local/etc/zfs-gui.conf 由 pkg 决定是否保留）"
    exit 0
fi

# 构建或定位安装包
if [ "$DO_REBUILD" -eq 1 ] || [ -z "$(find_pkg || true)" ]; then
    say "==> 构建 pkg 包"
    if [ "$DRY_RUN" -eq 1 ]; then
        say "    sh $ROOT/packaging/build-pkg.sh"
    else
        sh "$ROOT/packaging/build-pkg.sh"
    fi
fi

PKG_FILE=$(find_pkg || true)

if [ "$DRY_RUN" -eq 1 ]; then
    say "==> 安装包：${PKG_FILE:-（待构建）}"
    say "==> 将执行："
    say "    pkg install -y ${PKG_FILE:-dist/zfs-gui-*.pkg}"
    say "    sysrc zfsguid_enable=YES"
    say "    service zfsguid restart"
    say "    service zfsguid status"
    exit 0
fi

[ -n "$PKG_FILE" ] || die "打包失败：dist/ 中没有 zfs-gui-*.pkg"

# 非 root：只提权做安装
if [ "$(id -u)" -ne 0 ]; then
    if command -v doas >/dev/null 2>&1; then
        exec doas sh "$0" --install-root "$PKG_FILE"
    elif command -v sudo >/dev/null 2>&1; then
        exec sudo sh "$0" --install-root "$PKG_FILE"
    fi
    die "需要 root 权限安装"
fi

install_now
