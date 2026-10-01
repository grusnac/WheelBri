add_executable(wheelemu-service
    src/DinputReader.cpp
    src/FfbSelfTest.cpp
    src/HidHide.cpp
    src/HideFilter.cpp
    src/Housekeeping.cpp
    src/MappingView.cpp
    src/SdlFfbSink.cpp
    src/SdlInputSource.cpp
    src/SdlRuntime.cpp
    src/ServiceLogging.cpp
    src/VirtualG920.cpp
    src/VirtualXboxPad.cpp
    src/main.cpp
)
target_link_libraries(wheelemu-service PRIVATE wheelemu_core SDL3::SDL3)
if(WIN32)
    target_link_libraries(wheelemu-service PRIVATE dinput8 dxguid cfgmgr32)
endif()

# The --ui window, with each system's own tools: Win32/GDI on Windows, Xlib on Linux; SDL elsewhere
# (macOS), or on Linux when the X11 headers are missing (libx11-dev / libX11-devel) or WE_UI_SDL=ON.
option(WE_UI_SDL "Draw the --ui window with SDL instead of the system's own tools" OFF)
set(WE_UI_BACKEND SDL)
if(NOT WE_UI_SDL)
    if(WIN32)
        set(WE_UI_BACKEND Win32)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_package(X11 QUIET)
        if(X11_FOUND)
            set(WE_UI_BACKEND X11)
        else()
            message(WARNING "X11 headers not found: the --ui window uses SDL (install libx11-dev for the native one)")
        endif()
    endif()
endif()
if(WE_UI_BACKEND STREQUAL "Win32")
    target_sources(wheelemu-service PRIVATE src/UiWindowWin32.cpp)
    target_link_libraries(wheelemu-service PRIVATE user32 gdi32 shell32)
elseif(WE_UI_BACKEND STREQUAL "X11")
    target_sources(wheelemu-service PRIVATE src/UiWindowX11.cpp)
    target_link_libraries(wheelemu-service PRIVATE X11::X11)
else()
    target_sources(wheelemu-service PRIVATE src/UiWindowSdl.cpp)
endif()
message(STATUS "wheelemu-service --ui window: ${WE_UI_BACKEND}")
target_compile_definitions(wheelemu-service PRIVATE WHEELEMU_VERSION="${PROJECT_VERSION}")
install(TARGETS wheelemu-service RUNTIME DESTINATION bin)
