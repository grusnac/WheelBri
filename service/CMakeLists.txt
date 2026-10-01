add_executable(wheelemu-service
    src/DinputReader.cpp
    src/FfbSelfTest.cpp
    src/HidHide.cpp
    src/Housekeeping.cpp
    src/MappingView.cpp
    src/SdlFfbSink.cpp
    src/SdlInputSource.cpp
    src/SdlRuntime.cpp
    src/ServiceLogging.cpp
    src/VirtualG920.cpp
    src/main.cpp
)
target_link_libraries(wheelemu-service PRIVATE wheelemu_core SDL3::SDL3)
if(WIN32)
    target_link_libraries(wheelemu-service PRIVATE dinput8 dxguid)
endif()
target_compile_definitions(wheelemu-service PRIVATE WHEELEMU_VERSION="${PROJECT_VERSION}")
install(TARGETS wheelemu-service RUNTIME DESTINATION bin)
