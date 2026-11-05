package com.sidedroid.console

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.sidedroid.design.Choice
import com.sidedroid.design.ChoiceRow
import com.sidedroid.design.DisplaySettings
import com.sidedroid.design.DisplayValue
import com.sidedroid.design.ConnectionVisual
import com.sidedroid.design.MetricTile
import com.sidedroid.design.PairingCodeText
import com.sidedroid.design.SdIcons
import com.sidedroid.design.SectionCard
import com.sidedroid.design.SettingDropdownRow
import com.sidedroid.design.StatusHero
import com.sidedroid.design.UpdateCard

data class ConsoleActions(
    val onSetDisplay: (deviceId: String?, HostSettings) -> Unit = { _, _ -> },
    val onDisconnect: (deviceId: String) -> Unit = {},
    val onInstallDriver: () -> Unit = {},
    val onStartPairing: () -> Unit = {},
    val onCancelPairing: () -> Unit = {},
    val onUnpair: (String) -> Unit = {},
    val onStartService: () -> Unit = {},
    val onInstallUpdate: () -> Unit = {},
    val onSetConsoleAutostart: (Boolean) -> Unit = {},
    val onSetVddGpu: (String) -> Unit = {},
)


private fun errorText(code: String) = when (code) {
    "virtual_display_missing" -> "未找到虚拟显示驱动"
    "virtual_display_config_denied" -> "无法写入虚拟显示配置，已使用最接近的分辨率"
    "virtual_display_layout" -> "无法把虚拟屏设为独立显示器"
    "virtual_display_count" -> "无法再增加虚拟显示器"
    "virtual_display_busy" -> "虚拟屏正被其他程序占用，无法增加显示器"
    "display_service_failed" -> "显示服务出错，请重新安装 SideDroid"
    "port_in_use" -> "端口 27183 被其他程序占用，正在重试"
    "no_encoder" -> "没有找到可用的硬件编码器"
    else -> code
}

private fun waitingHint(s: HostStatus) = when {
    s.usbUnauthorized -> "请在手机上允许 USB 调试"
    s.usbCount > 0 -> "已检测到 USB 设备，在手机上打开 SideDroid"
    else -> "用 USB 连接手机，或在同一网络下打开 SideDroid"
}

@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun ConsoleScreen(
    status: HostStatus,
    actions: ConsoleActions,
    modifier: Modifier = Modifier,
    serviceStopped: Boolean = false,
    startupFailed: Boolean = false,
) {
    var hiddenUpdate by remember { mutableStateOf<String?>(null) }
    var selectedId by remember { mutableStateOf<String?>(null) }
    // Whoever was selected may have left. The oldest device takes over.
    val selected = status.devices.firstOrNull { it.id == selectedId } ?: status.devices.firstOrNull()
    val error = selected?.error?.ifEmpty { null } ?: status.error
    Column(
        modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(start = 20.dp, end = 20.dp, bottom = 20.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        val problem = !status.reachable || !status.virtualDisplayInstalled || error.isNotEmpty()
        if (status.devices.size > 1 && selected != null) {
            ChoiceRow(
                "已连接 ${status.devices.size} 台设备（最多 ${status.maxDevices} 台）",
                status.devices.map { Choice(it.name.ifEmpty { "Android" }, it.id) },
                selected.id,
                { selectedId = it },
            )
        }
        when {
            !status.reachable && serviceStopped -> StatusHero(
                ConnectionVisual.Idle,
                "服务已停止",
                "启动后手机才能连接",
                actions = { Button(onClick = actions.onStartService) { Text("启动服务") } },
            )
            !status.reachable && startupFailed -> StatusHero(
                ConnectionVisual.Problem,
                "服务没有启动",
                "详情见 %LOCALAPPDATA%\\SideDroid\\host.log",
                actions = { Button(onClick = actions.onStartService) { Text("重试") } },
            )
            !status.reachable -> StatusHero(ConnectionVisual.Waiting, "正在启动服务", "稍等片刻")
            !status.virtualDisplayInstalled -> StatusHero(
                ConnectionVisual.Problem,
                "还差一步",
                "需要安装虚拟显示驱动，Windows 会请你确认。",
                actions = { Button(onClick = actions.onInstallDriver) { Text("安装驱动") } },
            )
            selected != null -> StatusHero(
                if (error.isNotEmpty()) ConnectionVisual.Problem else ConnectionVisual.Connected,
                selected.name.ifEmpty { "Android" },
                if (error.isNotEmpty()) errorText(error)
                else "${if (selected.transport == "wifi") "Wi-Fi" else "USB"} · ${selected.encoder} ${selected.codec}",
                icon = if (selected.transport == "wifi") SdIcons.Wifi else SdIcons.Usb,
                actions = { Button(onClick = { actions.onDisconnect(selected.id) }) { Text("断开连接") } },
            )
            error.isNotEmpty() -> StatusHero(ConnectionVisual.Problem, "暂时无法连接", errorText(error))
            else -> StatusHero(ConnectionVisual.Waiting, "等待手机连接", waitingHint(status))
        }

        if (selected != null && !problem) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                MetricTile("延迟", "${selected.rttMs} ms", Modifier.weight(1f))
                MetricTile("码率", "%.1f Mbps".format(selected.kbps / 1000.0), Modifier.weight(1f))
                MetricTile("帧率", "${selected.measuredFps.toInt()} fps", Modifier.weight(1f))
            }
            if (selected.encodeW != selected.displayW) {
                Text(
                    "网络受限，当前编码 ${selected.encodeW}×${selected.encodeH}",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.tertiary,
                )
            }
        }

        status.update?.takeUnless { it.version == hiddenUpdate }?.let { update ->
            UpdateCard(update, actions.onInstallUpdate, { hiddenUpdate = update.version })
        }

        // Connected devices each have their own. Without any, these are what a new one starts with.
        val set = selected?.settings ?: status.settings
        val allowWireless = status.settings.allowWireless
        SectionCard {
            if (status.devices.size > 1 && selected != null) {
                Text(
                    "${selected.name.ifEmpty { "Android" }} 的显示设置",
                    style = MaterialTheme.typography.titleMedium,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            DisplaySettings(
                selected?.options ?: status.options,
                DisplayValue(set.mirror, set.width, set.height, set.hz, set.scale, set.maxKbps, set.fill),
                showQuality = true,
                enabled = status.reachable,
                onChange = { actions.onSetDisplay(selected?.id, set.copy(allowWireless = allowWireless, fill = it.fill, mirror = it.mirror, width = it.width, height = it.height, hz = it.hz, scale = it.scale, maxKbps = it.maxKbps)) },
            )
        }

        // With one GPU and one way to encode on it, there is nothing to choose.
        if (status.gpus.size > 1 || status.encoders.size > 1) {
            SectionCard {
                Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
                    if (status.gpus.size > 1) {
                        SettingDropdownRow(
                            SdIcons.AspectRatio,
                            "虚拟屏显卡",
                            if (status.vddGpuEditable) "所有副屏共用，更改时画面会中断几秒" else "这个驱动不是 SideDroid 安装的，不能更改",
                            listOf(Choice("自动（推荐）", "")) + status.gpus.map { Choice(if (it.startsWith("Microsoft")) "微软软件渲染（很慢）" else it, it) },
                            status.vddGpu,
                            actions.onSetVddGpu,
                            enabled = status.reachable && status.vddGpuEditable,
                        )
                    }
                    SettingDropdownRow(
                        SdIcons.Hd,
                        "编码器",
                        when {
                            selected?.encoderFallback == true -> "所选编码器不可用，已改用自动"
                            set.encoder.startsWith("sw|") -> "用 CPU 编码，只适合没有可用显卡时"
                            selected?.crossAdapter == true -> "和画面不在同一张显卡上，占用更高"
                            selected != null && status.devices.size > 1 -> "只作用于 ${selected.name.ifEmpty { "这台设备" }}"
                            else -> "把画面压缩成视频流"
                        },
                        listOf(Choice("自动（推荐）", "")) + status.encoders.map { Choice(it.label, it.id) },
                        set.encoder,
                        { actions.onSetDisplay(selected?.id, set.copy(allowWireless = allowWireless, encoder = it)) },
                        enabled = status.reachable,
                    )
                }
            }
        }

        SectionCard {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text("无线连接", style = MaterialTheme.typography.titleMedium)
                    Text(
                        "允许同一网络下已配对的手机连接",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Switch(allowWireless, { actions.onSetDisplay(selected?.id, set.copy(allowWireless = it)) }, enabled = status.reachable)
            }
            if (allowWireless) {
                status.paired.forEach { d ->
                    Surface(shape = MaterialTheme.shapes.medium, color = MaterialTheme.colorScheme.surfaceContainerHigh) {
                        Row(Modifier.fillMaxWidth().padding(start = 16.dp, end = 4.dp), verticalAlignment = Alignment.CenterVertically) {
                            Icon(SdIcons.Phone, null, Modifier.size(20.dp), tint = MaterialTheme.colorScheme.primary)
                            Text(
                                d.name,
                                Modifier.weight(1f).padding(start = 12.dp),
                                style = MaterialTheme.typography.bodyMedium,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                            )
                            IconButton(onClick = { actions.onUnpair(d.id) }) { Icon(SdIcons.Delete, "移除 ${d.name}", Modifier.size(20.dp)) }
                        }
                    }
                }
                FilledTonalButton(onClick = actions.onStartPairing, enabled = status.reachable) {
                    Icon(SdIcons.Add, null, Modifier.size(18.dp))
                    Spacer(Modifier.size(8.dp))
                    Text("添加设备")
                }
            }
        }

        SectionCard {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text("开机时打开控制台", style = MaterialTheme.typography.titleMedium)
                    Text(
                        "手机随时都能连接，这项只决定是否打开这个窗口",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Switch(status.consoleAutostart, actions.onSetConsoleAutostart, enabled = status.reachable)
            }
        }
    }

    if (status.reachable && !status.consoleAutostartAsked && status.pairingCode.isEmpty()) {
        AlertDialog(
            onDismissRequest = {},
            title = { Text("开机时打开控制台？") },
            text = { Text("以后可以在设置里更改。") },
            confirmButton = { TextButton(onClick = { actions.onSetConsoleAutostart(true) }) { Text("打开") } },
            dismissButton = { TextButton(onClick = { actions.onSetConsoleAutostart(false) }) { Text("不用") } },
        )
    }

    if (status.pairingCode.isNotEmpty()) {
        AlertDialog(
            onDismissRequest = actions.onCancelPairing,
            title = { Text("在手机上输入配对码") },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp), horizontalAlignment = Alignment.CenterHorizontally, modifier = Modifier.fillMaxWidth()) {
                    PairingCodeText(status.pairingCode)
                    Text("${status.pairingSeconds} 秒后失效", style = MaterialTheme.typography.bodySmall)
                }
            },
            confirmButton = { TextButton(onClick = actions.onCancelPairing) { Text("取消") } },
        )
    }
}
