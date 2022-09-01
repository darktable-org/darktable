include_directories("${DARKTABLE_BINDIR}/")

include(manage-symbol-visibility)

add_definitions(-include common/module_api.h)
add_definitions(-include libs/lib_api.h)

add_custom_command(
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/authors_h.sh ${CMAKE_SOURCE_DIR}/AUTHORS
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/tools/darktable_authors.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/tools/
  COMMAND bash ${CMAKE_SOURCE_DIR}/tools/authors_h.sh ${CMAKE_SOURCE_DIR}/AUTHORS ${CMAKE_CURRENT_BINARY_DIR}/tools/darktable_authors.h
  COMMENT "Generating authors.h for about dialog."
)

add_custom_target(
  generate_authors_h ALL
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/authors_h.sh ${CMAKE_SOURCE_DIR}/AUTHORS ${CMAKE_CURRENT_BINARY_DIR}/tools/darktable_authors.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# The modules
set(MODULES import export copy_history styles tagging image select collect recentcollect filtering metadata metadata_view navigation histogram history snapshots modulegroups backgroundjobs colorpicker masks session duplicate ioporder)

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
set(MODULES ${MODULES} timeline)
set(MODULES ${MODULES} image_infos)

if(BUILD_BATTERY_INDICATOR)
  set(MODULES ${MODULES} battery_indicator)
endif()

# modules
add_library(import MODULE "import.c")
add_library(export MODULE "export.c" "export_metadata.c")
add_library(copy_history MODULE "copy_history.c")
add_library(styles MODULE "styles.c")
add_library(image MODULE "image.c")
add_library(tagging MODULE "tagging.c")
add_library(select MODULE "select.c")
add_library(collect MODULE "collect.c")
add_library(recentcollect MODULE "recentcollect.c")
add_library(filtering MODULE "filtering.c")
add_library(metadata MODULE "metadata.c")
add_library(metadata_view MODULE "metadata_view.c")
add_library(navigation MODULE "navigation.c")
add_library(histogram MODULE "histogram.c")
add_library(history MODULE "history.c")
add_library(snapshots MODULE "snapshots.c")
add_library(modulegroups MODULE "modulegroups.c")
add_library(backgroundjobs MODULE "backgroundjobs.c")
add_library(colorpicker MODULE "colorpicker.c")
add_library(masks MODULE "masks.c")
add_library(session MODULE "session.c")
add_library(duplicate MODULE "duplicate.c")
add_library(ioporder MODULE "ioporder.c")

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
add_library(timeline MODULE "tools/timeline.c")
add_library(image_infos MODULE "tools/image_infos.c")

find_path(PORTMIDI_INCLUDE_DIR NAMES portmidi.h
          DOC "The Portmidi include directory"
)
find_library(PORTMIDI_LIBRARY NAMES portmidi
          DOC "The Portmidi library"
)
if(PORTMIDI_INCLUDE_DIR)
  add_definitions("-DHAVE_PORTMIDI")
  include_directories(${PORTMIDI_INCLUDE_DIRS})
  set(MODULES ${MODULES} midi)
  add_library(midi MODULE "tools/midi.c")
  target_link_libraries (midi ${PORTMIDI_LIBRARY})
endif()

find_package(SDL2)
if(SDL2_FOUND)
  add_definitions("-DHAVE_SDL")
  set(MODULES ${MODULES} gamepad)
  add_library(gamepad MODULE "tools/gamepad.c")
  if(TARGET SDL2::SDL2)
    target_link_libraries(gamepad SDL2::SDL2)
  else()
    include_directories(${SDL2_INCLUDE_DIRS})
    target_link_libraries(gamepad ${SDL2_LIBRARIES})
  endif()
endif()

if(BUILD_BATTERY_INDICATOR)
  add_library(battery_indicator MODULE "tools/battery_indicator.c")
endif()

# check if gphoto2 found and add build of camera modules...
if(Gphoto2_FOUND)
  add_library(camera MODULE "camera.c")
  add_library(live_view MODULE "live_view.c")
  set(MODULES ${MODULES} camera live_view)
endif(Gphoto2_FOUND)

# the modules specific to map mode
if(USE_MAP)
  add_library(location MODULE "location.c")
  add_library(map_settings MODULE "map_settings.c")
  add_library(map_locations MODULE "map_locations.c")
  set(MODULES ${MODULES} location map_settings map_locations)
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
  if(WIN32)
    _detach_debuginfo (${module} ${CMAKE_INSTALL_LIBDIR}/darktable/plugins/lighttable)
  else()
    set_target_properties(${module}
                          PROPERTIES
                          INSTALL_RPATH ${RPATH_ORIGIN}/../..)
  endif()
  install(TARGETS ${module} DESTINATION ${CMAKE_INSTALL_LIBDIR}/darktable/plugins/lighttable COMPONENT DTApplication)
endforeach(module)
