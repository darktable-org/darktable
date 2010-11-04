cmake_minimum_required(VERSION 2.6)

include_directories("${CMAKE_CURRENT_BINARY_DIR}/../" "${CMAKE_CURRENT_SOURCE_DIR}")
set(MODULES filmstrip darkroom lighttable views_capture)

add_library(filmstrip MODULE "filmstrip.c")
add_library(darkroom MODULE "darkroom.c")
add_library(lighttable MODULE "lighttable.c")
add_library(views_capture MODULE "capture.c")
set_target_properties(views_capture PROPERTIES OUTPUT_NAME capture)

foreach(module ${MODULES})
	target_link_libraries(${module} ${LIBS})
endforeach(module)

foreach(module ${MODULES})
	install(TARGETS  ${module} DESTINATION lib/darktable/views)
endforeach(module)

