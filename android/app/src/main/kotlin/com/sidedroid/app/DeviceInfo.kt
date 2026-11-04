package com.sidedroid.app

import android.app.Activity
import android.content.Context
import android.media.MediaCodecList
import android.os.Build
import android.util.DisplayMetrics
import android.view.Display

data class DecoderCaps(val codecs: Int, val maxLong: Int, val maxShort: Int)

const val CODEC_H264 = 1
const val CODEC_HEVC = 2

object DeviceInfo {
    /** Hardware decoders only: software decoding cannot keep up with a desktop stream. */
    fun decoderCaps(): DecoderCaps {
        var codecs = 0
        var maxLong = 0
        var maxShort = 0
        for (info in MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos) {
            if (info.isEncoder || !isHardware(info)) continue
            for (type in info.supportedTypes) {
                val bit = when (type.lowercase()) {
                    "video/avc" -> CODEC_H264
                    "video/hevc" -> CODEC_HEVC
                    else -> 0
                }
                if (bit == 0) continue
                codecs = codecs or bit
                val v = info.getCapabilitiesForType(type).videoCapabilities ?: continue
                val w = v.supportedWidths.upper
                val h = v.supportedHeights.upper
                maxLong = maxOf(maxLong, maxOf(w, h))
                maxShort = maxOf(maxShort, minOf(w, h))
            }
        }
        if (codecs == 0) {
            // Nothing identified as hardware on an old device: H.264 is mandatory on Android.
            codecs = CODEC_H264
            maxLong = 1920
            maxShort = 1080
        }
        return DecoderCaps(codecs, maxLong, maxShort)
    }

    private fun isHardware(info: android.media.MediaCodecInfo): Boolean {
        if (Build.VERSION.SDK_INT >= 29) return info.isHardwareAccelerated
        val n = info.name.lowercase()
        return !(n.startsWith("omx.google.") || n.startsWith("c2.android.") || n.contains(".sw."))
    }

    @Suppress("DEPRECATION")
    fun display(activity: Activity): Display =
        if (Build.VERSION.SDK_INT >= 30) activity.display!! else activity.windowManager.defaultDisplay

    /** The full panel in the current orientation, including any cutout areas. */
    @Suppress("DEPRECATION")
    fun panelSize(activity: Activity): Pair<Int, Int> {
        val m = DisplayMetrics()
        display(activity).getRealMetrics(m)
        return m.widthPixels to m.heightPixels
    }

    fun refreshRates(activity: Activity): IntArray {
        val d = display(activity)
        val current = d.mode
        return d.supportedModes
            .filter { it.physicalWidth == current.physicalWidth && it.physicalHeight == current.physicalHeight }
            .map { Math.round(it.refreshRate) }
            .distinct()
            .sorted()
            .toIntArray()
            .let { if (it.isEmpty()) intArrayOf(60) else it }
    }

    /** Asks the panel for the mode closest to [hz] at its current resolution. */
    fun modeIdFor(activity: Activity, hz: Int): Int? {
        val d = display(activity)
        val current = d.mode
        return d.supportedModes
            .filter { it.physicalWidth == current.physicalWidth && it.physicalHeight == current.physicalHeight }
            .minByOrNull { Math.abs(it.refreshRate - hz) }
            ?.modeId
    }

    fun deviceName(context: Context): String {
        val model = Build.MODEL ?: "Android"
        return if (model.startsWith(Build.MANUFACTURER ?: "", ignoreCase = true)) model else "${Build.MANUFACTURER} $model"
    }
}
