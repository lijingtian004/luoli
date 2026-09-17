#!/usr/bin/env python3
"""直接与 root daemon 的 abstract socket 通信（诊断用）
用法: dcmd.py <cmd> [json_params]
"""
import json
import socket
import struct
import sys

SOCK = "\0twt_svc_2778"


def request(cmd, params=None, timeout=15):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(SOCK)
    payload = {"cmd": cmd}
    if params:
        payload.update(params)
    data = json.dumps(payload).encode()
    s.sendall(struct.pack("<I", len(data)) + data)
    hdr = b""
    while len(hdr) < 4:
        chunk = s.recv(4 - len(hdr))
        if not chunk:
            raise EOFError("closed")
        hdr += chunk
    (n,) = struct.unpack("<I", hdr)
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise EOFError("closed mid-frame")
        buf += chunk
    s.close()
    return json.loads(buf)


if __name__ == "__main__":
    cmd = sys.argv[1]
    params = json.loads(sys.argv[2]) if len(sys.argv) > 2 else None
    print(json.dumps(request(cmd, params), ensure_ascii=False, indent=1))
