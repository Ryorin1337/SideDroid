package com.sidedroid.app

import android.annotation.SuppressLint
import android.content.res.Configuration
import android.os.Build
import android.os.Bundle
import android.view.MotionEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface as M3Surface
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.sidedroid.design.DiscoveredHost
import com.sidedroid.design.DisplayOptionsUi
import com.sidedroid.design.DisplaySettings
import com.sidedroid.design.DisplayValue
import com.sidedroid.design.PairingDialog
import com.sidedroid.design.PhoneHomeScreen
import com.sidedroid.design.PhoneHomeState
import com.sidedroid.design.SideDroidDark
import com.sidedroid.design.SideDroidLight
import com.sidedroid.design.SideDroidTheme
import com.sidedroid.design.StreamInfo
import com.sidedroid.design.StreamOverlay
import com.sidedroid.design.UpdateStage
import com.sidedroid.design.UpdateUi
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

private const val PORT = 27183

class MainActivity : ComponentActivity() {
    private lateinit var pairing: PairingStore
    private lateinit var discovery: HostDiscovery
    private val found = mutableStateListOf<FoundHost>()

    private var streaming by mutableStateOf(false)
    private var homeError by mutableStateOf<String?>(null)
    private var connecting by mutableStateOf(false)
    private var config by mutableStateOf<StreamConfig?>(null)
    private var usedUsb by mutableStateOf(true)
    private var options by mutableStateOf(DisplayOptionsUi())
    private val updater by lazy { Updater(applicationContext) }
    private var availableUpdate: Updater.Available? = null
    private var updateUi by mutableStateOf<UpdateUi?>(null)
    private var updateChecked = false

    private data class StreamConfig(
        val displayW: Int, val displayH: Int, val encodeW: Int, val encodeH: Int, val fps: Int, val kbps: Int,
        val mirror: Boolean, val manualW: Int, val manualH: Int, val manualHz: Int, val manualScale: Int,
    )

    private val listener = object : NativeBridge.Listener {
        override fun onConfig(displayW: Int, displayH: Int, encodeW: Int, encodeH: Int, fps: Int, codec: Int, kbps: Int, mirror: Boolean, manualW: Int, manualH: Int, manualHz: Int, manualScale: Int) {
            runOnUiThread {
                config = StreamConfig(displayW, displayH, encodeW, encodeH, fps, kbps, mirror, manualW, manualH, manualHz, manualScale)
                applyPanelRate(fps)
            }
        }
        override fun onDisplayOptions(phoneW: Int, phoneH: Int, pcW: Int, pcH: Int, recommendedScale: Int, currentScale: Int, resolutions: IntArray) {
            val sizes = resolutions.toList().chunked(2).filter { it.size == 2 }.map { it[0] to it[1] }
            runOnUiThread {
                options = DisplayOptionsUi(phoneW to phoneH, pcW to pcH, sizes, DeviceInfo.refreshRates(this@MainActivity).toList(), recommendedScale, currentScale)
            }
        }
        override fun onDisconnected(reason: String) {
            runOnUiThread {
                if (streaming) leaveStream(if (reason == "stopped") null else "与电脑的连接已断开")
            }
        }
        override fun onPaired(secret: ByteArray) {
            pendingHostKey?.let { pairing.save(it, secret) }
        }
    }
    private var pendingHostKey: String? = null
    private var currentSurface: Surface? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        pairing = PairingStore(this)
        discovery = HostDiscovery(this) { hosts ->
            runOnUiThread {
                found.clear()
                found.addAll(hosts)
            }
        }
        setContent { App() }
    }

    override fun onStart() {
        super.onStart()
        if (!streaming) discovery.start()
    }

    override fun onStop() {
        super.onStop()
        discovery.stop()
    }

    override fun onDestroy() {
        if (isFinishing) NativeBridge.stop()
        super.onDestroy()
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        if (streaming) {
            // Windows follows the phone: rotating here turns the extended display too.
            val (w, h) = DeviceInfo.panelSize(this)
            NativeBridge.sendViewport(w, h)
        }
    }

    private suspend fun connect(host: String, mode: Int, secret: ByteArray?, code: String?, udp: Boolean): String? {
        val caps = DeviceInfo.decoderCaps()
        val (w, h) = DeviceInfo.panelSize(this)
        val dpi = resources.displayMetrics.densityDpi
        val rates = DeviceInfo.refreshRates(this)
        val name = DeviceInfo.deviceName(this)
        val id = pairing.clientId
        return withContext(Dispatchers.IO) {
            NativeBridge.start(host, PORT, mode, id, secret, code, name, w, h, dpi, caps.codecs, caps.maxLong, caps.maxShort, rates, udp, listener)
        }
    }

    private fun enterStream(usb: Boolean) {
        usedUsb = usb
        streaming = true
        homeError = null
        discovery.stop()
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        if (Build.VERSION.SDK_INT >= 28) {
            // Use the whole panel, around any cutout.
            window.attributes = window.attributes.apply {
                layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
            }
        }
        WindowCompat.getInsetsController(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }

    private fun leaveStream(error: String?) {
        streaming = false
        config = null
        homeError = error
        NativeBridge.stop()
        currentSurface = null
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        WindowCompat.getInsetsController(window, window.decorView).show(WindowInsetsCompat.Type.systemBars())
        window.attributes = window.attributes.apply { preferredDisplayModeId = 0 }
        discovery.start()
    }

    private fun panelRatio(): Float {
        val (w, h) = DeviceInfo.panelSize(this)
        return w.toFloat() / h.coerceAtLeast(1)
    }

    private fun applyPanelRate(fps: Int) {
        DeviceInfo.modeIdFor(this, fps)?.let { id ->
            window.attributes = window.attributes.apply { preferredDisplayModeId = id }
        }
        if (Build.VERSION.SDK_INT >= 30) {
            currentSurface?.setFrameRate(fps.toFloat(), Surface.FRAME_RATE_COMPATIBILITY_DEFAULT)
        }
    }

    @Composable
    private fun App() {
        val context = LocalContext.current
        val dark = isSystemInDarkTheme()
        val scheme = when {
            Build.VERSION.SDK_INT >= 31 -> if (dark) dynamicDarkColorScheme(context) else dynamicLightColorScheme(context)
            dark -> SideDroidDark
            else -> SideDroidLight
        }
        SideDroidTheme(dark = dark, colorScheme = scheme) {
            if (streaming) StreamScreen() else Home()
        }
    }

    @Composable
    private fun Home() {
        val scope = rememberCoroutineScope()
        var pairingFor by remember { mutableStateOf<DiscoveredHost?>(null) }
        var wrongCode by remember { mutableStateOf(false) }

        fun hostKey(h: DiscoveredHost) = h.name

        // Once per launch, and never while a stream is competing for the network.
        LaunchedEffect(Unit) {
            if (updateChecked) return@LaunchedEffect
            updateChecked = true
            val latest = withContext(Dispatchers.IO) { updater.check() }
            availableUpdate = latest
            if (latest != null) updateUi = UpdateUi(latest.version, latest.notes)
            else withContext(Dispatchers.IO) { updater.deleteDownloads() }
        }

        fun installUpdate() {
            val update = availableUpdate ?: return
            updateUi = updateUi?.copy(stage = UpdateStage.Downloading, progress = 0)
            scope.launch {
                val apk = withContext(Dispatchers.IO) {
                    runCatching {
                        updater.download(update) { percent ->
                            runOnUiThread { updateUi = updateUi?.copy(progress = percent) }
                        }
                    }.getOrNull()
                }
                if (apk == null) {
                    updateUi = updateUi?.copy(stage = UpdateStage.Failed)
                    return@launch
                }
                // Back to the button afterwards: declining the installer is not an error,
                // and accepting it replaces this process.
                updateUi = updateUi?.copy(stage = if (runCatching { updater.install(apk) }.isSuccess) UpdateStage.Available else UpdateStage.Failed)
            }
        }

        fun attemptWireless(h: DiscoveredHost) {
            val secret = pairing.secretFor(hostKey(h))
            if (secret == null) {
                wrongCode = false
                pairingFor = h
                return
            }
            connecting = true
            homeError = null
            scope.launch {
                when (val err = connect(h.address, NativeBridge.MODE_PAIRED, secret, null, udp = true)) {
                    null -> enterStream(usb = false)
                    "auth" -> {
                        // The PC no longer knows us: pair again.
                        pairing.forget(hostKey(h))
                        wrongCode = false
                        pairingFor = h
                    }
                    else -> homeError = errorText(err, usb = false)
                }
                connecting = false
            }
        }

        M3Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.surface) {
            PhoneHomeScreen(
                state = PhoneHomeState(
                    hosts = found.map { DiscoveredHost(it.name, it.address, it.port, pairing.secretFor(it.name) != null) },
                    connecting = connecting,
                    error = homeError,
                    update = updateUi,
                ),
                onUsb = {
                    connecting = true
                    homeError = null
                    scope.launch {
                        when (val err = connect("127.0.0.1", NativeBridge.MODE_USB, null, null, udp = false)) {
                            null -> enterStream(usb = true)
                            else -> homeError = errorText(err, usb = true)
                        }
                        connecting = false
                    }
                },
                onHost = { attemptWireless(it) },
                onManual = { addr -> attemptWireless(DiscoveredHost(addr, addr, PORT, false)) },
                modifier = Modifier.safeDrawingPadding(),
                onInstallUpdate = { installUpdate() },
            )
        }

        pairingFor?.let { host ->
            PairingDialog(
                host.name,
                wrongCode,
                onSubmit = { code ->
                    pendingHostKey = hostKey(host)
                    connecting = true
                    scope.launch {
                        when (val err = connect(host.address, NativeBridge.MODE_PAIRING, null, code, udp = true)) {
                            null -> {
                                pairingFor = null
                                enterStream(usb = false)
                            }
                            "auth" -> wrongCode = true
                            else -> {
                                pairingFor = null
                                homeError = errorText(err, usb = false)
                            }
                        }
                        connecting = false
                    }
                },
                onCancel = { pairingFor = null },
            )
        }
    }

    private fun errorText(code: String, usb: Boolean) = when {
        usb -> "没有找到电脑。请打开 USB 调试，并确认电脑上的 SideDroid 已运行。"
        code == "connect" -> "连不上电脑。请确认在同一网络，且电脑允许无线连接。"
        else -> "连接失败（$code）"
    }

    @SuppressLint("ClickableViewAccessibility")
    @Composable
    private fun StreamScreen() {
        var toolbarOpen by remember { mutableStateOf(false) }
        var settingsOpen by remember { mutableStateOf(false) }
        var rttMs by remember { mutableStateOf(0) }
        val cfg = config

        BackHandler { leaveStream(null) }

        LaunchedEffect(Unit) {
            while (true) {
                rttMs = NativeBridge.rttUs() / 1000
                delay(1000)
            }
        }

        Box(Modifier.fillMaxSize().background(Color.Black), contentAlignment = Alignment.Center) {
            val ratio = cfg?.let { it.displayW.toFloat() / it.displayH }
            AndroidView(
                // Letterboxed within the panel, whichever side the picture is narrower on.
                modifier = if (ratio != null) Modifier.aspectRatio(ratio, matchHeightConstraintsFirst = ratio < panelRatio()) else Modifier.fillMaxSize(),
                factory = { ctx ->
                    SurfaceView(ctx).apply {
                        holder.addCallback(object : SurfaceHolder.Callback {
                            override fun surfaceCreated(h: SurfaceHolder) {
                                currentSurface = h.surface
                                NativeBridge.setSurface(h.surface)
                                config?.let { applyPanelRate(it.fps) }
                            }
                            override fun surfaceChanged(h: SurfaceHolder, format: Int, w: Int, hh: Int) {}
                            override fun surfaceDestroyed(h: SurfaceHolder) {
                                currentSurface = null
                                NativeBridge.setSurface(null)
                            }
                        })
                        setOnTouchListener(TouchForwarder())
                    }
                },
            )

            // A small handle on the edge is the only thing drawn over the desktop.
            Box(
                Modifier.align(Alignment.CenterEnd)
                    .padding(end = 2.dp)
                    .size(width = 8.dp, height = 72.dp)
                    .background(Color.White.copy(alpha = 0.35f), RoundedCornerShape(4.dp))
                    .clickable {
                        toolbarOpen = !toolbarOpen
                        if (!toolbarOpen) settingsOpen = false
                    },
            )

            StreamOverlay(
                info = cfg?.let {
                    StreamInfo("${it.encodeW}×${it.encodeH}", it.fps, "%.0f Mbps".format(it.kbps / 1000.0), if (usedUsb) "USB" else "Wi-Fi", rttMs)
                },
                toolbarOpen = toolbarOpen,
                settingsOpen = settingsOpen,
                onToggleToolbar = { toolbarOpen = !toolbarOpen },
                onOpenSettings = { settingsOpen = !settingsOpen },
                onDisconnect = { leaveStream(null) },
            ) {
                // Shown from what the PC reports, so both sides always agree.
                DisplaySettings(
                    options,
                    DisplayValue(cfg?.mirror ?: false, cfg?.manualW ?: 0, cfg?.manualH ?: 0, cfg?.manualHz ?: 0, cfg?.manualScale ?: 0),
                    onChange = { NativeBridge.sendSetDisplay(it.width, it.height, it.hz, it.mirror, it.scale) },
                )
            }
        }
    }

    /** Sends every finger, in view coordinates scaled to 0..65535 of the shown desktop. */
    private class TouchForwarder : View.OnTouchListener {
        private val buf = IntArray(5 * 10)

        @SuppressLint("ClickableViewAccessibility")
        override fun onTouch(v: View, e: MotionEvent): Boolean {
            val action = e.actionMasked
            val actionIndex = e.actionIndex
            val count = minOf(e.pointerCount, 10)
            var n = 0
            for (i in 0 until count) {
                val a = when (action) {
                    MotionEvent.ACTION_DOWN -> 0
                    MotionEvent.ACTION_POINTER_DOWN -> if (i == actionIndex) 0 else 1
                    MotionEvent.ACTION_UP -> 2
                    MotionEvent.ACTION_POINTER_UP -> if (i == actionIndex) 2 else 1
                    MotionEvent.ACTION_CANCEL -> 3
                    else -> 1
                }
                val x = (e.getX(i) / maxOf(v.width - 1, 1)).coerceIn(0f, 1f)
                val y = (e.getY(i) / maxOf(v.height - 1, 1)).coerceIn(0f, 1f)
                val o = n * 5
                buf[o] = e.getPointerId(i) and 0xFF
                buf[o + 1] = a
                buf[o + 2] = (x * 65535).toInt()
                buf[o + 3] = (y * 65535).toInt()
                buf[o + 4] = (e.getPressure(i).coerceIn(0.01f, 1f) * 1024).toInt()
                n++
            }
            NativeBridge.sendTouch(buf, n)
            return true
        }
    }
}
