package com.sidedroid.app

import android.view.Surface

object NativeBridge {
    init {
        System.loadLibrary("sidedroid")
    }

    /** Callbacks arrive on native threads. */
    interface Listener {
        fun onConfig(displayW: Int, displayH: Int, encodeW: Int, encodeH: Int, fps: Int, codec: Int, kbps: Int)
        fun onDisconnected(reason: String)
        fun onPaired(secret: ByteArray)
    }

    const val MODE_USB = 0
    const val MODE_PAIRING = 1
    const val MODE_PAIRED = 2

    /** Blocks through connect and handshake. Returns null, or an error code. */
    external fun start(
        host: String, port: Int, mode: Int, clientId: ByteArray, secret: ByteArray?, pairingCode: String?,
        deviceName: String, width: Int, height: Int, dpi: Int, codecs: Int, maxW: Int, maxH: Int, rates: IntArray,
        udp: Boolean, listener: Listener,
    ): String?

    external fun stop()
    external fun setSurface(surface: Surface?)
    external fun sendTouch(data: IntArray, count: Int)
    external fun sendViewport(width: Int, height: Int)
    external fun sendSetDisplay(width: Int, height: Int, hz: Int, kbps: Int)
    external fun rttUs(): Int
}
