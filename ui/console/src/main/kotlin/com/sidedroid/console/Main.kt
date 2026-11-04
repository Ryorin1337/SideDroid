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
import com.sidedroid.design.SdIcons
import com.sidedroid.design.SideDroidTheme
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** The title row of the borderless window: name, minimize, close. */
@Composable
fun TitleRow(onMinimize: () -> Unit, onClose: () -> Unit) {
    Row(
        Modifier.fillMaxWidth().padding(start = 24.dp, end = 8.dp, top = 8.dp, bottom = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text("SideDroid", style = MaterialTheme.typography.titleLarge)
        Spacer(Modifier.weight(1f))
        IconButton(onClick = onMinimize) { Icon(SdIcons.Minimize, "最小化", Modifier.size(20.dp)) }
        IconButton(onClick = onClose) { Icon(SdIcons.Close, "关闭", Modifier.size(20.dp)) }
    }
}

fun main(args: Array<String>) = application {
    // --self-check <file>: run for a few seconds, record what happened, and exit.
    // Lets scripts confirm that the packaged app starts and reaches the host.
    val selfCheckFile = args.indexOf("--self-check").takeIf { it >= 0 }?.let { args.getOrNull(it + 1) }
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
        val client = remember { HostClient() }
        var status by remember { mutableStateOf(HostStatus()) }
        val scope = rememberCoroutineScope()

        LaunchedEffect(Unit) {
            while (true) {
                status = withContext(Dispatchers.IO) { client.status() }
                delay(1000)
            }
        }

        if (selfCheckFile != null) {
            LaunchedEffect(Unit) {
                delay(5000)
                java.io.File(selfCheckFile).writeText(
                    "window=shown\nhostReachable=${status.reachable}\nvirtualDisplayInstalled=${status.virtualDisplayInstalled}\n",
                )
                exitApplication()
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
                        TitleRow(onMinimize = { windowState.isMinimized = true }, onClose = ::exitApplication)
                    }
                    ConsoleScreen(
                        status,
                        ConsoleActions(
                            onSetDisplay = { s -> act { client.setDisplay(s.width, s.height, s.hz, s.maxKbps, s.allowWireless) } },
                            onDisconnect = { act { client.disconnect() } },
                            onInstallDriver = { act { client.installDriver() } },
                            onStartPairing = { act { client.startPairing() } },
                            onCancelPairing = { act { client.cancelPairing() } },
                            onUnpair = { id -> act { client.unpair(id) } },
                        ),
                    )
                }
            }
        }
    }
}
