# Proxy dinput8.dll. Drop it next to the game's .exe (Windows), or next to the .exe inside the Proton prefix
# with WINEDLLOVERRIDES="dinput8=n,b" (Linux). Phase 2: presents the physical wheel as the emulated one,
# fed by wheelbri-service; every other device is passed through and logged (Phase 0 behaviour).
add_library(wheelbri_shim SHARED
    src/DiConvert.cpp
    src/DiFormat.cpp
    src/EmulatedEffect.cpp
    src/Exports.cpp
    src/FfbForwarder.cpp
    src/ProxyEffect.cpp
    src/RealDInput.cpp
    src/ShimConfig.cpp
    src/ShimLogging.cpp
    src/StateFeed.cpp
    dinput8.def
)

set_target_properties(wheelbri_shim PROPERTIES
    OUTPUT_NAME dinput8
    PREFIX "")

target_compile_definitions(wheelbri_shim PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
# dxguid provides the GUID/IID definitions as static data. Do NOT link dinput8: this DLL replaces it.
target_link_libraries(wheelbri_shim PRIVATE wheelbri_core dxguid ole32)

if(MINGW)
    # The DLL sits in the game's folder, where MinGW's runtime DLLs (libstdc++, libgcc, winpthread) are not:
    # link them in, or Windows cannot load it (error 0x7E, which EA's launcher reports as "Invalid license").
    target_link_options(wheelbri_shim PRIVATE -static -static-libgcc -static-libstdc++)
    # Strip symbols from release builds; keep them for Debug/RelWithDebInfo so crash dumps stay useful.
    target_link_options(wheelbri_shim PRIVATE $<$<CONFIG:Release,MinSizeRel>:-s>)
endif()
