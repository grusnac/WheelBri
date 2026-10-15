# The 32-bit in-game shim (dinput8.dll for 32-bit games: Need for Speed Carbon and its era) with MinGW-w64 i686.
# Only the shim (and the core it needs) is built this way: the app, the service and the drivers stay 64-bit.
#   cmake -S . -B build-x86 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw32.cmake -DCMAKE_BUILD_TYPE=Release
#         -DWE_BUILD_SERVICE=OFF -DWE_BUILD_TESTS=OFF -DWE_BUILD_LOGI=OFF
# Windows: MSYS2's MINGW32 compiler (pacman -S mingw-w64-i686-gcc), found in C:\msys64\mingw32\bin from any MSYS2
# shell (scripts/make/shim-x86.sh); Linux: the distribution's (Debian: g++-mingw-w64-i686).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)

set(MINGW_PREFIX i686-w64-mingw32)
set(WE_MINGW32_HINTS /mingw32/bin C:/msys64/mingw32/bin)
# The prefixed names first (anywhere); then MSYS2's plain ones, but only in mingw32/bin (never UCRT64's 64-bit g++).
find_program(WE_MINGW32_CXX NAMES ${MINGW_PREFIX}-g++ HINTS ${WE_MINGW32_HINTS})
find_program(WE_MINGW32_CXX NAMES g++ PATHS ${WE_MINGW32_HINTS} NO_DEFAULT_PATH REQUIRED)
find_program(WE_MINGW32_CC NAMES ${MINGW_PREFIX}-gcc HINTS ${WE_MINGW32_HINTS})
find_program(WE_MINGW32_CC NAMES gcc PATHS ${WE_MINGW32_HINTS} NO_DEFAULT_PATH REQUIRED)
find_program(WE_MINGW32_RC NAMES ${MINGW_PREFIX}-windres HINTS ${WE_MINGW32_HINTS})
find_program(WE_MINGW32_RC NAMES windres PATHS ${WE_MINGW32_HINTS} NO_DEFAULT_PATH)
set(CMAKE_C_COMPILER   ${WE_MINGW32_CC})
set(CMAKE_CXX_COMPILER ${WE_MINGW32_CXX})
if(WE_MINGW32_RC)
    set(CMAKE_RC_COMPILER ${WE_MINGW32_RC})
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# The shim is loaded into the game process: do not depend on MinGW runtime DLLs.
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-static -static-libgcc -static-libstdc++")
