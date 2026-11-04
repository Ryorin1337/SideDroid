package com.sidedroid.design

import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.ButtonGroupDefaults
import androidx.compose.material3.ContainedLoadingIndicator
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialShapes
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.ToggleButton
import androidx.compose.material3.ToggleButtonDefaults
import androidx.compose.material3.toShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp

/** Grouped content. Surfaces carry the hierarchy, so no outlines or shadows. */
@Composable
fun SectionCard(modifier: Modifier = Modifier, title: String? = null, content: @Composable () -> Unit) {
    Surface(
        modifier = modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.large,
        color = MaterialTheme.colorScheme.surfaceContainerLow,
    ) {
        Column(Modifier.padding(20.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
            if (title != null) {
                Text(title, style = MaterialTheme.typography.titleMedium, color = MaterialTheme.colorScheme.onSurface)
            }
            content()
        }
    }
}

enum class ConnectionVisual { Idle, Waiting, Connected, Problem }

/** The one large element on each screen: tells the state at a glance. */
@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun StatusHero(
    state: ConnectionVisual,
    headline: String,
    supporting: String,
    modifier: Modifier = Modifier,
    icon: ImageVector = SdIcons.Phone,
    actions: (@Composable RowScope.() -> Unit)? = null,
) {
    val container by animateColorAsState(
        when (state) {
            ConnectionVisual.Connected -> MaterialTheme.colorScheme.primaryContainer
            ConnectionVisual.Idle, ConnectionVisual.Waiting -> MaterialTheme.colorScheme.surfaceContainerHigh
            ConnectionVisual.Problem -> MaterialTheme.colorScheme.errorContainer
        },
    )
    val onContainer = when (state) {
        ConnectionVisual.Connected -> MaterialTheme.colorScheme.onPrimaryContainer
        ConnectionVisual.Idle, ConnectionVisual.Waiting -> MaterialTheme.colorScheme.onSurface
        ConnectionVisual.Problem -> MaterialTheme.colorScheme.onErrorContainer
    }
    val corner by animateDpAsState(if (state == ConnectionVisual.Connected) 44.dp else 28.dp)

    Surface(modifier.fillMaxWidth(), shape = RoundedCornerShape(corner), color = container, contentColor = onContainer) {
        Column(Modifier.padding(24.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(18.dp)) {
                AnimatedContent(state, label = "hero-mark") { s ->
                    when (s) {
                        ConnectionVisual.Idle -> ShapeBadge(icon, MaterialTheme.colorScheme.primaryContainer, MaterialTheme.colorScheme.onPrimaryContainer)
                        ConnectionVisual.Waiting -> ContainedLoadingIndicator(Modifier.size(64.dp))
                        ConnectionVisual.Connected -> ShapeBadge(icon, MaterialTheme.colorScheme.primary, MaterialTheme.colorScheme.onPrimary)
                        ConnectionVisual.Problem -> ShapeBadge(SdIcons.Warning, MaterialTheme.colorScheme.error, MaterialTheme.colorScheme.onError)
                    }
                }
                Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                    Text(headline, style = MaterialTheme.typography.headlineSmall, maxLines = 2, overflow = TextOverflow.Ellipsis)
                    Text(supporting, style = MaterialTheme.typography.bodyMedium, color = onContainer.copy(alpha = 0.78f))
                }
            }
            if (actions != null) {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically, content = actions)
            }
        }
    }
}

@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
private fun ShapeBadge(icon: ImageVector, container: Color, content: Color) {
    Box(
        Modifier.size(64.dp).clip(MaterialShapes.Cookie9Sided.toShape()).background(container),
        contentAlignment = Alignment.Center,
    ) {
        Icon(icon, null, Modifier.size(30.dp), tint = content)
    }
}

data class Choice<T>(val label: String, val value: T)

/** Connected button group, the Expressive replacement for segmented buttons. */
@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun <T> ChoiceRow(label: String, choices: List<Choice<T>>, selected: T, onSelect: (T) -> Unit, enabled: Boolean = true) {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(label, style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(ButtonGroupDefaults.ConnectedSpaceBetween)) {
            choices.forEachIndexed { i, c ->
                ToggleButton(
                    checked = c.value == selected,
                    onCheckedChange = { onSelect(c.value) },
                    enabled = enabled,
                    modifier = Modifier.weight(1f),
                    contentPadding = ToggleButtonDefaults.ContentPadding,
                    shapes = when (i) {
                        0 -> ButtonGroupDefaults.connectedLeadingButtonShapes()
                        choices.lastIndex -> ButtonGroupDefaults.connectedTrailingButtonShapes()
                        else -> ButtonGroupDefaults.connectedMiddleButtonShapes()
                    },
                ) {
                    Text(c.label, maxLines = 1, style = MaterialTheme.typography.labelLarge)
                }
            }
        }
    }
}

@Composable
fun MetricTile(label: String, value: String, modifier: Modifier = Modifier) {
    Surface(modifier, shape = MaterialTheme.shapes.medium, color = MaterialTheme.colorScheme.surfaceContainerHigh) {
        Column(Modifier.padding(horizontal = 14.dp, vertical = 12.dp)) {
            Text(value, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.Bold, maxLines = 1)
            Text(label, style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1)
        }
    }
}

/** Small colored dot with text, for transport and connection labels. */
@Composable
fun StateLabel(text: String, color: Color, modifier: Modifier = Modifier) {
    Row(modifier, verticalAlignment = Alignment.CenterVertically) {
        Box(Modifier.size(8.dp).clip(CircleShape).background(color))
        Box(Modifier.width(8.dp))
        Text(text, style = MaterialTheme.typography.labelLarge)
    }
}

/** A pairing code set large, with the separator kept readable. */
@Composable
fun PairingCodeText(code: String) {
    Text(
        code,
        style = MaterialTheme.typography.displaySmall,
        color = MaterialTheme.colorScheme.primary,
        maxLines = 1,
    )
}
