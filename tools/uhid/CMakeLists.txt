# wheelbri-uhid: the virtual G920 on Linux (Phase 4), see main.cpp.
add_executable(wheelbri-uhid main.cpp UhidWheel.cpp UhidLoop.cpp)
target_link_libraries(wheelbri-uhid PRIVATE wheelbri_core)
target_compile_definitions(wheelbri-uhid PRIVATE WHEELBRI_VERSION="${PROJECT_VERSION}")
install(TARGETS wheelbri-uhid RUNTIME DESTINATION bin)
