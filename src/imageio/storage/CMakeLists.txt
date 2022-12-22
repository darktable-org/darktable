include_directories("${DARKTABLE_BINDIR}")

include(manage-symbol-visibility)

add_definitions(-include common/module_api.h)
add_definitions(-include imageio/storage/imageio_storage_api.h)

set(MODULES disk email gallery latex)

find_package(CURL 7.56)
if(CURL_FOUND)
  message(STATUS "Found recent CURL version to build piwigo.")
  list(APPEND MODULES piwigo)
else(CURL_FOUND)
  message(STATUS "Cannot found recent CURL version (>= 7.56) to build piwigo.")
endif(CURL_FOUND)

foreach(module ${MODULES})
	add_library(${module} MODULE "${module}.c")
endforeach(module)

foreach(module ${MODULES})
    target_link_libraries(${module} lib_darktable)
    if (WIN32)
        _detach_debuginfo (${module} ${CMAKE_INSTALL_LIBDIR}/darktable/plugins/imageio/storage)
    else()
        set_target_properties(${module}
                              PROPERTIES
                              INSTALL_RPATH ${RPATH_ORIGIN}/../../..)
    endif(WIN32)
    install(TARGETS ${module} DESTINATION ${CMAKE_INSTALL_LIBDIR}/darktable/plugins/imageio/storage COMPONENT DTApplication)
endforeach(module)
