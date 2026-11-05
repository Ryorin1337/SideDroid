package com.sidedroid.console

import com.sidedroid.design.UpdateStage
import com.sidedroid.design.UpdateUi
import com.sidedroid.design.isNewerVersion
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class HostClientTest {
    @Test
    fun parsesUpdate() {
        val u = parseStatus("""{"update":{"state":"downloading","version":"0.0.2","notes":"a\nb","progress":42}}""").update
        assertEquals(UpdateUi("0.0.2", "a\nb", UpdateStage.Downloading, 42), u)
        assertEquals(null, parseStatus("""{"update":{"state":"none","version":"","notes":"","progress":0}}""").update)
        // A host older than the console says nothing about updates.
        assertEquals(null, parseStatus("""{"ok":true}""").update)
    }

    @Test
    fun comparesVersions() {
        assertTrue(isNewerVersion("0.0.2", "0.0.1"))
        assertTrue(isNewerVersion("0.0.10", "0.0.9"))
        assertTrue(isNewerVersion("0.1", "0.0.9"))
        assertFalse(isNewerVersion("0.0.1", "0.0.2"))
        assertFalse(isNewerVersion("0.0.2", "0.0.2"))
        assertFalse(isNewerVersion("1.0", "1.0.0"))
        assertFalse(isNewerVersion("../x", "0.0.1"))
        assertFalse(isNewerVersion("", "0.0.1"))
        assertFalse(isNewerVersion("1.2.3.4.5", "0.0.1"))
    }

    @Test
    fun parsesFullStatus() {
        val s = parseStatus(
            """{"ok":true,"connected":true,"device":"Pixel \"9\"","transport":"wifi","error":"","encoder":"NVENC","codec":"HEVC",
            "displayW":2400,"displayH":1080,"hz":120,"encodeW":1680,"encodeH":756,"fps":60,"kbps":12000,"measuredKbps":11000,
            "rttMs":7,"decodeMs":4,"encodeMs":3.5,"measuredFps":59.8,"lossPercent":1.25,"framesDropped":2,
            "usingVirtualDisplay":true,"virtualDisplayInstalled":true,"adbFound":false,"pairingCode":"K7QM-2XPD","pairingSeconds":88,
            "settings":{"width":1920,"height":1080,"hz":90,"maxKbps":25000,"allowWireless":false,"mirror":true,"scale":175},
            "options":{"phoneW":3168,"phoneH":1440,"pcW":2560,"pcH":1600,"recommendedScale":225,"currentScale":175,"rates":[60,120],"resolutions":[{"w":1920,"h":1080}]},
            "usbDevices":[{"serial":"A","state":"device"},{"serial":"B","state":"unauthorized"}],
            "pairedDevices":[{"id":"ab","name":"Tab"}]}""".replace("\n", ""),
        )
        assertTrue(s.reachable && s.connected)
        assertEquals("Pixel \"9\"", s.device)
        assertEquals(1680, s.encodeW)
        assertEquals(59.8, s.measuredFps)
        assertEquals(HostSettings(1920, 1080, 90, 25000, false, true, 175), s.settings)
        assertEquals(3168 to 1440, s.options.phone)
        assertEquals(listOf(1920 to 1080), s.options.resolutions)
        assertEquals(listOf(60, 120), s.options.rates)
        assertEquals(225, s.options.recommendedScale)
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
