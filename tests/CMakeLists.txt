include(FetchContent)
FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(wheelemu_tests
    DeviceProfileTest.cpp
    EmulatedJoystickTest.cpp
    EmulatorConfigTest.cpp
    FfbSafetyLimiterTest.cpp
    IniDocumentTest.cpp
    LoggingTest.cpp
    LoopStatsTest.cpp
    MapperTest.cpp
    PipelineTest.cpp
    StatePacketTest.cpp
    UdpLoopbackTest.cpp
)
target_link_libraries(wheelemu_tests PRIVATE wheelemu_core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(wheelemu_tests)
