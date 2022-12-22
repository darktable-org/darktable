if(USE_XCF)
  add_subdirectory(libxcf)
endif()

add_library(whereami STATIC "${CMAKE_CURRENT_SOURCE_DIR}/whereami/src/whereami.c")
target_include_directories(whereami PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/whereami/src/")

set(BUILD_TESTING_SAVE "${BUILD_TESTING}")
set(BUILD_BENCHMARKING_SAVE "${BUILD_BENCHMARKING}")

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_BENCHMARKING OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(WITH_OPENMP ${USE_OPENMP} CACHE BOOL "" FORCE)

set(RAWSPEED_PATH "${CMAKE_CURRENT_SOURCE_DIR}/rawspeed")
set(RAWSPEED_BINARY_PREFIX "darktable")

if(NOT(EXISTS "${RAWSPEED_PATH}" AND IS_DIRECTORY "${RAWSPEED_PATH}" AND EXISTS "${RAWSPEED_PATH}/CMakeLists.txt"))
  message(FATAL_ERROR "RawSpeed submodule not found. You probably want to run:\n$ git submodule init\nand then\n$ git submodule update")
endif()

set(OPENCL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/OpenCL")

if(NOT(EXISTS "${OPENCL_PATH}" AND IS_DIRECTORY "${OPENCL_PATH}" AND EXISTS "${OPENCL_PATH}/CL/opencl.h"))
  message("OpenCL submodule not found. You probably want to run:\n$ git submodule init\nand then\n$ git submodule update\nOr let the OS headers to be used.")
endif()

set(CMAKE_INSTALL_DATAROOTDIR_SAVE "${CMAKE_INSTALL_DATAROOTDIR}")

set(CMAKE_C_FLAGS_SAVE "${CMAKE_C_FLAGS}")
set(CMAKE_CXX_FLAGS_SAVE "${CMAKE_CXX_FLAGS}")

set(CMAKE_INSTALL_DATAROOTDIR "${CMAKE_INSTALL_DATAROOTDIR}/darktable")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -w")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w")

set(CMAKE_CXX_CLANG_TIDY_SAVE "${CMAKE_CXX_CLANG_TIDY}")
set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE_SAVE "${CMAKE_CXX_INCLUDE_WHAT_YOU_USE}")

unset(CMAKE_CXX_CLANG_TIDY)
unset(CMAKE_CXX_INCLUDE_WHAT_YOU_USE)

add_subdirectory(rawspeed ${DARKTABLE_LIBDIR}/rawspeed)
# Copying rawspeed resources to where dt expects them
add_custom_target("deploy" ALL)
SET(RAWSPEED_DATADIR "${CMAKE_CURRENT_SOURCE_DIR}/rawspeed/data")
add_custom_command(TARGET "deploy" PRE_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${DARKTABLE_DATADIR}/rawspeed/
  COMMAND ${CMAKE_COMMAND} -E copy ${RAWSPEED_DATADIR}/cameras.xml ${RAWSPEED_DATADIR}/showcameras.xsl ${DARKTABLE_DATADIR}/rawspeed/
)

set(CMAKE_INSTALL_DATAROOTDIR "${CMAKE_INSTALL_DATAROOTDIR_SAVE}")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS_SAVE}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS_SAVE}")

set(CMAKE_CXX_CLANG_TIDY "${CMAKE_CXX_CLANG_TIDY_SAVE}")
set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE "${CMAKE_CXX_INCLUDE_WHAT_YOU_USE_SAVE}")

set(BUILD_TESTING "${BUILD_TESTING_SAVE}" CACHE BOOL "" FORCE)
set(BUILD_BENCHMARKING "${BUILD_BENCHMARKING_SAVE}" CACHE BOOL "" FORCE)
