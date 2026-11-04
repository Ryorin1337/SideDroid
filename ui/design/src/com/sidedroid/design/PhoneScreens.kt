// Phone screens as pure functions of their state. The Android app supplies real
// data; the desktop renderer draws them with samples for visual checks.
package com.sidedroid.design

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
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
import androidx.compose.material3.IconButton
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

private enum class HomeLayout { Single, TwoPanes }

/** Two panes from 600dp, the width where one column starts to look stretched. */
private fun homeLayoutFor(widthDp: Float) = if (widthDp >= 600f) HomeLayout.TwoPanes else HomeLayout.Single

@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
private fun ConnectPane(state: PhoneHomeState, onUsb: () -> Unit, onAbout: () -> Unit, compactHeight: Boolean) {
    Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
        Row(Modifier.fillMaxWidth().padding(start = 4.dp, bottom = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            Text("SideDroid", style = MaterialTheme.typography.displaySmall, modifier = Modifier.weight(1f))
            IconButton(onClick = onAbout) { Icon(SdIcons.Info, "关于") }
        }
        StatusHero(
            state = when {
                state.error != null -> ConnectionVisual.Problem
                state.connecting -> ConnectionVisual.Waiting
                else -> ConnectionVisual.Idle
            },
            headline = when {
                state.error != null -> "\u8FDE\u63A5\u5931\u8D25"
                state.connecting -> "\u6B63\u5728\u8FDE\u63A5"
                else -> "\u9009\u62E9\u8FDE\u63A5\u65B9\u5F0F"
            },
            supporting = state.error ?: "\u628A\u8FD9\u53F0\u8BBE\u5907\u53D8\u6210\u7535\u8111\u7684\u526F\u5C4F",
        )
        val buttonHeight = if (compactHeight) ButtonDefaults.MediumContainerHeight else ButtonDefaults.LargeContainerHeight
        Button(
            onClick = onUsb,
            enabled = !state.connecting,
            modifier = Modifier.fillMaxWidth().height(buttonHeight),
            shapes = ButtonDefaults.shapes(),
        ) {
            Icon(SdIcons.Usb, null, Modifier.size(ButtonDefaults.iconSizeFor(buttonHeight)))
            Spacer(Modifier.size(ButtonDefaults.iconSpacingFor(buttonHeight)))
            Text("USB \u8FDE\u63A5", style = ButtonDefaults.textStyleFor(buttonHeight))
        }
    }
}

@Composable
private fun WirelessPane(state: PhoneHomeState, onHost: (DiscoveredHost) -> Unit, onManualOpen: () -> Unit) {
    SectionCard(title = "\u65E0\u7EBF\u8FDE\u63A5") {
        if (state.hosts.isEmpty()) {
            Text(
                "\u6B63\u5728\u5C40\u57DF\u7F51\u4E2D\u67E5\u627E\u7535\u8111\u2026",
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
                            if (host.paired) "\u5DF2\u914D\u5BF9 \u00B7 ${host.address}" else "\u672A\u914D\u5BF9 \u00B7 ${host.address}",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    Icon(SdIcons.Wifi, null, Modifier.size(20.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
        }
        TextButton(onClick = onManualOpen, enabled = !state.connecting) {
            Icon(SdIcons.Add, null, Modifier.size(18.dp))
            Spacer(Modifier.size(8.dp))
            Text("\u624B\u52A8\u8F93\u5165\u5730\u5740")
        }
    }
}

/** Follows the window, not the device: tablets, foldables, split screen and rotation. */
@Composable
fun PhoneHomeScreen(
    state: PhoneHomeState,
    onUsb: () -> Unit,
    onHost: (DiscoveredHost) -> Unit,
    onManual: (String) -> Unit,
    modifier: Modifier = Modifier,
) {
    var manualOpen by remember { mutableStateOf(false) }
    var aboutOpen by remember { mutableStateOf(false) }

    BoxWithConstraints(modifier.fillMaxSize(), contentAlignment = Alignment.TopCenter) {
        val compactHeight = maxHeight < 480.dp
        when (homeLayoutFor(maxWidth.value)) {
            HomeLayout.Single -> Column(
                Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = 20.dp, vertical = 28.dp),
                verticalArrangement = Arrangement.spacedBy(16.dp),
            ) {
                ConnectPane(state, onUsb, { aboutOpen = true }, compactHeight)
                WirelessPane(state, onHost) { manualOpen = true }
            }
            // On a large tablet the two panes stay readable rather than spreading out.
            HomeLayout.TwoPanes -> Row(
                Modifier.widthIn(max = 1040.dp).fillMaxSize().padding(horizontal = 24.dp, vertical = if (compactHeight) 12.dp else 32.dp),
                horizontalArrangement = Arrangement.spacedBy(24.dp),
            ) {
                Column(Modifier.weight(1f).verticalScroll(rememberScrollState())) { ConnectPane(state, onUsb, { aboutOpen = true }, compactHeight) }
                Column(Modifier.weight(1f).verticalScroll(rememberScrollState())) { WirelessPane(state, onHost) { manualOpen = true } }
            }
        }
    }

    if (aboutOpen) AboutDialog(onDismiss = { aboutOpen = false })

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

/**
 * Hidden edge handle plus a floating toolbar: the only chrome over the desktop.
 * Settings open beside the content on a wide window and above the toolbar on a narrow one.
 */
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
    BoxWithConstraints(modifier.fillMaxSize()) {
        if (!toolbarOpen) return@BoxWithConstraints
        val windowWidth = maxWidth
        val windowHeight = maxHeight
        val wide = windowWidth >= 720.dp
        val toolbarSpace = 96.dp

        @Composable
        fun panel(panelModifier: Modifier) {
            Surface(panelModifier, shape = MaterialTheme.shapes.large, color = MaterialTheme.colorScheme.surfaceContainerHigh) {
                Box(Modifier.verticalScroll(rememberScrollState()).padding(20.dp)) { settings() }
            }
        }

        if (settingsOpen && wide) {
            panel(
                // Clear of the toolbar, which a short window would otherwise put on top of it.
                Modifier.align(Alignment.CenterEnd).padding(end = 20.dp, top = 16.dp, bottom = toolbarSpace)
                    .width(420.dp).heightIn(max = windowHeight - 16.dp - toolbarSpace),
            )
        }
        Column(
            Modifier.align(Alignment.BottomCenter).padding(bottom = 16.dp, start = 16.dp, end = 16.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            if (settingsOpen && !wide) {
                panel(Modifier.widthIn(max = 560.dp).heightIn(max = windowHeight - toolbarSpace - 16.dp))
            }
            HorizontalFloatingToolbar(expanded = true) {
                FilledTonalIconButton(onClick = onOpenSettings) { Icon(SdIcons.Tune, "\u663E\u793A\u8BBE\u7F6E") }
                if (info != null) {
                    // The full line only where it fits: a narrow phone keeps the essentials.
                    val text = if (windowWidth >= 560.dp) "${info.resolution} \u00B7 ${info.fps}Hz \u00B7 ${info.bitrate} \u00B7 ${info.rttMs}ms"
                    else "${info.fps}Hz \u00B7 ${info.rttMs}ms"
                    Text(
                        text,
                        style = MaterialTheme.typography.labelLarge,
                        modifier = Modifier.align(Alignment.CenterVertically).padding(horizontal = 12.dp),
                        maxLines = 1,
                    )
                }
                FilledTonalButton(onClick = onDisconnect) { Text("\u65AD\u5F00") }
            }
        }
    }
}
