add_library(sidedroid_core STATIC
    src/wire.cpp
    src/net.cpp
    src/secure.cpp
    src/fec.cpp
    src/ratecontrol.cpp
    src/delaystats.cpp
    src/client.cpp
    third_party/monocypher/monocypher.c
)
target_include_directories(sidedroid_core
    PUBLIC include
    PRIVATE third_party/monocypher)
target_compile_features(sidedroid_core PUBLIC cxx_std_20)
if(WIN32)
    target_link_libraries(sidedroid_core PUBLIC ws2_32 bcrypt)
    target_compile_definitions(sidedroid_core PUBLIC _WINSOCK_DEPRECATED_NO_WARNINGS)
endif()

if(SIDEDROID_BUILD_TESTS)
    add_executable(core_tests tests/core_tests.cpp)
    target_link_libraries(core_tests PRIVATE sidedroid_core)
    add_test(NAME core_tests COMMAND core_tests)
endif()
