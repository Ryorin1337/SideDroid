// The about dialog, shared by the console and the phone app.
package com.sidedroid.design

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.unit.dp

object AppInfo {
    // scripts/verify_artifacts.ps1 checks this against the packaged version.
    const val VERSION = "0.0.2"
    const val AUTHOR = "Ryorin"
    const val REPOSITORY_URL = "https://github.com/Ryorin1337/SideDroid"
    const val REPOSITORY_LABEL = "github.com/Ryorin1337/SideDroid"
}

@Composable
fun AboutDialog(onDismiss: () -> Unit) {
    val uriHandler = LocalUriHandler.current
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("关于 SideDroid") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("版本 ${AppInfo.VERSION}", style = MaterialTheme.typography.bodyMedium)
                Text("作者 ${AppInfo.AUTHOR}", style = MaterialTheme.typography.bodyMedium)
                Text("开源于 GitHub，使用 MIT 许可证", style = MaterialTheme.typography.bodyMedium)
                Text(AppInfo.REPOSITORY_LABEL, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.primary)
            }
        },
        confirmButton = {
            // A device with no browser must not bring the app down.
            TextButton(onClick = { runCatching { uriHandler.openUri(AppInfo.REPOSITORY_URL) } }) { Text("打开 GitHub") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("关闭") } },
    )
}
