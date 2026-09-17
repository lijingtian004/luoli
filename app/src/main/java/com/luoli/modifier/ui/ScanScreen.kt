package com.luoli.modifier.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.FilterChip
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.luoli.modifier.core.EngineRepo
import com.luoli.modifier.core.HitUi

private val TYPES = listOf("i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "f32", "f64")
private val FLOAT_TYPES = setOf("f32", "f64")

private data class FilterOpDef(val op: String, val label: String, val needsValue: Boolean)

private val FILTER_OPS = listOf(
    FilterOpDef("eq", "=", true),
    FilterOpDef("ne", "≠", true),
    FilterOpDef("gt", ">", true),
    FilterOpDef("ge", "≥", true),
    FilterOpDef("lt", "<", true),
    FilterOpDef("le", "≤", true),
    FilterOpDef("inc", "增", false),
    FilterOpDef("dec", "减", false),
    FilterOpDef("changed", "变", false),
    FilterOpDef("unchanged", "未变", false),
)

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun ScanScreen() {
    val attached by EngineRepo.attached.collectAsStateWithLifecycle()
    val hits by EngineRepo.hits.collectAsStateWithLifecycle()
    val total by EngineRepo.total.collectAsStateWithLifecycle()
    val scanning by EngineRepo.scanning.collectAsStateWithLifecycle()
    val progress by EngineRepo.progress.collectAsStateWithLifecycle()

    var type by remember { mutableStateOf("i32") }
    var value by remember { mutableStateOf("") }
    var align by remember { mutableStateOf("4") }
    var nameFilter by remember { mutableStateOf("") }
    var filterOp by remember { mutableStateOf("eq") }
    var filterValue by remember { mutableStateOf("") }

    var dialogHit by remember { mutableStateOf<HitUi?>(null) }

    // 单一 LazyColumn 作为唯一滚动容器（避免嵌套滚动崩溃）
    LazyColumn(
        modifier = Modifier.fillMaxSize().padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        item {
            Text(
                if (attached != null) "目标: ${attached!!.name} (pid=${attached!!.pid})"
                else "未附加进程（先到「进程」页附加）",
                fontSize = 13.sp,
                color = MaterialTheme.colorScheme.primary,
            )
        }

        item {
            FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                TYPES.forEach { t ->
                    FilterChip(
                        selected = type == t,
                        onClick = {
                            type = t
                            if (t in FLOAT_TYPES) align = "4"
                        },
                        label = { Text(t) },
                    )
                }
            }
        }

        item {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(
                    value = value,
                    onValueChange = { value = it },
                    label = { Text("数值") },
                    modifier = Modifier.weight(1f),
                    singleLine = true,
                )
                OutlinedTextField(
                    value = align,
                    onValueChange = { align = it },
                    label = { Text("对齐") },
                    modifier = Modifier.weight(0.35f),
                    singleLine = true,
                )
            }
        }

        item {
            OutlinedTextField(
                value = nameFilter,
                onValueChange = { nameFilter = it },
                label = { Text("区域过滤 (如 CaLe/heap，留空全部)") },
                modifier = Modifier.fillMaxWidth(),
                singleLine = true,
            )
        }

        item {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(
                    onClick = {
                        EngineRepo.launch {
                            EngineRepo.search(type, value, align.toIntOrNull() ?: 4, nameFilter)
                        }
                    },
                    enabled = !scanning,
                ) { Text(if (scanning) "扫描中…" else "搜索") }
                OutlinedButton(onClick = { EngineRepo.launch { EngineRepo.reset() } }) { Text("清空") }
                OutlinedButton(onClick = { EngineRepo.launch { EngineRepo.refreshResults() } }) { Text("刷新") }
            }
        }

        if (scanning) {
            item {
                LinearProgressIndicator(
                    progress = { progress },
                    modifier = Modifier.fillMaxWidth(),
                )
            }
        }

        item {
            Text("结果: $total（显示前 ${hits.size}）", fontSize = 13.sp)
        }

        item {
            FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                FILTER_OPS.forEach { f ->
                    FilterChip(
                        selected = filterOp == f.op,
                        onClick = { filterOp = f.op },
                        label = { Text(f.label) },
                    )
                }
            }
        }

        item {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                val needsValue = FILTER_OPS.firstOrNull { it.op == filterOp }?.needsValue ?: true
                if (needsValue) {
                    OutlinedTextField(
                        value = filterValue,
                        onValueChange = { filterValue = it },
                        label = { Text("过滤值") },
                        modifier = Modifier.weight(1f),
                        singleLine = true,
                    )
                }
                Button(
                    onClick = {
                        EngineRepo.launch {
                            EngineRepo.filter(
                                filterOp, type,
                                if (FILTER_OPS.firstOrNull { it.op == filterOp }?.needsValue == true) filterValue else null,
                            )
                        }
                    },
                    enabled = !scanning && total > 0,
                ) { Text("过滤") }
            }
        }

        item {
            Card(modifier = Modifier.fillMaxWidth()) {
                Text(
                    "点击地址行可写入/冻结",
                    modifier = Modifier.padding(12.dp),
                    fontSize = 11.sp,
                    color = MaterialTheme.colorScheme.outline,
                )
            }
        }

        items(hits, key = { it.addr }) { h ->
            Card(modifier = Modifier.fillMaxWidth()) {
                Row(
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp),
                    horizontalArrangement = Arrangement.SpaceBetween,
                ) {
                    Column {
                        Text(
                            "0x%016X".format(h.addr),
                            fontSize = 12.sp,
                            fontFamily = androidx.compose.ui.text.font.FontFamily.Monospace,
                        )
                        Text("(${h.type})", fontSize = 10.sp, color = MaterialTheme.colorScheme.outline)
                    }
                    Text(
                        if (h.type in FLOAT_TYPES) "%.6f".format(h.value) else h.value.toLong().toString(),
                        fontSize = 14.sp,
                    )
                    TextButton(onClick = { dialogHit = h }) { Text("操作") }
                }
            }
        }
    }

    dialogHit?.let { h ->
        var editValue by remember(h.addr) { mutableStateOf(h.value.toLong().toString()) }
        AlertDialog(
            onDismissRequest = { dialogHit = null },
            title = { Text("0x%016X".format(h.addr), fontSize = 14.sp) },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text("当前值: ${h.value} (${h.type})", fontSize = 12.sp)
                    OutlinedTextField(
                        value = editValue,
                        onValueChange = { editValue = it },
                        label = { Text("新数值") },
                        singleLine = true,
                    )
                }
            },
            confirmButton = {
                TextButton(onClick = {
                    EngineRepo.launch { EngineRepo.write(h.addr, h.type, editValue) }
                    dialogHit = null
                }) { Text("写入") }
            },
            dismissButton = {
                Row {
                    TextButton(onClick = {
                        EngineRepo.launch { EngineRepo.freeze(h.addr, h.type, editValue, 150) }
                        dialogHit = null
                    }) { Text("冻结") }
                    TextButton(onClick = {
                        EngineRepo.launch { EngineRepo.unfreeze(h.addr) }
                        dialogHit = null
                    }) { Text("解冻") }
                    TextButton(onClick = { dialogHit = null }) { Text("取消") }
                }
            },
        )
    }
}
