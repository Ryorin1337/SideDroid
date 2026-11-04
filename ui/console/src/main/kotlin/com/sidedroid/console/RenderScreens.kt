// Draws every screen to PNG without a window, a host or a phone.
package com.sidedroid.console

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.ui.ImageComposeScene
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.dp
import androidx.compose.ui.use
import com.sidedroid.design.AboutDialog
import com.sidedroid.design.DiscoveredHost
import com.sidedroid.design.DisplayOptionsUi
import com.sidedroid.design.DisplaySettings
import com.sidedroid.design.DisplayValue
import com.sidedroid.design.SectionCard
import com.sidedroid.design.PairingDialog
import com.sidedroid.design.PhoneHomeScreen
import com.sidedroid.design.PhoneHomeState
import com.sidedroid.design.SideDroidTheme
import com.sidedroid.design.StreamInfo
import com.sidedroid.design.StreamOverlay
import java.io.File
import org.jetbrains.skia.EncodedImageFormat

private fun render(out: File, name: String, w: Int, h: Int, dark: Boolean, content: @Composable () -> Unit) {
    val density = 2f
    ImageComposeScene((w * density).toInt(), (h * density).toInt(), Density(density)) {
        SideDroidTheme(dark = dark) { content() }
    }.use { scene ->
        // Several passes: enter animations settle over simulated time.
        var t = 0L
        repeat(40) {
            scene.render(t)
            t += 100_000_000L
        }
        val data = scene.render(t).encodeToData(EncodedImageFormat.PNG) ?: error("encode failed")
        File(out, "$name-${if (dark) "dark" else "light"}.png").writeBytes(data.bytes)
    }
}

private val sampleOptions = DisplayOptionsUi(
    phone = 3168 to 1440,
    pc = 2560 to 1600,
    resolutions = listOf(2560 to 1600, 2560 to 1440, 1920 to 1200, 1920 to 1080, 1600 to 900, 1366 to 768, 1280 to 720),
    rates = listOf(60, 90, 120),
    recommendedScale = 225,
    currentScale = 225,
)

private val connected = HostStatus(
    reachable = true, connected = true, device = "Pixel 9 Pro", transport = "usb", encoder = "NVENC", codec = "HEVC",
    displayW = 2992, displayH = 1344, hz = 120, encodeW = 2992, encodeH = 1344, fps = 120, kbps = 42000, rttMs = 3, measuredFps = 118.0,
    settings = HostSettings(hz = 0), usbCount = 1, options = sampleOptions,
    paired = listOf(PairedDevice("a", "Pixel 9 Pro"), PairedDevice("b", "Galaxy Tab S9")),
)

@Composable
private fun Console(status: HostStatus) {
    Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.surface) {
        Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
            TitleRow({}, {}, {})
            ConsoleScreen(status, ConsoleActions())
        }
    }
}

fun main(args: Array<String>) {
    val out = File(args[0]).apply { mkdirs() }
    for (dark in listOf(false, true)) {
        render(out, "console-waiting", 460, 860, dark) { Console(HostStatus(reachable = true, usbCount = 1, paired = connected.paired)) }
        render(out, "console-connected", 460, 860, dark) { Console(connected) }
        render(out, "console-limited", 460, 860, dark) { Console(connected.copy(transport = "wifi", encodeW = 2094, encodeH = 940, kbps = 4800, rttMs = 18)) }
        render(out, "console-no-driver", 460, 860, dark) { Console(HostStatus(reachable = true, virtualDisplayInstalled = false)) }
        render(out, "console-pairing", 460, 860, dark) { Console(HostStatus(reachable = true, pairingCode = "K7QM-2XPD", pairingSeconds = 94)) }

        val hosts = listOf(
            DiscoveredHost("MY-LAPTOP", "192.168.1.23", 27183, true),
            DiscoveredHost("OFFICE-PC", "192.168.1.40", 27183, false),
        )
        // Phone portrait and landscape, a foldable, and tablets both ways.
        val sizes = listOf("phone" to (400 to 860), "phone-land" to (860 to 400), "fold" to (673 to 841), "tablet" to (800 to 1280), "tablet-land" to (1280 to 800))
        for ((name, size) in sizes) {
            render(out, "home-$name", size.first, size.second, dark) {
                Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.surface) {
                    PhoneHomeScreen(PhoneHomeState(hosts = hosts), {}, {}, {})
                }
            }
            render(out, "stream-$name", size.first, size.second, dark) {
                Box(Modifier.fillMaxSize()) {
                    Surface(Modifier.fillMaxSize(), color = Color(0xFF303846)) {}
                    StreamOverlay(
                        StreamInfo("3168\u00D71440", 120, "42 Mbps", "USB", 3),
                        toolbarOpen = true, settingsOpen = true, {}, {}, {},
                    ) {
                        DisplaySettings(sampleOptions, DisplayValue(), onChange = {})
                    }
                }
            }
        }
        render(out, "phone-error", 400, 860, dark) {
            Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.surface) {
                PhoneHomeScreen(PhoneHomeState(error = "\u7535\u8111\u4E0A\u7684 SideDroid \u6CA1\u6709\u54CD\u5E94"), {}, {}, {})
            }
        }
        render(out, "phone-pair", 400, 860, dark) {
            Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.surface) {
                PhoneHomeScreen(PhoneHomeState(hosts = hosts), {}, {}, {})
                PairingDialog("MY-LAPTOP", wrongCode = true, {}, {})
            }
        }
        render(out, "console-service-stopped", 460, 860, dark) {
            Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.surface) {
                Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    TitleRow({}, {}, {})
                    ConsoleScreen(HostStatus(), ConsoleActions(), serviceStopped = true)
                }
            }
        }
        render(out, "console-port-busy", 460, 860, dark) { Console(HostStatus(reachable = true, error = "port_in_use")) }
        render(out, "console-about", 460, 860, dark) {
            Console(connected)
            AboutDialog(onDismiss = {})
        }
        render(out, "phone-about", 400, 860, dark) {
            Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.surface) {
                PhoneHomeScreen(PhoneHomeState(hosts = hosts), {}, {}, {})
                AboutDialog(onDismiss = {})
            }
        }
        render(out, "console-resolution-open", 460, 860, dark) {
            Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.surface) {
                Column(Modifier.padding(20.dp)) {
                    SectionCard { DisplaySettings(sampleOptions, DisplayValue(), onChange = {}, showQuality = true, openRow = "resolution") }
                }
            }
        }
        render(out, "console-scale-open", 460, 860, dark) {
            Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.surface) {
                Column(Modifier.padding(20.dp)) {
                    SectionCard { DisplaySettings(sampleOptions, DisplayValue(), onChange = {}, showQuality = true, openRow = "scale") }
                }
            }
        }
    }
    println("rendered to ${out.absolutePath}")
}
