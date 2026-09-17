package com.luoli.modifier.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.luoli.modifier.core.EngineRepo
import com.luoli.modifier.core.ProcessUi

@Composable
fun ProcessScreen() {
    val ctx = LocalContext.current
    val attached by EngineRepo.attached.collectAsStateWithLifecycle()
    var processes by remember { mutableStateOf<List<ProcessUi>>(emptyList()) }
    var loading by remember { mutableStateOf(false) }
    var searchQuery by remember { mutableStateOf("") }
    var includeSystem by remember { mutableStateOf(false) }

    suspend fun loadProcs() {
        loading = true
        processes = EngineRepo.listProcesses(ctx, includeSystem)
        loading = false
    }

    LaunchedEffect(includeSystem) {
        loadProcs()
    }

    val filtered = remember(processes, searchQuery) {
        if (searchQuery.isBlank()) processes
        else {
            val q = searchQuery.trim().lowercase()
            processes.filter {
                it.name.lowercase().contains(q) ||
                it.label.lowercase().contains(q) ||
                it.pid.toString().contains(q)
            }
        }
    }

    Column(
        modifier = Modifier.fillMaxSize().padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column {
                Text("运行中的进程", fontSize = 18.sp, fontWeight = FontWeight.Bold)
                val statusDesc = if (attached != null) {
                    val meta = listOfNotNull(attached!!.arch.ifEmpty { null }, attached!!.engine.ifEmpty { null }).joinToString(" · ")
                    if (meta.isNotEmpty()) "当前附加: ${attached!!.name} (${attached!!.pid})  [$meta]"
                    else "当前附加: ${attached!!.name} (${attached!!.pid})"
                } else "当前未附加目标"
                Text(
                    statusDesc,
                    fontSize = 12.sp,
                    color = if (attached != null) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.outline,
                )
            }
            OutlinedButton(onClick = {
                EngineRepo.launch { loadProcs() }
            }) { Text(if (loading) "加载中…" else "刷新") }
        }

        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            OutlinedTextField(
                value = searchQuery,
                onValueChange = { searchQuery = it },
                placeholder = { Text("搜索应用名称或包名…", fontSize = 12.sp) },
                singleLine = true,
                modifier = Modifier.weight(1f),
            )
            FilterChip(
                selected = includeSystem,
                onClick = { includeSystem = !includeSystem },
                label = { Text("含系统", fontSize = 11.sp) },
            )
        }

        Card(modifier = Modifier.fillMaxWidth().weight(1f)) {
            if (filtered.isEmpty()) {
                Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Text(if (loading) "正在扫描进程…" else "无匹配应用进程", color = MaterialTheme.colorScheme.outline)
                }
            } else {
                LazyColumn {
                    items(filtered, key = { it.pid }) { p ->
                        val isAttached = attached?.pid == p.pid
                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(horizontal = 12.dp, vertical = 8.dp),
                            horizontalArrangement = Arrangement.SpaceBetween,
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Column(modifier = Modifier.weight(1f).padding(end = 8.dp)) {
                                Row(
                                    verticalAlignment = Alignment.CenterVertically,
                                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                                ) {
                                    if (p.foreground) {
                                        Text(
                                            "前台",
                                            fontSize = 10.sp,
                                            fontWeight = FontWeight.Bold,
                                            color = Color.White,
                                            modifier = Modifier
                                                .background(Color(0xFFE91E63), RoundedCornerShape(4.dp))
                                                .padding(horizontal = 5.dp, vertical = 2.dp),
                                        )
                                    }
                                    if (isAttached) {
                                        Text(
                                            "已附加",
                                            fontSize = 10.sp,
                                            fontWeight = FontWeight.Bold,
                                            color = Color.White,
                                            modifier = Modifier
                                                .background(Color(0xFF4CAF50), RoundedCornerShape(4.dp))
                                                .padding(horizontal = 5.dp, vertical = 2.dp),
                                        )
                                    }
                                    Text(
                                        p.label.ifEmpty { p.name },
                                        fontSize = 14.sp,
                                        fontWeight = FontWeight.SemiBold,
                                    )
                                }
                                if (p.label.isNotEmpty()) {
                                    Text(p.name, fontSize = 11.sp, color = MaterialTheme.colorScheme.outline)
                                }
                                Text(
                                    "pid=${p.pid}  uid=${p.uid}",
                                    fontSize = 11.sp,
                                    color = MaterialTheme.colorScheme.outline,
                                )
                            }
                            if (isAttached) {
                                OutlinedButton(
                                    onClick = { EngineRepo.launch { EngineRepo.detach() } },
                                    colors = ButtonDefaults.outlinedButtonColors(contentColor = MaterialTheme.colorScheme.error),
                                ) {
                                    Text("脱离", fontSize = 12.sp)
                                }
                            } else {
                                Button(onClick = {
                                    EngineRepo.launch { EngineRepo.attach(p.pid) }
                                }) {
                                    Text("附加", fontSize = 12.sp)
                                }
                            }
                        }
                        HorizontalDivider(thickness = 0.5.dp, color = MaterialTheme.colorScheme.outlineVariant)
                    }
                }
            }
        }
    }
}
