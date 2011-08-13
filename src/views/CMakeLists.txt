cmake_minimum_required(VERSION 2.6)

include_directories("${CMAKE_CURRENT_BINARY_DIR}/../" "${CMAKE_CURRENT_SOURCE_DIR}")
set(MODULES darkroom lighttable)

add_library(darkroom MODULE "darkroom.c")
add_library(lighttable MODULE "lighttable.c")

if(GPHOTO2_FOUND)
	add_library(views_capture MODULE "capture.c")
	set_target_properties(views_capture PROPERTIES OUTPUT_NAME capture)
	set(MODULES ${MODULES} views_capture)
endif(GPHOTO2_FOUND)

foreach(module ${MODULES})
	target_link_libraries(${module} ${LIBS})
endforeach(module)

foreach(module ${MODULES})
	install(TARGETS  ${module} DESTINATION ${LIB_INSTALL}/darktable/views)
endforeach(module)

