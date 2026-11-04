package com.sidedroid.console

import java.io.File
import java.io.RandomAccessFile
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.doubleOrNull
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put

data class HostSettings(
    val width: Int = 0,
    val height: Int = 0,
    val hz: Int = 0,
    val maxKbps: Int = 0,
    val allowWireless: Boolean = true,
    val mirror: Boolean = false,
)

data class PairedDevice(val id: String, val name: String)

data class HostStatus(
    val reachable: Boolean = false,
    val connected: Boolean = false,
    val device: String = "",
    val transport: String = "",
    val error: String = "",
    val encoder: String = "",
    val codec: String = "",
    val displayW: Int = 0,
    val displayH: Int = 0,
    val hz: Int = 0,
    val encodeW: Int = 0,
    val encodeH: Int = 0,
    val fps: Int = 0,
    val kbps: Int = 0,
    val rttMs: Int = 0,
    val decodeMs: Int = 0,
    val encodeMs: Double = 0.0,
    val measuredFps: Double = 0.0,
    val lossPercent: Double = 0.0,
    val virtualDisplayInstalled: Boolean = true,
    val adbFound: Boolean = true,
    val pairingCode: String = "",
    val pairingSeconds: Int = 0,
    val settings: HostSettings = HostSettings(),
    val usbUnauthorized: Boolean = false,
    val usbCount: Int = 0,
    val paired: List<PairedDevice> = emptyList(),
)

/** Parses the host's status line. Unknown or missing fields keep their defaults. */
fun parseStatus(line: String): HostStatus {
    val o = Json.parseToJsonElement(line).jsonObject
    fun s(k: String, src: JsonObject = o) = src[k]?.jsonPrimitive?.content ?: ""
    fun i(k: String, src: JsonObject = o) = src[k]?.jsonPrimitive?.intOrNull ?: 0
    fun d(k: String) = o[k]?.jsonPrimitive?.doubleOrNull ?: 0.0
    fun b(k: String, def: Boolean = false, src: JsonObject = o) = src[k]?.jsonPrimitive?.booleanOrNull ?: def
    val set = o["settings"]?.jsonObject ?: JsonObject(emptyMap())
    val usb = o["usbDevices"]?.jsonArray?.map { it.jsonObject } ?: emptyList()
    return HostStatus(
        reachable = true,
        connected = b("connected"),
        device = s("device"),
        transport = s("transport"),
        error = s("error"),
        encoder = s("encoder"),
        codec = s("codec"),
        displayW = i("displayW"),
        displayH = i("displayH"),
        hz = i("hz"),
        encodeW = i("encodeW"),
        encodeH = i("encodeH"),
        fps = i("fps"),
        kbps = i("kbps"),
        rttMs = i("rttMs"),
        decodeMs = i("decodeMs"),
        encodeMs = d("encodeMs"),
        measuredFps = d("measuredFps"),
        lossPercent = d("lossPercent"),
        virtualDisplayInstalled = b("virtualDisplayInstalled", true),
        adbFound = b("adbFound", true),
        pairingCode = s("pairingCode"),
        pairingSeconds = i("pairingSeconds"),
        settings = HostSettings(i("width", set), i("height", set), i("hz", set), i("maxKbps", set), b("allowWireless", true, set), b("mirror", false, set)),
        usbUnauthorized = usb.any { s("state", it) == "unauthorized" },
        usbCount = usb.count { s("state", it) == "device" },
        paired = o["pairedDevices"]?.jsonArray?.map { PairedDevice(s("id", it.jsonObject), s("name", it.jsonObject)) } ?: emptyList(),
    )
}

/**
 * Talks to sidedroid-host over its pipe. All calls block briefly, so run them off
 * the UI thread. Starts the host if it is not running, since the console is often
 * what the user opens first.
 */
class HostClient(private val pipePath: String = """\\.\pipe\SideDroidHost""") {
    private var pipe: RandomAccessFile? = null
    private var launchAttemptedAt = 0L

    private fun connect(): RandomAccessFile? {
        pipe?.let { return it }
        return try {
            RandomAccessFile(pipePath, "rw").also { pipe = it }
        } catch (_: Exception) {
            startHostOnce()
            null
        }
    }

    private fun startHostOnce() {
        val now = System.currentTimeMillis()
        if (now - launchAttemptedAt < 10_000) return
        launchAttemptedAt = now
        val installDir = File(System.getProperty("compose.application.resources.dir") ?: return)
        var dir: File? = installDir
        while (dir != null) {
            val exe = File(dir, "sidedroid-host.exe")
            if (exe.exists()) {
                runCatching { ProcessBuilder(exe.absolutePath).directory(dir).start() }
                return
            }
            dir = dir.parentFile
        }
    }

    private fun exchange(request: String): String? {
        val p = connect() ?: return null
        return try {
            p.write((request + "\n").toByteArray(Charsets.UTF_8))
            val bytes = java.io.ByteArrayOutputStream()
            while (true) {
                val c = p.read()
                if (c < 0) throw java.io.EOFException()
                if (c == '\n'.code) break
                bytes.write(c)
            }
            bytes.toString(Charsets.UTF_8)
        } catch (_: Exception) {
            runCatching { p.close() }
            pipe = null
            null
        }
    }

    fun status(): HostStatus = exchange("""{"cmd":"status"}""")?.let { runCatching { parseStatus(it) }.getOrNull() } ?: HostStatus()

    fun setDisplay(width: Int, height: Int, hz: Int, maxKbps: Int, allowWireless: Boolean, mirror: Boolean) {
        val body = buildJsonObject {
            put("cmd", "set")
            put("width", width)
            put("height", height)
            put("hz", hz)
            put("maxKbps", maxKbps)
            put("allowWireless", allowWireless)
            put("mirror", mirror)
        }
        exchange(body.toString())
    }

    fun startPairing() { exchange("""{"cmd":"pair_start"}""") }
    fun cancelPairing() { exchange("""{"cmd":"pair_cancel"}""") }
    fun disconnect() { exchange("""{"cmd":"disconnect"}""") }
    fun installDriver() { exchange("""{"cmd":"install_driver"}""") }
    fun unpair(id: String) {
        exchange(buildJsonObject { put("cmd", "unpair"); put("id", id) }.toString())
    }
}
