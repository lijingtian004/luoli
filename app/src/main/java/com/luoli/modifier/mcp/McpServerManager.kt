package com.luoli.modifier.mcp

import android.content.Context
import com.luoli.modifier.core.SettingsStore
import io.ktor.http.HttpStatusCode
import io.ktor.server.application.ApplicationCallPipeline
import io.ktor.server.application.call
import io.ktor.server.cio.CIO
import io.ktor.server.engine.EmbeddedServer
import io.ktor.server.engine.embeddedServer
import io.ktor.server.response.respondText
import io.modelcontextprotocol.kotlin.sdk.server.mcpStreamableHttp
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

data class McpState(
    val running: Boolean = false,
    val error: String? = null,
    val endpoint: String = "",
)

/** MCP Streamable HTTP Server 托管（Ktor CIO + Bearer Token） */
object McpServerManager {
    private var engine: EmbeddedServer<*, *>? = null

    private val _state = MutableStateFlow(McpState())
    val state: StateFlow<McpState> = _state

    fun start(ctx: Context) {
        if (engine != null) return
        val settings = SettingsStore.load(ctx)
        val host = if (settings.mcpBindAll) "0.0.0.0" else "127.0.0.1"
        val endpoint = "http://$host:${settings.mcpPort}/mcp"
        try {
            val server = McpTools.buildServer()
            val token = settings.mcpToken
            engine = embeddedServer(CIO, host = host, port = settings.mcpPort) {
                // Bearer Token 鉴权（仅 /mcp 路径）
                intercept(ApplicationCallPipeline.Plugins) {
                    if (call.request.local.uri.startsWith("/mcp")) {
                        val auth = call.request.headers["Authorization"]
                        if (auth != "Bearer $token") {
                            call.respondText("unauthorized", status = HttpStatusCode.Unauthorized)
                            finish()
                        }
                    }
                }
                mcpStreamableHttp { server }
            }.also { it.start(wait = false) }
            _state.value = McpState(running = true, error = null, endpoint = endpoint)
        } catch (t: Throwable) {
            engine = null
            _state.value = McpState(running = false, error = t.message ?: "start failed", endpoint = endpoint)
        }
    }

    fun stop() {
        runCatching { engine?.stop(500, 2000) }
        engine = null
        _state.value = McpState(running = false)
    }
}
