include(FetchContent)
FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(wheelbri_tests
    AsyncFfbSinkTest.cpp
    AutoMapTest.cpp
    AxisAuditTest.cpp
    AxisTuningTest.cpp
    CentringSpringTest.cpp
    ChangeCounterTest.cpp
    ConfigLibraryTest.cpp
    ControlFoldTest.cpp
    DailyLogFileTest.cpp
    DeviceHoldersTest.cpp
    DeviceLabelsTest.cpp
    DeviceProfileTest.cpp
    DrawListTest.cpp
    EmulatedJoystickTest.cpp
    EmulatorConfigTest.cpp
    EventLoopControlTest.cpp
    FfbForwardingTest.cpp
    FfbGroupsTest.cpp
    FfbIdleWatchTest.cpp
    FfbSafetyLimiterTest.cpp
    FfbShapeTest.cpp
    FfbUsageTest.cpp
    FileTextTest.cpp
    GameDetectorTest.cpp
    GameProfilesTest.cpp
    GameTuningTest.cpp
    GameRotationTest.cpp
    GameSettingsTest.cpp
    GameTabTest.cpp
    GameUseWatchTest.cpp
    HelperDispatchTest.cpp
    HelperProtocolTest.cpp
    HelperStatusTest.cpp
    HideIdsTest.cpp
    HidPidTest.cpp
    HidPpTest.cpp
    HidPpFfbTest.cpp
    InputReadStatsTest.cpp
    IniDocumentTest.cpp
    IniEditorTest.cpp
    LiveLogTest.cpp
    LiveMappingTest.cpp
    LogitechFfbDriverTest.cpp
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
    DriverSenderProbeTest.cpp
    ShapeSpansTest.cpp
    ShimGamesTest.cpp
    ShimWatchTest.cpp
    StartNeedsTest.cpp
    StatePacketTest.cpp
    TelemetryTest.cpp
    TrueForceTest.cpp
    TrueForceFfbTest.cpp
    UdpLoopbackTest.cpp
    VirtualWheelTest.cpp
    WaitForTest.cpp
    WayNoticesTest.cpp
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
        AppLogLevelTest.cpp UiCanvasTest.cpp ConfigPickerTest.cpp ShimGamesViewTest.cpp HoverTextTest.cpp
        MappingAutoSaveTest.cpp GamesListWidthTest.cpp GameFileSeenTest.cpp
        GameRemoveTest.cpp
        ${PROJECT_SOURCE_DIR}/service/src/DeviceChanges.cpp ${PROJECT_SOURCE_DIR}/service/src/DeviceWatch.cpp
        ${PROJECT_SOURCE_DIR}/service/src/HelperProbe.cpp ${PROJECT_SOURCE_DIR}/service/src/AppLogLevel.cpp
        ${PROJECT_SOURCE_DIR}/service/src/ConfigPicker.cpp ${PROJECT_SOURCE_DIR}/service/src/ShimGamesView.cpp
        ${PROJECT_SOURCE_DIR}/service/src/HoverText.cpp ${PROJECT_SOURCE_DIR}/service/src/MappingEditView.cpp
        ${PROJECT_SOURCE_DIR}/service/src/GameWatch.cpp ${PROJECT_SOURCE_DIR}/service/src/ServiceLogging.cpp)
    # MappingEditView shows the version (Version.hpp): the build's own, generated (service/CMakeLists.txt).
    target_compile_definitions(wheelbri_tests PRIVATE WHEELBRI_VERSION="${PROJECT_VERSION}")
    target_include_directories(wheelbri_tests PRIVATE "${PROJECT_BINARY_DIR}/service/generated")
    add_dependencies(wheelbri_tests wheelbri_build_info)
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

# The Linux virtual wheel's uhid protocol (tools/uhid), tested without /dev/uhid.
if(WE_LINUX)
    target_sources(wheelbri_tests PRIVATE UhidWheelTest.cpp UhidLoopTest.cpp ${PROJECT_SOURCE_DIR}/tools/uhid/UhidWheel.cpp
        ${PROJECT_SOURCE_DIR}/tools/uhid/UhidLoop.cpp)
    target_include_directories(wheelbri_tests PRIVATE ${PROJECT_SOURCE_DIR}/tools/uhid)
endif()

# The real-G923 capture decoder (tools/g923-capture), on a fake wheel's captures.
if(TARGET wheelbri_g923cap)
    target_sources(wheelbri_tests PRIVATE G923CapTest.cpp)
    target_link_libraries(wheelbri_tests PRIVATE wheelbri_g923cap)
    target_compile_definitions(wheelbri_tests PRIVATE WHEELBRI_G923CAP)
endif()

include(GoogleTest)
gtest_discover_tests(wheelbri_tests)

# --- coverage: core ---
target_sources(wheelbri_tests PRIVATE EqualityTest.cpp RobustnessTest.cpp fuzz/FuzzTargets.cpp)

# The same targets under libFuzzer, to search far longer than RobustnessTest does (Clang only).
if(WE_BUILD_FUZZ)
    add_executable(wheelbri_fuzz fuzz/FuzzMain.cpp fuzz/FuzzTargets.cpp)
    target_link_libraries(wheelbri_fuzz PRIVATE wheelbri_core)
    if(TARGET wheelbri_g923cap)
        target_link_libraries(wheelbri_fuzz PRIVATE wheelbri_g923cap)
        target_compile_definitions(wheelbri_fuzz PRIVATE WHEELBRI_G923CAP)
    endif()
    target_link_options(wheelbri_fuzz PRIVATE -fsanitize=fuzzer)
endif()
# wheelbri-uhid run as a program, a plain file standing in for /dev/uhid (UhidMainTest).
if(WE_LINUX AND TARGET wheelbri-uhid)
    target_sources(wheelbri_tests PRIVATE UhidMainTest.cpp)
    add_dependencies(wheelbri_tests wheelbri-uhid)
    target_compile_definitions(wheelbri_tests PRIVATE WHEELBRI_UHID_EXE="$<TARGET_FILE:wheelbri-uhid>")
endif()

# --- coverage: edit ---
# The Mapping tab's editor, the Games page and GameWatch driven through their seams (MappingEditView::System,
# GameWatch::System): service sources already linked above.
if(TARGET wheelbri_privileged)
    target_sources(wheelbri_tests PRIVATE MappingEditViewTest.cpp GameWatchTest.cpp)
endif()

# --- coverage: svc ---
# The service's own pieces that need no device: folders and retention, finding the wheel, --ffb-test, the privileged
# steps away from Windows; on Linux also the evdev backend on nodes that are not devices and wheelbri-uhid's start.
if(TARGET wheelbri_privileged)
    target_sources(wheelbri_tests PRIVATE WheelTest.cpp FfbSelfTestTest.cpp
        ${PROJECT_SOURCE_DIR}/service/src/Wheel.cpp ${PROJECT_SOURCE_DIR}/service/src/FfbSelfTest.cpp)
    # Away from Windows the privileged steps only say they are for Windows (on Windows they go through the helper).
    if(NOT WIN32)
        target_sources(wheelbri_tests PRIVATE PrivilegedStepsTest.cpp ${PROJECT_SOURCE_DIR}/service/src/PrivilegedSteps.cpp)
    endif()
    if(WE_LINUX)
        # The Linux folders (XDG_* / HOME) and compressors found on PATH (shell scripts stand in for them).
        target_sources(wheelbri_tests PRIVATE ServiceLoggingTest.cpp HousekeepingTest.cpp
            ${PROJECT_SOURCE_DIR}/service/src/Housekeeping.cpp)
        target_sources(wheelbri_tests PRIVATE WheelEvdevTest.cpp UhidProcessTest.cpp
            ${PROJECT_SOURCE_DIR}/service/src/WheelEvdev.cpp ${PROJECT_SOURCE_DIR}/service/src/DeviceWatchSystem.cpp
            ${PROJECT_SOURCE_DIR}/service/src/UhidProcess.cpp)
        # main.cpp as a user runs it: the built wheelbri-service, started by ServiceMainTest.
        if(TARGET wheelbri-service)
            target_sources(wheelbri_tests PRIVATE ServiceMainTest.cpp)
            target_compile_definitions(wheelbri_tests PRIVATE WHEELBRI_SERVICE_EXE="$<TARGET_FILE:wheelbri-service>")
            add_dependencies(wheelbri_tests wheelbri-service)
        endif()
    endif()
endif()

# --- coverage: ui ---
# The app's window and its tabs (App.cpp, MappingView, WheelSetupView) driven through a scripted window, with game
# controllers the tests plug in: a binary of its own, because it puts fakes where the app looks for the system's
# window, game controllers and their listing (tests/ui/UiFakes.cpp: ui::openWindow, startWheelBackend,
# DeviceWatch::systemLister), so UiWindowX11.cpp / UiWindowWin32.cpp, WheelEvdev.cpp / WheelDinput.cpp and
# DeviceWatchSystem.cpp are not linked here.
if(TARGET wheelbri_privileged)
    add_executable(wheelbri_ui_tests
        ui/UiFakes.cpp
        ui/WheelSetupViewTest.cpp
        ui/MappingViewTest.cpp
        ui/AppTest.cpp
        ${PROJECT_SOURCE_DIR}/service/src/App.cpp
        ${PROJECT_SOURCE_DIR}/service/src/AppLogLevel.cpp
        ${PROJECT_SOURCE_DIR}/service/src/ConfigPicker.cpp
        ${PROJECT_SOURCE_DIR}/service/src/DeviceChanges.cpp
        ${PROJECT_SOURCE_DIR}/service/src/DeviceWatch.cpp
        ${PROJECT_SOURCE_DIR}/service/src/GameWatch.cpp
        ${PROJECT_SOURCE_DIR}/service/src/HelperProbe.cpp
        ${PROJECT_SOURCE_DIR}/service/src/HoverText.cpp
        ${PROJECT_SOURCE_DIR}/service/src/MappingEditView.cpp
        ${PROJECT_SOURCE_DIR}/service/src/MappingView.cpp
        ${PROJECT_SOURCE_DIR}/service/src/ShadowView.cpp
        ${PROJECT_SOURCE_DIR}/service/src/PrivilegedSteps.cpp
        ${PROJECT_SOURCE_DIR}/service/src/ServiceLogging.cpp
        ${PROJECT_SOURCE_DIR}/service/src/ShimGamesView.cpp
        ${PROJECT_SOURCE_DIR}/service/src/Wheel.cpp
        ${PROJECT_SOURCE_DIR}/service/src/WheelSetupView.cpp)
    if(WIN32)
        target_sources(wheelbri_ui_tests PRIVATE ${PROJECT_SOURCE_DIR}/service/helper/HelperClient.cpp)
        target_include_directories(wheelbri_ui_tests PRIVATE ${PROJECT_SOURCE_DIR}/service/helper)
        target_link_libraries(wheelbri_ui_tests PRIVATE dinput8 dxguid cfgmgr32 advapi32 iphlpapi winmm user32
            gdi32 shell32 ole32 uuid)
    endif()
    target_include_directories(wheelbri_ui_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/service/src ${PROJECT_SOURCE_DIR}/service/helper "${PROJECT_BINARY_DIR}/service/generated")
    target_compile_definitions(wheelbri_ui_tests PRIVATE WHEELBRI_VERSION="${PROJECT_VERSION}"
        WHEELBRI_CONFIG_DIR="${PROJECT_SOURCE_DIR}/config")
    add_dependencies(wheelbri_ui_tests wheelbri_build_info)
    target_link_libraries(wheelbri_ui_tests PRIVATE wheelbri_privileged GTest::gtest_main)
    gtest_discover_tests(wheelbri_ui_tests)

    # The Linux window itself (UiWindowX11.cpp) on whatever X server DISPLAY names (Xvfb in CI); skipped without one.
    if(WE_LINUX)
        find_package(X11 QUIET)
    endif()
    if(WE_LINUX AND X11_FOUND)
        add_executable(wheelbri_x11_tests ui/UiWindowX11Test.cpp ${PROJECT_SOURCE_DIR}/service/src/UiWindowX11.cpp)
        target_include_directories(wheelbri_x11_tests PRIVATE ${PROJECT_SOURCE_DIR}/service/src)
        target_link_libraries(wheelbri_x11_tests PRIVATE wheelbri_privileged X11::X11 GTest::gtest_main)
        if(WE_HAVE_XIO_EXIT_HANDLER)
            target_compile_definitions(wheelbri_x11_tests PRIVATE WE_HAVE_XIO_EXIT_HANDLER)
        endif()
        gtest_discover_tests(wheelbri_x11_tests DISCOVERY_MODE PRE_TEST)
    endif()
endif()

# --- coverage: win ---
# The dinput8.dll shim's and wheelbri-logi.dll's logic, in the Windows test binary (MinGW builds run it under Wine:
# -DCMAKE_CROSSCOMPILING_EMULATOR=wine). Built as a library of their own with the DLLs' definitions (UNICODE, ...),
# without their exports (Exports.cpp: DllMain and the entry points the game calls).
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_library(wheelbri_shim_testable STATIC
        ${PROJECT_SOURCE_DIR}/shim/src/DeviceAudit.cpp
        ${PROJECT_SOURCE_DIR}/shim/src/DiConvert.cpp
        ${PROJECT_SOURCE_DIR}/shim/src/DiFormat.cpp
        ${PROJECT_SOURCE_DIR}/shim/src/EmulatedEffect.cpp
        ${PROJECT_SOURCE_DIR}/shim/src/FfbForwarder.cpp
        ${PROJECT_SOURCE_DIR}/shim/src/LogFolder.cpp
        ${PROJECT_SOURCE_DIR}/shim/src/ModuleWatch.cpp
        ${PROJECT_SOURCE_DIR}/shim/src/ProxyEffect.cpp
        ${PROJECT_SOURCE_DIR}/shim/src/RealDInput.cpp
        ${PROJECT_SOURCE_DIR}/shim/src/ShimConfig.cpp
        ${PROJECT_SOURCE_DIR}/shim/src/ShimLogging.cpp
        ${PROJECT_SOURCE_DIR}/shim/src/StateFeed.cpp
        ${PROJECT_SOURCE_DIR}/logi/src/Settings.cpp)
    # Its definitions and folders stay its own (PRIVATE): the other tests are built without them.
    set(WE_SHIM_TEST_FOLDERS ${PROJECT_SOURCE_DIR}/shim/src ${PROJECT_SOURCE_DIR}/logi/src)
    set(WE_SHIM_TEST_DEFINITIONS WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_include_directories(wheelbri_shim_testable PRIVATE ${WE_SHIM_TEST_FOLDERS})
    target_compile_definitions(wheelbri_shim_testable PRIVATE ${WE_SHIM_TEST_DEFINITIONS})
    target_link_libraries(wheelbri_shim_testable PUBLIC wheelbri_core dxguid ole32)
    # The tests themselves, with the same definitions (they include the shim's headers). They may link dinput8: only
    # the shim must not.
    add_library(wheelbri_shim_tests OBJECT ShimFormatTest.cpp ShimConfigTest.cpp ShimEffectTest.cpp ShimDeviceTest.cpp)
    target_include_directories(wheelbri_shim_tests PRIVATE ${WE_SHIM_TEST_FOLDERS} ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(wheelbri_shim_tests PRIVATE ${WE_SHIM_TEST_DEFINITIONS})
    target_link_libraries(wheelbri_shim_tests PRIVATE wheelbri_core GTest::gtest)
    target_link_libraries(wheelbri_shim_tests INTERFACE wheelbri_shim_testable dinput8)
    target_link_libraries(wheelbri_tests PRIVATE wheelbri_shim_tests)
    # wheelbri-logi.dll itself, loaded as a game loads it, in front of a stand-in for Logitech's library.
    if(TARGET wheelbri_logi)
        add_library(wheelbri_fake_logi SHARED logi/FakeLogiLibrary.cpp)
        set_target_properties(wheelbri_fake_logi PROPERTIES OUTPUT_NAME FakeLogiLibrary PREFIX "")
        target_sources(wheelbri_tests PRIVATE LogiDllTest.cpp)
        target_compile_definitions(wheelbri_tests PRIVATE WHEELBRI_LOGI_DLL="$<TARGET_FILE:wheelbri_logi>"
            WHEELBRI_FAKE_LOGI_DLL="$<TARGET_FILE:wheelbri_fake_logi>")
        add_dependencies(wheelbri_tests wheelbri_logi wheelbri_fake_logi)
    endif()
    # The dinput8.dll shim itself, loaded as a game loads it.
    if(TARGET wheelbri_shim)
        target_sources(wheelbri_tests PRIVATE ShimDllTest.cpp)
        target_compile_definitions(wheelbri_tests PRIVATE WHEELBRI_SHIM_DLL="$<TARGET_FILE:wheelbri_shim>")
        add_dependencies(wheelbri_tests wheelbri_shim)
    endif()
    # The service's own Windows code: the platform functions and DirectInput's helpers and reader.
    if(TARGET wheelbri_privileged)
        target_sources(wheelbri_tests PRIVATE PlatformWin32Test.cpp WinDinputTest.cpp UiWindowWin32Test.cpp
            ${PROJECT_SOURCE_DIR}/service/src/DinputReader.cpp ${PROJECT_SOURCE_DIR}/service/src/UiWindowWin32.cpp
            ${PROJECT_SOURCE_DIR}/service/src/WheelDinput.cpp)
        target_link_libraries(wheelbri_tests PRIVATE dinput8 dxguid)
    endif()
endif()

# --- every test file is built ---
# A *Test.cpp that no test program compiles here is coverage lost without a sound (PrivilegedStepsTest once was).
# Files only Windows builds, and files only the other systems build, are named below; anything else left out is a
# warning, an error with -DWE_STRICT_TESTS=ON (Linux CI).
option(WE_STRICT_TESTS "Fail the configure when a tests/*Test.cpp is in no test program" OFF)
set(WE_WINDOWS_ONLY_TESTS HelperPipeTest.cpp LogiDllTest.cpp PlatformWin32Test.cpp ShimConfigTest.cpp
    ShimDeviceTest.cpp ShimDllTest.cpp ShimEffectTest.cpp ShimFormatTest.cpp UiWindowWin32Test.cpp WinDinputTest.cpp)
set(WE_NOT_WINDOWS_TESTS HousekeepingTest.cpp PlatformPosixTest.cpp PrivilegedStepsTest.cpp ServiceLoggingTest.cpp
    ServiceMainTest.cpp UhidLoopTest.cpp UhidMainTest.cpp UhidProcessTest.cpp UhidWheelTest.cpp WheelEvdevTest.cpp
    UiWindowX11Test.cpp)
if(WIN32)
    set(WE_TESTS_ELSEWHERE ${WE_NOT_WINDOWS_TESTS})
else()
    set(WE_TESTS_ELSEWHERE ${WE_WINDOWS_ONLY_TESTS})
endif()
get_property(WE_TEST_TARGETS DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
set(WE_BUILT_TESTS)
foreach(testTarget IN LISTS WE_TEST_TARGETS)
    get_target_property(testSources ${testTarget} SOURCES)
    foreach(source IN LISTS testSources)
        cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} NORMALIZE OUTPUT_VARIABLE source)
        list(APPEND WE_BUILT_TESTS ${source})
    endforeach()
endforeach()
file(GLOB_RECURSE WE_TEST_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*Test.cpp)
set(WE_UNBUILT_TESTS)
foreach(testFile IN LISTS WE_TEST_FILES)
    cmake_path(GET testFile FILENAME testName)
    if(NOT testFile IN_LIST WE_BUILT_TESTS AND NOT testName IN_LIST WE_TESTS_ELSEWHERE)
        file(RELATIVE_PATH testFile ${PROJECT_SOURCE_DIR} ${testFile})
        list(APPEND WE_UNBUILT_TESTS ${testFile})
    endif()
endforeach()
if(WE_UNBUILT_TESTS AND WE_BUILD_TESTS AND NOT WE_BUILD_FUZZ)
    if(WE_STRICT_TESTS)
        message(FATAL_ERROR "Test files no test program builds: ${WE_UNBUILT_TESTS}")
    else()
        message(WARNING "Test files no test program builds: ${WE_UNBUILT_TESTS}")
    endif()
endif()
