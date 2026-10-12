include(FetchContent)
FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(wheelbri_tests
    AsyncFfbSinkTest.cpp
    AxisAuditTest.cpp
    AxisTuningTest.cpp
    ChangeCounterTest.cpp
    ConfigLibraryTest.cpp
    DailyLogFileTest.cpp
    DeviceLabelsTest.cpp
    DeviceProfileTest.cpp
    DrawListTest.cpp
    EmulatedJoystickTest.cpp
    EmulatorConfigTest.cpp
    EventLoopControlTest.cpp
    FfbForwardingTest.cpp
    FfbIdleWatchTest.cpp
    FfbSafetyLimiterTest.cpp
    FfbShapeTest.cpp
    FfbUsageTest.cpp
    FileTextTest.cpp
    GameDetectorTest.cpp
    GameProfilesTest.cpp
    GameRotationTest.cpp
    GameSettingsTest.cpp
    GameTabTest.cpp
    GameUseWatchTest.cpp
    HelperDispatchTest.cpp
    HelperProtocolTest.cpp
    HelperStatusTest.cpp
    HidHideCliTest.cpp
    HidPidTest.cpp
    InputReadStatsTest.cpp
    IniDocumentTest.cpp
    IniEditorTest.cpp
    LiveLogTest.cpp
    LogFilesTest.cpp
    LogiSdkTest.cpp
    LogLevelCounterTest.cpp
    LogLevelTest.cpp
    LoggingTest.cpp
    MapperTest.cpp
    Nfs2015SettingsTest.cpp
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
# The shipped configs (config/), which some tests read as they are.
target_compile_definitions(wheelbri_tests PRIVATE WHEELBRI_CONFIG_DIR="${PROJECT_SOURCE_DIR}/config")

# The service's platform code with real sockets, events, folders and threads: platform::WaitSet (the event loop),
# FolderWatch and DeviceChanges (device notices), and DeviceWatch with a fake lister.
if(TARGET wheelbri_privileged)
    target_sources(wheelbri_tests PRIVATE WaitSetTest.cpp FolderWatchTest.cpp DeviceWatchTest.cpp HelperProbeTest.cpp
        AppLogLevelTest.cpp
        ${PROJECT_SOURCE_DIR}/service/src/DeviceChanges.cpp ${PROJECT_SOURCE_DIR}/service/src/DeviceWatch.cpp
        ${PROJECT_SOURCE_DIR}/service/src/HelperProbe.cpp ${PROJECT_SOURCE_DIR}/service/src/AppLogLevel.cpp)
    target_include_directories(wheelbri_tests PRIVATE ${PROJECT_SOURCE_DIR}/service/src)
    target_link_libraries(wheelbri_tests PRIVATE wheelbri_privileged)
    # Linux: the single-instance lock and child processes with real files and processes (PlatformPosix.cpp).
    if(WE_LINUX)
        target_sources(wheelbri_tests PRIVATE PlatformPosixTest.cpp)
    endif()
    # The WheelBri Helper's pipe, client and log on a pipe of the tests' own (Windows only): HelperPipeTest.
    if(WIN32)
        target_sources(wheelbri_tests PRIVATE HelperPipeTest.cpp
            ${PROJECT_SOURCE_DIR}/service/helper/HelperClient.cpp
            ${PROJECT_SOURCE_DIR}/service/helper/HelperLog.cpp
            ${PROJECT_SOURCE_DIR}/service/helper/HelperServer.cpp)
        target_include_directories(wheelbri_tests PRIVATE ${PROJECT_SOURCE_DIR}/service/helper)
        target_link_libraries(wheelbri_tests PRIVATE shell32 ole32)
    endif()
endif()

# The Linux virtual G920's uhid protocol (tools/uhid), tested without /dev/uhid.
if(WE_LINUX)
    target_sources(wheelbri_tests PRIVATE UhidG920Test.cpp UhidLoopTest.cpp ${PROJECT_SOURCE_DIR}/tools/uhid/UhidG920.cpp
        ${PROJECT_SOURCE_DIR}/tools/uhid/UhidLoop.cpp)
    target_include_directories(wheelbri_tests PRIVATE ${PROJECT_SOURCE_DIR}/tools/uhid)
endif()

include(GoogleTest)
gtest_discover_tests(wheelbri_tests)
