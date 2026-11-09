# wheelbri_setup_logic: the install and the uninstall (SetupSteps.cpp) and their rules (SetupLogic.cpp), portable so
# the tests run them on a fake system everywhere.
add_library(wheelbri_setup_logic STATIC SetupLogic.cpp SetupSteps.cpp)
target_compile_features(wheelbri_setup_logic PUBLIC cxx_std_20)
target_include_directories(wheelbri_setup_logic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# wheelbri-setup.exe: the installer, and (as Uninstaller.exe) the uninstaller (SetupMain.cpp says how). Win32 only, no
# third-party code; linked statically. Built for the package with MSYS2 MINGW64 (msvcrt.dll, which every Windows has),
# not UCRT64, so it starts on a Windows 7, 8 or 8.1 without the Universal C Runtime (scripts/make/setup-msvcrt.sh;
# package.ps1 checks). WheelBri's icon (service/app/wheelbri.ico) and the project's version, from the .rc.in.
if(NOT (WIN32 AND WE_BUILD_SETUP))
    return()
endif()
enable_language(RC)
set(WE_ICON "${PROJECT_SOURCE_DIR}/service/app/wheelbri.ico")
configure_file(wheelbri-setup.rc.in wheelbri-setup.rc @ONLY)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/wheelbri-setup.rc" PROPERTIES OBJECT_DEPENDS "${WE_ICON}")
add_executable(wheelbri-setup WIN32 SetupMain.cpp SetupSystemWin32.cpp "${CMAKE_CURRENT_BINARY_DIR}/wheelbri-setup.rc")
target_compile_definitions(wheelbri-setup PRIVATE UNICODE _UNICODE)
target_link_libraries(wheelbri-setup PRIVATE wheelbri_setup_logic comctl32 shell32 ole32 uuid gdi32 user32 advapi32)
if(MINGW)
    target_link_options(wheelbri-setup PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()
