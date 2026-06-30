# Cross-compile Liu for Windows x86_64 from macOS/Linux with MinGW-w64.
#   cmake -B build_win -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake \
#         -DCMAKE_BUILD_TYPE=Release
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER   x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER  x86_64-w64-mingw32-windres)

# LTO-aware archiver/ranlib: plain ar writes no symbol index for LTO bitcode
# members, so static libs (sqlite3) silently fail to resolve at final link.
set(CMAKE_AR     x86_64-w64-mingw32-gcc-ar)
set(CMAKE_RANLIB x86_64-w64-mingw32-gcc-ranlib)

# Search target sysroot only for libs/headers; host for programs.
# LIU_WIN_DEPS_ROOT (env or cache) points at a prefix holding cross-built
# deps (libssh2 with the WinCNG backend); defaults to /tmp/winroot.
if(NOT DEFINED LIU_WIN_DEPS_ROOT)
    set(LIU_WIN_DEPS_ROOT "$ENV{LIU_WIN_DEPS_ROOT}")
    if(NOT LIU_WIN_DEPS_ROOT)
        set(LIU_WIN_DEPS_ROOT "/tmp/winroot")
    endif()
endif()
set(CMAKE_FIND_ROOT_PATH "${LIU_WIN_DEPS_ROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Static libgcc/winpthread so the .exe runs without MinGW runtime DLLs.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libgcc -static -lwinpthread")
