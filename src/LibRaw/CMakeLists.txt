cmake_minimum_required(VERSION 2.6)

set(LIBRAW_SOURCES internal/dcraw_common.cpp src/libraw_cxx.cpp internal/dcraw_fileio.cpp src/libraw_c_api.cpp)

#
# build libdarktable
#
add_library(libraw_r STATIC ${LIBRAW_SOURCES})
target_link_libraries(libraw_r)
