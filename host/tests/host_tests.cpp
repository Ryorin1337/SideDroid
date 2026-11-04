// Host logic that does not need a display, a GPU or a phone.
#include <cstdio>
#include <string>

#include "adb.h"
#include "display.h"
#include "input.h"
#include "ipc.h"

using namespace sd;

static int g_failures = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_failures;                                            \
        }                                                            \
    } while (0)

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

static void testTouchDevice() {
    // Creates a real synthetic touch device, without injecting anything.
    TouchInjector t;
    CHECK(t.available());
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

int main() {
    testVddIdentity();
    testChooseMode();
    testVddXml();
    testTouchMap();
    testTouchDevice();
    testAdb();
    testJson();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("host_tests: all passed\n");
    return 0;
}
