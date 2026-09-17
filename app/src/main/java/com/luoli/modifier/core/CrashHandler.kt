package com.luoli.modifier.core

import android.content.Context
import android.util.Log
import java.io.File
import java.util.Date

/** 崩溃记录：写入 filesDir/last_crash.txt，状态页可查看 */
object CrashHandler : Thread.UncaughtExceptionHandler {
    private var previous: Thread.UncaughtExceptionHandler? = null
    private var appContext: Context? = null

    fun install(context: Context) {
        appContext = context.applicationContext
        previous = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler(this)
    }

    override fun uncaughtException(t: Thread, e: Throwable) {
        runCatching {
            appContext?.let { ctx ->
                crashFile(ctx).writeText(
                    buildString {
                        appendLine("time: ${Date()}")
                        appendLine("thread: ${t.name}")
                        appendLine(Log.getStackTraceString(e))
                    }
                )
            }
        }
        previous?.uncaughtException(t, e)
    }

    private fun crashFile(ctx: Context) = File(ctx.filesDir, "last_crash.txt")

    fun lastCrash(ctx: Context): String? = runCatching {
        val f = crashFile(ctx)
        if (f.isFile) f.readText().take(6000) else null
    }.getOrNull()

    fun clear(ctx: Context) {
        runCatching { crashFile(ctx).delete() }
    }
}
