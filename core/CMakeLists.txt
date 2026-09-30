add_library(wheelemu_core STATIC
    src/DeviceProfile.cpp
    src/DiEffect.cpp
    src/EmulatedJoystick.cpp
    src/EmulatorConfig.cpp
    src/Ffb.cpp
    src/FfbController.cpp
    src/FfbPacket.cpp
    src/IniDocument.cpp
    src/LocalDate.cpp
    src/Logging.cpp
    src/LoopStats.cpp
    src/Mapper.cpp
    src/Pipeline.cpp
    src/Retention.cpp
    src/StatePacket.cpp
    src/Telemetry.cpp
    src/UdpLoopback.cpp
)

target_include_directories(wheelemu_core PUBLIC include)
target_link_libraries(wheelemu_core PUBLIC spdlog::spdlog)

if(WIN32)
    target_link_libraries(wheelemu_core PUBLIC ws2_32)
endif()
