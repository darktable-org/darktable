cmake_minimum_required(VERSION 2.8.5)

include_directories("${CMAKE_CURRENT_BINARY_DIR}/../" "${CMAKE_CURRENT_SOURCE_DIR}")

set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

add_definitions(-include common/module_api.h)
add_definitions(-include views/view_api.h)

set(MODULES darkroom lighttable slideshow knight)

add_library(darkroom MODULE "darkroom.c")
add_library(lighttable MODULE "lighttable.c")
add_library(slideshow MODULE "slideshow.c")
add_library(knight MODULE "knight.c")

if(USE_GEO)
    add_library(map MODULE "map.c")
    set(MODULES ${MODULES} map)
endif(USE_GEO)


if(GPHOTO2_FOUND)
    add_library(tethering MODULE "tethering.c")
    set(MODULES ${MODULES} tethering)
endif(GPHOTO2_FOUND)

if(CUPS_FOUND)
    add_library(print MODULE "print.c")
    set(MODULES ${MODULES} print)
endif(CUPS_FOUND)

foreach(module ${MODULES})
    target_link_libraries(${module} ${LIBS})
endforeach(module)

foreach(module ${MODULES})
    install(TARGETS  ${module} DESTINATION ${CMAKE_INSTALL_LIBDIR}/darktable/views)
endforeach(module)
