package com.luoli.modifier.core

import android.content.Context
import com.topjohnwu.superuser.Shell
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

/** Root daemon 的提取、启动与停止（libsu） */
object DaemonManager {
    const val SOCKET_NAME = "twt_svc_2778"
    const val PROC_NAME = "twt_svc"

    private var client: EngineClient? = null

    fun client(): EngineClient = client ?: EngineClient(SOCKET_NAME).also { client = it }

    fun binaryFile(ctx: Context): File = File(ctx.filesDir, "engine")

    fun logFile(ctx: Context): File = File(ctx.filesDir, "engine.log")

    /** 从 APK nativeLibraryDir 提取 daemon 并赋予执行权限 */
    fun ensureExtracted(ctx: Context): File {
        val src = File(ctx.applicationInfo.nativeLibraryDir, "libengine.so")
        val dst = binaryFile(ctx)
        if (src.isFile && (!dst.isFile || dst.length() != src.length() || dst.lastModified() < src.lastModified())) {
            src.copyTo(dst, overwrite = true)
        }
        Shell.cmd("chmod 700 '${dst.absolutePath}'").exec()
        return dst
    }

    suspend fun start(ctx: Context): Result<Unit> = withContext(Dispatchers.IO) {
        runCatching {
            val bin = ensureExtracted(ctx)
            val log = logFile(ctx)
            Shell.getShell()
            // exec -a: 伪装 argv[0]；& 后台运行，sh 退出后由 init 接管
            val result = Shell.cmd(
                "exec -a $PROC_NAME '${bin.absolutePath}'" +
                    " -s $SOCKET_NAME -n $PROC_NAME -l '${log.absolutePath}' >/dev/null 2>&1 &"
            ).exec()
            if (result.code != 0) error("shell exit ${result.code}: ${result.err.joinToString()}")
            // 等待 socket 就绪
            var tries = 0
            while (tries < 30) {
                if (isRunning()) return@runCatching
                Thread.sleep(100)
                tries++
            }
            error("daemon did not respond within 3s (root available? driver?)")
        }
    }

    suspend fun stop(ctx: Context): Result<Unit> = withContext(Dispatchers.IO) {
        runCatching {
            runCatching { client().request("stop") }
            Shell.cmd("pkill -f '${binaryFile(ctx).absolutePath}'").exec()
            Unit
        }
    }

    suspend fun isRunning(): Boolean = runCatching {
        client().request("ping")
    }.isSuccess
}
