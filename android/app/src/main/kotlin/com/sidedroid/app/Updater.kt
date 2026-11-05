package com.sidedroid.app

import android.content.Context
import android.content.Intent
import android.os.Build
import androidx.core.content.FileProvider
import com.sidedroid.design.AppInfo
import com.sidedroid.design.isNewerVersion
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import org.json.JSONObject

/**
 * Checks update.json in the repository and fetches this device's APK from that
 * version's GitHub release. The file names are fixed here, so the manifest can only
 * name a version and a checksum, never another place to download from. Android then
 * refuses any APK not signed with the key of the installed app.
 */
class Updater(private val context: Context) {
    data class Available(val version: String, val notes: String, val abi: String, val sha256: String)

    /** Blocks. Null when up to date, or when it cannot tell. */
    fun check(): Available? = runCatching {
        val text = open(MANIFEST_URL).use { c ->
            c.inputStream.use { input ->
                val bytes = input.readNBytesCompat(MAX_MANIFEST_BYTES)
                String(bytes, Charsets.UTF_8)
            }
        }
        // A byte order mark is what some editors add, and JSON does not allow it.
        val json = JSONObject(text.removePrefix("﻿"))
        val version = json.getString("version")
        if (!isNewerVersion(version, AppInfo.VERSION)) return@runCatching null
        // Most preferred first. No package for this CPU means no update for it.
        val abi = Build.SUPPORTED_ABIS.firstOrNull { it in PACKAGED_ABIS } ?: return@runCatching null
        val sha256 = json.getString(abi).lowercase()
        if (!Regex("[0-9a-f]{64}").matches(sha256)) return@runCatching null
        Available(version, json.optString("notes").take(MAX_NOTES_CHARS), abi, sha256)
    }.getOrNull()

    /** Blocks. Returns the verified APK. */
    fun download(update: Available, onProgress: (Int) -> Unit): File {
        val dir = File(context.cacheDir, UPDATES_DIR).apply { mkdirs() }
        val name = "SideDroid-${update.version}-android-${update.abi}.apk"
        val file = File(dir, name)
        // A finished download from an earlier attempt saves the repeat.
        if (file.exists() && sha256Of(file) == update.sha256) return file

        val part = File(dir, "$name.part")
        try {
            open("$RELEASE_BASE/v${update.version}/$name").use { c ->
                val total = c.contentLengthLong
                c.inputStream.use { input ->
                    part.outputStream().use { output ->
                        val buf = ByteArray(64 * 1024)
                        var done = 0L
                        var shown = -1
                        while (true) {
                            val n = input.read(buf)
                            if (n < 0) break
                            output.write(buf, 0, n)
                            done += n
                            val percent = if (total > 0) (done * 100 / total).toInt() else 0
                            if (percent != shown) {
                                shown = percent
                                onProgress(percent)
                            }
                        }
                    }
                }
            }
            if (sha256Of(part) != update.sha256) throw IOException("checksum mismatch")
            if (!part.renameTo(file)) throw IOException("cannot move the download into place")
        } finally {
            part.delete()
        }
        return file
    }

    /** Hands the APK to Android's installer, which asks the user. */
    fun install(apk: File) {
        val uri = FileProvider.getUriForFile(context, "${context.packageName}.updates", apk)
        context.startActivity(
            Intent(Intent.ACTION_VIEW)
                .setDataAndType(uri, "application/vnd.android.package-archive")
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK),
        )
    }

    /** What an earlier update left behind. */
    fun deleteDownloads() {
        File(context.cacheDir, UPDATES_DIR).deleteRecursively()
    }

    private fun open(url: String): CloseableConnection {
        val c = URL(url).openConnection() as HttpURLConnection
        c.connectTimeout = 15_000
        c.readTimeout = 30_000
        c.setRequestProperty("User-Agent", "SideDroid-Updater")
        if (c.responseCode != 200) {
            val code = c.responseCode
            c.disconnect()
            throw IOException("HTTP $code")
        }
        return CloseableConnection(c)
    }

    private class CloseableConnection(c: HttpURLConnection) : AutoCloseable {
        private val connection = c
        val inputStream get() = connection.inputStream
        val contentLengthLong get() = connection.contentLengthLong
        override fun close() = connection.disconnect()
    }

    private fun java.io.InputStream.readNBytesCompat(limit: Int): ByteArray {
        val out = java.io.ByteArrayOutputStream()
        val buf = ByteArray(8 * 1024)
        while (true) {
            val n = read(buf)
            if (n < 0) return out.toByteArray()
            if (out.size() + n > limit) throw IOException("manifest too large")
            out.write(buf, 0, n)
        }
    }

    private fun sha256Of(file: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        file.inputStream().use { input ->
            val buf = ByteArray(256 * 1024)
            while (true) {
                val n = input.read(buf)
                if (n < 0) break
                digest.update(buf, 0, n)
            }
        }
        return digest.digest().joinToString("") { "%02x".format(it) }
    }

    private companion object {
        const val MANIFEST_URL = "https://raw.githubusercontent.com/Ryorin1337/SideDroid/main/update.json"
        const val RELEASE_BASE = "https://github.com/Ryorin1337/SideDroid/releases/download"
        const val UPDATES_DIR = "updates"
        const val MAX_MANIFEST_BYTES = 64 * 1024
        const val MAX_NOTES_CHARS = 2000
        val PACKAGED_ABIS = setOf("arm64-v8a", "armeabi-v7a", "x86_64")
    }
}
