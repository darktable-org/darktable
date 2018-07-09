# want C++14 support.

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 5.0)
  message(FATAL_ERROR "GNU C compiler version ${CMAKE_C_COMPILER_VERSION} is too old. Need 5.0+")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 5.0)
  message(FATAL_ERROR "GNU C++ compiler version ${CMAKE_CXX_COMPILER_VERSION} is too old. Need 5.0+")
endif()

if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 3.5)
  message(FATAL_ERROR "LLVM Clang C compiler version ${CMAKE_C_COMPILER_VERSION} is too old. Need 3.5+")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 3.5)
  message(FATAL_ERROR "LLVM Clang C++ compiler version ${CMAKE_CXX_COMPILER_VERSION} is too old. Need 3.5+")
endif()

# if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 5.0)
#   message(WARNING "Support for GNU C compiler version ${CMAKE_C_COMPILER_VERSION} is soft-deprecated. Consider upgrading to 5.0+")
# endif()

# if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 5.0)
#   message(WARNING "Support for GNU C++ compiler version ${CMAKE_CXX_COMPILER_VERSION} is soft-deprecated. Consider upgrading to 5.0+")
# endif()

# if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 3.5)
#   message(WARNING "LLVM Clang C compiler version ${CMAKE_C_COMPILER_VERSION} is soft-deprecated. Consider upgrading to 3.5+")
# endif()

# if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 3.5)
#   message(WARNING "LLVM Clang C++ compiler version ${CMAKE_CXX_COMPILER_VERSION} is soft-deprecated. Consider upgrading to 3.5+")
# endif()
