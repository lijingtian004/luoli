package com.luoli.modifier.core

import android.content.Context
import java.util.UUID

/** MCP Server 与引擎的持久化设置 */
data class Settings(
    val mcpEnabled: Boolean,
    val mcpPort: Int,
    val mcpToken: String,
    val mcpBindAll: Boolean,
)

object SettingsStore {
    private const val FILE = "settings"

    fun load(ctx: Context): Settings {
        val sp = ctx.getSharedPreferences(FILE, Context.MODE_PRIVATE)
        return Settings(
            mcpEnabled = sp.getBoolean("mcp_enabled", false),
            mcpPort = sp.getInt("mcp_port", 8631),
            mcpToken = sp.getString("mcp_token", null) ?: newToken().also {
                sp.edit().putString("mcp_token", it).apply()
            },
            mcpBindAll = sp.getBoolean("mcp_bind_all", false),
        )
    }

    fun save(ctx: Context, s: Settings) {
        ctx.getSharedPreferences(FILE, Context.MODE_PRIVATE).edit()
            .putBoolean("mcp_enabled", s.mcpEnabled)
            .putInt("mcp_port", s.mcpPort)
            .putString("mcp_token", s.mcpToken)
            .putBoolean("mcp_bind_all", s.mcpBindAll)
            .apply()
    }

    fun newToken(): String = UUID.randomUUID().toString().replace("-", "")
}
