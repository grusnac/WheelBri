# Cross-compile the Windows parts (shim, core) from Linux or macOS with MinGW-w64.
#   cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MINGW_PREFIX x86_64-w64-mingw32)
set(CMAKE_C_COMPILER   ${MINGW_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${MINGW_PREFIX}-g++)
set(CMAKE_RC_COMPILER  ${MINGW_PREFIX}-windres)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# The shim is loaded into the game process: do not depend on MinGW runtime DLLs.
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-static -static-libgcc -static-libstdc++")
