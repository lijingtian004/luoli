package com.luoli.modifier.core

import android.net.LocalSocket
import android.net.LocalSocketAddress
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonObjectBuilder
import java.io.DataInputStream
import java.io.EOFException

/**
 * 与 root daemon 的 JSON 帧客户端。
 * 每个请求使用独立临时连接（daemon 每连接一线程），扫描进行中可另开连接查进度。
 */
class EngineClient(private val socketName: String = DaemonManager.SOCKET_NAME) {

    suspend fun request(cmd: String, params: JsonObjectBuilder.() -> Unit = {}): JsonObject =
        withContext(Dispatchers.IO) {
            val socket = LocalSocket()
            try {
                socket.connect(LocalSocketAddress(socketName, LocalSocketAddress.Namespace.ABSTRACT))
                socket.soTimeout = REQUEST_TIMEOUT_MS
                val id = System.nanoTime()
                socket.outputStream.write(EngineProtocol.request(cmd, id, params))
                socket.outputStream.flush()

                val input = DataInputStream(socket.inputStream)
                val len = readIntLE(input)
                require(len in 1..MAX_FRAME) { "bad frame length: $len" }
                val buf = ByteArray(len)
                input.readFully(buf)
                EngineProtocol.parse(buf)
            } finally {
                runCatching { socket.close() }
            }
        }

    private fun readIntLE(input: DataInputStream): Int {
        var v = 0
        var shift = 0
        repeat(4) {
            val b = input.read()
            if (b < 0) throw EOFException("socket closed")
            v = v or ((b and 0xFF) shl shift)
            shift += 8
        }
        return v
    }

    companion object {
        private const val REQUEST_TIMEOUT_MS = 600_000
        private const val MAX_FRAME = 16 * 1024 * 1024
    }
}
