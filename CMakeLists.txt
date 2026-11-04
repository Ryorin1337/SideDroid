cmake_minimum_required(VERSION 3.22)
project(SideDroid VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(SIDEDROID_BUILD_TESTS "Build tests" ON)

if(MSVC)
    add_compile_options(/W4 /utf-8 /permissive-)
    # Static CRT, so that the host runs without a VC++ redistributable.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(SIDEDROID_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(core)
if(WIN32)
    add_subdirectory(host)
endif()
