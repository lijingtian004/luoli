#!/bin/bash
# MCP Streamable HTTP 冒烟测试
# 用法: ./smoke_test_mcp.sh [url] [token]
# 示例: ./smoke_test_mcp.sh http://127.0.0.1:8631/mcp abcd1234...
set -euo pipefail

URL="${1:-http://127.0.0.1:8631/mcp}"
TOKEN="${2:-}"

# 从响应中提取 JSON（兼容纯 JSON 与 SSE data: 行两种编码）
extract() {
    python3 -c "
import sys, json
raw = sys.stdin.read()
data = None
for line in raw.splitlines():
    if line.startswith('data:'):
        try: data = json.loads(line[5:].strip()); break
        except Exception: pass
if data is None:
    try: data = json.loads(raw)
    except Exception: sys.exit('no json in response')
print(json.dumps(data, ensure_ascii=False))
"
}

req() {
    local payload="$1"
    local extra=()
    [ -n "${SESSION:-}" ] && extra=(-H "Mcp-Session-Id: $SESSION")
    [ -n "$TOKEN" ] && extra+=(-H "Authorization: Bearer $TOKEN")
    curl -s --max-time 30 "$URL" \
        -H "Content-Type: application/json" \
        -H "Accept: application/json, text/event-stream" \
        "${extra[@]}" \
        -d "$payload"
}

echo "== 1. initialize =="
INIT_RESP=$(req '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"smoke","version":"1.0"}}}')
echo "$INIT_RESP" | extract
# 会话 id 从响应头拿（重新请求一次以捕获 header）
SESSION=$(curl -s -o /dev/null -D - --max-time 30 "$URL" \
    -H "Content-Type: application/json" \
    -H "Accept: application/json, text/event-stream" \
    ${TOKEN:+-H "Authorization: Bearer $TOKEN"} \
    -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"smoke","version":"1.0"}}}' \
    | grep -i '^mcp-session-id:' | tr -d '\r' | awk '{print $2}')
echo "session: ${SESSION:-<none>}"

echo "== 2. notifications/initialized =="
req '{"jsonrpc":"2.0","method":"notifications/initialized"}' > /dev/null && echo ok

echo "== 3. tools/list =="
req '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' | extract

echo "== 4. tools/call driver_status =="
req '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"driver_status","arguments":{}}}' | extract

echo "== 5. tools/call get_status =="
req '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"get_status","arguments":{}}}' | extract

echo "== PASS =="
