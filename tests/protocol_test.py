#!/usr/bin/env python3
"""protocol_test.py - zfs-gui 守护进程协议自检

用法:
    python3 tests/protocol_test.py [socket路径]

覆盖:
    * 只读操作（池、数据集、快照、启动环境、磁盘清单）
    * 危险操作的二次认证（无密码 / 错误密码）
    * 未知操作与非法参数的拒绝
"""

import json
import os
import socket
import struct
import sys

TIMEOUT = 20
results = []


def call(sock_path, op, args=None, auth=None):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(TIMEOUT)
    s.connect(sock_path)

    req = {"v": 1, "op": op, "args": args or {}}
    if auth is not None:
        req["auth"] = auth

    payload = json.dumps(req).encode()
    s.sendall(struct.pack(">I", len(payload)) + payload)

    header = b""
    while len(header) < 4:
        chunk = s.recv(4 - len(header))
        if not chunk:
            raise RuntimeError("连接被关闭")
        header += chunk

    length = struct.unpack(">I", header)[0]
    body = b""
    while len(body) < length:
        chunk = s.recv(length - len(body))
        if not chunk:
            raise RuntimeError("响应不完整")
        body += chunk

    s.close()
    return json.loads(body)


def check(name, condition, detail=""):
    results.append((name, bool(condition), detail))
    mark = "PASS" if condition else "FAIL"
    print("[%s] %s%s" % (mark, name, ("  " + detail) if detail and not condition else ""))


def main():
    sock_path = sys.argv[1] if len(sys.argv) > 1 else os.environ.get(
        "ZFS_GUI_SOCKET", "/var/run/zfs-gui/zfs-gui.sock")

    if not os.path.exists(sock_path):
        print("找不到 socket：%s" % sock_path)
        return 2

    print("测试目标：%s\n" % sock_path)

    # ---------------- 系统信息 ----------------
    info = call(sock_path, "system.info")
    check("system.info 返回成功", info.get("ok") is True)
    data = info.get("data") or {}
    check("system.info 含 euid", "euid" in data, str(data)[:120])
    check("system.info 含 auth_mode", data.get("auth_mode") in ("dangerous", "all", "none"),
          str(data.get("auth_mode")))
    euid = data.get("euid", -1)

    # ---------------- 只读操作 ----------------
    pools = call(sock_path, "zpool.list", {"flags": ["-H", "-p"]})
    check("zpool.list 成功", pools.get("ok") is True, pools.get("message", ""))
    pool_names = [line.split("\t")[0]
                  for line in (pools.get("stdout") or "").splitlines() if line]
    check("至少有一个存储池", len(pool_names) > 0)

    status = call(sock_path, "zpool.status", {"flags": ["-v"]})
    check("zpool.status 成功", status.get("ok") is True, status.get("message", ""))

    if pool_names:
        pool = pool_names[0]
        props = call(sock_path, "zpool.get",
                     {"flags": ["-H", "-p"], "positional": ["all", pool]})
        check("zpool.get 成功", props.get("ok") is True, props.get("message", ""))

        history = call(sock_path, "zpool.history", {"positional": [pool]})
        check("zpool.history 有明确结果",
              history.get("ok") is True or history.get("code") is not None,
              history.get("message", ""))

    datasets = call(sock_path, "zfs.list",
                    {"flags": ["-H", "-p"],
                     "options": [{"key": "o", "value": "name,used,avail,mountpoint"},
                                 {"key": "t", "value": "filesystem"}]})
    check("zfs.list 成功", datasets.get("ok") is True, datasets.get("message", ""))
    ds_names = [line.split("\t")[0]
                for line in (datasets.get("stdout") or "").splitlines() if line]
    check("至少有一个数据集", len(ds_names) > 0)

    snaps = call(sock_path, "zfs.list",
                 {"flags": ["-H", "-p"],
                  "options": [{"key": "o", "value": "name,used,refer,creation"},
                              {"key": "t", "value": "snapshot"}]})
    check("zfs.list 快照成功", snaps.get("ok") is True, snaps.get("message", ""))
    snap_names = [line.split("\t")[0]
                  for line in (snaps.get("stdout") or "").splitlines() if line]

    if snap_names:
        holds = call(sock_path, "zfs.holds",
                     {"flags": ["-H"], "positional": [snap_names[0]]})
        check("zfs.holds 成功", holds.get("ok") is True, holds.get("message", ""))

        diff = call(sock_path, "zfs.diff",
                    {"flags": ["-H"], "positional": [snap_names[0]]})
        check("zfs.diff 有明确结果",
              diff.get("ok") is True or diff.get("code") is not None,
              diff.get("message", ""))

    be = call(sock_path, "bectl.list", {"flags": ["-H"]})
    check("bectl.list 成功", be.get("ok") is True, be.get("message", ""))

    disks = call(sock_path, "system.disks")
    check("system.disks 成功", disks.get("ok") is True, disks.get("message", ""))
    check("system.disks 能列出磁盘", "Geom name:" in (disks.get("stdout") or ""))

    scan = call(sock_path, "zpool.import", {"flags": ["-n"]})
    check("zpool.import 扫描有明确结果",
          scan.get("ok") is True or scan.get("code") is not None,
          scan.get("message", ""))

    # ---------------- 参数校验 ----------------
    bad = call(sock_path, "zpool.list", {"flags": ["; rm -rf /"]})
    check("非法参数被拒绝", bad.get("ok") is not True, "code=%s" % bad.get("code"))

    unknown = call(sock_path, "zpool.no-such-op")
    check("未知操作被拒绝", unknown.get("ok") is not True, "code=%s" % unknown.get("code"))

    # ---------------- 二次认证 ----------------
    auth_mode = data.get("auth_mode")
    if auth_mode == "none":
        print("\n[auth_mode=none] 跳过密码校验测试")
    else:
        no_password = call(sock_path, "zpool.destroy",
                           {"positional": ["zfs-gui-selftest-nonexistent"]})
        check("危险操作无密码被拒绝", no_password.get("code") == -10,
              "code=%s" % no_password.get("code"))

        wrong = call(sock_path, "zpool.destroy",
                     {"positional": ["zfs-gui-selftest-nonexistent"]},
                     {"user": "root", "password": "definitely-wrong-password"})
        expected = {-11, -13}
        check("错误密码被拒绝", wrong.get("code") in expected,
              "code=%s msg=%s" % (wrong.get("code"), wrong.get("message", "")[:60]))

        if wrong.get("code") == -13:
            print("      提示：守护进程未以 root 运行，密码无法验证（-13），属预期")

    # ---------------- 汇总 ----------------
    failed = [r for r in results if not r[1]]
    print("\n共 %d 项，通过 %d，失败 %d" %
          (len(results), len(results) - len(failed), len(failed)))

    if failed:
        for name, _, detail in failed:
            print("  失败: %s %s" % (name, detail))
        return 1

    print("全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
