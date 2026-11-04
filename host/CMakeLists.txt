set(HOST_COMMON
    src/log.cpp
    src/display.cpp
    src/topology.cpp
    src/capture.cpp
    src/encoder_nvenc.cpp
    src/encoder_mf.cpp
    src/input.cpp
    src/store.cpp
    src/ipc.cpp
    src/adb.cpp
    src/discovery.cpp
    src/host.cpp
)

add_library(sidedroid_host_lib STATIC ${HOST_COMMON})
target_include_directories(sidedroid_host_lib PUBLIC src PRIVATE third_party/nvenc)
target_link_libraries(sidedroid_host_lib PUBLIC
    sidedroid_core d3d11 dxgi dxguid d3dcompiler mfplat mfuuid mf
    crypt32 dnsapi iphlpapi user32 gdi32 shell32 ole32 advapi32)
target_compile_definitions(sidedroid_host_lib PUBLIC
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00)

add_executable(sidedroid-host WIN32 src/main.cpp resources/host.rc)
target_link_libraries(sidedroid-host PRIVATE sidedroid_host_lib)
set_target_properties(sidedroid-host PROPERTIES WIN32_EXECUTABLE TRUE
    LINK_FLAGS "/MANIFESTUAC:level='asInvoker' /ENTRY:wWinMainCRTStartup")

# Installs or removes the virtual display driver. Run elevated, by the MSI or by hand.
add_executable(sidedroid-driver-tool src/driver_tool.cpp)
target_link_libraries(sidedroid-driver-tool PRIVATE setupapi newdev advapi32 shlwapi)
target_compile_definitions(sidedroid-driver-tool PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00)

# Stream client for testing without a phone: same core code as the Android app.
add_executable(sd-loopback tools/sd_loopback.cpp)
target_link_libraries(sd-loopback PRIVATE sidedroid_core)
target_compile_definitions(sd-loopback PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

# Checks the mDNS announcement without a phone.
add_executable(sd-mdns-probe tools/sd_mdns_probe.cpp)
target_compile_definitions(sd-mdns-probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00)

if(SIDEDROID_BUILD_TESTS)
    add_executable(host_tests tests/host_tests.cpp)
    target_link_libraries(host_tests PRIVATE sidedroid_host_lib)
    add_test(NAME host_tests COMMAND host_tests)
endif()
