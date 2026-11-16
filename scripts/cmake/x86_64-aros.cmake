# Cross toolchain file for AROS x86_64 or Raspberry Pi. Paths come from UNARC_TC
# and UNARC_SDK, which scripts/abi.sh sets for the ABI being built.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR $ENV{UNARC_ARCH})
set(_cross $ENV{UNARC_CROSS_PREFIX})
set(_tc $ENV{UNARC_TC})
set(_sdk $ENV{UNARC_SDK})
set(CMAKE_C_COMPILER ${_tc}/${_cross}-gcc)
set(CMAKE_AR ${_tc}/${_cross}-ar)
set(CMAKE_RANLIB ${_tc}/${_cross}-ranlib)
set(CMAKE_C_FLAGS_INIT "--sysroot=${_sdk} -O2")
set(CMAKE_FIND_ROOT_PATH ${_sdk} $ENV{UNARC_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# collect-aros links executables; CMake's try_compile only needs an object that links.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,-e,0")

if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
  string(APPEND CMAKE_C_FLAGS_INIT " -fsigned-char")
endif()
