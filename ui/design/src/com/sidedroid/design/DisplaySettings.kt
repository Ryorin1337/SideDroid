// The display settings, shared by the console and the phone's overlay.
package com.sidedroid.design

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp

/** In place of a width and height: follow the PC's main display. Matches the host. */
const val RESOLUTION_MATCH_PC = 0xFFFF

val ScaleSteps = listOf(100, 125, 150, 175, 200, 225, 250, 300, 350)

data class DisplayOptionsUi(
    val phone: Pair<Int, Int> = 0 to 0,
    val pc: Pair<Int, Int> = 0 to 0,
    val resolutions: List<Pair<Int, Int>> = emptyList(),
    val rates: List<Int> = listOf(60, 90, 120),
    val recommendedScale: Int = 0,
    val currentScale: Int = 0,
)

data class DisplayValue(
    val mirror: Boolean = false,
    val width: Int = 0,
    val height: Int = 0,
    val hz: Int = 0,
    val scale: Int = 0,
    val maxKbps: Int = 0,
    val fill: Int = FILL_FIT,
)

private fun sizeText(s: Pair<Int, Int>) = "${s.first}×${s.second}"

fun resolutionChoices(o: DisplayOptionsUi): List<Choice<Pair<Int, Int>>> = buildList {
    add(Choice(if (o.phone.first > 0) "自适应手机 ${sizeText(o.phone)}（推荐）" else "自适应手机（推荐）", 0 to 0))
    add(Choice(if (o.pc.first > 0) "自适应电脑 ${sizeText(o.pc)}" else "自适应电脑", RESOLUTION_MATCH_PC to RESOLUTION_MATCH_PC))
    o.resolutions.forEach { add(Choice(sizeText(it), it)) }
}

/** The recommended step is stored as 0, so that it can follow the resolution. */
fun scaleChoices(o: DisplayOptionsUi): List<Choice<Int>> = ScaleSteps.map { step ->
    if (step == o.recommendedScale) Choice("$step%（推荐）", 0)
    else Choice("$step%", step)
}.let { list ->
    if (o.recommendedScale == 0) listOf(Choice("自动（推荐）", 0)) + list else list
}

fun rateChoices(o: DisplayOptionsUi): List<Choice<Int>> =
    listOf(Choice("最高（推荐）", 0)) + o.rates.map { Choice("$it Hz", it) }

val QualityChoices = listOf(
    Choice("自适应（推荐）", 0),
    Choice("省流 10 Mbps", 10_000),
    Choice("均衡 25 Mbps", 25_000),
    Choice("高清 50 Mbps", 50_000),
)

/** One setting on one line, as in Windows' display settings: what it is, then a list. */
@Composable
fun <T> SettingDropdownRow(
    icon: ImageVector,
    title: String,
    supporting: String,
    choices: List<Choice<T>>,
    selected: T,
    onSelect: (T) -> Unit,
    enabled: Boolean = true,
    initiallyOpen: Boolean = false,
    buttonLabel: String? = null,
) {
    var open by remember { mutableStateOf(initiallyOpen) }
    val label = buttonLabel ?: choices.firstOrNull { it.value == selected }?.label ?: when (selected) {
        is Pair<*, *> -> "${selected.first}×${selected.second}"
        is Int -> "$selected"
        else -> "$selected"
    }
    val content = if (enabled) MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.onSurface.copy(alpha = 0.38f)

    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Icon(icon, null, Modifier.size(24.dp), tint = if (enabled) MaterialTheme.colorScheme.primary else content)
        Column(Modifier.padding(start = 14.dp, end = 10.dp).weight(1f)) {
            Text(title, style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.SemiBold, color = content)
            Text(
                supporting,
                style = MaterialTheme.typography.bodySmall,
                color = if (enabled) MaterialTheme.colorScheme.onSurfaceVariant else content,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
        }
        Box {
            FilledTonalButton(onClick = { open = true }, enabled = enabled, modifier = Modifier.widthIn(max = 210.dp)) {
                Text(label, style = MaterialTheme.typography.labelLarge, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f, fill = false))
                Icon(SdIcons.ArrowDropDown, null, Modifier.size(20.dp))
            }
            DropdownMenu(open, onDismissRequest = { open = false }, modifier = Modifier.heightIn(max = 380.dp)) {
                choices.forEach { c ->
                    DropdownMenuItem(
                        text = {
                            Text(c.label, fontWeight = if (c.value == selected) FontWeight.Bold else FontWeight.Normal, maxLines = 1)
                        },
                        onClick = {
                            open = false
                            onSelect(c.value)
                        },
                    )
                }
            }
        }
    }
}

@Composable
fun DisplaySettings(
    options: DisplayOptionsUi,
    value: DisplayValue,
    onChange: (DisplayValue) -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    showQuality: Boolean = false,
    openRow: String? = null,
) {
    // A value that is no longer recommended stays selected by its number; the
    // recommended one is stored as 0, and both show as the same line in the list.
    val shownScale = if (value.scale == options.recommendedScale) 0 else value.scale
    val virtual = enabled && !value.mirror
    val isPhone = value.width == 0 && value.height == 0
    val isPc = value.width == RESOLUTION_MATCH_PC

    Column(modifier, verticalArrangement = Arrangement.spacedBy(16.dp)) {
        ChoiceRow(
            "模式",
            listOf(Choice("副屏", false), Choice("镜像", true)),
            value.mirror,
            { onChange(value.copy(mirror = it)) },
            enabled = enabled,
        )
        SettingDropdownRow(
            SdIcons.AspectRatio,
            "分辨率",
            when {
                value.mirror -> "使用主屏的分辨率"
                isPhone && options.phone.first > 0 -> sizeText(options.phone)
                isPc && options.pc.first > 0 -> sizeText(options.pc)
                else -> "副屏的尺寸"
            },
            resolutionChoices(options),
            value.width to value.height,
            { (w, h) -> onChange(value.copy(width = w, height = h)) },
            enabled = virtual,
            initiallyOpen = openRow == "resolution",
            // The list has the full wording. The button only has room for the short one.
            buttonLabel = when {
                isPhone -> "\u81EA\u9002\u5E94\u624B\u673A"
                isPc -> "\u81EA\u9002\u5E94\u7535\u8111"
                else -> null
            },
        )
        SettingDropdownRow(
            SdIcons.TextSize,
            "缩放",
            "文字和应用的大小",
            scaleChoices(options),
            shownScale,
            { onChange(value.copy(scale = it)) },
            enabled = virtual,
            initiallyOpen = openRow == "scale",
        )
        SettingDropdownRow(
            SdIcons.Speed,
            "刷新率",
            "越高越流畅",
            rateChoices(options),
            value.hz,
            { onChange(value.copy(hz = it)) },
            enabled = enabled,
        )
        SettingDropdownRow(
            SdIcons.FitScreen,
            "画面适配",
            "画面和屏幕比例不同时",
            FillChoices,
            value.fill,
            { onChange(value.copy(fill = it)) },
            enabled = enabled,
            initiallyOpen = openRow == "fill",
            buttonLabel = FillChoices.firstOrNull { it.value == value.fill }?.label?.substringBefore('（'),
        )
        if (showQuality) {
            SettingDropdownRow(
                SdIcons.Hd,
                "画质",
                "码率上限",
                QualityChoices,
                value.maxKbps,
                { onChange(value.copy(maxKbps = it)) },
                enabled = enabled,
            )
        }
    }
}
