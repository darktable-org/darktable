# NOTE: copy of src/external/rawspeed/cmake/compiler-versions.cmake

# We strive to keep the [darktable] software releases (but not necessarily
# development versions!) buildable with the versions of the dependencies
# provided out-of-the-box in the following distributions:
# * debian stable
# * latest(!) ubuntu LTS
# * oldest(*) maintained macOS release (assuming current cadence of
#   one major macOS release per year, and 3 (three) year shelf-life,
#   so last three releases are to be supported)
#
# Compiler-wise, that means that we support three compiler families:
# * GCC, with the required version being the newest one that is available
#   in *both* the debian stable *and* the latest ubuntu LTS
# * Xcode, with the required version being the newest one that is available
#   for the oldest supported macOS release
# * LLVM, with the required version being the oldest one between
#    * the Xcode's underlying LLVM version
#    * and the newest one that is available in *both* the
#      debian stable *and* the latest ubuntu LTS
#
# As of the time of writing (2023-03-14), the next (spring) darktable release
# will happen in 2023-06 ish. By that time:
# * debian 12 (Bookworm) will have been released,
#   coming with gcc-12 and LLVM15
# * Ubuntu 22.04.1 LTS (Jammy Jellyfish) will be the newest LTS,
#   coming with gcc-12 and LLVM15
# * macOS 11 (Big Sur) be the oldest supported macOS version,
#   with the newest supported Xcode version being 13.2 (LLVM12-based !)
#
# Therefore, we require GCC12, macOS 11 + Xcode 13.2, and LLVM12.
#
# The next+1 (fall) darktable release will happen 2023-12 ish,
# and by that time, macOS 11 will have EOL'd on 2023-10 ish,
# so after the spring release, macOS 12 will become required,
# and that will allow us to require Xcode 14.2 (LLVM14-based),
# and that will allow us to require LLVM14,

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 12)
  message(SEND_ERROR "GNU C compiler version ${CMAKE_C_COMPILER_VERSION} is too old and is unsupported. Version 12+ is required.")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12)
  message(SEND_ERROR "GNU C++ compiler version ${CMAKE_CXX_COMPILER_VERSION} is too old and is unsupported. Version 12+ is required.")
endif()

if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 12)
  message(SEND_ERROR "LLVM Clang C compiler version ${CMAKE_C_COMPILER_VERSION} is too old and is unsupported. Version 12+ is required.")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12)
  message(SEND_ERROR "LLVM Clang C++ compiler version ${CMAKE_CXX_COMPILER_VERSION} is too old and is unsupported. Version 12+ is required.")
endif()

# XCode 13.2 (apple clang 1300.0.29.3) is based on LLVM12
if(CMAKE_C_COMPILER_ID STREQUAL "AppleClang" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 13.0.0.13000029)
  message(SEND_ERROR "XCode (Apple clang) C compiler version ${CMAKE_C_COMPILER_VERSION} is too old and is unsupported. XCode version 13.2+ is required.")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13.0.0.13000029)
  message(SEND_ERROR "XCode (Apple clang) C++ compiler version ${CMAKE_CXX_COMPILER_VERSION} is too old and is unsupported. XCode version 13.2+ is required.")
endif()

if(CMAKE_OSX_DEPLOYMENT_TARGET AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 11.3)
  message(SEND_ERROR "Targeting OSX version ${CMAKE_OSX_DEPLOYMENT_TARGET} older than 11.3 is unsupported.")
endif()
