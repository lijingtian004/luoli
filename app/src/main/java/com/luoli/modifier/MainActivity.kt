package com.luoli.modifier

import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.Memory
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import com.luoli.modifier.core.EngineRepo
import com.luoli.modifier.ui.LuoliTheme
import com.luoli.modifier.ui.ScanScreen
import com.luoli.modifier.ui.StatusScreen
import com.luoli.modifier.ui.ProcessScreen
import kotlinx.coroutines.launch

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent { LuoliTheme { AppRoot() } }
    }
}

private data class Tab(val route: String, val label: String, val icon: ImageVector)

@Composable
fun AppRoot() {
    val nav = rememberNavController()
    val tabs = listOf(
        Tab("status", "状态", Icons.Filled.Info),
        Tab("process", "进程", Icons.Filled.Memory),
        Tab("scan", "扫描", Icons.Filled.Search),
        Tab("mcp", "MCP", Icons.Filled.Wifi),
    )
    val backStack by nav.currentBackStackEntryAsState()
    val currentRoute = backStack?.destination?.route

    val ctx = androidx.compose.ui.platform.LocalContext.current
    LaunchedEffect(Unit) {
        EngineRepo.refreshStatus()
        launch {
            EngineRepo.toast.collect { msg ->
                Toast.makeText(ctx, msg, Toast.LENGTH_SHORT).show()
            }
        }
        // MCP 开机自启（若配置启用）
        launch {
            if (com.luoli.modifier.core.SettingsStore.load(ctx).mcpEnabled) {
                com.luoli.modifier.service.EngineService.start(ctx)
            }
        }
        // 引导加入电池优化白名单（仅首次）
        launch {
            val prefs = ctx.getSharedPreferences("settings", android.content.Context.MODE_PRIVATE)
            val pm = ctx.getSystemService(android.content.Context.POWER_SERVICE) as android.os.PowerManager
            if (!pm.isIgnoringBatteryOptimizations(ctx.packageName) && !prefs.getBoolean("battery_prompted", false)) {
                prefs.edit().putBoolean("battery_prompted", true).apply()
                runCatching {
                    ctx.startActivity(
                        android.content.Intent(
                            android.provider.Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                            android.net.Uri.parse("package:${ctx.packageName}")
                        )
                    )
                }
            }
        }
    }

    Scaffold(
        bottomBar = {
            NavigationBar {
                tabs.forEach { tab ->
                    NavigationBarItem(
                        selected = currentRoute == tab.route,
                        onClick = {
                            if (currentRoute != tab.route) {
                                nav.navigate(tab.route) {
                                    popUpTo(nav.graph.startDestinationId) { saveState = true }
                                    launchSingleTop = true
                                    restoreState = true
                                }
                            }
                        },
                        icon = { Icon(tab.icon, contentDescription = tab.label) },
                        label = { Text(tab.label) },
                    )
                }
            }
        }
    ) { padding ->
        NavHost(
            navController = nav,
            startDestination = "status",
            modifier = Modifier.padding(padding),
        ) {
            composable("status") { StatusScreen() }
            composable("process") { ProcessScreen() }
            composable("scan") { ScanScreen() }
            composable("mcp") { com.luoli.modifier.ui.McpScreen() }
        }
    }
}
