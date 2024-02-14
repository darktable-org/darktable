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
# As of the time of writing (2024-02-14), the next (summer) darktable release
# will happen in 2024-06 ish. By that time:
# * debian 12 (Bookworm) is the newest debian stable,
#   coming with gcc-12 and LLVM16
# * Ubuntu 24.04 LTS (Noble Numbat) will have been released,
#   coming with gcc-14 and LLVM18
# * macOS 13 (Ventura) be the oldest supported macOS version,
#   with the newest supported Xcode version being 15.2 (LLVM16-based !)
#
# Therefore, we currently require GCC12, macOS 12 + Xcode 14.2, and LLVM14.
# but should be able to require GCC12, macOS 13 + Xcode 15.2, and LLVM16.

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 12)
  message(SEND_ERROR "GNU C compiler version ${CMAKE_C_COMPILER_VERSION} is too old and is unsupported. Version 12+ is required.")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12)
  message(SEND_ERROR "GNU C++ compiler version ${CMAKE_CXX_COMPILER_VERSION} is too old and is unsupported. Version 12+ is required.")
endif()

if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 14)
  message(SEND_ERROR "LLVM Clang C compiler version ${CMAKE_C_COMPILER_VERSION} is too old and is unsupported. Version 14+ is required.")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
  message(SEND_ERROR "LLVM Clang C++ compiler version ${CMAKE_CXX_COMPILER_VERSION} is too old and is unsupported. Version 14+ is required.")
endif()

# XCode 14.2 (apple clang 1400.0.29.202) is based on LLVM14
if(CMAKE_C_COMPILER_ID STREQUAL "AppleClang" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 14.0.0.14000029)
  message(SEND_ERROR "XCode (Apple clang) C compiler version ${CMAKE_C_COMPILER_VERSION} is too old and is unsupported. XCode version 14.2+ is required.")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14.0.0.14000029)
  message(SEND_ERROR "XCode (Apple clang) C++ compiler version ${CMAKE_CXX_COMPILER_VERSION} is too old and is unsupported. XCode version 14.2+ is required.")
endif()

if(CMAKE_OSX_DEPLOYMENT_TARGET AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 12.5)
  message(SEND_ERROR "Targeting OSX version ${CMAKE_OSX_DEPLOYMENT_TARGET} older than 12.5 is unsupported.")
endif()
