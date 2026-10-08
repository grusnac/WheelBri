include(FetchContent)
FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(wheelbri_tests
    AsyncFfbSinkTest.cpp
    ChangeCounterTest.cpp
    ConfigLibraryTest.cpp
    DeviceLabelsTest.cpp
    DeviceProfileTest.cpp
    DrawListTest.cpp
    EmulatedJoystickTest.cpp
    EmulatorConfigTest.cpp
    EventLoopControlTest.cpp
    FfbForwardingTest.cpp
    FfbSafetyLimiterTest.cpp
    FfbShapeTest.cpp
    FfbUsageTest.cpp
    FileTextTest.cpp
    GameUseWatchTest.cpp
    HelperProtocolTest.cpp
    HidHideCliTest.cpp
    HidPidTest.cpp
    InputReadStatsTest.cpp
    IniDocumentTest.cpp
    IniEditorTest.cpp
    LiveLogTest.cpp
    LoggingTest.cpp
    MapperTest.cpp
    MappingEditTest.cpp
    PacingTest.cpp
    PipelineTest.cpp
    PushLoopTest.cpp
    RelistTimerTest.cpp
    RetentionTest.cpp
    RigTest.cpp
    SenderGateTest.cpp
    ShimGamesTest.cpp
    StartNeedsTest.cpp
    StatePacketTest.cpp
    TelemetryTest.cpp
    TestClutchTest.cpp
    UdpLoopbackTest.cpp
    WaitForTest.cpp
    WakePlannerTest.cpp
    WheelOutputTest.cpp
    emulators/PushEmulators.cpp
)
target_link_libraries(wheelbri_tests PRIVATE wheelbri_core GTest::gtest_main)
# Emulated devices, sockets and waits for the events loop (PushLoopTest).
target_include_directories(wheelbri_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# The service's platform code with real sockets, events, folders and threads: platform::WaitSet (the event loop),
# FolderWatch and DeviceChanges (device notices), and DeviceWatch with a fake lister.
if(TARGET wheelbri_privileged)
    target_sources(wheelbri_tests PRIVATE WaitSetTest.cpp FolderWatchTest.cpp DeviceWatchTest.cpp
        ${PROJECT_SOURCE_DIR}/service/src/DeviceChanges.cpp ${PROJECT_SOURCE_DIR}/service/src/DeviceWatch.cpp)
    target_include_directories(wheelbri_tests PRIVATE ${PROJECT_SOURCE_DIR}/service/src)
    target_link_libraries(wheelbri_tests PRIVATE wheelbri_privileged)
endif()

# The Linux virtual G920's uhid protocol (tools/uhid), tested without /dev/uhid.
if(WE_LINUX)
    target_sources(wheelbri_tests PRIVATE UhidG920Test.cpp ${PROJECT_SOURCE_DIR}/tools/uhid/UhidG920.cpp)
    target_include_directories(wheelbri_tests PRIVATE ${PROJECT_SOURCE_DIR}/tools/uhid)
endif()

include(GoogleTest)
gtest_discover_tests(wheelbri_tests)
