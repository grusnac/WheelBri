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
    FfbForwardingTest.cpp
    FfbSafetyLimiterTest.cpp
    FfbUsageTest.cpp
    HidHideCliTest.cpp
    HidPidTest.cpp
    IniDocumentTest.cpp
    LoggingTest.cpp
    LoopStatsTest.cpp
    MapperTest.cpp
    PipelineTest.cpp
    RetentionTest.cpp
    StatePacketTest.cpp
    TelemetryTest.cpp
    TestClutchTest.cpp
    UdpLoopbackTest.cpp
    WheelOutputTest.cpp
    XboxPadTest.cpp
)
target_link_libraries(wheelemu_tests PRIVATE wheelemu_core GTest::gtest_main)

# The Linux virtual G920's uhid protocol (tools/uhid), tested without /dev/uhid.
if(WE_LINUX)
    target_sources(wheelemu_tests PRIVATE UhidG920Test.cpp ${PROJECT_SOURCE_DIR}/tools/uhid/UhidG920.cpp)
    target_include_directories(wheelemu_tests PRIVATE ${PROJECT_SOURCE_DIR}/tools/uhid)
endif()

include(GoogleTest)
gtest_discover_tests(wheelemu_tests)
