// The only chrome over the desktop while streaming: a handle resting on a side of the screen. Tapped,
// it pulls out as a bar with a floating panel under it. It is the same arrangement on every screen,
// so the handle only rests where the panel has room below it.
package com.sidedroid.design

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.expandHorizontally
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.shrinkHorizontally
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.onClick
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.launch

enum class DockSide { Start, End }

/** Which side the handle rests on, and how far along the places it may rest, from 0 to 1. */
data class DockPlacement(val side: DockSide = DockSide.End, val fraction: Float = 0f)

/** The side nearest to where a handle was let go. Pure, for testing. */
fun dockSideFor(centerX: Float, width: Float): DockSide = if (centerX < width / 2) DockSide.Start else DockSide.End

/**
 * The lowest top a handle may rest at, with the panel still fitting under it. On a screen too short
 * for that, the top margin: the panel scrolls there. Pure, for testing.
 */
fun dockLowestTop(height: Float, handleHeight: Float, gap: Float, panelHeight: Float, margin: Float): Float =
    (height - handleHeight - gap - panelHeight - margin).coerceAtLeast(margin)

/** Where a handle let go at this top comes to rest. Pure, for testing. */
fun dockFractionFor(top: Float, margin: Float, lowestTop: Float): Float =
    if (lowestTop <= margin) 0f else ((top - margin) / (lowestTop - margin)).coerceIn(0f, 1f)

private val HandleWidth = 52.dp
private val HandleHeight = 56.dp
private val PanelGap = 12.dp
private val ScreenMargin = 16.dp
private val BarMaxWidth = 400.dp
// About what the settings and the disconnect button take. Used only to choose where to rest.
private val PanelRoom = 460.dp
private const val IdleAlpha = 0.45f

/**
 * @param tabModifier for what only the platform can do, such as keeping the system's edge
 *   gestures off the handle.
 */
@Composable
fun StreamDock(
    info: StreamInfo?,
    placement: DockPlacement,
    onPlacementChange: (DockPlacement) -> Unit,
    onDisconnect: () -> Unit,
    modifier: Modifier = Modifier,
    tabModifier: Modifier = Modifier,
    initiallyOpen: Boolean = false,
    settings: @Composable () -> Unit,
) {
    var open by remember { mutableStateOf(initiallyOpen) }
    var dragging by remember { mutableStateOf(false) }
    val latestPlacement by rememberUpdatedState(placement)
    val latestOnChange by rememberUpdatedState(onPlacementChange)
    val scope = rememberCoroutineScope()
    // How far the handle is from where its placement says it should be, while it is being moved.
    val driftX = remember { Animatable(0f) }
    val driftY = remember { Animatable(0f) }

    BoxWithConstraints(modifier.fillMaxSize()) {
        val density = LocalDensity.current
        val widthPx = with(density) { maxWidth.toPx() }
        val heightPx = with(density) { maxHeight.toPx() }
        val handleWidthPx = with(density) { HandleWidth.toPx() }
        val handleHeightPx = with(density) { HandleHeight.toPx() }
        val marginPx = with(density) { ScreenMargin.toPx() }
        val lowestTopPx = dockLowestTop(heightPx, handleHeightPx, with(density) { PanelGap.toPx() }, with(density) { PanelRoom.toPx() }, marginPx)
        val restingTopPx = marginPx + placement.fraction * (lowestTopPx - marginPx)
        val restingTop = with(density) { restingTopPx.toDp() }
        val atEnd = placement.side == DockSide.End
        val edge = if (atEnd) Alignment.TopEnd else Alignment.TopStart

        val barWidth = minOf(BarMaxWidth, maxWidth - ScreenMargin)
        val panelWidth = barWidth - ScreenMargin * 2
        val panelTop = restingTop + HandleHeight + PanelGap

        AnimatedVisibility(
            visible = open,
            modifier = Modifier.align(edge).offset(x = if (atEnd) -ScreenMargin else ScreenMargin, y = panelTop),
            enter = slideInHorizontally { full -> if (atEnd) full else -full } + fadeIn(),
            exit = slideOutHorizontally { full -> if (atEnd) full else -full } + fadeOut(),
        ) {
            Surface(
                Modifier.width(panelWidth).heightIn(max = (maxHeight - panelTop - ScreenMargin).coerceAtLeast(0.dp)),
                shape = MaterialTheme.shapes.large,
                color = MaterialTheme.colorScheme.surfaceContainerHigh,
            ) {
                Column {
                    // The foot stays in view however little room the settings have.
                    Box(Modifier.weight(1f, fill = false).verticalScroll(rememberScrollState()).padding(start = 20.dp, end = 20.dp, top = 20.dp, bottom = 12.dp)) { settings() }
                    FilledTonalButton(onClick = onDisconnect, Modifier.fillMaxWidth().padding(start = 20.dp, end = 20.dp, bottom = 16.dp)) { Text("断开") }
                }
            }
        }

        val shownAlpha by animateFloatAsState(if (open || dragging) 1f else IdleAlpha)
        val moved = dragging || driftX.value != 0f
        Surface(
            Modifier
                .align(edge)
                .offset { IntOffset(driftX.value.toInt(), (restingTopPx + driftY.value).toInt()) }
                .alpha(shownAlpha),
            // Flat against the edge. Away from it, while moving, it has no reason to be.
            shape = if (moved) RoundedCornerShape(HandleHeight / 2) else sideShape(atEnd),
            color = MaterialTheme.colorScheme.surfaceContainerHigh,
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                val handle: @Composable () -> Unit = {
                    Box(
                        Modifier
                            .size(HandleWidth, HandleHeight)
                            .then(tabModifier)
                            .semantics {
                                contentDescription = if (open) "收起" else "展开"
                                onClick {
                                    open = !open
                                    true
                                }
                            }
                            .pointerInput(Unit) { detectTapGestures { open = !open } }
                            .pointerInput(open, atEnd, widthPx, heightPx) {
                                // Open, it stays where it is: what it has opened hangs from it.
                                if (open) return@pointerInput
                                detectDragGestures(
                                    onDragStart = { dragging = true },
                                    onDragEnd = {
                                        dragging = false
                                        val p = latestPlacement
                                        val startLeft = if (p.side == DockSide.End) widthPx - handleWidthPx else 0f
                                        val left = startLeft + driftX.value
                                        val top = marginPx + p.fraction * (lowestTopPx - marginPx) + driftY.value
                                        val next = DockPlacement(dockSideFor(left + handleWidthPx / 2, widthPx), dockFractionFor(top, marginPx, lowestTopPx))
                                        val nextLeft = if (next.side == DockSide.End) widthPx - handleWidthPx else 0f
                                        // Same place on the screen, now measured from where it will rest.
                                        val fromEdge = left - nextLeft
                                        val fromTop = top - (marginPx + next.fraction * (lowestTopPx - marginPx))
                                        latestOnChange(next)
                                        scope.launch {
                                            driftX.snapTo(fromEdge)
                                            driftY.snapTo(fromTop)
                                            launch { driftY.animateTo(0f) }
                                            driftX.animateTo(0f)
                                        }
                                    },
                                    onDragCancel = {
                                        dragging = false
                                        scope.launch {
                                            launch { driftY.animateTo(0f) }
                                            driftX.animateTo(0f)
                                        }
                                    },
                                ) { change, amount ->
                                    change.consume()
                                    val p = latestPlacement
                                    val startLeft = if (p.side == DockSide.End) widthPx - handleWidthPx else 0f
                                    val top = marginPx + p.fraction * (lowestTopPx - marginPx)
                                    scope.launch {
                                        driftX.snapTo((driftX.value + amount.x).coerceIn(-startLeft, widthPx - handleWidthPx - startLeft))
                                        // Anywhere while held. Letting go is what brings it back to where it fits.
                                        driftY.snapTo((driftY.value + amount.y).coerceIn(-top, heightPx - handleHeightPx - top))
                                    }
                                }
                            },
                        contentAlignment = Alignment.Center,
                    ) {
                        Box(
                            Modifier.size(40.dp).clip(CircleShape).background(MaterialTheme.colorScheme.secondaryContainer),
                            contentAlignment = Alignment.Center,
                        ) {
                            Icon(if (open) SdIcons.Close else SdIcons.Tune, null, Modifier.size(22.dp), tint = MaterialTheme.colorScheme.onSecondaryContainer)
                        }
                    }
                }
                // Pulled out from the edge, so the button travels to the far end of the bar.
                val status: @Composable () -> Unit = {
                    AnimatedVisibility(
                        visible = open,
                        enter = expandHorizontally(expandFrom = if (atEnd) Alignment.End else Alignment.Start) + fadeIn(),
                        exit = shrinkHorizontally(shrinkTowards = if (atEnd) Alignment.End else Alignment.Start) + fadeOut(),
                    ) {
                        // As long as what it says, with the words next to the button.
                        Box(Modifier.widthIn(max = barWidth - HandleWidth).padding(start = if (atEnd) 4.dp else 20.dp, end = if (atEnd) 20.dp else 4.dp)) {
                            if (info != null) {
                                Text(
                                    "${info.resolution} · ${info.fps}Hz · ${info.bitrate} · ${info.transport} · ${info.rttMs}ms",
                                    style = MaterialTheme.typography.labelLarge,
                                    maxLines = 1,
                                    overflow = TextOverflow.Ellipsis,
                                )
                            }
                        }
                    }
                }
                if (atEnd) {
                    handle()
                    status()
                } else {
                    status()
                    handle()
                }
            }
        }
    }
}

// Flat where it meets the edge of the screen.
private fun sideShape(atEnd: Boolean) = if (atEnd) {
    RoundedCornerShape(topStart = HandleHeight / 2, bottomStart = HandleHeight / 2)
} else {
    RoundedCornerShape(topEnd = HandleHeight / 2, bottomEnd = HandleHeight / 2)
}
