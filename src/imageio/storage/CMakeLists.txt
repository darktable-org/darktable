include_directories("${DARKTABLE_BINDIR}")

include(manage-symbol-visibility)

add_definitions(-include common/module_api.h)
add_definitions(-include imageio/storage/imageio_storage_api.h)

set(MODULES disk email gallery latex piwigo)

foreach(module ${MODULES})
	add_library(${module} MODULE "${module}.c")
    if(ANDROID)
        set_target_properties(${module} PROPERTIES PREFIX "libdt_storage_")
    endif()
endforeach(module)

set_property(GLOBAL PROPERTY DT_PLUGIN_STORAGES ${MODULES})

foreach(module ${MODULES})
    target_link_libraries(${module} lib_darktable)
    if(ANDROID)
        set_target_properties(${module} PROPERTIES INSTALL_RPATH "${RPATH_ORIGIN}")
    elseif (WIN32)
        _detach_debuginfo (${module} ${CMAKE_INSTALL_LIBDIR}/darktable/plugins/imageio/storage)
    else()
        set_target_properties(${module}
                              PROPERTIES
                              INSTALL_RPATH ${RPATH_ORIGIN}/../../..)
    endif()
    install(TARGETS ${module} DESTINATION ${CMAKE_INSTALL_LIBDIR}/darktable/plugins/imageio/storage COMPONENT DTApplication)
endforeach(module)
