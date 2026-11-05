// Host logic that does not need a display, a GPU or a phone.
#include <cstdio>
#include <string>

#include "adb.h"
#include "display.h"
#include "input.h"
#include "ipc.h"
#include "store.h"
#include "topology.h"
#include "updater.h"

using namespace sd;

static int g_failures = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_failures;                                            \
        }                                                            \
    } while (0)

static void testRefusesRealDisplay() {
    // Whatever the layout, a display with a real monitor on it is never "the virtual display".
    for (const auto& o : listOutputs()) {
        if (o.isVdd) CHECK(!o.primary || !o.isVddShared);
        CHECK(!(o.isVdd && o.isVddShared));
    }
}

static void testVddIdentity() {
    CHECK(isVddMonitorId(L"\\\\?\\DISPLAY#MTT1337#5&abc&0&UID256#{e6f07b5f}"));
    CHECK(isVddMonitorId(L"\\\\?\\display#mtt1337#5&abc"));
    // The other virtual displays on a real machine must never match.
    CHECK(!isVddMonitorId(L"\\\\?\\DISPLAY#VDM0001#5&abc&0&UID0#{e6f07b5f}"));
    CHECK(!isVddMonitorId(L"\\\\?\\DISPLAY#TDK0000#5&abc"));
    CHECK(!isVddMonitorId(L""));
}

static void testChooseMode() {
    Hello h;
    h.nativeWidth = 2400;
    h.nativeHeight = 1080;
    h.refreshRates = {60, 90, 120};
    h.maxDecodeWidth = 3840;
    h.maxDecodeHeight = 2160;

    auto m = chooseMode(h, 0, 0, {});
    CHECK(m.width == 2400 && m.height == 1080 && m.hz == 120);

    // Rotation follows the viewport.
    m = chooseMode(h, 1080, 2400, {});
    CHECK(m.width == 1080 && m.height == 2400);

    // Manual size keeps the current orientation, whichever way it was written.
    SetDisplay man{1920, 1080, 90, 0};
    m = chooseMode(h, 2400, 1080, man);
    CHECK(m.width == 1920 && m.height == 1080 && m.hz == 90);
    m = chooseMode(h, 1080, 2400, man);
    CHECK(m.width == 1080 && m.height == 1920);

    // An unsupported rate falls to the next one below.
    man = {0, 0, 100, 0};
    CHECK(chooseMode(h, 0, 0, man).hz == 90);
    man = {0, 0, 30, 0};
    CHECK(chooseMode(h, 0, 0, man).hz == 60);

    // The decoder limit wins, keeps aspect, and sizes stay even.
    Hello small = h;
    small.nativeWidth = 3200;
    small.nativeHeight = 1441;
    small.maxDecodeWidth = 1920;
    small.maxDecodeHeight = 1080;
    m = chooseMode(small, 0, 0, {});
    CHECK(m.width <= 1920 && m.height <= 1080 && m.width % 2 == 0 && m.height % 2 == 0);
    double aspect = static_cast<double>(m.width) / m.height;
    CHECK(aspect > 2.1 && aspect < 2.3);

    // Following the PC keeps its size, turned to the phone's orientation.
    SetDisplay pc{kResolutionMatchPc, kResolutionMatchPc, 0, 0};
    m = chooseMode(h, 2400, 1080, pc, 2560, 1600);
    CHECK(m.width == 2560 && m.height == 1600);
    m = chooseMode(h, 1080, 2400, pc, 2560, 1600);
    CHECK(m.width == 1600 && m.height == 2560);
    // With the PC's size unknown, the phone's is the safe answer.
    m = chooseMode(h, 2400, 1080, pc, 0, 0);
    CHECK(m.width == 2400 && m.height == 1080);

    // Recommended scale: usable logical size, never above the phone's own density.
    CHECK(recommendedScale(3168, 1440, 520) == 225);
    CHECK(recommendedScale(2560, 1600, 320) == 200);
    CHECK(recommendedScale(1920, 1080, 420) == 150);
    CHECK(recommendedScale(1280, 800, 213) == 100);
    CHECK(recommendedScale(800, 480, 240) == 100);
    CHECK(recommendedScale(3840, 2160, 160) == 100);
    CHECK(recommendedScale(1920, 1080, 0) == 150);

    // The list obeys the decoder's limits.
    for (const auto& s : commonResolutions(1920, 1080)) CHECK(s.width <= 1920 && s.height <= 1080);
    CHECK(!commonResolutions(1920, 1080).empty());
    CHECK(commonResolutions(0, 0).size() > commonResolutions(1920, 1080).size());

    // Device with nothing to report still gets something sane.
    Hello empty;
    m = chooseMode(empty, 0, 0, {});
    CHECK(m.width == 1920 && m.height == 1080 && m.hz == 60);
}

static const char* kXml = R"(<?xml version='1.0' encoding='utf-8'?>
<vdd_settings>
    <monitors><count>1</count></monitors>
    <global>
        <g_refresh_rate>60</g_refresh_rate>
        <g_refresh_rate>90</g_refresh_rate>
    </global>
    <resolutions>
        <resolution>
            <width>1920</width>
            <height>1080</height>
            <refresh_rate>30</refresh_rate>
        </resolution>
    </resolutions>
    <options><HardwareCursor>true</HardwareCursor></options>
</vdd_settings>)";

static int countOf(const std::string& s, const std::string& what) {
    int n = 0;
    for (size_t p = s.find(what); p != std::string::npos; p = s.find(what, p + 1)) ++n;
    return n;
}

static void testVddXml() {
    // Nothing new, nothing written.
    CHECK(!addModesToVddXml(kXml, {{1920, 1080, 60}}).has_value());

    auto x = addModesToVddXml(kXml, {{2400, 1080, 120}, {1080, 2400, 120}});
    CHECK(x.has_value());
    CHECK(countOf(*x, "<g_refresh_rate>120</g_refresh_rate>") == 1);
    CHECK(countOf(*x, "<width>2400</width>") == 1);
    CHECK(countOf(*x, "<width>1080</width>") == 1);
    CHECK(countOf(*x, "<resolution>") == 3);
    // Everything else survives untouched.
    CHECK(x->find("<HardwareCursor>true</HardwareCursor>") != std::string::npos);
    CHECK(x->find("<count>1</count>") != std::string::npos);
    // New entries land inside their sections.
    CHECK(x->find("<width>2400</width>") < x->find("</resolutions>"));
    CHECK(x->find("<g_refresh_rate>120") < x->find("</global>"));
    // Applying it again changes nothing.
    CHECK(!addModesToVddXml(*x, {{2400, 1080, 120}, {1080, 2400, 120}}).has_value());

    // A file we do not recognize is left alone.
    CHECK(!addModesToVddXml("<other/>", {{1, 2, 3}}).has_value());
}

static void testTouchMap() {
    RECT r{1920, 0, 1920 + 2400, 1080};
    auto a = mapTouchToDesktop(0, 0, r);
    CHECK(a.x == 1920 && a.y == 0);
    auto b = mapTouchToDesktop(65535, 65535, r);
    CHECK(b.x == 1920 + 2399 && b.y == 1079);
    // Displays left of the primary have negative coordinates.
    RECT l{-1080, -200, 0, 2200};
    auto c = mapTouchToDesktop(32768, 32768, l);
    CHECK(c.x > -1080 + 530 && c.x < -1080 + 550);
    auto d = mapTouchToDesktop(0, 65535, l);
    CHECK(d.x == -1080 && d.y == 2199);
}

static void testPlaceDisplay() {
    const RECT main{0, 0, 2560, 1600};
    const std::vector<RECT> others{main};
    auto at = [](POINT p, LONG x, LONG y) { return p.x == x && p.y == y; };

    // First time, with nothing known: right of everything, top aligned.
    CHECK(at(placeDisplay(nullptr, 1920, 1080, others), 2560, 0));

    // Right of the main display, wherever along its edge the user put it.
    RECT right{2560, 200, 2560 + 1920, 200 + 1080};
    CHECK(at(placeDisplay(&right, 1920, 1080, others), 2560, 200));
    CHECK(at(placeDisplay(&right, 2400, 1200, others), 2560, 200));

    // Left of it: it grows and shrinks away from the display it touches.
    RECT left{-1920, 100, 0, 100 + 1080};
    CHECK(at(placeDisplay(&left, 1920, 1080, others), -1920, 100));
    CHECK(at(placeDisplay(&left, 2400, 1200, others), -2400, 100));
    CHECK(at(placeDisplay(&left, 1280, 720, others), -1280, 100));

    // Above it, the same.
    RECT above{300, -1080, 300 + 1920, 0};
    CHECK(at(placeDisplay(&above, 2400, 1200, others), 300, -1200));

    // Below it.
    RECT below{300, 1600, 300 + 1920, 1600 + 1080};
    CHECK(at(placeDisplay(&below, 2400, 1200, others), 300, 1600));

    // Turned to landscape, a tall display high above the edge would float: it slides back.
    RECT tall{-1080, -1500, 0, -1500 + 2400};
    CHECK(at(placeDisplay(&tall, 1920, 1080, others), -1920, 0));

    // A place that no longer works, because the display it touched has gone.
    RECT stale{5000, 0, 5000 + 1920, 1080};
    CHECK(at(placeDisplay(&stale, 1920, 1080, others), 2560, 0));
    // A place now taken by another display.
    RECT taken{100, 100, 2020, 1180};
    CHECK(at(placeDisplay(&taken, 1920, 1080, others), 2560, 0));

    // Growing into a third display is not possible, so it moves to a free side.
    const std::vector<RECT> two{main, RECT{2560, 0, 2560 + 1000, 1000}};
    RECT between{2560, 1000, 2560 + 1000, 1000 + 600};
    CHECK(at(placeDisplay(&between, 1000, 600, two), 2560, 1000));
    RECT squeezed{-1000, 0, 0, 800};
    CHECK(at(placeDisplay(&squeezed, 1400, 800, two), -1400, 0));

    // Alone on the desktop, it stays put.
    CHECK(at(placeDisplay(&right, 1920, 1080, {}), 2560, 200));
}

static void testUpdateManifest() {
    CHECK(compareVersions("0.0.2", "0.0.1") > 0);
    CHECK(compareVersions("0.0.1", "0.0.2") < 0);
    CHECK(compareVersions("0.0.2", "0.0.2") == 0);
    CHECK(compareVersions("0.0.10", "0.0.9") > 0);
    CHECK(compareVersions("0.1", "0.0.9") > 0);
    CHECK(compareVersions("1.0", "1.0.0") == 0);

    CHECK(isValidVersion("0.0.2"));
    CHECK(isValidVersion("1.2.3.4"));
    CHECK(!isValidVersion(""));
    CHECK(!isValidVersion("1..2"));
    CHECK(!isValidVersion("1.2."));
    CHECK(!isValidVersion("1.2.3.4.5"));
    CHECK(!isValidVersion("../../evil"));
    CHECK(!isValidVersion("1.2/x"));

    const std::string sha(64, 'a');
    auto m = parseUpdateManifest("{\n  \"version\": \"0.0.2\",\n  \"notes\": \"修复\\n第二行\",\n  \"msi\": \"" + sha + "\"\n}\n");
    CHECK(m.has_value());
    if (m) {
        CHECK(m->version == "0.0.2");
        CHECK(m->notes == "修复\n第二行");
        CHECK(m->msiSha256 == sha);
    }
    // Upper case checksums, as Windows' own tools print them.
    m = parseUpdateManifest("{\"version\":\"0.0.2\",\"msi\":\"" + std::string(64, 'A') + "\"}");
    CHECK(m && m->msiSha256 == sha);

    CHECK(!parseUpdateManifest("").has_value());
    CHECK(!parseUpdateManifest("<html>Not Found</html>").has_value());
    CHECK(!parseUpdateManifest("{\"version\":\"0.0.2\"}").has_value());
    CHECK(!parseUpdateManifest("{\"version\":\"0.0.2\",\"msi\":\"abc\"}").has_value());
    CHECK(!parseUpdateManifest("{\"version\":\"0.0.2\",\"msi\":\"" + std::string(64, 'z') + "\"}").has_value());
    CHECK(!parseUpdateManifest("{\"version\":\"../x\",\"msi\":\"" + sha + "\"}").has_value());

    // Long notes are cut between characters, not inside one.
    std::string longNotes;
    for (int i = 0; i < 2000; ++i) longNotes += "修";
    m = parseUpdateManifest("{\"version\":\"0.0.2\",\"notes\":\"" + longNotes + "\",\"msi\":\"" + sha + "\"}");
    CHECK(m && m->notes.size() <= 4000 && m->notes.size() % 3 == 0);
}

static void testTouchDevice() {
    // Creates a real synthetic touch device, without injecting anything.
    TouchInjector t;
    CHECK(t.available());
}

// A clone group as Windows reports it: laptop panel and virtual display share
// source mode 0, each with a target mode of its own. An external display follows.
static DisplayLayout cloneLayout() {
    DisplayLayout l;
    l.modes.resize(5);
    for (UINT32 i = 0; i < 5; ++i) l.modes[i].id = 100 + i;
    DISPLAYCONFIG_PATH_INFO panel{}, vdd{}, external{};
    panel.targetInfo.id = 1;
    panel.sourceInfo.modeInfoIdx = 0;
    panel.targetInfo.modeInfoIdx = 1;
    vdd.targetInfo.id = 2;
    vdd.sourceInfo.modeInfoIdx = 0;
    vdd.targetInfo.modeInfoIdx = 2;
    external.targetInfo.id = 3;
    external.sourceInfo.modeInfoIdx = 3;
    external.targetInfo.modeInfoIdx = 4;
    l.paths = {panel, vdd, external};
    return l;
}

static void testLayoutEdit() {
    auto isVdd = [](const DISPLAYCONFIG_PATH_INFO& p) { return p.targetInfo.id == 2; };
    auto out = layoutWithout(cloneLayout(), isVdd);
    CHECK(out.paths.size() == 2);
    // The virtual display's own mode is gone and the source mode it shared stays.
    CHECK(out.modes.size() == 4);
    for (const auto& p : out.paths) {
        CHECK(p.targetInfo.id != 2);
        CHECK(p.sourceInfo.modeInfoIdx < out.modes.size());
        CHECK(p.targetInfo.modeInfoIdx < out.modes.size());
    }
    // References still lead to the same modes after renumbering.
    CHECK(out.modes[out.paths[0].sourceInfo.modeInfoIdx].id == 100);
    CHECK(out.modes[out.paths[0].targetInfo.modeInfoIdx].id == 101);
    CHECK(out.modes[out.paths[1].sourceInfo.modeInfoIdx].id == 103);
    CHECK(out.modes[out.paths[1].targetInfo.modeInfoIdx].id == 104);

    // An invalid index stays invalid.
    auto l = cloneLayout();
    l.paths[0].targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    out = layoutWithout(l, isVdd);
    CHECK(out.paths[0].targetInfo.modeInfoIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID);

    // Saved layouts come back intact, and damaged files are refused.
    auto src = cloneLayout();
    DisplayLayout back;
    auto bytes = serializeLayout(src);
    CHECK(deserializeLayout(bytes, back));
    CHECK(back.paths.size() == 3 && back.modes.size() == 5 && back.modes[4].id == 104 && back.paths[2].targetInfo.id == 3);
    CHECK(!deserializeLayout(bytes.substr(0, bytes.size() - 1), back));
    CHECK(!deserializeLayout("", back));
    CHECK(!deserializeLayout(std::string(64, 'x'), back));
}

static void testAdb() {
    auto d = parseAdbDevices(
        "* daemon not running; starting now at tcp:5037\r\n"
        "* daemon started successfully\r\n"
        "List of devices attached\r\n"
        "R58M123\tdevice\r\n"
        "emulator-5554\tunauthorized\r\n"
        "\r\n");
    CHECK(d.size() == 2);
    CHECK(d[0].serial == "R58M123" && d[0].state == "device");
    CHECK(d[1].state == "unauthorized");
    CHECK(parseAdbDevices("List of devices attached\n\n").empty());
}

static void testJson() {
    std::string j = R"({"cmd":"set","width":2400,"name":"a\"b","allowWireless": false})";
    CHECK(jsonGetString(j, "cmd") == "set");
    CHECK(jsonGetString(j, "name") == "a\"b");
    CHECK(jsonGetNumber(j, "width", 0) == 2400);
    CHECK(jsonGetNumber(j, "missing", 7) == 7);
    CHECK(jsonGetBool(j, "allowWireless", true) == false);
    CHECK(jsonEscape("a\"b\\c\n") == "a\\\"b\\\\c\\n");
    CHECK(jsonEscape(std::string("\x01")) == "\\u0001");
}

static void testVddGpu() {
    const std::string xml = "<x><gpu>\n<friendlyname>default</friendlyname>\n</gpu><other><friendlyname>keep</friendlyname></other></x>";
    auto intel = setVddGpuInXml(xml, "Intel(R) UHD Graphics");
    CHECK(intel.has_value());
    CHECK(intel->find("<friendlyname>Intel(R) UHD Graphics</friendlyname>") != std::string::npos);
    CHECK(intel->find("<friendlyname>keep</friendlyname>") != std::string::npos);  // nothing else changes
    CHECK(!setVddGpuInXml(*intel, "Intel(R) UHD Graphics").has_value());           // no change, so no restart
    CHECK(setVddGpuInXml(*intel, "default").value() == xml);
    CHECK(setVddGpuInXml(xml, "A&B <C>").value().find("A&amp;B &lt;C&gt;") != std::string::npos);
    CHECK(!setVddGpuInXml("<x></x>", "a").has_value());
    CHECK(!setVddGpuInXml("<gpu></gpu><friendlyname>a</friendlyname>", "b").has_value());
}

static void testVddModeLimit() {
    const auto sizesOf = [](const std::string& x) {
        size_t n = 0;
        for (size_t p = x.find("<resolution>"); p != std::string::npos; p = x.find("<resolution>", p + 1)) ++n;
        return n;
    };
    std::string xml = "<x><global><g_refresh_rate>60</g_refresh_rate></global><resolutions>\n";
    for (int i = 0; i < 14; ++i)
        xml += "<resolution><width>" + std::to_string(1000 + i) + "</width><height>500</height><refresh_rate>60</refresh_rate></resolution>\n";
    xml += "</resolutions></x>";
    const DisplayMode oldest{1000, 500, 60}, second{1001, 500, 60}, fresh{2000, 1000, 60};

    // Room for no more: the oldest goes, and the one a device is using stays.
    auto out = addModesToVddXml(xml, {fresh}, {oldest});
    CHECK(out.has_value());
    CHECK(sizesOf(*out) == 14);
    CHECK(out->find("<width>2000</width>") != std::string::npos);
    CHECK(out->find("<width>1000</width>") != std::string::npos);
    CHECK(out->find("<width>1001</width>") == std::string::npos);

    // A file over the limit is repaired even when nothing is added: the driver shows no monitors.
    std::string over = xml;
    over.insert(over.find("</resolutions>"), "<resolution><width>3000</width><height>500</height><refresh_rate>60</refresh_rate></resolution>\n");
    auto repaired = addModesToVddXml(over, {second});
    CHECK(repaired.has_value() && sizesOf(*repaired) == 14);
    CHECK(repaired->find("<width>1001</width>") != std::string::npos);

    // More rates leave room for fewer sizes.
    std::string rates = xml;
    rates.insert(rates.find("</global>"), "<g_refresh_rate>90</g_refresh_rate><g_refresh_rate>120</g_refresh_rate><g_refresh_rate>144</g_refresh_rate><g_refresh_rate>165</g_refresh_rate><g_refresh_rate>244</g_refresh_rate><g_refresh_rate>30</g_refresh_rate>");
    auto limited = addModesToVddXml(rates, {second});
    CHECK(limited.has_value() && sizesOf(*limited) == 12);

    // Within the limit, nothing to do.
    CHECK(!addModesToVddXml(xml, {oldest}).has_value());
}

static void testVddMonitorCount() {
    const std::string xml = "<vdd_settings>\n<monitors>\n<count>1</count>\n</monitors>\n<other><count>9</count></other></vdd_settings>";
    auto four = setVddMonitorCountInXml(xml, 4);
    CHECK(four.has_value());
    CHECK(four->find("<monitors>\n<count>4</count>") != std::string::npos);
    CHECK(four->find("<other><count>9</count>") != std::string::npos);  // nothing else changes
    CHECK(!setVddMonitorCountInXml(*four, 4).has_value());              // no change, so no reload
    CHECK(!setVddMonitorCountInXml("<vdd_settings></vdd_settings>", 4).has_value());
    CHECK(!setVddMonitorCountInXml("<monitors></monitors><count>1</count>", 4).has_value());
}

static void testDeviceSettings() {
    DeviceSettings d;
    d.width = 1920; d.height = 1080; d.hz = 120; d.maxKbps = 25000; d.mirror = true; d.scalePercent = 175; d.custom = true;
    d.left = -1920; d.top = -40; d.right = 0; d.bottom = 1040;
    DeviceSettings r;
    CHECK(parseDeviceSettings(serializeDeviceSettings(d), r));
    CHECK(r.width == 1920 && r.height == 1080 && r.hz == 120 && r.maxKbps == 25000 && r.mirror && r.scalePercent == 175 && r.custom);
    CHECK(r.left == -1920 && r.top == -40 && r.right == 0 && r.bottom == 1040);
    CHECK(r.encoder.empty());
    d.encoder = "mf|Vendor, Inc. GPU";
    CHECK(parseDeviceSettings(serializeDeviceSettings(d), r));
    CHECK(r.encoder == "mf|Vendor, Inc. GPU" && r.bottom == 1040);  // a comma in a name is fine
    CHECK(r.fill == 0);
    d.fill = 3;
    CHECK(parseDeviceSettings(serializeDeviceSettings(d), r));
    CHECK(r.fill == 3 && r.encoder == "mf|Vendor, Inc. GPU");
    CHECK(parseDeviceSettings("1,2,3,4,0,6,1,7,8,9,10,mf|Old GPU", r));  // an earlier version, with an encoder
    CHECK(r.fill == 0 && r.encoder == "mf|Old GPU");
    CHECK(parseDeviceSettings("1,2,3,4,0,6,1,7,8,9,10", r));        // as written by an earlier version
    CHECK(r.encoder.empty() && r.bottom == 10);
    CHECK(!parseDeviceSettings("1,2,3", r));
    CHECK(!parseDeviceSettings("", r));

    Settings s;
    s.width = 1280; s.mirror = true; s.allowWireless = false;
    DeviceSettings kept = d;
    kept.mirror = false;
    copyDisplayDefaults(s, kept);
    CHECK(kept.width == 1280 && kept.mirror && kept.height == 0);
    CHECK(kept.left == -1920 && kept.custom);  // its place and flag are not defaults
}

int main() {
    testVddIdentity();
    testRefusesRealDisplay();
    testChooseMode();
    testVddXml();
    testTouchMap();
    testPlaceDisplay();
    testDeviceSettings();
    testVddMonitorCount();
    testVddModeLimit();
    testVddGpu();
    testUpdateManifest();
    testTouchDevice();
    testLayoutEdit();
    testAdb();
    testJson();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("host_tests: all passed\n");
    return 0;
}
