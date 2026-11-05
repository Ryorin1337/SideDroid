package com.sidedroid.console

import com.sidedroid.design.DockSide
import com.sidedroid.design.UpdateStage
import com.sidedroid.design.dockFractionFor
import com.sidedroid.design.FILL_CROP
import com.sidedroid.design.FILL_FIT
import com.sidedroid.design.FILL_STRETCH
import com.sidedroid.design.PictureRect
import com.sidedroid.design.dockLowestTop
import com.sidedroid.design.pictureRect
import com.sidedroid.design.tileFraction
import com.sidedroid.design.tileIndex
import com.sidedroid.design.tileSize
import com.sidedroid.design.dockSideFor
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
            """{"ok":true,"connected":true,"maxDevices":4,"error":"port_in_use",
            "devices":[{"id":"aa","name":"Pixel \"9\"","transport":"wifi","error":"","encoder":"NVENC","codec":"HEVC",
            "displayW":2400,"displayH":1080,"hz":120,"encodeW":1680,"encodeH":756,"fps":60,"kbps":12000,"measuredKbps":11000,
            "rttMs":7,"decodeMs":4,"encodeMs":3.5,"measuredFps":59.8,"lossPercent":1.25,"framesDropped":2,"usingVirtualDisplay":true,
            "settings":{"width":1280,"height":720,"hz":60,"maxKbps":9000,"mirror":false,"scale":100},
            "options":{"phoneW":3168,"phoneH":1440,"pcW":2560,"pcH":1600,"recommendedScale":225,"currentScale":175,"rates":[60,120],"resolutions":[{"w":1920,"h":1080}]}},
            {"id":"bb","name":"Tab","settings":{"mirror":true}}],
            "virtualDisplayInstalled":true,"adbFound":false,"pairingCode":"K7QM-2XPD","pairingSeconds":88,
            "settings":{"width":1920,"height":1080,"hz":90,"maxKbps":25000,"allowWireless":false,"mirror":true,"scale":175},
            "options":{"phoneW":0,"phoneH":0,"pcW":2560,"pcH":1600,"recommendedScale":0,"currentScale":0,"rates":[],"resolutions":[]},
            "usbDevices":[{"serial":"A","state":"device"},{"serial":"B","state":"unauthorized"}],
            "pairedDevices":[{"id":"ab","name":"Tab"}]}""".replace("\n", ""),
        )
        assertTrue(s.reachable)
        assertEquals("port_in_use", s.error)
        assertEquals(listOf("aa", "bb"), s.devices.map { it.id })
        val d = s.devices[0]
        assertEquals("Pixel \"9\"", d.name)
        assertEquals(1680, d.encodeW)
        assertEquals(59.8, d.measuredFps)
        assertEquals(HostSettings(1920, 1080, 90, 25000, false, true, 175), s.settings)
        // Each device has its own settings, and wireless is the host's alone.
        assertEquals(HostSettings(1280, 720, 60, 9000, false, false, 100), d.settings)
        assertTrue(s.devices[1].settings.mirror)
        assertEquals(3168 to 1440, d.options.phone)
        assertEquals(listOf(1920 to 1080), d.options.resolutions)
        assertEquals(listOf(60, 120), d.options.rates)
        assertEquals(225, d.options.recommendedScale)
        assertEquals(1, s.usbCount)
        assertTrue(s.usbUnauthorized)
        assertFalse(s.adbFound)
        assertEquals(listOf(PairedDevice("ab", "Tab")), s.paired)
    }

    @Test
    fun dockSnapsToTheNearestSide() {
        assertEquals(DockSide.Start, dockSideFor(100f, 1000f))
        assertEquals(DockSide.End, dockSideFor(900f, 1000f))
        assertEquals(DockSide.End, dockSideFor(500f, 1000f))  // exactly in the middle: its usual side
    }

    @Test
    fun fitShowsAllOfThePictureWithBars() {
        assertEquals(PictureRect(0f, 150f, 1600f, 900f), pictureRect(FILL_FIT, 1600f, 1200f, 1920, 1080))
        assertEquals(PictureRect(200f, 0f, 1200f, 1200f), pictureRect(FILL_FIT, 1600f, 1200f, 1000, 1000))
    }

    @Test
    fun cropLeavesNoBarsAndStaysCentred() {
        val r = pictureRect(FILL_CROP, 1600f, 1200f, 1920, 1080)
        assertEquals(1200f, r.height, 0.01f)
        assertEquals(2133.3f, r.width, 0.1f)
        assertEquals((1600f - r.width) / 2, r.left, 0.01f)
    }

    @Test
    fun stretchIsTheWholeScreen() {
        assertEquals(PictureRect(0f, 0f, 1600f, 1200f), pictureRect(FILL_STRETCH, 1600f, 1200f, 1920, 1080))
    }

    @Test
    fun tilesAreActualSizeOrSmallerToFit() {
        assertEquals(640 to 360, tileSize(2000, 1200, 640, 360))
        assertEquals(1600 to 900, tileSize(1600, 1200, 1920, 1080))  // never larger than the screen
    }

    @Test
    fun aTouchInAnyTileMeansTheSamePlaceInThePicture() {
        assertEquals(1, tileIndex(150f, 100))
        assertEquals(0.5f, tileFraction(150f, 100, 1), 0.01f)
        assertEquals(0.5f, tileFraction(50f, 100, 0), 0.01f)
        // A finger crossing into the next tile stays at the edge of the one it started in.
        assertEquals(1f, tileFraction(120f, 100, 0))
        assertEquals(0f, tileFraction(90f, 100, 1))
    }

    @Test
    fun dockOnlyRestsWhereThePanelFits() {
        // Tall screen: the panel fits under anything down to here.
        assertEquals(1000f - 100f - 10f - 400f - 20f, dockLowestTop(1000f, 100f, 10f, 400f, 20f))
        // Too short for the panel at all: right at the top, and the panel scrolls.
        assertEquals(20f, dockLowestTop(400f, 100f, 10f, 400f, 20f))
    }

    @Test
    fun dockSnapsBackAlongTheSide() {
        assertEquals(0f, dockFractionFor(20f, 20f, 420f))
        assertEquals(0.5f, dockFractionFor(220f, 20f, 420f))
        assertEquals(1f, dockFractionFor(420f, 20f, 420f))
        assertEquals(1f, dockFractionFor(900f, 20f, 420f))  // let go too low: back to where it fits
        assertEquals(0f, dockFractionFor(-50f, 20f, 420f))  // and too high
        assertEquals(0f, dockFractionFor(300f, 20f, 20f))   // only one place to rest
    }

    @Test
    fun parsesGpuChoices() {
        val s = parseStatus(
            """{"vddGpu":"Intel","vddGpuEditable":false,"gpus":["NVIDIA","Intel"],"encoders":[{"id":"nvenc|NVIDIA","label":"NVENC · NVIDIA"}],
            "settings":{"encoder":"mf|Intel"},
            "devices":[{"id":"a","crossAdapter":true,"encoderFallback":true,"settings":{"encoder":"nvenc|NVIDIA"}}]}""".replace("\n", ""),
        )
        assertEquals("Intel", s.vddGpu)
        assertFalse(s.vddGpuEditable)
        assertEquals(listOf("NVIDIA", "Intel"), s.gpus)
        assertEquals(listOf(EncoderOption("nvenc|NVIDIA", "NVENC · NVIDIA")), s.encoders)
        assertEquals("mf|Intel", s.settings.encoder)
        assertEquals("nvenc|NVIDIA", s.devices[0].settings.encoder)
        assertTrue(s.devices[0].crossAdapter && s.devices[0].encoderFallback)
    }

    @Test
    fun parsesConsoleAutostart() {
        val first = parseStatus("""{"consoleAutostart":false,"consoleAutostartAsked":false}""")
        assertFalse(first.consoleAutostart)
        assertFalse(first.consoleAutostartAsked)
        assertTrue(parseStatus("""{"consoleAutostart":true,"consoleAutostartAsked":true}""").consoleAutostart)
        // A host that does not know about it must not make the console ask.
        assertTrue(parseStatus("""{"ok":true}""").consoleAutostartAsked)
    }

    @Test
    fun toleratesMissingFields() {
        val s = parseStatus("""{"ok":true}""")
        assertTrue(s.reachable)
        assertTrue(s.devices.isEmpty())
        assertEquals(HostSettings(), s.settings)
        assertTrue(s.paired.isEmpty())
    }
}
