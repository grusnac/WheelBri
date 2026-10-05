add_library(wheelbri_core STATIC
    src/ConfigLibrary.cpp
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
    src/FfbPacket.cpp
    src/FfbUsage.cpp
    src/GameUseWatch.cpp
    src/HidG920.cpp
    src/HidHideCli.cpp
    src/HidPid.cpp
    src/IniDocument.cpp
    src/IniEditor.cpp
    src/LiveLog.cpp
    src/LocalDate.cpp
    src/Logging.cpp
    src/LoopStats.cpp
    src/MappingEdit.cpp
    src/Mapper.cpp
    src/Pipeline.cpp
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
    src/WheelOutput.cpp
)

target_include_directories(wheelbri_core PUBLIC include)
target_link_libraries(wheelbri_core PUBLIC spdlog::spdlog)

if(WIN32)
    target_link_libraries(wheelbri_core PUBLIC ws2_32)
endif()
