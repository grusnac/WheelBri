# wheelbri-service (command line) and wheelbri (the app: a window with Start / Stop), for Windows and Linux,
# with only the system's own APIs (no third-party libraries):
#   Windows: DirectInput for the wheel and its force feedback, Win32/GDI for the windows
#   Linux:   evdev for the wheel and its force feedback, Xlib for the windows (needs libx11-dev / libX11-devel)
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
    message(WARNING "wheelbri-service runs on Windows and Linux only - skipping it on ${CMAKE_SYSTEM_NAME}.")
    return()
endif()

# Everything but main(), built once for both programs.
add_library(wheelbri_service_objects OBJECT
    src/App.cpp
    src/FfbSelfTest.cpp
    src/HideFilter.cpp
    src/Housekeeping.cpp
    src/MappingView.cpp
    src/ServiceLogging.cpp
    src/VirtualG920.cpp
    src/Wheel.cpp
    src/WheelSetupView.cpp
    src/ConfigPicker.cpp
    src/ShimGamesView.cpp
    src/MappingEditView.cpp
)
target_link_libraries(wheelbri_service_objects PUBLIC wheelbri_core)
target_compile_definitions(wheelbri_service_objects PUBLIC WHEELBRI_VERSION="${PROJECT_VERSION}")

if(WE_SERVICE_PLATFORM STREQUAL "Windows")
    target_sources(wheelbri_service_objects PRIVATE
        src/DinputDevice.cpp
        src/DinputReader.cpp
        src/PlatformWin32.cpp
        src/UiWindowWin32.cpp
        src/WheelDinput.cpp)
    target_link_libraries(wheelbri_service_objects PUBLIC dinput8 dxguid cfgmgr32 advapi32 winmm user32 gdi32 shell32 ole32 uuid)
else()
    target_sources(wheelbri_service_objects PRIVATE
        src/PlatformPosix.cpp
        src/UiWindowX11.cpp
        src/WheelEvdev.cpp)
    target_link_libraries(wheelbri_service_objects PUBLIC X11::X11)
endif()

# The command line service (console).
add_executable(wheelbri-service src/main.cpp)
target_link_libraries(wheelbri-service PRIVATE wheelbri_service_objects)

# The app. On Windows a window program (no console) that asks for Administrator when started: the virtual
# G920 and hiding the real wheel need it.
add_executable(wheelbri src/main.cpp)
target_link_libraries(wheelbri PRIVATE wheelbri_service_objects)
target_compile_definitions(wheelbri PRIVATE WHEELBRI_APP=1)
if(WE_SERVICE_PLATFORM STREQUAL "Windows")
    enable_language(RC)
    target_sources(wheelbri PRIVATE app/wheelbri.rc)
    set_target_properties(wheelbri PROPERTIES WIN32_EXECUTABLE ON)
    if(MSVC)
        target_link_options(wheelbri PRIVATE /ENTRY:mainCRTStartup /MANIFEST:NO)
    endif()
endif()

# MinGW (MSYS2): the C++ runtime linked in, so both programs start from Explorer or a shortcut too, without
# MSYS2's libstdc++/libgcc/libwinpthread DLLs on the PATH.
if(WE_SERVICE_PLATFORM STREQUAL "Windows" AND NOT MSVC)
    target_link_options(wheelbri-service PRIVATE -static -static-libgcc -static-libstdc++)
    target_link_options(wheelbri PRIVATE -static -static-libgcc -static-libstdc++)
endif()

install(TARGETS wheelbri-service wheelbri RUNTIME DESTINATION bin)
