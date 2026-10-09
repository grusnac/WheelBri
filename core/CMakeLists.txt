add_library(wheelbri_core STATIC
    src/AsyncFfbSink.cpp
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
    src/GameUseWatch.cpp
    src/HidG920.cpp
    src/HelperDispatch.cpp
    src/HelperProtocol.cpp
    src/HelperStatus.cpp
    src/HidHideCli.cpp
    src/HidPid.cpp
    src/IniDocument.cpp
    src/IniEditor.cpp
    src/KnownGames.cpp
    src/InputReadStats.cpp
    src/LiveLog.cpp
    src/LocalDate.cpp
    src/LogFiles.cpp
    src/LogLevelCounter.cpp
    src/Logging.cpp
    src/MappingEdit.cpp
    src/Pacing.cpp
    src/Mapper.cpp
    src/PedalLayout.cpp
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
find_package(Threads REQUIRED)
target_link_libraries(wheelbri_core PUBLIC spdlog::spdlog Threads::Threads)

if(WIN32)
    target_link_libraries(wheelbri_core PUBLIC ws2_32)
endif()
