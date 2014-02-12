cmake_minimum_required(VERSION 2.6)

set(OSM_GPS_MAP_SOURCES
	"src/converter.c"
	"src/osd-utils.c"
	"src/osm-gps-map-compat.c"
	"src/osm-gps-map-image.c"
	"src/osm-gps-map-layer.c"
	"src/osm-gps-map-osd.c"
	"src/osm-gps-map-point.c"
	"src/osm-gps-map-source.c"
	"src/osm-gps-map-track.c"
	"src/osm-gps-map-widget.c"
    )
if(NOT WIN32)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
endif(NOT WIN32)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-type-limits") # gcc < 4.8 choke on enum < 0 comparisons
add_definitions("-D M_PI=3.14159265358979323846" "-D M_LN2=0.69314718055994530942" "-D M_LOG10E=0.43429448190325182765" )

if(LIBSOUP22_FOUND)
  add_definitions("-DUSE_LIBSOUP22")
endif(LIBSOUP22_FOUND)


add_library(osmgpsmap STATIC ${OSM_GPS_MAP_SOURCES})
target_link_libraries(osmgpsmap)

