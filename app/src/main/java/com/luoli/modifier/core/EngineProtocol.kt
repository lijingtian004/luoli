package com.luoli.modifier.core

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonObjectBuilder
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import java.io.ByteArrayOutputStream

/** 帧协议: [u32 LE 长度][JSON UTF-8]，JSON: {id, cmd, ...params} -> {ok, id, ...} */
object EngineProtocol {
    val json = Json { ignoreUnknownKeys = true; isLenient = true }

    fun request(cmd: String, id: Long = 0, params: JsonObjectBuilder.() -> Unit = {}): ByteArray {
        val obj = buildJsonObject {
            put("cmd", cmd)
            put("id", id)
            params()
        }
        return frame(obj.toString().toByteArray(Charsets.UTF_8))
    }

    fun frame(data: ByteArray): ByteArray {
        val out = ByteArrayOutputStream(data.size + 4)
        var len = data.size
        out.write(len and 0xFF)
        out.write((len shr 8) and 0xFF)
        out.write((len shr 16) and 0xFF)
        out.write((len shr 24) and 0xFF)
        out.write(data)
        return out.toByteArray()
    }

    fun parse(bytes: ByteArray): JsonObject =
        json.parseToJsonElement(String(bytes, Charsets.UTF_8)).jsonObjectSafe()

    private fun kotlinx.serialization.json.JsonElement.jsonObjectSafe(): JsonObject =
        this as? JsonObject ?: error("response is not an object")

    fun hexToLong(s: String): Long = s.removePrefix("0x").removePrefix("0X").toLong(16)

    fun longToHex(v: Long): String = "0x" + java.lang.Long.toHexString(v)

    fun longToQHex(v: Long): String = "0x" + String.format("%016x", v)
}
