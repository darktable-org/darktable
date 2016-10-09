include_directories("${CMAKE_CURRENT_BINARY_DIR}/../../" "${CMAKE_CURRENT_SOURCE_DIR}")

include(manage-symbol-visibility)

add_definitions(-include common/module_api.h)
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
	add_dependencies(${module} generate_metadata)
endforeach(module)

foreach(module ${MODULES})
	target_link_libraries(${module} lib_darktable)
    install(TARGETS  ${module} DESTINATION ${CMAKE_INSTALL_LIBDIR}/darktable/plugins/imageio/storage)
endforeach(module)
