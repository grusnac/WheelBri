add_executable(wheelemu-service
    src/FfbSelfTest.cpp
    src/Housekeeping.cpp
    src/MappingView.cpp
    src/SdlFfbSink.cpp
    src/SdlInputSource.cpp
    src/SdlRuntime.cpp
    src/ServiceLogging.cpp
    src/main.cpp
)
target_link_libraries(wheelemu-service PRIVATE wheelemu_core SDL3::SDL3)
target_compile_definitions(wheelemu-service PRIVATE WHEELEMU_VERSION="${PROJECT_VERSION}")
