package com.luoli.modifier.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.luoli.modifier.core.EngineRepo
import com.luoli.modifier.core.FrozenUi

@Composable
fun StatusScreen() {
    val daemon by EngineRepo.daemon.collectAsStateWithLifecycle()
    val attached by EngineRepo.attached.collectAsStateWithLifecycle()
    val ctx = LocalContext.current
    var frozen by remember { mutableStateOf<List<FrozenUi>>(emptyList()) }
    var logs by remember { mutableStateOf<List<String>>(emptyList()) }

    LaunchedEffect(Unit) { frozen = EngineRepo.frozenList() }

    val lastCrash = remember { com.luoli.modifier.core.CrashHandler.lastCrash(ctx) }

    Column(
        modifier = Modifier.fillMaxSize().padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        if (lastCrash != null) {
            Card(modifier = Modifier.fillMaxWidth()) {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween,
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text("上次崩溃记录", fontSize = 18.sp, color = MaterialTheme.colorScheme.error)
                        TextButton(onClick = {
                            com.luoli.modifier.core.CrashHandler.clear(ctx)
                            EngineRepo.launch { EngineRepo.toast.emit("已清除") }
                        }) { Text("清除") }
                    }
                    LazyColumn(modifier = Modifier.heightIn(max = 160.dp)) {
                        items(lastCrash.lines()) { l -> Text(l, fontSize = 10.sp, lineHeight = 12.sp) }
                    }
                }
            }
        }

        Card(modifier = Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Daemon", fontSize = 18.sp)
                Text(if (daemon.running) "运行中 v${daemon.version}" else "未运行")
                Text("驱动: " + if (daemon.driverReady) "已连接 (${daemon.driverMode})" else "未连接")
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (daemon.running) {
                        OutlinedButton(onClick = {
                            EngineRepo.launch { EngineRepo.stopDaemon(ctx) }
                        }) { Text("停止") }
                    } else {
                        Button(onClick = {
                            EngineRepo.launch { EngineRepo.startDaemon(ctx) }
                        }) { Text("启动 (需要 root)") }
                    }
                    TextButton(onClick = {
                        if (android.provider.Settings.canDrawOverlays(ctx)) {
                            com.luoli.modifier.service.OverlayService.start(ctx)
                        } else {
                            runCatching {
                                ctx.startActivity(
                                    android.content.Intent(
                                        android.provider.Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                                        android.net.Uri.parse("package:${ctx.packageName}")
                                    )
                                )
                            }
                        }
                    }) { Text("悬浮窗") }
                    TextButton(onClick = {
                        EngineRepo.launch {
                            EngineRepo.refreshStatus()
                            frozen = EngineRepo.frozenList()
                        }
                    }) { Text("刷新") }
                }
            }
        }

        Card(modifier = Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("目标进程", fontSize = 18.sp)
                val a = attached
                if (a == null) Text("未附加")
                else {
                    Text("${a.name}  (pid=${a.pid})")
                    Row {
                        OutlinedButton(onClick = {
                            EngineRepo.launch { EngineRepo.detach() }
                        }) { Text("脱离") }
                    }
                }
            }
        }

        Card(modifier = Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text("冻结列表 (${frozen.size})", fontSize = 18.sp)
                    if (frozen.isNotEmpty()) {
                        TextButton(onClick = {
                            EngineRepo.launch {
                                EngineRepo.unfreezeAll()
                                frozen = EngineRepo.frozenList()
                            }
                        }) { Text("全部解除") }
                    }
                }
                if (frozen.isEmpty()) Text("暂无冻结项", fontSize = 12.sp)
                LazyColumn(modifier = Modifier.heightIn(max = 200.dp)) {
                    items(frozen) { f ->
                        Row(
                            Modifier.fillMaxWidth().padding(vertical = 2.dp),
                            horizontalArrangement = Arrangement.SpaceBetween,
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Text("0x%016X".format(f.addr), fontSize = 12.sp)
                            Text("${f.value}${if (f.fails > 0) " ⚠${f.fails}" else ""}", fontSize = 12.sp)
                            TextButton(onClick = {
                                EngineRepo.launch {
                                    EngineRepo.unfreeze(f.addr)
                                    frozen = EngineRepo.frozenList()
                                }
                            }) { Text("解除") }
                        }
                        HorizontalDivider()
                    }
                }
            }
        }

        Card(modifier = Modifier.fillMaxWidth().weight(1f)) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text("Daemon 日志", fontSize = 18.sp)
                    TextButton(onClick = {
                        EngineRepo.launch { logs = EngineRepo.logs() }
                    }) { Text("读取") }
                }
                LazyColumn {
                    items(logs) { l -> Text(l, fontSize = 11.sp, lineHeight = 14.sp) }
                }
            }
        }
    }
}
