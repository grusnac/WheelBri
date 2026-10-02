add_library(wheelemu_core STATIC
    src/DeviceMatch.cpp
    src/DeviceProfile.cpp
    src/DiEffect.cpp
    src/EmulatedJoystick.cpp
    src/EmulatorConfig.cpp
    src/EvdevLayout.cpp
    src/Ffb.cpp
    src/FfbController.cpp
    src/FfbPacket.cpp
    src/FfbUsage.cpp
    src/HidG920.cpp
    src/HidHideCli.cpp
    src/HidPid.cpp
    src/IniDocument.cpp
    src/LocalDate.cpp
    src/Logging.cpp
    src/LoopStats.cpp
    src/Mapper.cpp
    src/Pipeline.cpp
    src/Retention.cpp
    src/StatePacket.cpp
    src/Telemetry.cpp
    src/TestClutch.cpp
    src/UdpLoopback.cpp
    src/WheelOutput.cpp
    src/XboxPad.cpp
)

target_include_directories(wheelemu_core PUBLIC include)
target_link_libraries(wheelemu_core PUBLIC spdlog::spdlog)

if(WIN32)
    target_link_libraries(wheelemu_core PUBLIC ws2_32)
endif()
