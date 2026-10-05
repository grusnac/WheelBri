include(FetchContent)
FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(wheelbri_tests
    ConfigLibraryTest.cpp
    DeviceLabelsTest.cpp
    DeviceProfileTest.cpp
    EmulatedJoystickTest.cpp
    EmulatorConfigTest.cpp
    FfbForwardingTest.cpp
    FfbSafetyLimiterTest.cpp
    FfbShapeTest.cpp
    FfbUsageTest.cpp
    FileTextTest.cpp
    GameUseWatchTest.cpp
    HidHideCliTest.cpp
    HidPidTest.cpp
    IniDocumentTest.cpp
    IniEditorTest.cpp
    LiveLogTest.cpp
    LoggingTest.cpp
    LoopStatsTest.cpp
    MapperTest.cpp
    MappingEditTest.cpp
    PipelineTest.cpp
    RetentionTest.cpp
    RigTest.cpp
    SenderGateTest.cpp
    ShimGamesTest.cpp
    StatePacketTest.cpp
    TelemetryTest.cpp
    TestClutchTest.cpp
    UdpLoopbackTest.cpp
    WaitForTest.cpp
    WheelOutputTest.cpp
)
target_link_libraries(wheelbri_tests PRIVATE wheelbri_core GTest::gtest_main)

# The Linux virtual G920's uhid protocol (tools/uhid), tested without /dev/uhid.
if(WE_LINUX)
    target_sources(wheelbri_tests PRIVATE UhidG920Test.cpp ${PROJECT_SOURCE_DIR}/tools/uhid/UhidG920.cpp)
    target_include_directories(wheelbri_tests PRIVATE ${PROJECT_SOURCE_DIR}/tools/uhid)
endif()

include(GoogleTest)
gtest_discover_tests(wheelbri_tests)
