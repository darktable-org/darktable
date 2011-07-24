cmake_minimum_required(VERSION 2.6)

set(LIBRAW_SOURCES internal/dcraw_common.cpp src/libraw_cxx.cpp internal/dcraw_fileio.cpp internal/demosaic_packs.cpp src/libraw_c_api.cpp)

#
# build liblibraw_r
#
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
add_library(libraw_r STATIC ${LIBRAW_SOURCES})
target_link_libraries(libraw_r)

