// Phone screens as pure functions of their state. The Android app supplies real
// data; the desktop renderer draws them with samples for visual checks.
package com.sidedroid.design

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.FilledTonalIconButton
import androidx.compose.material3.HorizontalFloatingToolbar
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp

data class DiscoveredHost(val name: String, val address: String, val port: Int, val paired: Boolean)

data class PhoneHomeState(
    val hosts: List<DiscoveredHost> = emptyList(),
    val connecting: Boolean = false,
    val error: String? = null,
)

@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun PhoneHomeScreen(
    state: PhoneHomeState,
    onUsb: () -> Unit,
    onHost: (DiscoveredHost) -> Unit,
    onManual: (String) -> Unit,
    modifier: Modifier = Modifier,
) {
    var manualOpen by remember { mutableStateOf(false) }

    Column(
        modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = 20.dp, vertical = 28.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Text("SideDroid", style = MaterialTheme.typography.displaySmall, modifier = Modifier.padding(start = 4.dp, bottom = 8.dp))

        StatusHero(
            state = when {
                state.error != null -> ConnectionVisual.Problem
                state.connecting -> ConnectionVisual.Waiting
                else -> ConnectionVisual.Idle
            },
            headline = when {
                state.error != null -> "连接失败"
                state.connecting -> "正在连接"
                else -> "选择连接方式"
            },
            supporting = state.error ?: "把这台设备变成电脑的副屏",
        )

        Button(
            onClick = onUsb,
            enabled = !state.connecting,
            modifier = Modifier.fillMaxWidth().height(ButtonDefaults.LargeContainerHeight),
            shapes = ButtonDefaults.shapes(),
        ) {
            Icon(SdIcons.Usb, null, Modifier.size(ButtonDefaults.LargeIconSize))
            Spacer(Modifier.size(ButtonDefaults.LargeIconSpacing))
            Text("USB 连接", style = ButtonDefaults.textStyleFor(ButtonDefaults.LargeContainerHeight))
        }

        SectionCard(title = "无线连接") {
            if (state.hosts.isEmpty()) {
                Text(
                    "正在局域网中查找电脑…",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            state.hosts.forEach { host ->
                Surface(
                    shape = MaterialTheme.shapes.medium,
                    color = MaterialTheme.colorScheme.surfaceContainerHigh,
                    modifier = Modifier.fillMaxWidth().clickable(enabled = !state.connecting) { onHost(host) },
                ) {
                    Row(Modifier.padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
                        Icon(SdIcons.Computer, null, tint = MaterialTheme.colorScheme.primary)
                        Column(Modifier.padding(start = 14.dp).weight(1f)) {
                            Text(host.name, style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.SemiBold)
                            Text(
                                if (host.paired) "已配对 · ${host.address}" else "未配对 · ${host.address}",
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                        Icon(SdIcons.Wifi, null, Modifier.size(20.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                }
            }
            TextButton(onClick = { manualOpen = true }, enabled = !state.connecting) {
                Icon(SdIcons.Add, null, Modifier.size(18.dp))
                Spacer(Modifier.size(8.dp))
                Text("手动输入地址")
            }
        }
    }

    if (manualOpen) {
        var address by remember { mutableStateOf("") }
        AlertDialog(
            onDismissRequest = { manualOpen = false },
            title = { Text("电脑地址") },
            text = {
                OutlinedTextField(
                    value = address,
                    onValueChange = { address = it.trim() },
                    singleLine = true,
                    placeholder = { Text("192.168.1.10") },
                    keyboardActions = KeyboardActions.Default,
                )
            },
            confirmButton = {
                TextButton(enabled = address.isNotEmpty(), onClick = { manualOpen = false; onManual(address) }) { Text("连接") }
            },
            dismissButton = { TextButton(onClick = { manualOpen = false }) { Text("取消") } },
        )
    }
}

@Composable
fun PairingDialog(hostName: String, wrongCode: Boolean, onSubmit: (String) -> Unit, onCancel: () -> Unit) {
    var code by remember { mutableStateOf("") }
    AlertDialog(
        onDismissRequest = onCancel,
        title = { Text("与 $hostName 配对") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text("在电脑控制台点击“添加设备”，然后输入显示的配对码。")
                OutlinedTextField(
                    value = code,
                    onValueChange = { code = it.uppercase().take(9) },
                    singleLine = true,
                    isError = wrongCode,
                    label = { Text("配对码") },
                    supportingText = if (wrongCode) ({ Text("配对码不正确或已过期") }) else null,
                    placeholder = { Text("K7QM-2XPD") },
                )
            }
        },
        confirmButton = { TextButton(enabled = code.length >= 8, onClick = { onSubmit(code) }) { Text("配对") } },
        dismissButton = { TextButton(onClick = onCancel) { Text("取消") } },
    )
}

data class StreamInfo(val resolution: String, val fps: Int, val bitrate: String, val transport: String, val rttMs: Int)

/** Hidden edge handle plus a floating toolbar: the only chrome over the desktop. */
@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun StreamOverlay(
    info: StreamInfo?,
    toolbarOpen: Boolean,
    settingsOpen: Boolean,
    onToggleToolbar: () -> Unit,
    onOpenSettings: () -> Unit,
    onDisconnect: () -> Unit,
    modifier: Modifier = Modifier,
    settings: @Composable () -> Unit,
) {
    Box(modifier.fillMaxSize()) {
        if (toolbarOpen) {
            Column(
                Modifier.align(Alignment.BottomCenter).padding(bottom = 24.dp, start = 16.dp, end = 16.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                if (settingsOpen) {
                    Surface(shape = MaterialTheme.shapes.large, color = MaterialTheme.colorScheme.surfaceContainerHigh) {
                        Box(Modifier.padding(20.dp)) { settings() }
                    }
                }
                HorizontalFloatingToolbar(expanded = true) {
                    FilledTonalIconButton(onClick = onOpenSettings) { Icon(SdIcons.Tune, "显示设置") }
                    if (info != null) {
                        Text(
                            "${info.resolution} · ${info.fps}Hz · ${info.bitrate} · ${info.rttMs}ms",
                            style = MaterialTheme.typography.labelLarge,
                            modifier = Modifier.align(Alignment.CenterVertically).padding(horizontal = 12.dp),
                            maxLines = 1,
                        )
                    }
                    FilledTonalButton(onClick = onDisconnect) { Text("断开") }
                }
            }
        }
    }
}
