# wheelbri-setup.exe: the graphical installer in front of install.ps1, and (as Uninstaller.exe) the uninstaller
# (SetupMain.cpp says how). Win32 only, no third-party code; linked statically, so it starts on a PC without MSYS2
# (package.ps1 checks). WheelBri's icon (service/app/wheelbri.ico) and the project's version, from the .rc.in.
enable_language(RC)
set(WE_ICON "${PROJECT_SOURCE_DIR}/service/app/wheelbri.ico")
configure_file(wheelbri-setup.rc.in wheelbri-setup.rc @ONLY)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/wheelbri-setup.rc" PROPERTIES OBJECT_DEPENDS "${WE_ICON}")
add_executable(wheelbri-setup WIN32 SetupMain.cpp "${CMAKE_CURRENT_BINARY_DIR}/wheelbri-setup.rc")
target_compile_features(wheelbri-setup PRIVATE cxx_std_20)
target_compile_definitions(wheelbri-setup PRIVATE UNICODE _UNICODE)
target_link_libraries(wheelbri-setup PRIVATE comctl32 shell32 ole32 uuid gdi32 user32 advapi32)
if(MINGW)
    target_link_options(wheelbri-setup PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()
