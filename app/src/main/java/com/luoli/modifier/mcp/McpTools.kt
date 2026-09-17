package com.luoli.modifier.mcp

import com.luoli.modifier.core.EngineClient
import com.luoli.modifier.core.EngineProtocol
import com.luoli.modifier.core.EngineRepo
import com.luoli.modifier.core.SettingsStore
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.doubleOrNull
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.longOrNull
import kotlinx.serialization.json.put

/** MCP 工具实现：AI agent 通过这些工具操控修改器 */
object McpTools {

    private fun client() = EngineClient()

    // ---------- 结果封装 ----------

    private fun toolOk(json: String) =
        io.modelcontextprotocol.kotlin.sdk.types.CallToolResult(
            content = listOf(io.modelcontextprotocol.kotlin.sdk.types.TextContent(json))
        )

    private fun toolErr(msg: String) =
        io.modelcontextprotocol.kotlin.sdk.types.CallToolResult(
            content = listOf(io.modelcontextprotocol.kotlin.sdk.types.TextContent(msg)),
            isError = true,
        )

    private fun ok(vararg pairs: Pair<String, kotlinx.serialization.json.JsonElement>) =
        toolOk(buildJsonObject { pairs.forEach { (k, v) -> put(k, v) } }.toString())

    // ---------- 参数提取 ----------

    private fun JsonObject?.str(name: String): String? =
        this?.get(name)?.jsonPrimitive?.contentOrNull

    private fun JsonObject?.intArg(name: String): Int? =
        this?.get(name)?.jsonPrimitive?.intOrNull

    private fun JsonObject?.addrArg(name: String = "addr"): Long? {
        val v = this?.get(name) ?: return null
        return try {
            if (v is JsonPrimitive && v.isString) EngineProtocol.hexToLong(v.content)
            else v.jsonPrimitive.longOrNull
        } catch (t: Throwable) {
            null
        }
    }

    private fun valueText(args: JsonObject?, name: String = "value"): String? {
        val v = args?.get(name) ?: return null
        return if (v is JsonPrimitive) v.contentOrNull ?: v.toString() else v.toString()
    }

    private fun prop(type: String, desc: String) = buildJsonObject {
        put("type", type)
        put("description", desc)
    }

    private fun schema(vararg props: Pair<String, JsonObject>) =
        io.modelcontextprotocol.kotlin.sdk.types.ToolSchema(
            properties = buildJsonObject { props.forEach { (k, v) -> put(k, v) } }
        )

    // ---------- Server 构建 ----------

    fun buildServer(): io.modelcontextprotocol.kotlin.sdk.server.Server {
        val server = io.modelcontextprotocol.kotlin.sdk.server.Server(
            serverInfo = io.modelcontextprotocol.kotlin.sdk.types.Implementation(
                name = "luoli-modifier",
                version = "0.2.0",
            ),
            options = io.modelcontextprotocol.kotlin.sdk.server.ServerOptions(
                capabilities = io.modelcontextprotocol.kotlin.sdk.types.ServerCapabilities(
                    tools = io.modelcontextprotocol.kotlin.sdk.types.ServerCapabilities.Tools(listChanged = false),
                )
            )
        )

        fun reg(name: String, desc: String, s: io.modelcontextprotocol.kotlin.sdk.types.ToolSchema, handler: suspend (JsonObject?) -> io.modelcontextprotocol.kotlin.sdk.types.CallToolResult) {
            server.addTool(name = name, description = desc, inputSchema = s) { request ->
                try {
                    // AI 活动上报（悬浮窗 AI 模式展示）；轮询类工具不打日志
                    val quiet = name in setOf("get_status", "results", "frozen_list")
                    val argsStr = request.arguments?.toString()?.take(160) ?: ""
                    if (!quiet) com.luoli.modifier.overlay.AiActivityBus.log("▶ $name $argsStr")
                    val result = handler(request.arguments)
                    if (!quiet) {
                        if (result.isError == true) com.luoli.modifier.overlay.AiActivityBus.log("✗ $name 失败")
                        else com.luoli.modifier.overlay.AiActivityBus.log("✓ $name")
                    }
                    result
                } catch (t: Throwable) {
                    toolErr("error: ${t.message}")
                }
            }
        }

        // ---- 状态/驱动 ----

        reg("driver_status", "Check kernel driver availability and mode (syscall/procfd). No params.",
            schema()) {
            ok("result" to kotlinx.serialization.json.JsonPrimitive(
                runCatching { client().request("driver_status").toString() }.getOrElse { "driver query failed: ${it.message}" }
            ))
        }

        reg("stealth_hide", "Enable kernel-level VFS dentry hiding for files/directories matching keyword (bypasses anti-cheat scans).",
            schema(
                "directory" to prop("string", "root directory to filter (e.g. '/data/user/0', '/data/data', or '/data/local/tmp'). Default '/data/user/0'"),
                "keyword" to prop("string", "keyword substring to hide (e.g. 'luoli' or 'twt'). Default 'luoli'"),
            )) { args ->
            val dir = args.str("directory") ?: "/data/user/0"
            val kw = args.str("keyword") ?: "luoli"
            val resp = client().request("stealth_hide") {
                put("directory", dir)
                put("keyword", kw)
            }
            ok("result" to resp)
        }

        reg("stealth_clear", "Clear kernel-level file hiding filter.", schema()) {
            ok("result" to client().request("stealth_clear"))
        }

        reg("stealth_status", "Query kernel-level file hiding status.", schema()) {
            ok("result" to client().request("stealth_status"))
        }

        reg("list_processes", "List running app processes (uid>=10000). Foreground focus app is prioritized at the top.",
            schema(
                "filter" to prop("string", "process name filter substring (e.g. 'mindustry' or 'game')"),
                "limit" to prop("integer", "max items to return (default 25)"),
                "all" to prop("boolean", "return all processes without truncation (default false)"),
            )) { args ->
            val resp = client().request("list_processes") {
                args.str("filter")?.let { put("filter", it) }
                args.intArg("limit")?.let { put("limit", it) }
                args?.get("all")?.let { put("all", it) }
            }
            val arr = resp["processes"]?.jsonArray ?: JsonArray(emptyList())
            ok("total" to (resp["total"] ?: JsonPrimitive(arr.size)), "processes" to arr)
        }

        reg("attach", "Attach to a target process by pid.",
            schema("pid" to prop("integer", "target process pid"))) { args ->
            val pid = args.intArg("pid") ?: return@reg toolErr("missing pid")
            EngineRepo.attach(pid)
            val att = EngineRepo.attached.value
            if (att == null) toolErr("attach failed")
            else ok("result" to JsonPrimitive("attached ${att.name} (pid=${att.pid})"))
        }

        reg("attach_by_name", "Attach to a target process by name/package (kernel GET_PID).",
            schema("name" to prop("string", "process name or package name, e.g. com.xxx.yyy"))) { args ->
            val name = args.str("name") ?: return@reg toolErr("missing name")
            EngineRepo.attachByName(name)
            val att = EngineRepo.attached.value
            if (att == null) toolErr("attach failed")
            else ok("result" to JsonPrimitive("attached ${att.name} (pid=${att.pid})"))
        }

        reg("detach", "Detach from current process and clear scan results.", schema()) {
            EngineRepo.detach()
            ok("result" to JsonPrimitive("detached"))
        }

        reg("get_status", "Get engine status: attached process, driver, scan progress, result count, frozen entries.",
            schema()) {
            val who = runCatching { client().request("who") }.getOrNull()
            val ds = runCatching { client().request("driver_status") }.getOrNull()
            val prog = runCatching { client().request("scan_progress") }.getOrNull()
            val frozen = runCatching { client().request("frozen_list") }.getOrNull()
            ok(
                "attached" to (who?.get("name")?.jsonPrimitive ?: JsonPrimitive("")),
                "pid" to (who?.get("pid")?.jsonPrimitive ?: JsonPrimitive(0)),
                "driver_ready" to (ds?.get("ready")?.jsonPrimitive ?: JsonPrimitive(false)),
                "driver_mode" to (ds?.get("mode")?.jsonPrimitive ?: JsonPrimitive("")),
                "scan_running" to (prog?.get("running")?.jsonPrimitive ?: JsonPrimitive(false)),
                "result_count" to (prog?.get("found")?.jsonPrimitive ?: JsonPrimitive(0)),
                "frozen_count" to (frozen?.get("entries")?.jsonArray?.size?.let { JsonPrimitive(it) } ?: JsonPrimitive(0)),
            )
        }

        // ---- 模块 ----

        reg("list_modules", "List loaded modules (name/base/end/size) of attached process.",
            schema()) {
            ok("result" to JsonPrimitive(client().request("list_modules").toString()))
        }

        reg("module_base", "Get module base address (kernel MODULE_BASE).",
            schema("module" to prop("string", "module name, e.g. libxxx.so"))) { args ->
            val m = args.str("module") ?: return@reg toolErr("missing module")
            ok("result" to JsonPrimitive(client().request("module_base") { put("name", m) }.toString()))
        }

        reg("module_bss", "Get module .bss section address (kernel MODULE_BSS).",
            schema("module" to prop("string", "module name, e.g. libxxx.so"))) { args ->
            val m = args.str("module") ?: return@reg toolErr("missing module")
            ok("result" to JsonPrimitive(client().request("module_bss") { put("name", m) }.toString()))
        }

        // ---- 扫描/过滤 ----

        reg("search", "Initial memory scan for a value. Blocks until done (progress via get_status).",
            schema(
                "type" to prop("string", "value type: i8/u8/i16/u16/i32/u32/i64/u64/f32/f64"),
                "value" to prop("string", "target value, e.g. 9999 / 3.14 / 0x1F4"),
                "align" to prop("integer", "alignment in bytes (default = type size)"),
                "name_filter" to prop("string", "region path substring filter, e.g. CaLe/heap; empty = all writable"),
                "tags" to prop("string", "comma-separated region classification tags (e.g. 'Jh' for Java Heap, 'Ch' for C++ Heap, 'B' for BSS, 'A' for Anon; empty = all writable)"),
                "max" to prop("integer", "max results cap (default 1000000)"),
            )) { args ->
            val type = args.str("type") ?: "i32"
            val value = valueText(args) ?: return@reg toolErr("missing value")
            val tags = args.str("tags")?.split(",")?.map { it.trim() }?.filter { it.isNotEmpty() } ?: emptyList()
            EngineRepo.search(
                type, value,
                args.intArg("align") ?: 4,
                args.str("name_filter") ?: "",
                tags,
            )
            val count = EngineRepo.total.value
            com.luoli.modifier.overlay.AiActivityBus.log("  ↳ 搜索命中 $count")
            ok("count" to JsonPrimitive(count), "note" to JsonPrimitive("use filter() to narrow, results() to list"))
        }

        reg("search_group", "Search for structured group/pattern values at relative offsets in a single pass.",
            schema(
                "patterns" to prop("string", "JSON array of pattern items, e.g. [{\"offset\": 0, \"type\": \"i32\", \"value\": 813}, {\"offset\": 4, \"type\": \"i32\", \"value\": 4000}]"),
                "base_align" to prop("integer", "alignment of base address in bytes (default 4)"),
                "name_filter" to prop("string", "region path substring filter"),
                "tags" to prop("string", "comma-separated region tags (e.g. 'Jh' for Java Heap)"),
                "max" to prop("integer", "max results cap (default 100000)"),
            )) { args ->
            val patEl = args?.get("patterns") ?: return@reg toolErr("missing patterns")
            val patArr = try {
                if (patEl is JsonPrimitive && patEl.isString) {
                    Json.parseToJsonElement(patEl.content).jsonArray
                } else {
                    patEl.jsonArray
                }
            } catch (t: Throwable) {
                return@reg toolErr("invalid patterns format: ${t.message}")
            }
            val tags = args.str("tags")?.split(",")?.map { it.trim() }?.filter { it.isNotEmpty() } ?: emptyList()
            val resp = client().request("search_group") {
                put("patterns", patArr)
                put("base_align", args.intArg("base_align") ?: 4)
                put("name_filter", args.str("name_filter") ?: "")
                if (tags.isNotEmpty()) {
                    put("tags", buildJsonArray { tags.forEach { add(JsonPrimitive(it)) } })
                }
                args.intArg("max")?.let { put("max", it) }
            }
            val count = resp["count"]?.jsonPrimitive?.longOrNull ?: 0
            EngineRepo.refreshResults()
            com.luoli.modifier.overlay.AiActivityBus.log("  ↳ 群组搜索命中 $count")
            ok("count" to JsonPrimitive(count), "note" to JsonPrimitive("use results() to inspect matched addresses"))
        }

        reg("search_unknown", "Initial snapshot of all memory values for fuzzy/unknown value scanning (e.g. unknown HP, timer, gauge).",
            schema(
                "type" to prop("string", "value type: i8/u8/i16/u16/i32/u32/i64/u64/f32/f64 (default i32)"),
                "align" to prop("integer", "alignment in bytes (default = type size)"),
                "tags" to prop("string", "comma-separated region classification tags (e.g. 'Jh' for Java Heap, 'Ch' for C++ Heap)"),
                "name_filter" to prop("string", "region path substring filter"),
                "max" to prop("integer", "max results cap (default 500000)"),
            )) { args ->
            val type = args.str("type") ?: "i32"
            val tags = args.str("tags")?.split(",")?.map { it.trim() }?.filter { it.isNotEmpty() } ?: emptyList()
            val resp = client().request("search_unknown") {
                put("type", type)
                put("align", args.intArg("align") ?: 4)
                put("name_filter", args.str("name_filter") ?: "")
                if (tags.isNotEmpty()) {
                    put("tags", buildJsonArray { tags.forEach { add(JsonPrimitive(it)) } })
                }
                args.intArg("max")?.let { put("max", it) }
            }
            val count = resp["count"]?.jsonPrimitive?.longOrNull ?: 0
            EngineRepo.refreshResults()
            com.luoli.modifier.overlay.AiActivityBus.log("  ↳ 模糊快照基准已建立: $count 个变量")
            ok("count" to JsonPrimitive(count), "note" to JsonPrimitive("now change value in game and use filter(op='dec'/'inc'/'changed'/'unchanged')"))
        }

        reg("search_string", "Search memory for a UTF-8 or UTF-16LE text string.",
            schema(
                "text" to prop("string", "text string to search"),
                "encoding" to prop("string", "encoding: 'utf8' or 'utf16' (default 'utf8')"),
                "tags" to prop("string", "comma-separated region classification tags (e.g. 'Jh,A')"),
                "name_filter" to prop("string", "region path substring filter"),
                "max" to prop("integer", "max results cap (default 100000)"),
            )) { args ->
            val text = args.str("text") ?: return@reg toolErr("missing text")
            val tags = args.str("tags")?.split(",")?.map { it.trim() }?.filter { it.isNotEmpty() } ?: emptyList()
            val resp = client().request("search_string") {
                put("text", text)
                put("encoding", args.str("encoding") ?: "utf8")
                put("name_filter", args.str("name_filter") ?: "")
                if (tags.isNotEmpty()) {
                    put("tags", buildJsonArray { tags.forEach { add(JsonPrimitive(it)) } })
                }
                args.intArg("max")?.let { put("max", it) }
            }
            val count = resp["count"]?.jsonPrimitive?.longOrNull ?: 0
            EngineRepo.refreshResults()
            com.luoli.modifier.overlay.AiActivityBus.log("  ↳ 字符串搜索命中 $count")
            ok("count" to JsonPrimitive(count), "note" to JsonPrimitive("use results() to inspect matched addresses"))
        }

        reg("search_pattern", "Search memory for an Array of Bytes (AOB) with hex wildcards (e.g. '1F 20 03 D5 ?? ?? 00 94' or '1f20??d5').",
            schema(
                "pattern" to prop("string", "hex byte pattern with '?' or '??' wildcards"),
                "align" to prop("integer", "byte alignment: 1, 2, 4, 8 (default 4 for machine code)"),
                "tags" to prop("string", "comma-separated region classification tags (e.g. 'Ca,Cd' for code/data segments)"),
                "name_filter" to prop("string", "region path substring filter"),
                "max" to prop("integer", "max results cap (default 100000)"),
            )) { args ->
            val pat = args.str("pattern") ?: return@reg toolErr("missing pattern")
            val tags = args.str("tags")?.split(",")?.map { it.trim() }?.filter { it.isNotEmpty() } ?: emptyList()
            val resp = client().request("search_pattern") {
                put("pattern", pat)
                put("align", args.intArg("align") ?: 4)
                put("name_filter", args.str("name_filter") ?: "")
                if (tags.isNotEmpty()) {
                    put("tags", buildJsonArray { tags.forEach { add(JsonPrimitive(it)) } })
                }
                args.intArg("max")?.let { put("max", it) }
            }
            val count = resp["count"]?.jsonPrimitive?.longOrNull ?: 0
            EngineRepo.refreshResults()
            com.luoli.modifier.overlay.AiActivityBus.log("  ↳ 特征码搜索命中 $count")
            ok("count" to JsonPrimitive(count), "note" to JsonPrimitive("use results() to inspect matched addresses, disasm() to decode"))
        }

        reg("filter", "Narrow current results by re-reading values. Ops: eq/ne/gt/ge/lt/le/inc/dec/changed/unchanged.",
            schema(
                "op" to prop("string", "eq/ne/gt/ge/lt/le/inc/dec/changed/unchanged"),
                "type" to prop("string", "same type as search"),
                "value" to prop("string", "compare value (needed for eq/ne/gt/ge/lt/le)"),
            )) { args ->
            val op = args.str("op") ?: "eq"
            val type = args.str("type") ?: "i32"
            val needsValue = op !in setOf("inc", "dec", "changed", "unchanged")
            EngineRepo.filter(op, type, if (needsValue) valueText(args) else null)
            com.luoli.modifier.overlay.AiActivityBus.log("  ↳ 过滤后剩 ${EngineRepo.total.value}")
            ok("count" to JsonPrimitive(EngineRepo.total.value))
        }

        reg("results", "List current scan results (paged).",
            schema(
                "offset" to prop("integer", "start index (default 0)"),
                "limit" to prop("integer", "page size (default 200, max 1000)"),
            )) { args ->
            val offset = args.intArg("offset") ?: 0
            val limit = (args.intArg("limit") ?: 200).coerceAtMost(1000)
            ok("result" to JsonPrimitive(
                client().request("results") { put("offset", offset); put("limit", limit) }.toString()
            ))
        }

        reg("reset", "Clear all scan results.", schema()) {
            EngineRepo.reset()
            ok("result" to JsonPrimitive("cleared"))
        }

        reg("driver_probe", "Diagnostic: probe which ioctl command numbers the loaded kernel driver accepts for memory read on the attached target. Requires an attached process. No params.",
            schema()) {
            ok("result" to JsonPrimitive(client().request("driver_probe").toString()))
        }

        // ---- 读写 ----

        reg("read", "Read value at address.",
            schema(
                "addr" to prop("string", "address, 0x-prefixed hex or decimal string"),
                "type" to prop("string", "i32/f32/... (default i32)"),
            )) { args ->
            val addr = args.addrArg() ?: return@reg toolErr("missing/bad addr")
            val type = args.str("type") ?: "i32"
            ok("result" to JsonPrimitive(
                client().request("read") { put("addr", addr); put("type", type) }.toString()
            ))
        }

        reg("write", "Write value to address (auto-unfreezes same address).",
            schema(
                "addr" to prop("string", "address, 0x-prefixed hex or decimal string"),
                "value" to prop("string", "new value, number or 0x-hex"),
                "type" to prop("string", "i32/f32/... (default i32)"),
            )) { args ->
            val addr = args.addrArg() ?: return@reg toolErr("missing/bad addr")
            val type = args.str("type") ?: "i32"
            val value = valueText(args) ?: return@reg toolErr("missing value")
            EngineRepo.write(addr, type, value)
            ok("result" to JsonPrimitive("written $value ($type) to 0x${java.lang.Long.toHexString(addr)}"))
        }

        reg("inspect_memory", "Inspect memory around an address (hexdump + multi-type decodings + summary).",
            schema(
                "addr" to prop("string", "target address (0x-hex or decimal)"),
                "before" to prop("integer", "number of words before target (default 4)"),
                "after" to prop("integer", "number of words after target (default 8)"),
                "unit_size" to prop("integer", "word size in bytes: 1/2/4/8 (default 4)"),
                "raw" to prop("boolean", "include verbose raw word AST objects (default false to save tokens)"),
            )) { args ->
            val addr = args.addrArg() ?: return@reg toolErr("missing/bad addr")
            val resp = client().request("inspect_memory") {
                put("addr", addr)
                put("before", args.intArg("before") ?: 4)
                put("after", args.intArg("after") ?: 8)
                put("unit_size", args.intArg("unit_size") ?: 4)
            }
            val summary = resp["summary"]?.jsonPrimitive?.contentOrNull ?: ""
            val wantRaw = args?.get("raw")?.jsonPrimitive?.booleanOrNull == true
            if (wantRaw) ok("summary" to JsonPrimitive(summary), "raw" to resp)
            else ok("summary" to JsonPrimitive(summary))
        }

        reg("find_pointers", "Find pointers/references pointing to target address (pointer scanning).",
            schema(
                "target_addr" to prop("string", "target memory address to find references to"),
                "max_offset" to prop("integer", "max interior pointer offset in bytes (default 0 for exact pointer)"),
                "align" to prop("integer", "pointer alignment in bytes: 4 or 8 (default 8)"),
                "tags" to prop("string", "comma-separated region tags to search in (e.g. 'B,Jh,Ch'; default BSS and Heaps)"),
            )) { args ->
            val targetAddr = args.addrArg("target_addr") ?: return@reg toolErr("missing/bad target_addr")
            val tags = args.str("tags")?.split(",")?.map { it.trim() }?.filter { it.isNotEmpty() } ?: emptyList()
            val resp = client().request("find_pointers") {
                put("target_addr", targetAddr)
                put("max_offset", args.intArg("max_offset") ?: 0)
                put("align", args.intArg("align") ?: 8)
                if (tags.isNotEmpty()) {
                    put("tags", buildJsonArray { tags.forEach { add(JsonPrimitive(it)) } })
                }
            }
            ok("result" to resp)
        }

        reg("resolve_pointer_chain", "Resolve and dereference multi-level pointer paths (e.g. 'base+offset1->offset2->value').",
            schema(
                "base" to prop("string", "base specification (e.g. 'libil2cpp.so+0x1234' or '0x2a50c18')"),
                "offsets" to prop("string", "comma-separated offsets or JSON array of offsets (e.g. '0, 4' or [0, 4])"),
                "ptr_size" to prop("integer", "pointer size in bytes: 4 (compressed) or 8 (default 8)"),
                "type" to prop("string", "final target value type: i32/f32/i64/... (default i32)"),
            )) { args ->
            val base = args.str("base") ?: return@reg toolErr("missing base")
            val offStr = args.str("offsets")
            val offList = mutableListOf<Long>()
            if (offStr != null) {
                if (offStr.trim().startsWith("[")) {
                    val arr = Json.parseToJsonElement(offStr).jsonArray
                    for (el in arr) {
                        if (el is JsonPrimitive && el.isString) offList.add(EngineProtocol.hexToLong(el.content))
                        else el.jsonPrimitive.longOrNull?.let { offList.add(it) }
                    }
                } else {
                    for (part in offStr.split(",")) {
                        val p = part.trim()
                        if (p.isNotEmpty()) {
                            if (p.startsWith("0x") || p.startsWith("0X")) offList.add(EngineProtocol.hexToLong(p))
                            else p.toLongOrNull()?.let { offList.add(it) }
                        }
                    }
                }
            }
            val resp = client().request("resolve_pointer_chain") {
                put("base", base)
                put("offsets", buildJsonArray { offList.forEach { add(JsonPrimitive(it)) } })
                put("ptr_size", args.intArg("ptr_size") ?: 8)
                put("type", args.str("type") ?: "i32")
            }
            ok("result" to resp)
        }

        reg("disasm", "Disassemble AArch64 machine instructions at target code address.",
            schema(
                "addr" to prop("string", "target code address (0x-hex or decimal)"),
                "count" to prop("integer", "number of instructions to decode (default 10, max 256)"),
            )) { args ->
            val addr = args.addrArg() ?: return@reg toolErr("missing/bad addr")
            val count = args.intArg("count") ?: 10
            val resp = client().request("disasm") {
                put("addr", addr)
                put("count", count)
            }
            val summary = resp["summary"]?.jsonPrimitive?.contentOrNull ?: ""
            ok("summary" to JsonPrimitive(summary), "raw" to resp)
        }

        reg("patch_code", "Patch code instructions in memory (supports NOPing or arbitrary hex instructions).",
            schema(
                "addr" to prop("string", "target code address (0x-hex or decimal)"),
                "value" to prop("string", "patch mode: 'nop' to replace with NOPs"),
                "count" to prop("integer", "number of NOPs to write if value='nop' (default 1)"),
                "hex" to prop("string", "hex byte string to write (e.g. '1f2003d5' for NOP) if not using value='nop'"),
            )) { args ->
            val addr = args.addrArg() ?: return@reg toolErr("missing/bad addr")
            val resp = client().request("patch_code") {
                put("addr", addr)
                args.str("value")?.let { put("value", it) }
                args.intArg("count")?.let { put("count", it) }
                args.str("hex")?.let { put("hex", it) }
            }
            val count = resp["patched_bytes"]?.jsonPrimitive?.intOrNull ?: 0
            ok("result" to JsonPrimitive("patched $count bytes at 0x${java.lang.Long.toHexString(addr)}"))
        }

        reg("watch_point", "Set a kernel hardware watchpoint on an address to monitor read/write accesses.",
            schema(
                "addr" to prop("string", "target address to monitor (0x-hex or decimal)"),
                "type" to prop("string", "watchpoint type: 'w' (write), 'r' (read), 'rw' (read/write), 'x' (execute). Default 'w'"),
                "len" to prop("integer", "monitored length in bytes: 1, 2, 4, 8 (default 4)"),
            )) { args ->
            val addr = args.addrArg() ?: return@reg toolErr("missing/bad addr")
            val resp = client().request("watch_point") {
                put("addr", addr)
                put("type", args.str("type") ?: "w")
                put("len", args.intArg("len") ?: 4)
            }
            val handle = resp["handle"]?.jsonPrimitive?.contentOrNull ?: ""
            ok("handle" to JsonPrimitive(handle), "note" to JsonPrimitive("watchpoint installed. Trigger actions in app then call get_watch_hits(handle)"))
        }

        reg("unwatch_point", "Remove a kernel hardware watchpoint.",
            schema("handle" to prop("string", "watchpoint handle returned from watch_point"))) { args ->
            val handle = args.str("handle") ?: return@reg toolErr("missing handle")
            val resp = client().request("unwatch_point") { put("handle", handle) }
            ok("result" to resp)
        }

        reg("get_watch_hits", "Retrieve access hit items (PC instruction, register states, caller module) from hardware watchpoint.",
            schema(
                "handle" to prop("string", "watchpoint handle"),
                "max_items" to prop("integer", "max items to retrieve (default 50)"),
            )) { args ->
            val handle = args.str("handle") ?: return@reg toolErr("missing handle")
            val resp = client().request("get_watch_hits") {
                put("handle", handle)
                put("max_items", args.intArg("max_items") ?: 50)
            }
            ok("result" to resp)
        }

        // ---- 冻结 ----

        reg("freeze", "Lock a value at address (daemon rewrites periodically). Supports GC guard verification.",
            schema(
                "addr" to prop("string", "address, 0x-prefixed hex or decimal string"),
                "value" to prop("string", "value to lock; empty = lock current value"),
                "type" to prop("string", "i32/f32/... (default i32)"),
                "interval_ms" to prop("integer", "write interval (default 150)"),
                "guard_offset" to prop("integer", "optional relative offset to check before rewriting (e.g. -4 for array length)"),
                "guard_type" to prop("string", "optional type of guard value (default i32)"),
                "guard_value" to prop("string", "optional expected guard value (e.g. 22). If mismatch, freeze is paused to prevent GC heap corruption"),
            )) { args ->
            val addr = args.addrArg() ?: return@reg toolErr("missing/bad addr")
            val type = args.str("type") ?: "i32"
            val value = valueText(args) ?: ""
            val gOff = args.intArg("guard_offset")?.toLong()
            val gType = args.str("guard_type")
            val gVal = args.str("guard_value")
            EngineRepo.freeze(addr, type, value, args.intArg("interval_ms") ?: 150, gOff, gType, gVal)
            ok("result" to JsonPrimitive("frozen 0x${java.lang.Long.toHexString(addr)}${if (gOff != null) " (guarded)" else ""}"))
        }

        reg("unfreeze", "Remove freeze at address.",
            schema("addr" to prop("string", "address"))) { args ->
            val addr = args.addrArg() ?: return@reg toolErr("missing/bad addr")
            EngineRepo.unfreeze(addr)
            ok("result" to JsonPrimitive("unfrozen"))
        }

        reg("unfreeze_all", "Remove all freezes.", schema()) {
            EngineRepo.unfreezeAll()
            ok("result" to JsonPrimitive("all unfrozen"))
        }

        reg("frozen_list", "List frozen entries.", schema()) {
            ok("result" to JsonPrimitive(client().request("frozen_list").toString()))
        }

        // ---- 转储脱壳 ----

        reg("dump_memory", "Dump raw memory range to file.",
            schema(
                "addr" to prop("string", "start memory address (0x-hex or decimal)"),
                "size" to prop("integer", "number of bytes to dump"),
                "path" to prop("string", "target output file path (e.g. '/data/local/tmp/dump.bin')"),
            )) { args ->
            val addr = args.addrArg() ?: return@reg toolErr("missing/bad addr")
            val size = args.intArg("size") ?: return@reg toolErr("missing size")
            val path = args.str("path") ?: return@reg toolErr("missing path")
            val resp = client().request("dump_memory") {
                put("addr", addr)
                put("size", size)
                put("path", path)
            }
            ok("result" to resp)
        }

        reg("dump_module", "Dump and reconstruct loaded module (SO file) from memory with ELF segment headers fixed for IDA/Ghidra.",
            schema(
                "module" to prop("string", "module name or substring (e.g. 'libil2cpp.so' or 'boot-core-libart.oat')"),
                "path" to prop("string", "optional output file path (defaults to '/data/local/tmp/<module>.dump')"),
            )) { args ->
            val mod = args.str("module") ?: return@reg toolErr("missing module")
            val path = args.str("path") ?: ""
            val resp = client().request("dump_module") {
                put("module", mod)
                if (path.isNotEmpty()) put("path", path)
            }
            ok("result" to resp)
        }

        // ---- 规则引擎 (Rules Engine 2.0) ----

        reg("register_rule", "Register an autonomous background cheat rule (pointer-chain tracking + auto enforcement + RuleEngine 2.0 multi-variable dynamic ratio linkage).",
            schema(
                "name" to prop("string", "rule name (e.g. 'auto_gold_lock' or 'dynamic_hp_link')"),
                "base" to prop("string", "base address or module+offset of target variable A (e.g. '0x2986570' or 'libil2cpp.so+0x1234')"),
                "offsets" to prop("string", "pointer chain offsets for target variable A (e.g. '0, 12' or '[0, 12]')"),
                "value" to prop("string", "target static value to enforce (e.g. '99999'). Optional if using dynamic ratio linkage"),
                "type" to prop("string", "value type of variable A: i32/f32/i64/f64 (default i32)"),
                "action" to prop("string", "action mode: 'lock' (default), 'clamp_min', 'clamp_max', 'ratio_lock', 'clamp_min_ratio', 'clamp_max_ratio'"),
                "ptr_size" to prop("integer", "pointer size: 4 (compressed) or 8 (default 4)"),
                "interval_ms" to prop("integer", "check interval in ms (default 150)"),
                "base2" to prop("string", "optional base address or module+offset of second reference variable B (e.g. MaxHP)"),
                "offsets2" to prop("string", "optional pointer chain offsets for reference variable B"),
                "ptr_size2" to prop("integer", "pointer size for variable B (default 4)"),
                "type2" to prop("string", "value type for variable B (default i32)"),
                "ratio" to prop("number", "multiplier ratio for dynamic linkage: A = B * ratio + offset_val (default 1.0)"),
                "offset_val" to prop("number", "additive offset for dynamic linkage (default 0.0)"),
            )) { args ->
            val base = args.str("base") ?: return@reg toolErr("missing base")
            val value = valueText(args)
            val offStr = args.str("offsets")
            val offList = mutableListOf<Long>()
            if (offStr != null) {
                if (offStr.trim().startsWith("[")) {
                    val arr = Json.parseToJsonElement(offStr).jsonArray
                    for (el in arr) {
                        if (el is JsonPrimitive && el.isString) offList.add(EngineProtocol.hexToLong(el.content))
                        else el.jsonPrimitive.longOrNull?.let { offList.add(it) }
                    }
                } else {
                    for (part in offStr.split(",")) {
                        val p = part.trim()
                        if (p.isNotEmpty()) {
                            if (p.startsWith("0x") || p.startsWith("0X")) offList.add(EngineProtocol.hexToLong(p))
                            else p.toLongOrNull()?.let { offList.add(it) }
                        }
                    }
                }
            }

            val offStr2 = args.str("offsets2")
            val offList2 = mutableListOf<Long>()
            if (offStr2 != null) {
                if (offStr2.trim().startsWith("[")) {
                    val arr = Json.parseToJsonElement(offStr2).jsonArray
                    for (el in arr) {
                        if (el is JsonPrimitive && el.isString) offList2.add(EngineProtocol.hexToLong(el.content))
                        else el.jsonPrimitive.longOrNull?.let { offList2.add(it) }
                    }
                } else {
                    for (part in offStr2.split(",")) {
                        val p = part.trim()
                        if (p.isNotEmpty()) {
                            if (p.startsWith("0x") || p.startsWith("0X")) offList2.add(EngineProtocol.hexToLong(p))
                            else p.toLongOrNull()?.let { offList2.add(it) }
                        }
                    }
                }
            }

            val resp = client().request("register_rule") {
                put("name", args.str("name") ?: "")
                put("base", base)
                put("offsets", buildJsonArray { offList.forEach { add(JsonPrimitive(it)) } })
                if (value != null) put("value", value)
                put("type", args.str("type") ?: "i32")
                put("action", args.str("action") ?: "lock")
                put("ptr_size", args.intArg("ptr_size") ?: 4)
                put("interval_ms", args.intArg("interval_ms") ?: 150)

                args.str("base2")?.let { put("base2", it) }
                if (offList2.isNotEmpty()) {
                    put("offsets2", buildJsonArray { offList2.forEach { add(JsonPrimitive(it)) } })
                }
                args.intArg("ptr_size2")?.let { put("ptr_size2", it) }
                args.str("type2")?.let { put("type2", it) }
                args?.get("ratio")?.let { put("ratio", it) }
                args?.get("offset_val")?.let { put("offset_val", it) }
            }
            ok("result" to resp)
        }

        reg("list_rules", "List all active autonomous background cheat rules and their current statuses.", schema()) {
            ok("result" to client().request("list_rules"))
        }

        reg("delete_rule", "Delete an autonomous cheat rule by ID.",
            schema("rule_id" to prop("integer", "rule ID to delete"))) { args ->
            val id = args.intArg("rule_id") ?: return@reg toolErr("missing rule_id")
            val resp = client().request("delete_rule") { put("rule_id", id) }
            ok("result" to resp)
        }

        reg("clear_rules", "Delete and stop all autonomous cheat rules.", schema()) {
            ok("result" to client().request("clear_rules"))
        }

        // ---- Unity IL2CPP 探针 ----

        reg("il2cpp_status", "Probe target process for Unity IL2CPP runtime, libil2cpp.so base, and global-metadata.dat mapping.", schema()) {
            ok("result" to client().request("il2cpp_status"))
        }

        reg("il2cpp_apis", "List exported runtime IL2CPP APIs from memory in libil2cpp.so.", schema()) {
            ok("result" to client().request("il2cpp_apis"))
        }

        reg("il2cpp_find_class", "Search in-memory IL2CPP global-metadata for class names and extract fields with runtime memory offsets.",
            schema(
                "class" to prop("string", "class name or substring to find (e.g. 'Player', 'Inventory', 'GameManager')"),
            )) { args ->
            val cname = args.str("class") ?: return@reg toolErr("missing class")
            val resp = client().request("il2cpp_find_class") { put("class", cname) }
            val summary = resp["summary"]?.jsonPrimitive?.contentOrNull ?: ""
            ok("summary" to JsonPrimitive(summary), "result" to resp)
        }

        // ---- 态势遥测看板 (Zero-Vision Telemetry Monitor) ----

        reg("telemetry_query", "High-density, micro-token game world state telemetry monitor for zero-vision agent situation awareness.",
            schema(
                "items" to prop("string", "JSON array of items to query, e.g. [{\"label\": \"copper\", \"addr\": \"0x2986584\", \"type\": \"i32\"}, {\"label\": \"lead\", \"addr\": \"0x2986588\", \"type\": \"i32\"}]"),
            )) { args ->
            val itemEl = args?.get("items") ?: return@reg toolErr("missing items")
            val itemArr = try {
                if (itemEl is JsonPrimitive && itemEl.isString) {
                    Json.parseToJsonElement(itemEl.content).jsonArray
                } else {
                    itemEl.jsonArray
                }
            } catch (t: Throwable) {
                return@reg toolErr("invalid items format: ${t.message}")
            }
            val resp = client().request("telemetry_query") { put("items", itemArr) }
            val summary = resp["summary"]?.jsonPrimitive?.contentOrNull ?: ""
            ok("summary" to JsonPrimitive(summary), "data" to (resp["data"] ?: buildJsonObject {}))
        }

        // ---- 应用私有存储与 SQLite 数据库 (App Storage & SQLite) ----

        reg("app_storage_list", "List files in target application private sandbox directory (/data/user/0/<pkg>/ or /sdcard/Android/data/<pkg>/).",
            schema(
                "subpath" to prop("string", "subpath or prefix (e.g. 'databases', 'files', 'shared_prefs', or 'ext:saves'). Default root sandbox"),
                "limit" to prop("integer", "max items to return (default 50)"),
            )) { args ->
            val resp = client().request("app_storage_list") {
                args.str("subpath")?.let { put("subpath", it) }
                args.intArg("limit")?.let { put("limit", it) }
            }
            val summary = resp["summary"]?.jsonPrimitive?.contentOrNull ?: ""
            ok("summary" to JsonPrimitive(summary), "result" to resp)
        }

        reg("app_storage_read", "Read content of a file in target application private sandbox (XML, JSON, configs).",
            schema(
                "path" to prop("string", "relative subpath (e.g. 'shared_prefs/settings.xml') or absolute path"),
                "max_bytes" to prop("integer", "max bytes to read (default 4096)"),
                "offset" to prop("integer", "file offset in bytes (default 0)"),
            )) { args ->
            val path = args.str("path") ?: return@reg toolErr("missing path")
            val resp = client().request("app_storage_read") {
                put("path", path)
                args.intArg("max_bytes")?.let { put("max_bytes", it) }
                args.intArg("offset")?.let { put("offset", it) }
            }
            val content = resp["content"]?.jsonPrimitive?.contentOrNull ?: ""
            val isText = resp["is_text"]?.jsonPrimitive?.booleanOrNull ?: true
            ok("content" to JsonPrimitive(content), "is_text" to JsonPrimitive(isText), "total_size" to (resp["total_size"] ?: JsonPrimitive(0)))
        }

        reg("app_storage_write", "Write content to a file in target application private sandbox.",
            schema(
                "path" to prop("string", "relative subpath or absolute path"),
                "content" to prop("string", "text content or hex string to write"),
                "is_hex" to prop("boolean", "whether content is a hex string (default false)"),
            )) { args ->
            val path = args.str("path") ?: return@reg toolErr("missing path")
            val content = args.str("content") ?: return@reg toolErr("missing content")
            val resp = client().request("app_storage_write") {
                put("path", path)
                put("content", content)
                args?.get("is_hex")?.let { put("is_hex", it) }
            }
            ok("result" to resp)
        }

        reg("sqlite_query", "Execute SQL SELECT query on target application SQLite database via in-process libsqlite.",
            schema(
                "db" to prop("string", "database file subpath (e.g. 'databases/game.db') or absolute path"),
                "sql" to prop("string", "SQL query statement (e.g. 'SELECT id, count FROM inventory')"),
                "limit" to prop("integer", "max rows to return (default 25)"),
            )) { args ->
            val db = args.str("db") ?: return@reg toolErr("missing db")
            val sql = args.str("sql") ?: return@reg toolErr("missing sql")
            val resp = client().request("sqlite_query") {
                put("db", db)
                put("sql", sql)
                args.intArg("limit")?.let { put("limit", it) }
            }
            val summary = resp["summary"]?.jsonPrimitive?.contentOrNull ?: ""
            ok("summary" to JsonPrimitive(summary), "result" to resp)
        }

        reg("sqlite_exec", "Execute SQL UPDATE/INSERT/DELETE statement on target application SQLite database.",
            schema(
                "db" to prop("string", "database file subpath or absolute path"),
                "sql" to prop("string", "SQL statement to execute"),
            )) { args ->
            val db = args.str("db") ?: return@reg toolErr("missing db")
            val sql = args.str("sql") ?: return@reg toolErr("missing sql")
            val resp = client().request("sqlite_exec") {
                put("db", db)
                put("sql", sql)
            }
            ok("result" to resp)
        }

        // ---- 崩溃自愈诊断 (Crash Triage) ----

        reg("crash_triage", "Diagnose recent fatal crash signals (SIGSEGV, SIGBUS, SIGABRT), fault address, and stack backtrace of target application.", schema()) {
            val resp = client().request("crash_triage")
            val summary = resp["summary"]?.jsonPrimitive?.contentOrNull ?: ""
            ok("summary" to JsonPrimitive(summary), "result" to resp)
        }

        // ---- 进程内原生函数远程调用 (Remote RPC) ----

        reg("invoke_function", "Remotely invoke an internal native C/C++ or C# AArch64 function inside target process with arguments and retrieve return value.",
            schema(
                "addr" to prop("string", "function entry point address (0x-hex or decimal)"),
                "args" to prop("string", "optional JSON array of up to 8 integer/pointer arguments (e.g. ['0x1', '9999'] or [1, 9999])"),
                "timeout_ms" to prop("integer", "invocation execution timeout in ms (default 2000)"),
            )) { args ->
            val addr = args.addrArg("addr") ?: return@reg toolErr("missing/bad addr")
            val argEl = args?.get("args")
            val argList = mutableListOf<String>()
            if (argEl != null) {
                val arr = if (argEl is JsonPrimitive && argEl.isString) {
                    Json.parseToJsonElement(argEl.content).jsonArray
                } else {
                    argEl.jsonArray
                }
                for (a in arr) {
                    if (a is JsonPrimitive) argList.add(a.contentOrNull ?: a.toString())
                }
            }
            val resp = client().request("invoke_function") {
                put("addr", addr)
                if (argList.isNotEmpty()) {
                    put("args", buildJsonArray { argList.forEach { add(JsonPrimitive(it)) } })
                }
                args.intArg("timeout_ms")?.let { put("timeout_ms", it) }
            }
            ok("result" to resp)
        }

        return server
    }
}
