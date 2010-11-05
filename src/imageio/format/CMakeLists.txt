cmake_minimum_required(VERSION 2.6)

include_directories("${CMAKE_CURRENT_BINARY_DIR}/../../" "${CMAKE_CURRENT_SOURCE_DIR}")
set(MODULES jpeg png ppm pfm tiff )


add_library(jpeg MODULE "jpeg.c")
add_library(png MODULE "png.c")
add_library(ppm MODULE "ppm.c")
add_library(pfm MODULE "pfm.c")
add_library(tiff MODULE "tiff.c")

if(OPENEXR_FOUND)
	set(MODULES ${MODUELS} "exr")
	add_library(exr MODULE "exr.cc")
endif(OPENEXR_FOUND)


foreach(module ${MODULES})
	target_link_libraries(${module} ${LIBS})
	install(TARGETS  ${module} DESTINATION lib/darktable/plugins/imageio/format)
endforeach(module)

