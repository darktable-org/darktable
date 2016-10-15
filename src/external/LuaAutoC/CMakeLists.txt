set(LUAAUTOC_SOURCES
	"lautoc.c"
	)

#
# build liblautoc
#
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu99 -Wall -Wno-unused -O3 -g ")

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(lautoc STATIC ${LUAAUTOC_SOURCES})

set_target_properties(lautoc
  PROPERTIES
    POSITION_INDEPENDENT_CODE True
)

target_link_libraries(lautoc)
