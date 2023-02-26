# We want C++17 and OpenMP 4.5 support.

# As per https://en.cppreference.com/w/cpp/compiler_support
# For "C++17 core language features" this means:
#   GCC 7, Clang 6, Apple XCode 10.1; MSVC 19.14 (VS 2017 15.7)
# For "C++17 library features" this means:
#   GCC 7 (w/o C11 (GCC9), FS (GCC8)), Clang 7 (w/ C11, FS), Apple XCode 10.3; MSVC STL 19.14 (w/o std::is_aggregate)

# As per https://gcc.gnu.org/gcc-6/changes.html, GCC6 fully supports OpenMP 4.5.
# OpenMP 5.0 support is partial even in GCC11, but perhaps all the interesting bits are already available earlier?

# As per https://releases.llvm.org/6.0.0/tools/clang/docs/OpenMPSupport.html / https://releases.llvm.org/7.0.0/tools/clang/docs/OpenMPSupport.html
# Clang 7 is the first one with full OpenMP 4.5 support.

# As per https://gcc.gnu.org/releases.html, GCC 8.1 was first released on May 2, 2018 (4+ years ago)
# As per https://releases.llvm.org/, Clang 7 was first released on 19 Sep 2018 (~3 years ago)


# Baseline requirements.

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 8)
  message(FATAL_ERROR "GNU C compiler version ${CMAKE_C_COMPILER_VERSION} is too old. Need 8+")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 8)
  message(FATAL_ERROR "GNU C++ compiler version ${CMAKE_CXX_COMPILER_VERSION} is too old. Need 8+")
endif()

if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 7)
  message(FATAL_ERROR "LLVM Clang C compiler version ${CMAKE_C_COMPILER_VERSION} is too old. Need 7+")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 7)
  message(FATAL_ERROR "LLVM Clang C++ compiler version ${CMAKE_CXX_COMPILER_VERSION} is too old. Need 7+")
endif()

# XCode 10.3 (apple clang 10.0.1) is based on LLVM7
if(CMAKE_C_COMPILER_ID STREQUAL "AppleClang" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 10.0.1)
  message(FATAL_ERROR "XCode (Apple clang) C compiler version ${CMAKE_C_COMPILER_VERSION} is too old. Need version 10.0.1+ (XCode 10.3+)")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 10.0.1)
  message(FATAL_ERROR "XCode (Apple clang) C++ compiler version ${CMAKE_CXX_COMPILER_VERSION} is too old. Need version 10.0.1+ (XCode 10.3+)")
endif()

if(WITH_OPENMP)
  # OpenMP is not supported with LLVM7 / LLVM9.
  if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND
      ((CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 7 AND CMAKE_C_COMPILER_VERSION VERSION_LESS 8) OR
       (CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 9 AND CMAKE_C_COMPILER_VERSION VERSION_LESS 10)))
    message(FATAL_ERROR "LLVM Clang C compiler version ${CMAKE_C_COMPILER_VERSION} is not supported in with-OpenMP build mode.")
  endif()
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND
      ((CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 7 AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 8) OR
       (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 9 AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 10)))
    message(FATAL_ERROR "LLVM Clang C++ compiler version ${CMAKE_CXX_COMPILER_VERSION} is not supported in with-OpenMP build mode.")
  endif()

  # OpenMP is not supported with XCode 10.2-10.3 (based on LLVM7) / XCode 11.4-11.7 (based on LLVM9).
  if(CMAKE_C_COMPILER_ID STREQUAL "AppleClang" AND
      ((CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 10.0.1 AND CMAKE_C_COMPILER_VERSION VERSION_LESS 11.0.0) OR
       (CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 11.0.3 AND CMAKE_C_COMPILER_VERSION VERSION_LESS 12.0.0)))
    message(FATAL_ERROR "XCode (Apple clang) C compiler version ${CMAKE_C_COMPILER_VERSION} is not supported in with-OpenMP build mode.")
  endif()
  if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" AND
      ((CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 10.0.1 AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 11.0.0) OR
       (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 11.0.3 AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12.0.0)))
    message(FATAL_ERROR "XCode (Apple clang) C++ compiler version ${CMAKE_CXX_COMPILER_VERSION} is not supported in with-OpenMP build mode.")
  endif()
endif()

# The road forward.

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 10)
  message(WARNING "GNU C compiler version ${CMAKE_C_COMPILER_VERSION} support is soft-deprecated and will be removed in the future. Version 10+ is recommended.")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 10)
  message(WARNING "GNU C++ compiler version ${CMAKE_CXX_COMPILER_VERSION} support is soft-deprecated and will be removed in the future. Version 10+ is recommended.")
endif()

if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 10)
  message(WARNING "LLVM Clang C compiler version ${CMAKE_C_COMPILER_VERSION} support is soft-deprecated and will be removed in the future. Version 10+ is recommended.")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 10)
  message(WARNING "LLVM Clang C++ compiler version ${CMAKE_CXX_COMPILER_VERSION} support is soft-deprecated and will be removed in the future. Version 10+ is recommended.")
endif()

# XCode 12.0 (apple clang 12.0.0) is based on LLVM10
if(CMAKE_C_COMPILER_ID STREQUAL "AppleClang" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 12.0.0)
  message(WARNING "XCode (Apple clang) C compiler version ${CMAKE_C_COMPILER_VERSION} support is soft-deprecated and will be removed in the future. XCode version 12.0+ is recommended.")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12.0.0)
  message(WARNING "XCode (Apple clang) C++ compiler version ${CMAKE_CXX_COMPILER_VERSION} support is soft-deprecated and will be removed in the future. XCode version 12.0+ is recommended.")
endif()
