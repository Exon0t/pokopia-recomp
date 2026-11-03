# CMake toolchain: Windows x64 (MSVC ABI) from Linux with clang-cl + lld-link, against the MSVC CRT and Windows SDK that
# `xwin splat` laid out in $XWIN (default /xwin, the container mount). What the Windows build gets from a vcvars64 prompt.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_LINKER lld-link)
set(CMAKE_AR llvm-lib)
set(CMAKE_RC_COMPILER llvm-rc)
set(CMAKE_MT llvm-mt)
# CMake's compiler checks build in Debug, which these projects link against the static debug CRT (libcmtd.lib): xwin lays out
# only the release CRT, and only Release builds are made here.
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

if(DEFINED ENV{XWIN})
    set(XWIN "$ENV{XWIN}")
else()
    set(XWIN "/xwin")
endif()

set(_xwin_inc "/imsvc${XWIN}/crt/include /imsvc${XWIN}/sdk/include/ucrt /imsvc${XWIN}/sdk/include/um /imsvc${XWIN}/sdk/include/shared /imsvc${XWIN}/sdk/include/winrt")
set(CMAKE_C_FLAGS_INIT "--target=x86_64-pc-windows-msvc ${_xwin_inc}")
set(CMAKE_CXX_FLAGS_INIT "--target=x86_64-pc-windows-msvc ${_xwin_inc}")
set(CMAKE_RC_FLAGS_INIT "-I${XWIN}/sdk/include/um -I${XWIN}/sdk/include/shared -I${XWIN}/sdk/include/ucrt -I${XWIN}/crt/include")

set(_xwin_lib "/libpath:${XWIN}/crt/lib/x86_64 /libpath:${XWIN}/sdk/lib/um/x86_64 /libpath:${XWIN}/sdk/lib/ucrt/x86_64")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_xwin_lib}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_xwin_lib}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_xwin_lib}")

# nothing of the Linux host may leak into the Windows build
set(CMAKE_FIND_ROOT_PATH "${XWIN}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
