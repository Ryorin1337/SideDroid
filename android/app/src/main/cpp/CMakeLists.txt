cmake_minimum_required(VERSION 3.22)
project(sidedroid_android LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_subdirectory(../../../../../core core)

add_library(sidedroid SHARED bridge.cpp)
target_link_libraries(sidedroid PRIVATE sidedroid_core mediandk android log)
# 16 KB page devices need aligned segments.
target_link_options(sidedroid PRIVATE "-Wl,-z,max-page-size=16384")
