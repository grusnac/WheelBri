add_library(wheelbri_core STATIC
    src/AsyncFfbSink.cpp
    src/AxisTuning.cpp
    src/AxisAudit.cpp
    src/ChangeCounter.cpp
    src/ConfigLibrary.cpp
    src/DailyLogFile.cpp
    src/DeviceMatch.cpp
    src/DeviceLabels.cpp
    src/DeviceProfile.cpp
    src/DiEffect.cpp
    src/EmulatedJoystick.cpp
    src/EmulatorConfig.cpp
    src/EvdevLayout.cpp
    src/FileText.cpp
    src/Ffb.cpp
    src/FfbController.cpp
    src/FfbIdleWatch.cpp
    src/FfbPacket.cpp
    src/FfbUsage.cpp
    src/GameDetector.cpp
    src/GameProfiles.cpp
    src/GameRotation.cpp
    src/GameSettings.cpp
    src/GameTab.cpp
    src/GameUseWatch.cpp
    src/HidG920.cpp
    src/HelperDispatch.cpp
    src/HelperProtocol.cpp
    src/HelperStatus.cpp
    src/HidHideCli.cpp
    src/HidPid.cpp
    src/IniDocument.cpp
    src/IniEditor.cpp
    src/InputReadStats.cpp
    src/LiveLog.cpp
    src/LocalDate.cpp
    src/LogFiles.cpp
    src/LogiSdk.cpp
    src/LogLevelCounter.cpp
    src/Logging.cpp
    src/MappingEdit.cpp
    src/Nfs2015Settings.cpp
    src/Pacing.cpp
    src/Mapper.cpp
    src/Pipeline.cpp
    src/RelistTimer.cpp
    src/Retention.cpp
    src/Rig.cpp
    src/SenderGate.cpp
    src/ShimGames.cpp
    src/StartNeeds.cpp
    src/StatePacket.cpp
    src/Telemetry.cpp
    src/TestClutch.cpp
    src/UdpLoopback.cpp
    src/WaitFor.cpp
    src/EventLoopControl.cpp
    src/WakePlanner.cpp
    src/WheelOutput.cpp
)

target_include_directories(wheelbri_core PUBLIC include)

# Game profiles (GameProfiles.hpp): every config/games/*.ini compiled in, so the service and the shim carry the same
# profiles with nothing to install. Written at configure time; editing, adding or removing a file reconfigures (the
# glob is CONFIGURE_DEPENDS, each file a configure dependency), and the header changes only when the files do.
file(GLOB WE_GAME_PROFILES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/config/games/*.ini")
set(WE_GAME_PROFILES_TEXT "// Generated from config/games/*.ini by core/CMakeLists.txt: {id, text} per file.\n")
foreach(profile IN LISTS WE_GAME_PROFILES)
    get_filename_component(profile_id "${profile}" NAME_WE)
    file(READ "${profile}" profile_text)
    string(FIND "${profile_text}" ")wbgame" profile_clash)
    if(NOT profile_clash EQUAL -1)
        message(FATAL_ERROR "${profile} contains ')wbgame', which ends the embedded text")
    endif()
    string(APPEND WE_GAME_PROFILES_TEXT "{\"${profile_id}\", R\"wbgame(${profile_text})wbgame\"},\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${profile}")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/GameProfileFiles.inc.tmp" "${WE_GAME_PROFILES_TEXT}")
configure_file("${CMAKE_CURRENT_BINARY_DIR}/generated/GameProfileFiles.inc.tmp"
               "${CMAKE_CURRENT_BINARY_DIR}/generated/GameProfileFiles.inc" COPYONLY)
target_include_directories(wheelbri_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
find_package(Threads REQUIRED)
target_link_libraries(wheelbri_core PUBLIC spdlog::spdlog Threads::Threads)

if(WIN32)
    target_link_libraries(wheelbri_core PUBLIC ws2_32)
endif()
