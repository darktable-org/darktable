include_directories("${CMAKE_CURRENT_BINARY_DIR}/../../" "${CMAKE_CURRENT_SOURCE_DIR}")
include_directories(SYSTEM "${PNG_PNG_INCLUDE_DIR}")

include(manage-symbol-visibility)

add_definitions(-include common/module_api.h)
add_definitions(-include imageio/format/imageio_format_api.h)

set(MODULES copy jpeg pdf png ppm pfm tiff )

add_library(copy MODULE "copy.c")
add_library(jpeg MODULE "jpeg.c")
add_library(pdf MODULE "pdf.c")
add_library(png MODULE "png.c")
add_library(ppm MODULE "ppm.c")
add_library(pfm MODULE "pfm.c")
add_library(tiff MODULE "tiff.c")

if(WEBP_FOUND)
        list(APPEND MODULES "webp")
        add_library(webp MODULE "webp.c")
endif(WEBP_FOUND)

if(OPENEXR_FOUND)
	list(APPEND MODULES "exr")
	add_library(exr MODULE "exr.cc")
endif(OPENEXR_FOUND)

if(OpenJPEG_FOUND)
	list(APPEND MODULES "j2k")
	add_library(j2k MODULE "j2k.c")
endif(OpenJPEG_FOUND)

foreach(module ${MODULES})
    target_link_libraries(${module} lib_darktable)
    if (WIN32)
        _detach_debuginfo (${module} ${CMAKE_INSTALL_LIBDIR}/darktable/plugins/imageio/format)
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
    install(TARGETS  ${module} DESTINATION ${CMAKE_INSTALL_LIBDIR}/darktable/plugins/imageio/format COMPONENT DTApplication)
endforeach(module)
