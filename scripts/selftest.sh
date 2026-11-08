#!/bin/sh
#
# selftest.sh - 一键自检
#
# 在临时目录启动一个独立的后台服务实例（不影响系统里正在运行的服务），
# 执行协议测试与可选的界面启动测试，结束后自动清理。
#
# 用法:
#   sh scripts/selftest.sh            # 仅协议测试
#   sh scripts/selftest.sh --gui      # 额外做一次界面启动冒烟测试
#
set -eu

cd "$(dirname "$0")/.."
ROOT=$(pwd)
BUILD=${BUILD:-build}
WITH_GUI=0

for arg in "$@"; do
  case "$arg" in
    --gui) WITH_GUI=1 ;;
    -h|--help)
      sed -n '2,14p' "$0"
      exit 0
      ;;
    *)
      echo "未知参数: $arg" >&2
      exit 2
      ;;
  esac
done

if [ ! -x "$BUILD/zfs-guid" ] || [ ! -x "$BUILD/zfs-gui" ]; then
  echo "[selftest] 编译中..."
  if [ ! -d "$BUILD" ]; then
    meson setup "$BUILD" >/dev/null
  fi
  ninja -C "$BUILD" >/dev/null
fi

TMP=$(mktemp -d /tmp/zfs-gui-selftest.XXXXXX)
SOCK="$TMP/zfs-gui.sock"
PIDFILE="$TMP/zfs-guid.pid"
DAEMON_PID=""

cleanup() {
  if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
    kill "$DAEMON_PID" 2>/dev/null || true
    wait "$DAEMON_PID" 2>/dev/null || true
  fi
  rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

cat > "$TMP/zfs-gui.conf" <<EOF
allowed_users = root
allowed_groups = wheel
socket_path = $SOCK
socket_group = wheel
socket_mode = 0660
io_timeout = 600
auth_mode = dangerous
auth_max_attempts = 3
auth_lockout = 300
EOF

echo "[selftest] 启动测试守护进程（socket: $SOCK）"

if [ "$(id -u)" -ne 0 ]; then
  echo "[selftest] 非 root：使用 ZFS_GUI_ALLOW_NONROOT=1（只读测试 + 认证负向测试）"
  ZFS_GUI_ALLOW_NONROOT=1 "$ROOT/$BUILD/zfs-guid" -f -c "$TMP/zfs-gui.conf" \
    -p "$PIDFILE" >"$TMP/daemon.log" 2>&1 &
else
  "$ROOT/$BUILD/zfs-guid" -f -c "$TMP/zfs-gui.conf" \
    -p "$PIDFILE" >"$TMP/daemon.log" 2>&1 &
fi

DAEMON_PID=$!

i=0
while [ ! -S "$SOCK" ]; do
  i=$((i + 1))
  if [ "$i" -gt 100 ]; then
    echo "[selftest] 守护进程启动失败：" >&2
    cat "$TMP/daemon.log" >&2
    exit 1
  fi
  if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
    echo "[selftest] 守护进程已退出：" >&2
    cat "$TMP/daemon.log" >&2
    exit 1
  fi
  sleep 0.1
done

STATUS=0
python3 tests/protocol_test.py "$SOCK" || STATUS=$?

if [ "$WITH_GUI" -eq 1 ]; then
  if [ -z "${DISPLAY:-}" ]; then
    echo "[selftest] 未设置 DISPLAY，跳过界面启动测试"
  else
    echo "[selftest] 界面启动冒烟测试..."
    ZFS_GUI_SOCKET="$SOCK" timeout 6 "$ROOT/$BUILD/zfs-gui" >"$TMP/gui.log" 2>&1 || true
    if grep -qiE "Segmentation|zg_row_set_text|Gtk-CRITICAL" "$TMP/gui.log"; then
      echo "[selftest] 界面启动日志中发现严重错误：" >&2
      grep -iE "Segmentation|zg_row_set_text|Gtk-CRITICAL" "$TMP/gui.log" >&2
      STATUS=1
    else
      echo "[selftest] 界面启动正常"
    fi
  fi
fi

if [ "$STATUS" -eq 0 ]; then
  echo "[selftest] 自检通过"
else
  echo "[selftest] 自检失败（退出码 $STATUS）" >&2
fi

exit "$STATUS"
