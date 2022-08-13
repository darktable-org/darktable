include_directories("${DARKTABLE_BINDIR}/")

include(manage-symbol-visibility)

add_definitions(-include common/module_api.h)
add_definitions(-include views/view_api.h)

set(MODULES darkroom lighttable slideshow)

add_library(darkroom MODULE "darkroom.c")
add_library(lighttable MODULE "lighttable.c")
add_library(slideshow MODULE "slideshow.c")

if(USE_GAME)
    add_library(knight MODULE "knight.c")
    set(MODULES ${MODULES} knight)
endif(USE_GAME)

if(USE_MAP)
    add_library(map MODULE "map.c")
    set(MODULES ${MODULES} map)
endif(USE_MAP)


if(Gphoto2_FOUND)
    add_library(tethering MODULE "tethering.c")
    set(MODULES ${MODULES} tethering)
endif(Gphoto2_FOUND)

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
        set_target_properties(${module}
                              PROPERTIES
                              INSTALL_RPATH ${RPATH_ORIGIN}/..)
    endif(WIN32)

    install(TARGETS ${module} DESTINATION ${CMAKE_INSTALL_LIBDIR}/darktable/views COMPONENT DTApplication)
endforeach(module)
