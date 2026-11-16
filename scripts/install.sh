#!/bin/sh
#
# install.sh - ZFS 管理工具一键安装
#
# 用法（普通用户直接运行，安装阶段会提示输入密码）：
#   sh scripts/install.sh
#
# 其他用法：
#   sh scripts/install.sh --no-service          # 只安装，不启动服务
#   sh scripts/install.sh --destdir /tmp/stage  # 暂存到目录（不需要 root）
#   sudo sh scripts/install.sh --uninstall      # 卸载（--purge 同时删配置）
#
set -eu

cd "$(dirname "$0")/.."
ROOT=$(pwd)
BUILD=${BUILD:-build}
PREFIX=${PREFIX:-/usr/local}
DESTDIR=""
DO_SERVICE=1
DO_UNINSTALL=0
PURGE_CONFIG=0
INSTALL_ONLY=0

usage() {
    sed -n '2,16p' "$0"
    exit 0
}

while [ $# -gt 0 ]; do
    case "$1" in
    --prefix) PREFIX=$2; shift 2 ;;
    --prefix=*) PREFIX=${1#*=}; shift ;;
    --destdir) DESTDIR=$2; shift 2 ;;
    --destdir=*) DESTDIR=${1#*=}; shift ;;
    --no-service) DO_SERVICE=0; shift ;;
    --uninstall) DO_UNINSTALL=1; shift ;;
    --purge) PURGE_CONFIG=1; shift ;;
    --install-only) INSTALL_ONLY=1; shift ;;
    -h | --help) usage ;;
    *)
        echo "未知参数: $1" >&2
        exit 2
        ;;
    esac
done

say() { printf '%s\n' "$*"; }
die() {
    printf '错误: %s\n' "$*" >&2
    exit 1
}

# 需要 root 时自动提权重跑本脚本（sudo 或 doas）
reexec_as_root() {
    if [ "$(id -u)" -eq 0 ]; then
        return 0
    fi

    if command -v sudo >/dev/null 2>&1; then
        exec sudo sh "$ROOT/scripts/install.sh" "$@"
    elif command -v doas >/dev/null 2>&1; then
        exec doas sh "$ROOT/scripts/install.sh" "$@"
    fi

    die "需要 root 权限，请使用：su -c 'sh $ROOT/scripts/install.sh'"
}

# ---------------------------------------------------------------------------
# 卸载
# ---------------------------------------------------------------------------
if [ "$DO_UNINSTALL" -eq 1 ]; then
    if [ -z "$DESTDIR" ] && [ "$(id -u)" -ne 0 ]; then
        if [ "$PURGE_CONFIG" -eq 1 ]; then
            reexec_as_root --uninstall --prefix "$PREFIX" --purge
        else
            reexec_as_root --uninstall --prefix "$PREFIX"
        fi
    fi

    say "==> 停止并禁用服务"
    if [ -z "$DESTDIR" ]; then
        service zfsguid stop 2>/dev/null || true
        sysrc -x zfsguid_enable 2>/dev/null || true
    fi

    say "==> 删除程序与资源"
    for f in \
        "$PREFIX/sbin/zfs-guid" \
        "$PREFIX/bin/zfs-gui" \
        "$PREFIX/share/applications/org.freebsd.zfsgui.desktop" \
        "$PREFIX/share/icons/hicolor/scalable/apps/zfs-gui.svg" \
        "$PREFIX/etc/rc.d/zfsguid" \
        "$PREFIX/share/man/man8/zfs-guid.8" \
        "/etc/pam.d/zfs-gui"; do
        rm -f "$DESTDIR$f"
    done

    if [ "$PURGE_CONFIG" -eq 1 ]; then
        rm -f "$DESTDIR$PREFIX/etc/zfs-gui.conf"
    fi

    if [ -z "$DESTDIR" ]; then
        rm -rf /var/run/zfs-gui
        if command -v update-desktop-database >/dev/null 2>&1; then
            update-desktop-database "$PREFIX/share/applications" >/dev/null 2>&1 || true
        fi
    fi

    if [ "$PURGE_CONFIG" -eq 1 ]; then
        say "卸载完成，配置已删除"
    else
        say "卸载完成（配置文件保留：$PREFIX/etc/zfs-gui.conf）"
    fi
    exit 0
fi

# ---------------------------------------------------------------------------
# 依赖检查
# ---------------------------------------------------------------------------
check_deps() {
    say "==> 检查依赖"
    command -v cc >/dev/null 2>&1 || die "缺少 C 编译器（pkg install clang）"
    command -v meson >/dev/null 2>&1 || die "缺少 meson（pkg install meson）"
    command -v ninja >/dev/null 2>&1 || die "缺少 ninja（pkg install ninja）"
    command -v pkg-config >/dev/null 2>&1 || die "缺少 pkg-config"

    pkg-config --exists glib-2.0 || die "缺少 glib-2.0（pkg install glib）"
    pkg-config --exists gtk4 || die "缺少 gtk4（pkg install gtk4）"

    printf 'int main(void){return 0;}\n' >/tmp/zg-pam-test.c
    if ! ${CC:-cc} /tmp/zg-pam-test.c -lpam -o /tmp/zg-pam-test >/dev/null 2>&1; then
        rm -f /tmp/zg-pam-test.c /tmp/zg-pam-test
        die "缺少 PAM 开发库"
    fi
    rm -f /tmp/zg-pam-test.c /tmp/zg-pam-test
}

# ---------------------------------------------------------------------------
# 编译（始终以调用者身份进行，避免 build/ 被 root 拥有）
# ---------------------------------------------------------------------------
build_project() {
    say "==> 编译（前缀 $PREFIX）"

    if [ "$(id -u)" -eq 0 ] && [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != "root" ]; then
        say "    以 $SUDO_USER 身份编译"
        su -m "$SUDO_USER" -c "cd '$ROOT' && meson setup --reconfigure --prefix='$PREFIX' '$BUILD' >/dev/null && ninja -C '$BUILD'" ||
            die "编译失败"
        return
    fi

    if [ -d "$BUILD" ]; then
        meson setup --reconfigure --prefix="$PREFIX" "$BUILD" >/dev/null
    else
        meson setup --prefix="$PREFIX" "$BUILD" >/dev/null
    fi
    ninja -C "$BUILD" || die "编译失败"
}

if [ "$INSTALL_ONLY" -eq 0 ]; then
    check_deps
    build_project
fi

# 普通用户：编译完成后自动提权做安装
if [ -z "$DESTDIR" ] && [ "$(id -u)" -ne 0 ]; then
    say "==> 需要管理员权限安装到系统"
    if [ "$DO_SERVICE" -eq 1 ]; then
        reexec_as_root --install-only --prefix "$PREFIX"
    else
        reexec_as_root --install-only --prefix "$PREFIX" --no-service
    fi
fi

# ---------------------------------------------------------------------------
# 安装
# ---------------------------------------------------------------------------
say "==> 安装文件"
# 用 meson install --no-rebuild：只复制文件，不写 ninja 日志，
# 避免以 root 安装后 build/ 里的文件属主变成 root。
if [ -n "$DESTDIR" ]; then
    mkdir -p "$DESTDIR"
    DESTDIR="$DESTDIR" meson install -C "$BUILD" --no-rebuild
else
    meson install -C "$BUILD" --no-rebuild
fi

INSTALL_ROOT="$DESTDIR"

say ""
say "已安装："
for f in \
    "$PREFIX/sbin/zfs-guid" \
    "$PREFIX/bin/zfs-gui" \
    "$PREFIX/share/applications/org.freebsd.zfsgui.desktop" \
    "$PREFIX/share/icons/hicolor/scalable/apps/zfs-gui.svg" \
    "$PREFIX/etc/rc.d/zfsguid" \
    "$PREFIX/etc/zfs-gui.conf" \
    "/etc/pam.d/zfs-gui"; do
    if [ -e "$INSTALL_ROOT$f" ]; then
        say "  $INSTALL_ROOT$f"
    fi
done

if [ -n "$DESTDIR" ]; then
    say ""
    say "暂存安装完成（未启用服务）。部署到系统请执行："
    say "  sh $ROOT/scripts/install.sh"
    exit 0
fi

# ---------------------------------------------------------------------------
# 桌面集成
# ---------------------------------------------------------------------------
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$PREFIX/share/applications" >/dev/null 2>&1 || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t -f "$PREFIX/share/icons/hicolor" >/dev/null 2>&1 || true
fi

# ---------------------------------------------------------------------------
# 服务
# ---------------------------------------------------------------------------
if [ "$DO_SERVICE" -eq 1 ]; then
    say "==> 启用并启动后台服务"
    sysrc zfsguid_enable=YES >/dev/null
    service zfsguid restart >/dev/null 2>&1 || service zfsguid start >/dev/null 2>&1 || true

    i=0
    while [ ! -S /var/run/zfs-gui/zfs-gui.sock ]; do
        i=$((i + 1))
        if [ "$i" -gt 50 ]; then
            say "警告: 后台服务未能在 5 秒内创建 socket，请检查："
            say "  service zfsguid status"
            say "  tail -n 30 /var/log/zfs-guid.log"
            break
        fi
        sleep 0.1
    done

    if [ -S /var/run/zfs-gui/zfs-gui.sock ]; then
        say "后台服务已就绪：/var/run/zfs-gui/zfs-gui.sock"
    fi
fi

say ""
say "安装完成。"
say "  · 应用菜单：系统 → ZFS 管理工具（或搜索 “ZFS”）"
say "  · 命令行：zfs-gui"
say "  · 服务管理：service zfsguid status|restart"
say "  · 配置文件：$PREFIX/etc/zfs-gui.conf"
say "  · 自检：sh $ROOT/scripts/selftest.sh"
