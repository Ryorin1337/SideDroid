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
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.sidedroid.design.DisplaySettings
import com.sidedroid.design.DisplayValue
import com.sidedroid.design.ConnectionVisual
import com.sidedroid.design.MetricTile
import com.sidedroid.design.PairingCodeText
import com.sidedroid.design.SdIcons
import com.sidedroid.design.SectionCard
import com.sidedroid.design.StatusHero

data class ConsoleActions(
    val onSetDisplay: (HostSettings) -> Unit = {},
    val onDisconnect: () -> Unit = {},
    val onInstallDriver: () -> Unit = {},
    val onStartPairing: () -> Unit = {},
    val onCancelPairing: () -> Unit = {},
    val onUnpair: (String) -> Unit = {},
)


private fun errorText(code: String) = when (code) {
    "virtual_display_missing" -> "未找到虚拟显示驱动"
    "virtual_display_config_denied" -> "无法写入虚拟显示配置，已使用最接近的分辨率"
    "virtual_display_layout" -> "无法把虚拟屏设为独立显示器"
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
fun ConsoleScreen(status: HostStatus, actions: ConsoleActions, modifier: Modifier = Modifier) {
    Column(
        modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(start = 20.dp, end = 20.dp, bottom = 20.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        val problem = !status.reachable || !status.virtualDisplayInstalled || status.error.isNotEmpty()
        when {
            !status.reachable -> StatusHero(ConnectionVisual.Waiting, "正在启动服务", "稍等片刻")
            !status.virtualDisplayInstalled -> StatusHero(
                ConnectionVisual.Problem,
                "还差一步",
                "需要安装虚拟显示驱动，Windows 会请你确认。",
                actions = { Button(onClick = actions.onInstallDriver) { Text("安装驱动") } },
            )
            status.connected -> StatusHero(
                if (status.error.isNotEmpty()) ConnectionVisual.Problem else ConnectionVisual.Connected,
                status.device.ifEmpty { "Android" },
                if (status.error.isNotEmpty()) errorText(status.error)
                else "${if (status.transport == "wifi") "Wi-Fi" else "USB"} · ${status.encoder} ${status.codec}",
                icon = if (status.transport == "wifi") SdIcons.Wifi else SdIcons.Usb,
                actions = { Button(onClick = actions.onDisconnect) { Text("断开连接") } },
            )
            else -> StatusHero(ConnectionVisual.Waiting, "等待手机连接", waitingHint(status))
        }

        if (status.connected && !problem) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                MetricTile("延迟", "${status.rttMs} ms", Modifier.weight(1f))
                MetricTile("码率", "%.1f Mbps".format(status.kbps / 1000.0), Modifier.weight(1f))
                MetricTile("帧率", "${status.measuredFps.toInt()} fps", Modifier.weight(1f))
            }
            if (status.encodeW != status.displayW) {
                Text(
                    "网络受限，当前编码 ${status.encodeW}×${status.encodeH}",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.tertiary,
                )
            }
        }

        val set = status.settings
        SectionCard {
            DisplaySettings(
                status.options,
                DisplayValue(set.mirror, set.width, set.height, set.hz, set.scale, set.maxKbps),
                showQuality = true,
                enabled = status.reachable,
                onChange = { actions.onSetDisplay(set.copy(mirror = it.mirror, width = it.width, height = it.height, hz = it.hz, scale = it.scale, maxKbps = it.maxKbps)) },
            )
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
                Switch(set.allowWireless, { actions.onSetDisplay(set.copy(allowWireless = it)) }, enabled = status.reachable)
            }
            if (set.allowWireless) {
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
