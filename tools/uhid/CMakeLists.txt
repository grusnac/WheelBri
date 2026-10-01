# wheelemu-uhid: the virtual G920 on Linux (Phase 4), see main.cpp.
add_executable(wheelemu-uhid main.cpp UhidG920.cpp)
target_link_libraries(wheelemu-uhid PRIVATE wheelemu_core)
target_compile_definitions(wheelemu-uhid PRIVATE WHEELEMU_VERSION="${PROJECT_VERSION}")
install(TARGETS wheelemu-uhid RUNTIME DESTINATION bin)
