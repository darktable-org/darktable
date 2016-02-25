cmake_minimum_required(VERSION 2.6)

include_directories("${CMAKE_CURRENT_BINARY_DIR}/../../" "${CMAKE_CURRENT_SOURCE_DIR}")

set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

add_definitions(-include module_api.h)
add_definitions(-include imageio/storage/imageio_storage_api.h)

set(MODULES disk email gallery latex)

if(FLICKCURL_FOUND)
	list(APPEND MODULES flickr)
endif(FLICKCURL_FOUND)

if (JsonGlib_FOUND)
	list(APPEND MODULES facebook picasa)
endif(JsonGlib_FOUND)

foreach(module ${MODULES})
	add_library(${module} MODULE "${module}.c")
endforeach(module)
add_dependencies(gallery metadata_dummy)

foreach(module ${MODULES})
	target_link_libraries(${module} ${LIBS})
	install(TARGETS  ${module} DESTINATION ${LIB_INSTALL}/darktable/plugins/imageio/storage)
endforeach(module)
