# wheelemu-service: Windows and Linux, with only the system's own APIs (no third-party libraries):
#   Windows: DirectInput for the wheel and its force feedback, Win32/GDI for the --ui window
#   Linux:   evdev for the wheel and its force feedback, Xlib for the --ui window (needs libx11-dev / libX11-devel)
if(WIN32)
    set(WE_SERVICE_PLATFORM Windows)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(X11 QUIET)
    if(NOT X11_FOUND)
        message(WARNING "X11 headers not found - skipping the service. Install libx11-dev (Debian/Ubuntu) or "
                        "libX11-devel (Fedora), or pass -DWE_BUILD_SERVICE=OFF.")
        return()
    endif()
    set(WE_SERVICE_PLATFORM Linux)
else()
    message(WARNING "wheelemu-service runs on Windows and Linux only - skipping it on ${CMAKE_SYSTEM_NAME}.")
    return()
endif()

add_executable(wheelemu-service
    src/FfbSelfTest.cpp
    src/HidHide.cpp
    src/HideFilter.cpp
    src/Housekeeping.cpp
    src/MappingView.cpp
    src/ServiceLogging.cpp
    src/VirtualG920.cpp
    src/VirtualXboxPad.cpp
    src/Wheel.cpp
    src/main.cpp
)
target_link_libraries(wheelemu-service PRIVATE wheelemu_core)
target_compile_definitions(wheelemu-service PRIVATE WHEELEMU_VERSION="${PROJECT_VERSION}")

if(WE_SERVICE_PLATFORM STREQUAL "Windows")
    target_sources(wheelemu-service PRIVATE
        src/DinputDevice.cpp
        src/DinputReader.cpp
        src/PlatformWin32.cpp
        src/UiWindowWin32.cpp
        src/WheelDinput.cpp)
    target_link_libraries(wheelemu-service PRIVATE dinput8 dxguid cfgmgr32 winmm user32 gdi32 shell32)
else()
    target_sources(wheelemu-service PRIVATE
        src/PlatformPosix.cpp
        src/UiWindowX11.cpp
        src/WheelEvdev.cpp)
    target_link_libraries(wheelemu-service PRIVATE X11::X11)
endif()

install(TARGETS wheelemu-service RUNTIME DESTINATION bin)
