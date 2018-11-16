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
    if (WIN32)
        _detach_debuginfo (${module} ${CMAKE_INSTALL_LIBDIR}/darktable/plugins/imageio/storage)
    else()
        # Note that $ORIGIN is not a variable but has a special meaning at runtime.
        # The string "$ORIGIN" should end up in the executable as-is.
        set(RPATH_DT "$ORIGIN")
        if (APPLE)
            # The string "@loader_path" should end up in the executable as-is.
            set(RPATH_DT "@loader_path")
        endif()
        set_target_properties(${module}
                              PROPERTIES
                              INSTALL_RPATH ${RPATH_DT}/../${CMAKE_INSTALL_LIBDIR}/darktable)
    endif(WIN32)
    install(TARGETS  ${module} DESTINATION ${CMAKE_INSTALL_LIBDIR}/darktable/plugins/imageio/storage COMPONENT DTApplication)
endforeach(module)
