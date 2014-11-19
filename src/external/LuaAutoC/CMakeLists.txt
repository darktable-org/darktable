cmake_minimum_required(VERSION 2.6)


set(LUAAUTOC_SOURCES
	"lautoc.c"
	)

#
# build liblautoc
#
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu99 -Wall -Werror -Wno-unused -O3 -g ")
if(NOT WIN32)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
endif(NOT WIN32)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
add_library(lautoc STATIC ${LUAAUTOC_SOURCES})
target_link_libraries(lautoc)

