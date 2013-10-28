cmake_minimum_required(VERSION 2.6)

set(SQUISH_SOURCES
    "alpha.cpp"
    "clusterfit.cpp"
    "colourblock.cpp"
    "colourfit.cpp"
    "colourset.cpp"
    "maths.cpp"
    "rangefit.cpp"
    "singlecolourfit.cpp"
    "squish.cpp"
    )

#
# build libsquish
#
if(NOT WIN32)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")
endif(NOT WIN32)
add_definitions("-DSQUISH_USE_SSE=2")

add_library(squish STATIC ${SQUISH_SOURCES})
target_link_libraries(squish)

