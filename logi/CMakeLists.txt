# wheelbri-logi.dll: a stand-in for Logitech's Steering Wheel SDK helper DLL, for games built with that SDK (NFS
# Payback). Pointed at by the SDK's registry entry (HKCR\CLSID\{63BD165D-1584-4E75-AB56-08330350545F}\ServerBinary),
# it passes every call on to Logitech's real DLL and logs what the game asks and gets. x64 only (Exports.cpp).
add_library(wheelbri_logi SHARED
    src/Exports.cpp
    src/LogiLogging.cpp
    src/RealLibrary.cpp
    ../shim/src/LogFolder.cpp
    wheelbri-logi.def
)

set_target_properties(wheelbri_logi PROPERTIES
    OUTPUT_NAME wheelbri-logi
    PREFIX "")

target_include_directories(wheelbri_logi PRIVATE ../shim/src)
target_compile_definitions(wheelbri_logi PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(wheelbri_logi PRIVATE wheelbri_core)

if(MINGW)
    # Loaded into the game, where MinGW's runtime DLLs are not: link them in (as the shim does).
    target_link_options(wheelbri_logi PRIVATE -static -static-libgcc -static-libstdc++)
    target_link_options(wheelbri_logi PRIVATE $<$<CONFIG:Release,MinSizeRel>:-s>)
endif()
