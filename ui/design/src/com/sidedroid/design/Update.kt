// The update prompt, shared by the console and the phone app.
package com.sidedroid.design

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.Button
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp

enum class UpdateStage { Available, Downloading, Installing, Failed }

data class UpdateUi(
    val version: String,
    val notes: String = "",
    val stage: UpdateStage = UpdateStage.Available,
    val progress: Int = 0,
)

/** Numbers separated by dots, compared one by one. Anything else is never newer. */
fun isNewerVersion(candidate: String, current: String): Boolean {
    val pattern = Regex("""\d{1,9}(\.\d{1,9}){0,3}""")
    if (!pattern.matches(candidate) || !pattern.matches(current)) return false
    val a = candidate.split('.').map { it.toLong() }
    val b = current.split('.').map { it.toLong() }
    for (i in 0 until maxOf(a.size, b.size)) {
        val x = a.getOrElse(i) { 0 }
        val y = b.getOrElse(i) { 0 }
        if (x != y) return x > y
    }
    return false
}

@Composable
fun UpdateCard(update: UpdateUi, onInstall: () -> Unit, onLater: () -> Unit, modifier: Modifier = Modifier) {
    SectionCard(modifier, title = "发现新版本 ${update.version}") {
        if (update.notes.isNotBlank()) {
            Text(update.notes, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        when (update.stage) {
            UpdateStage.Downloading -> {
                LinearProgressIndicator(progress = { update.progress / 100f }, modifier = Modifier.fillMaxWidth())
                Text("正在下载 ${update.progress}%", style = MaterialTheme.typography.bodySmall)
            }
            UpdateStage.Installing -> Text("正在打开系统安装程序，请按提示操作", style = MaterialTheme.typography.bodySmall)
            UpdateStage.Available, UpdateStage.Failed -> {
                if (update.stage == UpdateStage.Failed) {
                    Text("更新失败，请检查网络后重试", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(onClick = onInstall) { Text(if (update.stage == UpdateStage.Failed) "重试" else "立即更新") }
                    TextButton(onClick = onLater) { Text("稍后") }
                }
            }
        }
    }
}
