# NOTE: copy of src/external/rawspeed/cmake/compiler-versions.cmake

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 10)
  message(WARNING "GNU C compiler version ${CMAKE_C_COMPILER_VERSION} is too old and is unsupported. Version 10+ is required.")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 10)
  message(WARNING "GNU C++ compiler version ${CMAKE_CXX_COMPILER_VERSION} is too old and is unsupported. Version 10+ is required.")
endif()

if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 10)
  message(WARNING "LLVM Clang C compiler version ${CMAKE_C_COMPILER_VERSION} is too old and is unsupported. Version 10+ is required.")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 10)
  message(WARNING "LLVM Clang C++ compiler version ${CMAKE_CXX_COMPILER_VERSION} is too old and is unsupported. Version 10+ is required.")
endif()

# XCode 12.0 (apple clang 12.0.0) is based on LLVM10
if(CMAKE_C_COMPILER_ID STREQUAL "AppleClang" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 12.0.0)
  message(WARNING "XCode (Apple clang) C compiler version ${CMAKE_C_COMPILER_VERSION} is too old and is unsupported. XCode version 12.0+ is required.")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12.0.0)
  message(WARNING "XCode (Apple clang) C++ compiler version ${CMAKE_CXX_COMPILER_VERSION} is too old and is unsupported. XCode version 12.0+ is required.")
endif()

if(CMAKE_OSX_DEPLOYMENT_TARGET AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 10.14)
  message(WARNING "Targeting OSX version ${CMAKE_OSX_DEPLOYMENT_TARGET} older than 10.14 is unsupported.")
endif()
