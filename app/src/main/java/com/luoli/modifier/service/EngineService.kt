package com.luoli.modifier.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.IBinder
import com.luoli.modifier.core.EngineRepo
import com.luoli.modifier.core.SettingsStore
import com.luoli.modifier.mcp.McpServerManager
import kotlinx.coroutines.runBlocking

/** 前台服务：提升进程优先级并承载 MCP server */
class EngineService : Service() {

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        nm.createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "引擎", NotificationManager.IMPORTANCE_MIN)
        )
        val notification: Notification =
            Notification.Builder(this, CHANNEL_ID)
                .setContentTitle("引擎运行中")
                .setSmallIcon(android.R.drawable.ic_menu_manage)
                .setOngoing(true)
                .build()
        startForeground(NOTIFY_ID, notification)

        if (SettingsStore.load(this).mcpEnabled) {
            McpServerManager.start(this)
        }
        // 向 root daemon 注册看门狗（守护拉活本服务后也自动重注册）
        runBlocking {
            runCatching { EngineRepo.setGuard() }
        }
        return START_STICKY
    }

    companion object {
        private const val CHANNEL_ID = "engine"
        private const val NOTIFY_ID = 1

        fun start(context: Context) {
            context.startForegroundService(Intent(context, EngineService::class.java))
        }

        fun stop(context: Context) {
            context.stopService(Intent(context, EngineService::class.java))
        }
    }
}
