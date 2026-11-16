#!/usr/bin/env python3
"""模拟 GUI：打开备份文件，把 fd 经 SCM_RIGHTS 传给守护进程，执行 zfs.receive。

用法：
  python3 tests/receive_stream_chain.py <socket> <target-dataset> <backup-file>
"""

import array
import json
import os
import socket
import struct
import sys


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__)
        return 2

    sock_path, target, backup = sys.argv[1:]

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(600)
    s.connect(sock_path)

    request = {
        "v": 1,
        "op": "zfs.receive",
        "has_fd": True,
        "args": {"positional": [target], "input": backup},
    }
    payload = json.dumps(request).encode()
    s.sendall(struct.pack(">I", len(payload)) + payload)

    fd = os.open(backup, os.O_RDONLY)
    try:
        s.sendmsg([b"R"], [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                            array.array("i", [fd]))])
    finally:
        os.close(fd)

    length = struct.unpack(">I", s.recv(4))[0]
    body = b""
    while len(body) < length:
        body += s.recv(length - len(body))

    response = json.loads(body)
    print(json.dumps(response, ensure_ascii=False))
    return 0 if response.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
