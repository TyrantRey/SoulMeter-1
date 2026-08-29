# Cross-compile the Windows x64 binaries from Linux/macOS with llvm-mingw
# (https://github.com/mstorsjo/llvm-mingw). Clang is required rather than
# GCC-mingw: the hook DLL uses __try/__except, which only clang implements
# for mingw targets.
#
# Put <llvm-mingw>/bin on PATH, or set LLVM_MINGW to the llvm-mingw root.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(_triple x86_64-w64-mingw32)
set(_hints "$ENV{LLVM_MINGW}/bin" "${LLVM_MINGW}/bin")

find_program(CMAKE_C_COMPILER   NAMES ${_triple}-clang   HINTS ${_hints} REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES ${_triple}-clang++ HINTS ${_hints} REQUIRED)
find_program(CMAKE_RC_COMPILER  NAMES ${_triple}-windres HINTS ${_hints} REQUIRED)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
