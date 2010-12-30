cmake_minimum_required(VERSION 2.6)

include_directories("${CMAKE_CURRENT_BINARY_DIR}/../../" "${CMAKE_CURRENT_SOURCE_DIR}")
set(MODULES disk picasa email)

if(FLICKCURL_FOUND)
	list(APPEND MODULES flickr)
endif(FLICKCURL_FOUND)

foreach(module ${MODULES})
	add_library(${module} MODULE "${module}.c")
endforeach(module)

foreach(module ${MODULES})
	target_link_libraries(${module} ${LIBS})
	install(TARGETS  ${module} DESTINATION ${LIB_INSTALL}/darktable/plugins/imageio/storage)
endforeach(module)

