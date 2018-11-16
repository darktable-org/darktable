include_directories("${CMAKE_CURRENT_BINARY_DIR}/../" "${CMAKE_CURRENT_SOURCE_DIR}")

include(manage-symbol-visibility)

add_definitions(-include common/module_api.h)
add_definitions(-include libs/lib_api.h)

add_custom_command(
  DEPENDS ${CMAKE_SOURCE_DIR}/AUTHORS ${CMAKE_SOURCE_DIR}/tools/authors_h.sh
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/tools/darktable_authors.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/tools/
  COMMAND ${CMAKE_COMMAND} -E env LANG=C LC_ALL=C sh ${CMAKE_SOURCE_DIR}/tools/authors_h.sh ${CMAKE_SOURCE_DIR}/AUTHORS ${CMAKE_CURRENT_BINARY_DIR}/tools/darktable_authors.h
  COMMENT "Generating authors.h for about dialog."
)

add_custom_target(
  generate_authors_h ALL
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/authors_h.sh ${CMAKE_SOURCE_DIR}/AUTHORS ${CMAKE_CURRENT_BINARY_DIR}/tools/darktable_authors.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# The modules
set(MODULES import export copy_history styles tagging image select collect recentcollect metadata metadata_view navigation histogram history snapshots modulelist modulegroups backgroundjobs colorpicker masks session duplicate)

# The tools
set(MODULES ${MODULES} viewswitcher)
set(MODULES ${MODULES} darktable_label)
set(MODULES ${MODULES} ratings)
set(MODULES ${MODULES} colorlabels)
set(MODULES ${MODULES} filter)
set(MODULES ${MODULES} lighttable_mode)
set(MODULES ${MODULES} view_toolbox)
set(MODULES ${MODULES} module_toolbox)
set(MODULES ${MODULES} filmstrip)
set(MODULES ${MODULES} hinter)
set(MODULES ${MODULES} global_toolbox)

if(BUILD_BATTERY_INDICATOR)
  set(MODULES ${MODULES} battery_indicator)
endif()

# modules
add_library(import MODULE "import.c")
add_library(export MODULE "export.c")
add_library(copy_history MODULE "copy_history.c")
add_library(styles MODULE "styles.c")
add_library(image MODULE "image.c")
add_library(tagging MODULE "tagging.c")
add_library(select MODULE "select.c")
add_library(collect MODULE "collect.c")
add_library(recentcollect MODULE "recentcollect.c")
add_library(metadata MODULE "metadata.c")
add_library(metadata_view MODULE "metadata_view.c")
add_library(navigation MODULE "navigation.c")
add_library(histogram MODULE "histogram.c")
add_library(history MODULE "history.c")
add_library(snapshots MODULE "snapshots.c")
add_library(modulelist MODULE "modulelist.c")
add_library(modulegroups MODULE "modulegroups.c")
add_library(backgroundjobs MODULE "backgroundjobs.c")
add_library(colorpicker MODULE "colorpicker.c")
add_library(masks MODULE "masks.c")
add_library(session MODULE "session.c")
add_library(duplicate MODULE "duplicate.c")

# tools
add_library(viewswitcher MODULE "tools/viewswitcher.c")

add_library(darktable_label MODULE "tools/darktable.c")
add_dependencies(darktable_label generate_authors_h)

add_library(colorlabels MODULE "tools/colorlabels.c")
add_library(ratings MODULE "tools/ratings.c")
add_library(filter MODULE "tools/filter.c")
add_library(lighttable_mode MODULE "tools/lighttable.c")
add_library(view_toolbox MODULE "tools/view_toolbox.c")
add_library(module_toolbox MODULE "tools/module_toolbox.c")
add_library(filmstrip MODULE "tools/filmstrip.c")
add_library(hinter MODULE "tools/hinter.c")
add_library(global_toolbox MODULE "tools/global_toolbox.c")

if(BUILD_BATTERY_INDICATOR)
  add_library(battery_indicator MODULE "tools/battery_indicator.c")
endif()

# check if gphoto2 found and add build of camera modules...
if(GPHOTO2_FOUND)
	add_library(camera MODULE "camera.c")
	add_library(live_view MODULE "live_view.c")
	set(MODULES ${MODULES} camera live_view)
endif(GPHOTO2_FOUND)

# the modules specific to map mode
if(USE_MAP)
	add_library(location MODULE "location.c")
	add_library(map_settings MODULE "map_settings.c")
	set(MODULES ${MODULES} location map_settings)
endif(USE_MAP)

# the modules specific to geotagging module
add_library(geotagging MODULE "geotagging.c")
set(MODULES ${MODULES} geotagging)

# the module specific to print mode
if(CUPS_FOUND)
  add_library(print_settings MODULE "print_settings.c")
  set(MODULES ${MODULES} print_settings)
endif(CUPS_FOUND)

# Add libs references
foreach(module ${MODULES})
	target_link_libraries(${module} lib_darktable)
endforeach(module)

# Add installation target for each module
# TODO: Modules are now not only lighttable related and should be placed
#       in a better matching structure then just in lighttable directory.
foreach(module ${MODULES})
    if (WIN32)
      _detach_debuginfo (${module} ${CMAKE_INSTALL_LIBDIR}/darktable/plugins/lighttable)
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
    install(TARGETS  ${module} DESTINATION ${CMAKE_INSTALL_LIBDIR}/darktable/plugins/lighttable COMPONENT DTApplication)
endforeach(module)
