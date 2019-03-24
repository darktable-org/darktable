include_directories("${CMAKE_CURRENT_BINARY_DIR}/../" "${CMAKE_CURRENT_SOURCE_DIR}")

include(manage-symbol-visibility)

add_definitions(-include common/module_api.h)
add_definitions(-include views/view_api.h)

set(MODULES darkroom lighttable slideshow knight)

add_library(darkroom MODULE "darkroom.c")
add_library(lighttable MODULE "lighttable.c")
add_library(slideshow MODULE "slideshow.c")
add_library(knight MODULE "knight.c")

if(USE_MAP)
    add_library(map MODULE "map.c")
    set(MODULES ${MODULES} map)
endif(USE_MAP)


if(GPHOTO2_FOUND)
    add_library(tethering MODULE "tethering.c")
    set(MODULES ${MODULES} tethering)
endif(GPHOTO2_FOUND)

if(CUPS_FOUND)
    add_library(print MODULE "print.c")
    set(MODULES ${MODULES} print)
endif(CUPS_FOUND)

foreach(module ${MODULES})
    target_link_libraries(${module} lib_darktable)
endforeach(module)

foreach(module ${MODULES})
    if (WIN32)
      _detach_debuginfo (${module} ${CMAKE_INSTALL_LIBDIR}/darktable/views)
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

    install(TARGETS  ${module} DESTINATION ${CMAKE_INSTALL_LIBDIR}/darktable/views COMPONENT DTApplication)
endforeach(module)
