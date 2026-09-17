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
| `get_status` | 查看引擎状态（目标附加状态、应用架构特征、驱动模式、扫描进度等） |
| `app_summary` | 获取目标应用架构特征摘要（主引擎类型、架构位数、核心模块概览） |
| `search` | 初扫（type: i8..f64, value, align, name_filter, max） |
| `search_string` | 字符串搜索（支持 utf8/utf16le，支持全段与只读数据段扫描） |
| `search_group` | 群组/联合特征扫描 |
| `search_unknown` | 模糊快照基准建立 |
| `search_pattern` | 特征码扫描（支持 ?? 通配符） |
| `filter` | 增量过滤（eq/ne/gt/ge/lt/le/inc/dec/changed/unchanged） |
| `results` | 分页列出当前结果 |
| `read` / `write` | 按地址读写单个数据值 |
| `read_bytes` | 批量直接读取连续内存块（支持十六进制/Base64编码返回，避免落盘） |
| `read_struct` | 结构体/多字段联动批量读取（一次往返打包装箱返回） |
| `inspect_memory` | 内存视检与多类型解析 |
| `find_pointers` / `resolve_pointer_chain` | 指针反查（支持只读段与代码段）与多级解引用 |
| `find_code_xrefs` | 代码段指令交叉引用反查（自动解析 ADRP+ADD/LDR/STR 等相对寻址） |
| `freeze` / `unfreeze` / `unfreeze_all` / `frozen_list` | 数值冻结（支持防崩溃哨兵） |
| `list_modules` / `module_base` / `module_bss` | 模块信息（base+offset 工作流） |
| `disasm` / `patch_code` | AArch64 反汇编与代码修补 |
| `il2cpp_status` / `il2cpp_find_class` | Unity IL2CPP 元数据多版本自适应分析与类型字段解析 |
| `reset` | 清空扫描结果 |

## 典型 AI 工作流示例

> 目标进程由用户在修改器主界面或悬浮窗手动选择附加，包名不流向 AI Agent。
>
> 1. 用户在手机修改器或悬浮窗「进程」页选择并附加目标游戏。
> 2. Agent 调用 `get_status` 确认 `attached` 为 true。
> 3. `search(type="i32", value=当前金币数)`
> 4. 游戏里花掉一些金币 → `filter(op="dec")`
> 5. 再花/得一点 → `filter(op="dec")` 或 `filter(op="eq", value=新值)`
> 6. 结果收敛到几个地址 → `read` 验证 → `write(addr, value=999999)`
> 7. 需要锁定则 `freeze(addr, value=999999)`
