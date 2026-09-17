package com.luoli.modifier.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Card
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
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.luoli.modifier.core.EngineRepo
import com.luoli.modifier.core.ProcessUi

@Composable
fun ProcessScreen() {
    var processes by remember { mutableStateOf<List<ProcessUi>>(emptyList()) }
    var loading by remember { mutableStateOf(false) }

    LaunchedEffect(Unit) {
        loading = true
        processes = EngineRepo.listProcesses()
        loading = false
    }

    Column(
        modifier = Modifier.fillMaxSize().padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
        ) {
            Text("运行中的 App 进程", fontSize = 18.sp)
            OutlinedButton(onClick = {
                EngineRepo.launch {
                    loading = true
                    processes = EngineRepo.listProcesses()
                    loading = false
                }
            }) { Text(if (loading) "加载中…" else "刷新") }
        }

        Card(modifier = Modifier.fillMaxWidth()) {
            LazyColumn {
                items(processes, key = { it.pid }) { p ->
                    Row(
                        modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp),
                        horizontalArrangement = Arrangement.SpaceBetween,
                    ) {
                        Column {
                            Text(p.name, fontSize = 14.sp)
                            Text("pid=${p.pid} uid=${p.uid}", fontSize = 11.sp, color = MaterialTheme.colorScheme.outline)
                        }
                        TextButton(onClick = {
                            EngineRepo.launch { EngineRepo.attach(p.pid) }
                        }) { Text("附加") }
                    }
                }
            }
        }
    }
}
