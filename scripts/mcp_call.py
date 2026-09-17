#!/usr/bin/env python3
"""MCP 单次调用助手: 自动 initialize + tools/call
用法: mcp_call.py <tool> [json_arguments] [url] [token]
"""
import json
import sys
import urllib.request

URL = "http://127.0.0.1:8631/mcp"
TOKEN = ""
SESSION = None


def post(payload: dict, expect_header: bool = False):
    global SESSION
    req = urllib.request.Request(URL, data=json.dumps(payload).encode())
    req.add_header("Content-Type", "application/json")
    req.add_header("Accept", "application/json, text/event-stream")
    if TOKEN:
        req.add_header("Authorization", f"Bearer {TOKEN}")
    if SESSION:
        req.add_header("Mcp-Session-Id", SESSION)
    try:
        resp = urllib.request.urlopen(req, timeout=300)
    except urllib.error.HTTPError as e:
        print(f"HTTP {e.code}: {e.read().decode()[:300]}", file=sys.stderr)
        sys.exit(1)
    sid = resp.headers.get("Mcp-Session-Id")
    if sid:
        SESSION = sid
    raw = resp.read().decode()
    if not raw.strip():
        return None
    for line in raw.splitlines():
        if line.startswith("data:"):
            return json.loads(line[5:])
    return json.loads(raw)


def init():
    post({"jsonrpc": "2.0", "id": 0, "method": "initialize",
          "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                     "clientInfo": {"name": "cli", "version": "1.0"}}})
    post({"jsonrpc": "2.0", "method": "notifications/initialized"})


def call(tool: str, arguments: dict):
    resp = post({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                 "params": {"name": tool, "arguments": arguments}})
    if "error" in resp:
        print("RPC ERROR:", json.dumps(resp["error"], ensure_ascii=False))
        sys.exit(1)
    result = resp["result"]
    text = result["content"][0]["text"]
    if result.get("isError"):
        print("TOOL ERROR:", text)
        sys.exit(1)
    try:
        return json.loads(text)
    except Exception:
        return text


if __name__ == "__main__":
    tool = sys.argv[1]
    args = {}
    for extra in sys.argv[2:]:
        if extra.startswith("http://") or extra.startswith("https://"):
            URL = extra
        elif len(extra) >= 16 and all(c in "0123456789abcdef" for c in extra):
            TOKEN = extra
        else:
            args = json.loads(extra)
    init()
    out = call(tool, args)
    print(json.dumps(out, ensure_ascii=False, indent=1))
