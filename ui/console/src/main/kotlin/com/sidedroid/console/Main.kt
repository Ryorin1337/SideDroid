package com.sidedroid.console

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.window.WindowDraggableArea
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.unit.DpSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Window
import androidx.compose.ui.window.WindowPosition
import androidx.compose.ui.window.application
import androidx.compose.ui.window.rememberWindowState
import com.sidedroid.design.AboutDialog
import com.sidedroid.design.SdIcons
import com.sidedroid.design.SideDroidTheme
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** The title row of the borderless window: name, minimize, close. */
@Composable
fun TitleRow(onAbout: () -> Unit, onMinimize: () -> Unit, onClose: () -> Unit) {
    Row(
        Modifier.fillMaxWidth().padding(start = 24.dp, end = 8.dp, top = 8.dp, bottom = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text("SideDroid", style = MaterialTheme.typography.titleLarge)
        Spacer(Modifier.weight(1f))
        IconButton(onClick = onAbout) { Icon(SdIcons.Info, "关于", Modifier.size(20.dp)) }
        IconButton(onClick = onMinimize) { Icon(SdIcons.Minimize, "最小化", Modifier.size(20.dp)) }
        IconButton(onClick = onClose) { Icon(SdIcons.Close, "关闭", Modifier.size(20.dp)) }
    }
}

private fun argAfter(args: Array<String>, name: String) = args.indexOf(name).takeIf { it >= 0 }?.let { args.getOrNull(it + 1) }

fun main(args: Array<String>) {
    // Test runs talk to a host on another pipe and have a window of their own.
    val instance = argAfter(args, "--instance")
    val single = SingleInstance(instance ?: "main")
    if (!single.acquire()) {
        // Already open: ask that window to come forward, and leave.
        single.signalExisting()
        return
    }
    runConsole(args, instance, single)
}

private fun runConsole(args: Array<String>, instance: String?, single: SingleInstance) = application {
    // --self-check <file>: run for a few seconds, record what happened, and exit.
    // Lets scripts confirm that the packaged app starts and reaches the host.
    val selfCheckFile = argAfter(args, "--self-check")
    val selfCheckMs = argAfter(args, "--self-check-ms")?.toLongOrNull() ?: 5000L
    val windowState = rememberWindowState(size = DpSize(460.dp, 860.dp), position = WindowPosition(Alignment.Center))
    // Closing this window does not stop streaming: the host keeps running in the tray.
    Window(
        onCloseRequest = ::exitApplication,
        state = windowState,
        title = "SideDroid",
        undecorated = true,
        transparent = true,
        resizable = false,
        icon = painterResource("sidedroid.png"),
    ) {
        val client = remember {
            if (instance == null) HostClient()
            else HostClient("""\\.\pipe\SideDroidHost-$instance""", mayStartHost = false)
        }
        var status by remember { mutableStateOf(HostStatus()) }
        val scope = rememberCoroutineScope()
        var aboutOpen by remember { mutableStateOf(false) }

        var serviceStopped by remember { mutableStateOf(false) }
        var unreachableSeconds by remember { mutableStateOf(0) }
        LaunchedEffect(Unit) {
            while (true) {
                status = withContext(Dispatchers.IO) { client.status() }
                serviceStopped = client.serviceStopped
                unreachableSeconds = if (status.reachable) 0 else unreachableSeconds + 1
                delay(1000)
            }
        }

        if (selfCheckFile != null) {
            LaunchedEffect(Unit) {
                delay(selfCheckMs)
                java.io.File(selfCheckFile).writeText(
                    "window=shown\nhostReachable=${status.reachable}\nvirtualDisplayInstalled=${status.virtualDisplayInstalled}\n",
                )
                exitApplication()
            }
        }

        // Opening SideDroid again brings this window back instead of adding another.
        LaunchedEffect(Unit) {
            while (true) {
                delay(400)
                if (withContext(Dispatchers.IO) { single.takeSignal() }) {
                    windowState.isMinimized = false
                    window.isVisible = true
                    window.toFront()
                    window.requestFocus()
                }
            }
        }

        fun act(block: () -> Unit) = scope.launch {
            withContext(Dispatchers.IO) {
                block()
                status = client.status()
            }
        }

        SideDroidTheme(dark = isSystemInDarkTheme()) {
            Surface(Modifier.fillMaxSize(), shape = MaterialTheme.shapes.large, color = MaterialTheme.colorScheme.surface) {
                Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    WindowDraggableArea {
                        TitleRow(onAbout = { aboutOpen = true }, onMinimize = { windowState.isMinimized = true }, onClose = ::exitApplication)
                    }
                    ConsoleScreen(
                        status,
                        ConsoleActions(
                            onSetDisplay = { s -> act { client.setDisplay(s.width, s.height, s.hz, s.maxKbps, s.allowWireless, s.mirror, s.scale) } },
                            onDisconnect = { act { client.disconnect() } },
                            onInstallDriver = { act { client.installDriver() } },
                            onStartPairing = { act { client.startPairing() } },
                            onCancelPairing = { act { client.cancelPairing() } },
                            onUnpair = { id -> act { client.unpair(id) } },
                            onStartService = { act { client.startHost() } },
                            onInstallUpdate = { act { client.installUpdate() } },
                        ),
                        serviceStopped = serviceStopped,
                        // A host takes well under a second to start. This long means it will not.
                        startupFailed = unreachableSeconds >= 10,
                    )
                }
            }
            if (aboutOpen) AboutDialog(onDismiss = { aboutOpen = false })
        }
    }
}
