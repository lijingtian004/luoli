package com.luoli.modifier.core

import android.content.Context
import com.luoli.modifier.service.EngineService
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.add
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.doubleOrNull
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.longOrNull
import kotlinx.serialization.json.put

data class DaemonStatus(
    val running: Boolean = false,
    val version: String = "",
    val driverReady: Boolean = false,
    val driverMode: String = "",
)

data class AttachedInfo(
    val pid: Int,
    val name: String,
    val arch: String = "",
    val bitness: Int = 0,
    val engine: String = "",
)

data class ProcessUi(
    val pid: Int,
    val name: String,
    val uid: Int,
    val foreground: Boolean = false,
    val label: String = "",
)

data class HitUi(val addr: Long, val type: String, val bits: Long, val value: Double)

data class FrozenUi(val addr: Long, val type: String, val bits: Long, val value: Double, val fails: Int)

/** 引擎状态与操作的单一入口 */
object EngineRepo {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    private val _daemon = MutableStateFlow(DaemonStatus())
    val daemon: StateFlow<DaemonStatus> = _daemon

    private val _attached = MutableStateFlow<AttachedInfo?>(null)
    val attached: StateFlow<AttachedInfo?> = _attached

    private val _hits = MutableStateFlow<List<HitUi>>(emptyList())
    val hits: StateFlow<List<HitUi>> = _hits

    private val _total = MutableStateFlow(0L)
    val total: StateFlow<Long> = _total

    private val _scanning = MutableStateFlow(false)
    val scanning: StateFlow<Boolean> = _scanning

    private val _progress = MutableStateFlow(0f)
    val progress: StateFlow<Float> = _progress

    val toast = MutableSharedFlow<String>(extraBufferCapacity = 8)

    /** UI 触发的后台协程 */
    fun launch(block: suspend () -> Unit) = scope.launch { runCatching { block() } }

    private fun client() = DaemonManager.client()

    private fun err(j: kotlinx.serialization.json.JsonObject): String? =
        if (j["ok"]?.jsonPrimitive?.booleanOrNull == true) null
        else j["err"]?.jsonPrimitive?.contentOrNull ?: "unknown error"

    // ---------- daemon 生命周期 ----------

    private fun parseAttached(who: JsonObject?): AttachedInfo? {
        if (who?.get("attached")?.jsonPrimitive?.booleanOrNull != true) return null
        val pid = who["pid"]?.jsonPrimitive?.intOrNull ?: 0
        if (pid <= 0) return null
        return AttachedInfo(
            pid = pid,
            name = who["name"]?.jsonPrimitive?.contentOrNull ?: "",
            arch = who["arch"]?.jsonPrimitive?.contentOrNull ?: "",
            bitness = who["bitness"]?.jsonPrimitive?.intOrNull ?: 0,
            engine = who["engine"]?.jsonPrimitive?.contentOrNull ?: "",
        )
    }

    suspend fun refreshStatus() {
        val running = DaemonManager.isRunning()
        if (!running) {
            _daemon.value = DaemonStatus(false)
            return
        }
        val ping = runCatching { client().request("ping") }.getOrNull()
        val ds = runCatching { client().request("driver_status") }.getOrNull()
        _daemon.value = DaemonStatus(
            running = true,
            version = ping?.get("version")?.jsonPrimitive?.contentOrNull ?: "",
            driverReady = ds?.get("ready")?.jsonPrimitive?.booleanOrNull ?: false,
            driverMode = ds?.get("mode")?.jsonPrimitive?.contentOrNull ?: "",
        )
        val who = runCatching { client().request("who") }.getOrNull()
        _attached.value = parseAttached(who)
    }

    suspend fun startDaemon(ctx: Context) {
        DaemonManager.start(ctx)
            .onSuccess {
                EngineService.start(ctx)
                setGuard()
                refreshStatus()
                toast.emit("daemon 已启动")
            }
            .onFailure { toast.emit("启动失败: ${it.message}") }
    }

    /** 注册 root 看门狗：App 被系统杀死时由 daemon 拉活 */
    suspend fun setGuard() {
        runCatching {
            client().request("set_guard") {
                put("pid", android.os.Process.myPid())
                put("package", "com.luoli.modifier")
            }
        }
    }

    suspend fun stopDaemon(ctx: Context) {
        DaemonManager.stop(ctx)
        EngineService.stop(ctx)
        _attached.value = null
        _hits.value = emptyList()
        refreshStatus()
        toast.emit("daemon 已停止")
    }

    // ---------- 进程 ----------

    suspend fun listProcesses(ctx: Context? = null, includeSystem: Boolean = false): List<ProcessUi> {
        val resp = runCatching {
            client().request("list_processes") {
                put("all", true)
                put("include_system", includeSystem)
            }
        }.getOrNull() ?: return emptyList()

        val arr = resp["processes"]?.jsonArray ?: return emptyList()
        val pm = ctx?.packageManager
        val myPkg = ctx?.packageName ?: "com.luoli.modifier"
        val myPid = android.os.Process.myPid()

        val list = arr.mapNotNull { el ->
            val o = el.jsonObject
            val pid = o["pid"]?.jsonPrimitive?.intOrNull ?: return@mapNotNull null
            val name = o["name"]?.jsonPrimitive?.contentOrNull ?: ""
            val uid = o["uid"]?.jsonPrimitive?.intOrNull ?: 0
            val fg = o["foreground"]?.jsonPrimitive?.booleanOrNull ?: false

            // 过滤自身与看门狗
            if (pid == myPid || name.startsWith(myPkg) || name == "twt_svc" || name == "engine") {
                return@mapNotNull null
            }

            val pkg = name.substringBefore(':')
            var label = ""
            var isSys = false
            if (pm != null) {
                try {
                    val appInfo = pm.getApplicationInfo(pkg, 0)
                    isSys = (appInfo.flags and android.content.pm.ApplicationInfo.FLAG_SYSTEM) != 0
                    label = pm.getApplicationLabel(appInfo).toString()
                } catch (_: Throwable) {}
            }
            if (!includeSystem && isSys) return@mapNotNull null

            ProcessUi(
                pid = pid,
                name = name,
                uid = uid,
                foreground = fg,
                label = if (label.isNotEmpty() && label != name) label else "",
            )
        }

        return list.sortedWith(
            compareByDescending<ProcessUi> { it.foreground }
                .thenBy { (it.label.ifEmpty { it.name }).lowercase() }
        )
    }

    suspend fun attach(pid: Int) {
        runCatching { client().request("attach") { put("pid", pid) } }
            .onSuccess { resp ->
                val e = err(resp)
                if (e != null) toast.emit("附加失败: $e")
                else {
                    refreshStatus()
                    _hits.value = emptyList()
                    _total.value = 0
                    toast.emit("已附加 ${_attached.value?.name ?: pid.toString()}")
                }
            }
            .onFailure { toast.emit("附加失败: ${it.message}") }
    }

    suspend fun attachByName(name: String) {
        runCatching { client().request("attach_by_name") { put("name", name) } }
            .onSuccess { resp ->
                val e = err(resp)
                if (e != null) toast.emit("附加失败: $e")
                else {
                    refreshStatus()
                    _hits.value = emptyList()
                    _total.value = 0
                    toast.emit("已附加 ${_attached.value?.name ?: name}")
                }
            }
            .onFailure { toast.emit("附加失败: ${it.message}") }
    }

    suspend fun detach() {
        runCatching { client().request("detach") }
        _attached.value = null
        _hits.value = emptyList()
        _total.value = 0
    }

    // ---------- 数值输入解析 ----------

    fun parseValue(type: String, text: String): kotlinx.serialization.json.JsonPrimitive? {
        val t = text.trim()
        if (t.isEmpty()) return null
        return if (type == "f32" || type == "f64") {
            val d = t.toDoubleOrNull() ?: return null
            kotlinx.serialization.json.JsonPrimitive(d)
        } else {
            val v = if (t.startsWith("0x") || t.startsWith("0X")) {
                EngineProtocol.hexToLong(t)
            } else {
                t.toLongOrNull() ?: t.toDoubleOrNull()?.toLong() ?: return null
            }
            kotlinx.serialization.json.JsonPrimitive(v)
        }
    }

    // ---------- 扫描 ----------

    suspend fun search(type: String, valueText: String, align: Int, nameFilter: String, tags: List<String> = emptyList()) {
        if (_attached.value == null) {
            val who = runCatching { client().request("who") }.getOrNull()
            _attached.value = parseAttached(who)
            if (_attached.value == null) {
                toast.emit("请先附加进程")
                return
            }
        }
        val value = parseValue(type, valueText) ?: run { toast.emit("数值无效"); return }
        if (_scanning.value) { toast.emit("扫描进行中"); return }
        _scanning.value = true
        _progress.value = 0f

        val pollJob = scope.launch { pollProgress() }
        try {
            val resp = client().request("search") {
                put("type", type)
                put("value", value)
                put("align", align)
                put("name_filter", nameFilter)
                if (tags.isNotEmpty()) {
                    put("tags", buildJsonArray { tags.forEach { add(JsonPrimitive(it)) } })
                }
            }
            val e = err(resp)
            if (e != null) toast.emit("搜索失败: $e")
            else {
                _total.value = resp["count"]?.jsonPrimitive?.longOrNull ?: 0
                toast.emit("找到 ${_total.value} 个结果${if (resp["truncated"]?.jsonPrimitive?.booleanOrNull == true) "（已截断）" else ""}")
                refreshResults()
            }
        } catch (t: Throwable) {
            toast.emit("搜索失败: ${t.message}")
        } finally {
            pollJob.cancel()
            _scanning.value = false
        }
    }

    suspend fun searchGroup(patterns: List<JsonObject>, baseAlign: Int = 4, nameFilter: String = "", tags: List<String> = emptyList()): Long {
        if (_attached.value == null) {
            val who = runCatching { client().request("who") }.getOrNull()
            _attached.value = parseAttached(who)
        }
        val resp = client().request("search_group") {
            put("patterns", buildJsonArray { patterns.forEach { add(it) } })
            put("base_align", baseAlign)
            put("name_filter", nameFilter)
            if (tags.isNotEmpty()) {
                put("tags", buildJsonArray { tags.forEach { add(JsonPrimitive(it)) } })
            }
        }
        val count = resp["count"]?.jsonPrimitive?.longOrNull ?: 0
        _total.value = count
        refreshResults()
        return count
    }

    suspend fun filter(op: String, type: String, valueText: String?) {
        if (_scanning.value) { toast.emit("扫描进行中"); return }
        _scanning.value = true
        _progress.value = 0f
        val pollJob = scope.launch { pollProgress() }
        try {
            val resp = client().request("filter") {
                put("op", op)
                put("type", type)
                if (valueText != null) {
                    parseValue(type, valueText)?.let { put("value", it) }
                }
            }
            val e = err(resp)
            if (e != null) toast.emit("过滤失败: $e")
            else {
                _total.value = resp["count"]?.jsonPrimitive?.longOrNull ?: 0
                refreshResults()
            }
        } catch (t: Throwable) {
            toast.emit("过滤失败: ${t.message}")
        } finally {
            pollJob.cancel()
            _scanning.value = false
        }
    }

    private suspend fun pollProgress() {
        val c = EngineClient(DaemonManager.SOCKET_NAME)
        while (kotlinx.coroutines.currentCoroutineContext().isActive) {
            delay(300)
            val resp = runCatching { c.request("scan_progress") }.getOrNull() ?: continue
            val done = resp["bytes_done"]?.jsonPrimitive?.longOrNull ?: 0
            val total = resp["bytes_total"]?.jsonPrimitive?.longOrNull ?: 0
            if (total > 0) _progress.value = (done.toDouble() / total).toFloat().coerceIn(0f, 1f)
        }
    }

    suspend fun refreshResults() {
        val resp = runCatching { client().request("results") { put("offset", 0); put("limit", 500) } }
            .getOrNull() ?: return
        val arr = resp["hits"]?.jsonArray ?: return
        _hits.value = arr.mapNotNull { el ->
            val o = el.jsonObject
            HitUi(
                addr = EngineProtocol.hexToLong(o["addr"]?.jsonPrimitive?.contentOrNull ?: return@mapNotNull null),
                type = o["type"]?.jsonPrimitive?.contentOrNull ?: "i32",
                bits = EngineProtocol.hexToLong(o["bits"]?.jsonPrimitive?.contentOrNull ?: "0x0"),
                value = o["value"]?.jsonPrimitive?.doubleOrNull ?: 0.0,
            )
        }
        _total.value = resp["total"]?.jsonPrimitive?.longOrNull ?: _hits.value.size.toLong()
    }

    suspend fun reset() {
        runCatching { client().request("reset") }
        _hits.value = emptyList()
        _total.value = 0
    }

    // ---------- 读写 ----------

    suspend fun write(addr: Long, type: String, valueText: String) {
        val value = parseValue(type, valueText) ?: run { toast.emit("数值无效"); return }
        runCatching { client().request("write") { put("addr", addr); put("type", type); put("value", value) } }
            .onSuccess { resp ->
                val e = err(resp)
                if (e != null) toast.emit("写入失败: $e")
                else toast.emit("写入成功")
                refreshResults()
            }
            .onFailure { toast.emit("写入失败: ${it.message}") }
    }

    suspend fun batchWrite(type: String, valueText: String): Int {
        val value = parseValue(type, valueText) ?: run { toast.emit("数值无效"); return 0 }
        val currentHits = _hits.value
        if (currentHits.isEmpty()) {
            toast.emit("无可用搜索结果")
            return 0
        }
        var count = 0
        currentHits.forEach { h ->
            val res = runCatching {
                client().request("write") {
                    put("addr", h.addr)
                    put("type", type)
                    put("value", value)
                }
            }.getOrNull()
            if (res != null && err(res) == null) count++
        }
        toast.emit("批量修改完成: $count/${currentHits.size}")
        refreshResults()
        return count
    }

    // ---------- 冻结 ----------

    suspend fun freeze(
        addr: Long, type: String, valueText: String, intervalMs: Int,
        guardOffset: Long? = null, guardType: String? = null, guardValue: String? = null
    ) {
        val value = parseValue(type, valueText) ?: run { toast.emit("数值无效"); return }
        runCatching {
            client().request("freeze") {
                put("addr", addr); put("type", type); put("value", value); put("interval_ms", intervalMs)
                if (guardOffset != null) {
                    put("guard_offset", guardOffset)
                    if (guardType != null) put("guard_type", guardType)
                    if (guardValue != null) {
                        parseValue(guardType ?: "i32", guardValue)?.let { put("guard_value", it) }
                    }
                }
            }
        }.onSuccess { resp ->
            val e = err(resp)
            if (e != null) toast.emit("冻结失败: $e") else toast.emit("已冻结")
        }.onFailure { toast.emit("冻结失败: ${it.message}") }
    }

    suspend fun unfreeze(addr: Long) {
        runCatching { client().request("unfreeze") { put("addr", addr) } }
        toast.emit("已解除")
    }

    suspend fun unfreezeAll() {
        runCatching { client().request("unfreeze_all") }
        toast.emit("已全部解除")
    }

    suspend fun frozenList(): List<FrozenUi> {
        val resp = runCatching { client().request("frozen_list") }.getOrNull() ?: return emptyList()
        val arr = resp["entries"]?.jsonArray ?: return emptyList()
        return arr.mapNotNull { el ->
            val o = el.jsonObject
            FrozenUi(
                addr = EngineProtocol.hexToLong(o["addr"]?.jsonPrimitive?.contentOrNull ?: return@mapNotNull null),
                type = o["type"]?.jsonPrimitive?.contentOrNull ?: "i32",
                bits = EngineProtocol.hexToLong(o["bits"]?.jsonPrimitive?.contentOrNull ?: "0x0"),
                value = o["value"]?.jsonPrimitive?.doubleOrNull ?: 0.0,
                fails = o["fails"]?.jsonPrimitive?.intOrNull ?: 0,
            )
        }
    }

    // ---------- 日志 ----------

    suspend fun logs(): List<String> {
        val resp = runCatching { client().request("logs") { put("after", 0) } }.getOrNull() ?: return emptyList()
        val arr = resp["lines"]?.jsonArray ?: return emptyList()
        return arr.map { it.jsonObject["text"]?.jsonPrimitive?.contentOrNull ?: "" }
    }
}
