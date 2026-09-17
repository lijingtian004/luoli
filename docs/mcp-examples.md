# AI Agent 接入萝莉修改器 MCP Server

## 前置步骤（手机端）
1. 打开萝莉修改器 → MCP 页
2. 按需开启「允许局域网连接」（PC 上的 agent 需要；Termux 本机 agent 用 127.0.0.1 即可）
3. 点「启动」并「复制 Token」

## opencode（推荐）

`~/.config/opencode/opencode.json`：

```jsonc
{
  "$schema": "https://opencode.ai/config.json",
  "mcp": {
    "luoli": {
      "type": "remote",
      "url": "http://127.0.0.1:8631/mcp",
      "headers": {
        "Authorization": "Bearer <你的Token>",
        "Mcp-Protocol-Version": "2025-06-18"
      },
      "enabled": true
    }
  }
}
```

- 手机 Termux 里的 opencode：`url` 用 `http://127.0.0.1:8631/mcp`
- PC 上的 opencode：`url` 用 MCP 页显示的局域网地址，如 `http://192.168.1.23:8631/mcp`

## Claude Code

```bash
# Termux 本机
claude mcp add --transport http luoli http://127.0.0.1:8631/mcp \
  --header "Authorization: Bearer <你的Token>"

# PC（局域网）
claude mcp add --transport http luoli http://<手机IP>:8631/mcp \
  --header "Authorization: Bearer <你的Token>"
```

## MCP Inspector（调试）

```bash
npx -y @modelcontextprotocol/inspector
# UI 中选择 Streamable HTTP，填入 url + Authorization header
```

## 工具一览

| 工具 | 说明 |
|------|------|
| `driver_status` | 内核驱动状态与自检 |
| `list_processes` | 可附加的 App 进程列表 |
| `attach` / `attach_by_name` | 按 pid / 按包名附加 |
| `detach` / `get_status` | 脱离 / 总状态 |
| `search` | 初扫（type: i8..f64, value, align, name_filter, max） |
| `filter` | 增量过滤（eq/ne/gt/ge/lt/le/inc/dec/changed/unchanged） |
| `results` | 分页列出当前结果 |
| `read` / `write` | 按地址读写 |
| `freeze` / `unfreeze` / `unfreeze_all` / `frozen_list` | 数值冻结 |
| `list_modules` / `module_base` / `module_bss` | 模块信息（base+offset 工作流） |
| `reset` | 清空扫描结果 |

## 典型 AI 工作流示例

> "帮我把 XX 游戏的金币改成 999999"
>
> 1. `list_processes` → 找到 pid → `attach`
> 2. `search(type="i32", value=当前金币数)`
> 3. 游戏里花掉一些金币 → `filter(op="dec")`
> 4. 再花/得一点 → `filter(op="dec")` 或 `filter(op="eq", value=新值)`
> 5. 结果收敛到几个地址 → `read` 验证 → `write(addr, value=999999)`
> 6. 需要锁定则 `freeze(addr, value=999999)`
