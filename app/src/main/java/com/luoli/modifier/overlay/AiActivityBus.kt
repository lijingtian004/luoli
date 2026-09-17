package com.luoli.modifier.overlay

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** AI agent 活动事件流（MCP 工具调用），供悬浮窗 AI 模式展示 */
object AiActivityBus {
    private const val MAX = 80

    private val _lines = MutableStateFlow<List<String>>(emptyList())
    val lines: StateFlow<List<String>> = _lines

    fun log(msg: String) {
        val ts = SimpleDateFormat("HH:mm:ss", Locale.US).format(Date())
        val list = _lines.value + "[$ts] $msg"
        _lines.value = if (list.size > MAX) list.takeLast(MAX) else list
    }
}
