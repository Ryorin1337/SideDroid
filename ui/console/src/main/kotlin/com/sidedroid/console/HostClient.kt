package com.sidedroid.console

import com.sidedroid.design.DisplayOptionsUi
import com.sidedroid.design.UpdateStage
import com.sidedroid.design.UpdateUi
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
    val scale: Int = 0,
    val encoder: String = "",
    val fill: Int = 0,
)

data class EncoderOption(val id: String, val label: String)

data class PairedDevice(val id: String, val name: String)

/** One connected device, with its own stream and its own display settings. */
data class ConnectedDevice(
    val id: String = "",
    val name: String = "",
    val transport: String = "",
    val error: String = "",
    val encoder: String = "",
    val codec: String = "",
    val displayW: Int = 0,
    val displayH: Int = 0,
    val encodeW: Int = 0,
    val encodeH: Int = 0,
    val kbps: Int = 0,
    val rttMs: Int = 0,
    val measuredFps: Double = 0.0,
    val crossAdapter: Boolean = false,
    val encoderFallback: Boolean = false,
    val settings: HostSettings = HostSettings(),
    val options: DisplayOptionsUi = DisplayOptionsUi(),
)

data class HostStatus(
    val reachable: Boolean = false,
    val devices: List<ConnectedDevice> = emptyList(),
    val maxDevices: Int = 4,
    val error: String = "",
    val virtualDisplayInstalled: Boolean = true,
    val adbFound: Boolean = true,
    val pairingCode: String = "",
    val pairingSeconds: Int = 0,
    val settings: HostSettings = HostSettings(),
    val usbUnauthorized: Boolean = false,
    val usbCount: Int = 0,
    val paired: List<PairedDevice> = emptyList(),
    val options: DisplayOptionsUi = DisplayOptionsUi(),
    val update: UpdateUi? = null,
    val gpus: List<String> = emptyList(),
    val vddGpu: String = "",
    val vddGpuEditable: Boolean = true,
    val encoders: List<EncoderOption> = emptyList(),
    val consoleAutostart: Boolean = false,
    // True unless the host says otherwise, so that nothing asks before the status is known.
    val consoleAutostartAsked: Boolean = true,
)

/** Parses the host's status line. Unknown or missing fields keep their defaults. */
fun parseStatus(line: String): HostStatus {
    val o = Json.parseToJsonElement(line).jsonObject
    fun s(k: String, src: JsonObject = o) = src[k]?.jsonPrimitive?.content ?: ""
    fun i(k: String, src: JsonObject = o) = src[k]?.jsonPrimitive?.intOrNull ?: 0
    fun d(k: String, src: JsonObject = o) = src[k]?.jsonPrimitive?.doubleOrNull ?: 0.0
    fun b(k: String, def: Boolean = false, src: JsonObject = o) = src[k]?.jsonPrimitive?.booleanOrNull ?: def
    fun settings(src: JsonObject?, allowWireless: Boolean = true): HostSettings {
        val set = src ?: JsonObject(emptyMap())
        return HostSettings(i("width", set), i("height", set), i("hz", set), i("maxKbps", set), b("allowWireless", allowWireless, set), b("mirror", false, set), i("scale", set), s("encoder", set), i("fill", set))
    }
    fun options(src: JsonObject?): DisplayOptionsUi {
        val opt = src ?: JsonObject(emptyMap())
        return DisplayOptionsUi(
            phone = i("phoneW", opt) to i("phoneH", opt),
            pc = i("pcW", opt) to i("pcH", opt),
            resolutions = opt["resolutions"]?.jsonArray?.map { i("w", it.jsonObject) to i("h", it.jsonObject) } ?: emptyList(),
            rates = opt["rates"]?.jsonArray?.mapNotNull { it.jsonPrimitive.intOrNull }?.ifEmpty { null } ?: listOf(60, 90, 120),
            recommendedScale = i("recommendedScale", opt),
            currentScale = i("currentScale", opt),
        )
    }
    val defaults = settings(o["settings"]?.jsonObject)
    val usb = o["usbDevices"]?.jsonArray?.map { it.jsonObject } ?: emptyList()
    return HostStatus(
        reachable = true,
        devices = o["devices"]?.jsonArray?.map { it.jsonObject }?.map { e ->
            ConnectedDevice(
                id = s("id", e),
                name = s("name", e),
                transport = s("transport", e),
                error = s("error", e),
                encoder = s("encoder", e),
                codec = s("codec", e),
                displayW = i("displayW", e),
                displayH = i("displayH", e),
                encodeW = i("encodeW", e),
                encodeH = i("encodeH", e),
                kbps = i("kbps", e),
                rttMs = i("rttMs", e),
                measuredFps = d("measuredFps", e),
                crossAdapter = b("crossAdapter", false, e),
                encoderFallback = b("encoderFallback", false, e),
                settings = settings(e["settings"]?.jsonObject, defaults.allowWireless),
                options = options(e["options"]?.jsonObject),
            )
        } ?: emptyList(),
        maxDevices = o["maxDevices"]?.jsonPrimitive?.intOrNull ?: 4,
        gpus = o["gpus"]?.jsonArray?.map { it.jsonPrimitive.content } ?: emptyList(),
        vddGpu = s("vddGpu"),
        vddGpuEditable = b("vddGpuEditable", true),
        encoders = o["encoders"]?.jsonArray?.map { EncoderOption(s("id", it.jsonObject), s("label", it.jsonObject)) } ?: emptyList(),
        consoleAutostart = b("consoleAutostart"),
        consoleAutostartAsked = b("consoleAutostartAsked", true),
        error = s("error"),
        virtualDisplayInstalled = b("virtualDisplayInstalled", true),
        adbFound = b("adbFound", true),
        pairingCode = s("pairingCode"),
        pairingSeconds = i("pairingSeconds"),
        settings = defaults,
        options = options(o["options"]?.jsonObject),
        usbUnauthorized = usb.any { s("state", it) == "unauthorized" },
        usbCount = usb.count { s("state", it) == "device" },
        paired = o["pairedDevices"]?.jsonArray?.map { PairedDevice(s("id", it.jsonObject), s("name", it.jsonObject)) } ?: emptyList(),
        update = o["update"]?.jsonObject?.let { u ->
            val stage = when (s("state", u)) {
                "available" -> UpdateStage.Available
                "downloading" -> UpdateStage.Downloading
                "installing" -> UpdateStage.Installing
                "failed" -> UpdateStage.Failed
                else -> null
            }
            stage?.let { UpdateUi(s("version", u), s("notes", u), it, i("progress", u)) }
        },
    )
}

/**
 * Talks to sidedroid-host over its pipe. All calls block briefly, so run them off
 * the UI thread. Starts the host if it is not running, since the console is often
 * what the user opens first.
 */
class HostClient(private val pipePath: String = """\\.\pipe\SideDroidHost""", private val mayStartHost: Boolean = true) {
    private var pipe: RandomAccessFile? = null
    private var launchAttemptedAt = 0L
    private var everReached = false

    private fun connect(): RandomAccessFile? {
        pipe?.let { return it }
        return try {
            RandomAccessFile(pipePath, "rw").also {
                pipe = it
                everReached = true
            }
        } catch (_: Exception) {
            // Lost after working means the user quit it from the tray. Bringing it
            // straight back would undo that, so this waits for them to ask.
            if (!everReached) startHost()
            null
        }
    }

    /** True once a host answered and then went away. */
    val serviceStopped: Boolean get() = everReached && pipe == null

    // Harmless when a host is already running: a second one exits right away, and it
    // never opens a console in return.
    fun startHost() {
        if (!mayStartHost) return
        val now = System.currentTimeMillis()
        if (now - launchAttemptedAt < 5_000) return
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

    /** With a device id, for that device. Without, new defaults for every device. */
    fun setDisplay(deviceId: String?, width: Int, height: Int, hz: Int, maxKbps: Int, allowWireless: Boolean, mirror: Boolean, scale: Int, encoder: String, fill: Int) {
        val body = buildJsonObject {
            put("cmd", "set")
            if (deviceId != null) put("id", deviceId)
            put("width", width)
            put("height", height)
            put("hz", hz)
            put("maxKbps", maxKbps)
            put("allowWireless", allowWireless)
            put("mirror", mirror)
            put("scale", scale)
            put("encoder", encoder)
            put("fill", fill)
        }
        exchange(body.toString())
    }

    fun setVddGpu(gpu: String) {
        exchange(buildJsonObject { put("cmd", "set_gpu"); put("gpu", gpu) }.toString())
    }

    fun setConsoleAutostart(enabled: Boolean) {
        exchange(buildJsonObject { put("cmd", "console_autostart"); put("enabled", enabled) }.toString())
    }

    fun startPairing() { exchange("""{"cmd":"pair_start"}""") }
    fun cancelPairing() { exchange("""{"cmd":"pair_cancel"}""") }
    fun disconnect(deviceId: String) {
        exchange(buildJsonObject { put("cmd", "disconnect"); put("id", deviceId) }.toString())
    }
    fun installDriver() { exchange("""{"cmd":"install_driver"}""") }
    fun installUpdate() { exchange("""{"cmd":"update_install"}""") }
    fun unpair(id: String) {
        exchange(buildJsonObject { put("cmd", "unpair"); put("id", id) }.toString())
    }
}
