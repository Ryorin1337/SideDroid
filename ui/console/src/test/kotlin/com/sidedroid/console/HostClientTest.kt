package com.sidedroid.console

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class HostClientTest {
    @Test
    fun parsesFullStatus() {
        val s = parseStatus(
            """{"ok":true,"connected":true,"device":"Pixel \"9\"","transport":"wifi","error":"","encoder":"NVENC","codec":"HEVC",
            "displayW":2400,"displayH":1080,"hz":120,"encodeW":1680,"encodeH":756,"fps":60,"kbps":12000,"measuredKbps":11000,
            "rttMs":7,"decodeMs":4,"encodeMs":3.5,"measuredFps":59.8,"lossPercent":1.25,"framesDropped":2,
            "usingVirtualDisplay":true,"virtualDisplayInstalled":true,"adbFound":false,"pairingCode":"K7QM-2XPD","pairingSeconds":88,
            "settings":{"width":1920,"height":1080,"hz":90,"maxKbps":25000,"allowWireless":false},
            "usbDevices":[{"serial":"A","state":"device"},{"serial":"B","state":"unauthorized"}],
            "pairedDevices":[{"id":"ab","name":"Tab"}]}""".replace("\n", ""),
        )
        assertTrue(s.reachable && s.connected)
        assertEquals("Pixel \"9\"", s.device)
        assertEquals(1680, s.encodeW)
        assertEquals(59.8, s.measuredFps)
        assertEquals(HostSettings(1920, 1080, 90, 25000, false), s.settings)
        assertEquals(1, s.usbCount)
        assertTrue(s.usbUnauthorized)
        assertFalse(s.adbFound)
        assertEquals(listOf(PairedDevice("ab", "Tab")), s.paired)
    }

    @Test
    fun toleratesMissingFields() {
        val s = parseStatus("""{"ok":true}""")
        assertTrue(s.reachable)
        assertFalse(s.connected)
        assertEquals(HostSettings(), s.settings)
        assertTrue(s.paired.isEmpty())
    }
}
