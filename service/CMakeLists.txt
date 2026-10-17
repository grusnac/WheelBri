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

# The steps that need Administrator (src/Privileged.hpp) and what they use, for the app and the WheelBri Helper.
add_library(wheelbri_privileged STATIC
    src/HideFilter.cpp
    src/Privileged.cpp
    src/VirtualG920.cpp)
target_link_libraries(wheelbri_privileged PUBLIC wheelbri_core)
if(WE_SERVICE_PLATFORM STREQUAL "Windows")
    target_sources(wheelbri_privileged PRIVATE src/DinputDevice.cpp src/PlatformWin32.cpp)
    target_link_libraries(wheelbri_privileged PUBLIC
        dinput8 dxguid cfgmgr32 advapi32 iphlpapi winmm shell32 ole32 comdlg32)
else()
    target_sources(wheelbri_privileged PRIVATE src/PlatformPosix.cpp)
endif()

# Everything but main(), built once for both programs.
add_library(wheelbri_service_objects OBJECT
    src/App.cpp
    src/AppLogLevel.cpp
    src/DeviceChanges.cpp
    src/DeviceWatch.cpp
    src/DeviceWatchSystem.cpp
    src/FfbSelfTest.cpp
    src/GameWatch.cpp
    src/HelperProbe.cpp
    src/Housekeeping.cpp
    src/MappingView.cpp
    src/PrivilegedSteps.cpp
    src/ServiceLogging.cpp
    src/Wheel.cpp
    src/WheelSetupView.cpp
    src/ConfigPicker.cpp
    src/ShimGamesView.cpp
    src/MappingEditView.cpp
)
target_link_libraries(wheelbri_service_objects PUBLIC wheelbri_privileged)
target_include_directories(wheelbri_service_objects PUBLIC src helper)
target_compile_definitions(wheelbri_service_objects PUBLIC WHEELBRI_VERSION="${PROJECT_VERSION}")
# The git commit of the build, for the version in the app's top-right corner (BuildInfo.hpp, WHEELBRI_BUILD): looked
# up on every build, the header rewritten only when it changes.
set(WE_BUILD_INFO "${CMAKE_CURRENT_BINARY_DIR}/generated/BuildInfo.hpp")
add_custom_target(wheelbri_build_info
    COMMAND "${CMAKE_COMMAND}" -DSRC=${PROJECT_SOURCE_DIR} -DOUT=${WE_BUILD_INFO}
            -P "${PROJECT_SOURCE_DIR}/cmake/BuildInfo.cmake"
    BYPRODUCTS "${WE_BUILD_INFO}"
    COMMENT "build info (git commit)"
    VERBATIM)
add_dependencies(wheelbri_service_objects wheelbri_build_info)
target_include_directories(wheelbri_service_objects PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/generated")

if(WE_SERVICE_PLATFORM STREQUAL "Windows")
    target_sources(wheelbri_service_objects PRIVATE
        helper/HelperClient.cpp
        src/DinputReader.cpp
        src/UiWindowWin32.cpp
        src/WheelDinput.cpp)
    target_link_libraries(wheelbri_service_objects PUBLIC dinput8 dxguid cfgmgr32 advapi32 iphlpapi winmm user32 gdi32 shell32 ole32 uuid)
else()
    target_sources(wheelbri_service_objects PRIVATE
        src/UhidProcess.cpp
        src/UiWindowX11.cpp
        src/WheelEvdev.cpp)
    target_link_libraries(wheelbri_service_objects PUBLIC X11::X11)
    # libX11 1.7+: lets the window survive the X server going away (UiWindowX11.cpp), so the session cleans up.
    include(CheckSymbolExists)
    set(CMAKE_REQUIRED_INCLUDES ${X11_INCLUDE_DIR})
    set(CMAKE_REQUIRED_LIBRARIES ${X11_LIBRARIES})
    check_symbol_exists(XSetIOErrorExitHandler "X11/Xlib.h" WE_HAVE_XIO_EXIT_HANDLER)
    unset(CMAKE_REQUIRED_INCLUDES)
    unset(CMAKE_REQUIRED_LIBRARIES)
    if(WE_HAVE_XIO_EXIT_HANDLER)
        target_compile_definitions(wheelbri_service_objects PRIVATE WE_HAVE_XIO_EXIT_HANDLER)
    endif()
endif()

# The command line service (console).
add_executable(wheelbri-service src/main.cpp)
target_link_libraries(wheelbri-service PRIVATE wheelbri_service_objects)

# The app. On Windows a window program (no console) that runs as the user who starts it: the virtual G920 and hiding
# the rig go through the WheelBri Helper then (or are done directly when it is started as Administrator).
add_executable(wheelbri src/main.cpp)
target_link_libraries(wheelbri PRIVATE wheelbri_service_objects)
target_compile_definitions(wheelbri PRIVATE WHEELBRI_APP=1)
if(WE_SERVICE_PLATFORM STREQUAL "Windows")
    # Both programs carry WheelBri's icon (app/wheelbri.ico, made from app/wheelbri.svg) and the project's version;
    # the app also its manifest. The .rc and manifest are made from the .in files with the version filled in.
    enable_language(RC)
    set(WE_APP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/app")
    set(WE_APP_BIN_DIR "${CMAKE_CURRENT_BINARY_DIR}/app")
    configure_file(app/wheelbri.exe.manifest.in app/wheelbri.exe.manifest @ONLY)
    configure_file(app/wheelbri.rc.in app/wheelbri.rc @ONLY)
    configure_file(app/wheelbri-service.rc.in app/wheelbri-service.rc @ONLY)
    set_source_files_properties("${WE_APP_BIN_DIR}/wheelbri.rc" PROPERTIES
        OBJECT_DEPENDS "${WE_APP_DIR}/wheelbri.ico;${WE_APP_BIN_DIR}/wheelbri.exe.manifest")
    set_source_files_properties("${WE_APP_BIN_DIR}/wheelbri-service.rc" PROPERTIES
        OBJECT_DEPENDS "${WE_APP_DIR}/wheelbri.ico")
    target_sources(wheelbri PRIVATE "${WE_APP_BIN_DIR}/wheelbri.rc")
    target_sources(wheelbri-service PRIVATE "${WE_APP_BIN_DIR}/wheelbri-service.rc")
    set_target_properties(wheelbri PROPERTIES WIN32_EXECUTABLE ON)
    if(MSVC)
        target_link_options(wheelbri PRIVATE /ENTRY:mainCRTStartup /MANIFEST:NO)
    endif()
endif()

# The WheelBri Helper (Windows only): a service running as SYSTEM that does the steps needing Administrator for the
# app. Only core, wheelbri_privileged and Windows' libraries, not the app's objects.
if(WE_SERVICE_PLATFORM STREQUAL "Windows")
    configure_file(app/wheelbri-helper.rc.in app/wheelbri-helper.rc @ONLY)
    set_source_files_properties("${WE_APP_BIN_DIR}/wheelbri-helper.rc" PROPERTIES
        OBJECT_DEPENDS "${WE_APP_DIR}/wheelbri.ico")
    add_executable(wheelbri-helper
        helper/HelperMain.cpp
        helper/HelperClient.cpp
        helper/HelperLog.cpp
        helper/HelperServer.cpp
        helper/HelperSession.cpp
        "${WE_APP_BIN_DIR}/wheelbri-helper.rc")
    target_include_directories(wheelbri-helper PRIVATE src)
    target_link_libraries(wheelbri-helper PRIVATE wheelbri_privileged shell32 ole32)
    target_compile_definitions(wheelbri-helper PRIVATE WHEELBRI_VERSION="${PROJECT_VERSION}")
    if(NOT MSVC)
        target_link_options(wheelbri-helper PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    install(TARGETS wheelbri-helper RUNTIME DESTINATION bin)
endif()

# MinGW (MSYS2): the C++ runtime linked in, so both programs start from Explorer or a shortcut too, without
# MSYS2's libstdc++/libgcc/libwinpthread DLLs on the PATH.
if(WE_SERVICE_PLATFORM STREQUAL "Windows" AND NOT MSVC)
    target_link_options(wheelbri-service PRIVATE -static -static-libgcc -static-libstdc++)
    target_link_options(wheelbri PRIVATE -static -static-libgcc -static-libstdc++)
endif()

install(TARGETS wheelbri-service wheelbri RUNTIME DESTINATION bin)
