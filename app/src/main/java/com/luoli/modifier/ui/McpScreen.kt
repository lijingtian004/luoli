package com.luoli.modifier.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.luoli.modifier.core.EngineRepo
import com.luoli.modifier.core.Settings
import com.luoli.modifier.core.SettingsStore
import com.luoli.modifier.mcp.McpServerManager
import com.luoli.modifier.service.EngineService
import java.net.Inet4Address
import java.net.NetworkInterface

@Composable
fun McpScreen() {
    val ctx = LocalContext.current
    val clipboard = LocalClipboardManager.current
    val state by McpServerManager.state.collectAsStateWithLifecycle()

    var settings by remember { mutableStateOf(SettingsStore.load(ctx)) }
    var portText by remember(settings.mcpPort) { mutableStateOf(settings.mcpPort.toString()) }

    fun persist(new: Settings) {
        settings = new
        SettingsStore.save(ctx, new)
    }

    Column(
        modifier = Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Card(modifier = Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("MCP Server", fontSize = 18.sp)
                if (state.running) {
                    Text("运行中: ${state.endpoint}", fontSize = 12.sp, color = MaterialTheme.colorScheme.primary)
                    val ips = remember { lanIps() }
                    if (settings.mcpBindAll && ips.isNotEmpty()) {
                        Text("局域网地址:", fontSize = 12.sp)
                        ips.forEach { ip ->
                            TextButton(onClick = {
                                clipboard.setText(AnnotatedString("http://$ip:${settings.mcpPort}/mcp"))
                            }) { Text("http://$ip:${settings.mcpPort}/mcp (点击复制)", fontSize = 12.sp) }
                        }
                    }
                } else {
                    Text("未运行", fontSize = 13.sp)
                }
                state.error?.let { Text("错误: $it", fontSize = 12.sp, color = MaterialTheme.colorScheme.error) }

                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (state.running) {
                        OutlinedButton(onClick = {
                            persist(settings.copy(mcpEnabled = false))
                            McpServerManager.stop()
                        }) { Text("停止") }
                    } else {
                        Button(onClick = {
                            val port = portText.toIntOrNull()?.coerceIn(1024, 65535) ?: settings.mcpPort
                            val new = settings.copy(mcpEnabled = true, mcpPort = port)
                            persist(new)
                            EngineService.start(ctx)
                            McpServerManager.start(ctx)
                        }) { Text("启动") }
                    }
                    TextButton(onClick = {
                        clipboard.setText(AnnotatedString(settings.mcpToken))
                        EngineRepo.launch { EngineRepo.toast.emit("Token 已复制") }
                    }) { Text("复制 Token") }
                }
            }
        }

        Card(modifier = Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("配置", fontSize = 18.sp)
                OutlinedTextField(
                    value = portText,
                    onValueChange = { portText = it },
                    label = { Text("端口 (默认 8631)") },
                    singleLine = true,
                )
                OutlinedTextField(
                    value = settings.mcpToken,
                    onValueChange = { settings = settings.copy(mcpToken = it) },
                    label = { Text("Bearer Token") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                )
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column(Modifier.weight(1f)) {
                        Text("允许局域网连接 (0.0.0.0)")
                        Text("默认仅本机 127.0.0.1；开启后请保管好 Token",
                            fontSize = 11.sp, color = MaterialTheme.colorScheme.outline)
                    }
                    Switch(
                        checked = settings.mcpBindAll,
                        onCheckedChange = { settings = settings.copy(mcpBindAll = it) },
                    )
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(onClick = {
                        val port = portText.toIntOrNull()?.coerceIn(1024, 65535) ?: settings.mcpPort
                        val wasRunning = state.running
                        if (wasRunning) McpServerManager.stop()
                        val new = settings.copy(mcpPort = port)
                        persist(new)
                        if (new.mcpEnabled) {
                            EngineService.start(ctx)
                            McpServerManager.start(ctx)
                        }
                        EngineRepo.launch { EngineRepo.toast.emit("配置已保存") }
                    }) { Text("保存并应用") }
                    TextButton(onClick = {
                        settings = settings.copy(mcpToken = SettingsStore.newToken())
                        persist(settings)
                        if (state.running) {
                            McpServerManager.stop()
                            McpServerManager.start(ctx)
                        }
                    }) { Text("重新生成 Token") }
                }
            }
        }

        Card(modifier = Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Text("AI Agent 接入", fontSize = 18.sp)
                Text(
                    """
                    1. 启动 MCP Server 并复制 Token
                    2. 在 opencode/Claude Code 配置中添加:
                       type: remote (Streamable HTTP)
                       url: ${state.endpoint.ifEmpty { "http://127.0.0.1:8631/mcp" }}
                       headers: { "Authorization": "Bearer <token>" }
                    3. agent 即可调用 attach/search/filter/write/freeze 等工具
                    """.trimIndent(),
                    fontSize = 12.sp,
                    lineHeight = 16.sp,
                )
            }
        }
    }
}

private fun lanIps(): List<String> = runCatching {
    NetworkInterface.getNetworkInterfaces().asSequence()
        .flatMap { it.inetAddresses.asSequence() }
        .filterIsInstance<Inet4Address>()
        .filter { !it.isLoopbackAddress }
        .map { it.hostAddress ?: "" }
        .filter { it.isNotEmpty() && !it.startsWith("169.254") && !it.startsWith("192.0.0") }
        .toList()
}.getOrDefault(emptyList())
