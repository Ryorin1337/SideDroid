set(HOST_COMMON
    src/log.cpp
    src/display.cpp
    src/topology.cpp
    src/capture.cpp
    src/encoder_nvenc.cpp
    src/encoder_mf.cpp
    src/encoder_amf.cpp
    src/encoder_qsv.cpp
    src/encoder_soft.cpp
    src/input.cpp
    src/store.cpp
    src/ipc.cpp
    src/adb.cpp
    src/discovery.cpp
    src/host.cpp
    src/updater.cpp
    src/vdd_device.cpp
)

# Intel's dispatcher, which finds the runtime that comes with the driver. Unmodified.
set(VPL third_party/vpl/libvpl/src)
add_library(vpl_dispatcher STATIC
    ${VPL}/windows/main.cpp ${VPL}/windows/mfx_critical_section.cpp ${VPL}/windows/mfx_dispatcher.cpp
    ${VPL}/windows/mfx_dispatcher_log.cpp ${VPL}/windows/mfx_driver_store_loader.cpp ${VPL}/windows/mfx_dxva2_device.cpp
    ${VPL}/windows/mfx_function_table.cpp ${VPL}/windows/mfx_library_iterator.cpp ${VPL}/windows/mfx_load_dll.cpp
    ${VPL}/windows/mfx_win_reg_key.cpp ${VPL}/mfx_dispatcher_vpl.cpp ${VPL}/mfx_dispatcher_vpl_loader.cpp
    ${VPL}/mfx_dispatcher_vpl_config.cpp ${VPL}/mfx_dispatcher_vpl_lowlatency.cpp ${VPL}/mfx_dispatcher_vpl_log.cpp
    ${VPL}/mfx_dispatcher_vpl_msdk.cpp ${VPL}/mfx_config_interface/mfx_config_interface.cpp
    ${VPL}/mfx_config_interface/mfx_config_interface_string_api.cpp)
target_include_directories(vpl_dispatcher PUBLIC third_party/vpl/api PRIVATE ${VPL} third_party/vpl/libvpl)
target_compile_definitions(vpl_dispatcher PRIVATE MFX_DEPRECATED_OFF)
target_compile_options(vpl_dispatcher PRIVATE /W0)

add_library(sidedroid_host_lib STATIC ${HOST_COMMON})
target_include_directories(sidedroid_host_lib PUBLIC src PRIVATE third_party/nvenc third_party third_party/vpl/api)
target_link_libraries(sidedroid_host_lib PUBLIC
    sidedroid_core d3d11 dxgi dxguid d3dcompiler mfplat mfuuid mf
    crypt32 dnsapi iphlpapi user32 gdi32 shell32 ole32 advapi32 winhttp bcrypt setupapi
    vpl_dispatcher)
target_compile_definitions(sidedroid_host_lib PUBLIC
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00
    SIDEDROID_VERSION="${PROJECT_VERSION}")

add_executable(sidedroid-host WIN32 src/main.cpp resources/host.rc)
target_link_libraries(sidedroid-host PRIVATE sidedroid_host_lib)
set_target_properties(sidedroid-host PROPERTIES WIN32_EXECUTABLE TRUE
    LINK_FLAGS "/MANIFESTUAC:level='asInvoker' /ENTRY:wWinMainCRTStartup")

# Installs or removes the virtual display driver. Run elevated, by the MSI or by hand.
add_executable(sidedroid-driver-tool src/driver_tool.cpp src/vdd_device.cpp)
target_link_libraries(sidedroid-driver-tool PRIVATE setupapi newdev advapi32 shlwapi)
target_compile_definitions(sidedroid-driver-tool PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00)

# Turns the virtual display device on and off for the host. Runs as a service, as SYSTEM.
add_executable(sidedroid-service src/service.cpp src/vdd_device.cpp)
target_link_libraries(sidedroid-service PRIVATE setupapi advapi32)
target_compile_definitions(sidedroid-service PRIVATE
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
