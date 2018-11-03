# want C++14 support, openmp 4.0 support.

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 5.0)
  message(FATAL_ERROR "GNU C compiler version ${CMAKE_C_COMPILER_VERSION} is too old. Need 5.0+")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 5.0)
  message(FATAL_ERROR "GNU C++ compiler version ${CMAKE_CXX_COMPILER_VERSION} is too old. Need 5.0+")
endif()

if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 3.9)
  message(FATAL_ERROR "LLVM Clang C compiler version ${CMAKE_C_COMPILER_VERSION} is too old. Need 3.9+")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 3.9)
  message(FATAL_ERROR "LLVM Clang C++ compiler version ${CMAKE_CXX_COMPILER_VERSION} is too old. Need 3.9+")
endif()


# C++17 ?

# if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 7)
#   message(WARNING "Support for GNU C compiler version ${CMAKE_C_COMPILER_VERSION} is soft-deprecated. Consider upgrading to 7+")
# endif()

# if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 7)
#   message(WARNING "Support for GNU C++ compiler version ${CMAKE_CXX_COMPILER_VERSION} is soft-deprecated. Consider upgrading to 7+")
# endif()

# if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 4.0)
#   message(WARNING "LLVM Clang C compiler version ${CMAKE_C_COMPILER_VERSION} is soft-deprecated. Consider upgrading to 4.0+")
# endif()

# if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 4.0)
#   message(WARNING "LLVM Clang C++ compiler version ${CMAKE_CXX_COMPILER_VERSION} is soft-deprecated. Consider upgrading to 4.0+")
# endif()
