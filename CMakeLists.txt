cmake_minimum_required(VERSION 3.24)

project(wheel_emulator
    VERSION 0.9.3
    DESCRIPTION "Presents your rig (a wheel and its accessories) to games as a wheel they support (e.g. Logitech G920)"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(WE_BUILD_TESTS   "Build unit tests"                               ON)
option(WE_BUILD_SERVICE "Build the emulator service"                      ON)
option(WE_BUILD_SHIM    "Build the dinput8.dll proxy (Windows targets)"   ${WIN32})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(WE_LINUX ON)
else()
    set(WE_LINUX OFF)
endif()
option(WE_BUILD_UHID    "Build wheelbri-uhid, the virtual G920 for Linux"  ${WE_LINUX})

if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

include(FetchContent)
FetchContent_Declare(spdlog
    URL https://github.com/gabime/spdlog/archive/refs/tags/v1.15.3.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(spdlog)

add_subdirectory(core)

if(WE_BUILD_SERVICE)
    add_subdirectory(service)
endif()

if(WE_BUILD_SHIM)
    if(WIN32)
        add_subdirectory(shim)
    else()
        message(WARNING "WE_BUILD_SHIM is only supported for Windows targets (use cmake/toolchain-mingw64.cmake).")
    endif()
endif()

if(WE_BUILD_UHID)
    if(WE_LINUX)
        add_subdirectory(tools/uhid)
    else()
        message(WARNING "WE_BUILD_UHID is only supported on Linux.")
    endif()
endif()

if(WE_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    enable_testing()
    add_subdirectory(tests)
endif()
