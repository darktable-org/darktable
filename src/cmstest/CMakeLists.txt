include_directories(${CMAKE_CURRENT_BINARY_DIR}/..)
add_executable(darktable-cmstest main.c ${CMAKE_BINARY_DIR}/src/version_gen.c)

# cmake implicitly sets GENERATED on the source file in the directory of the custom command
# since this isn't the same directory we do have to manually set it
set_source_files_properties(${CMAKE_BINARY_DIR}/src/version_gen.c PROPERTIES GENERATED TRUE)
add_dependencies(darktable-cmstest generate_version)

target_link_libraries(darktable-cmstest ${LCMS2_LIBRARIES})
add_definitions(${LCMS2_DEFINITIONS})

target_link_libraries(darktable-cmstest ${Glib_LIBRARIES})
add_definitions(${Glib_DEFINITIONS})

find_package(Glib REQUIRED)
include_directories(SYSTEM ${GObject_INCLUDE_DIRS})
target_link_libraries(darktable-cmstest ${GObject_LIBRARIES})
add_definitions(${GObject_DEFINITIONS})

find_package(X11)
if(X11_FOUND AND X11_Xrandr_FOUND)
  include_directories(SYSTEM ${X11_INCLUDE_DIRS} ${X11_Xrandr_INCLUDE_PATH})
  target_link_libraries(darktable-cmstest ${X11_LIBRARIES} ${X11_Xrandr_LIB})
  add_definitions(${X11_DEFINITIONS})
  add_definitions("-DHAVE_X11")
endif(X11_FOUND AND X11_Xrandr_FOUND)

if(COLORD_FOUND AND NOT ${COLORD_VERSION} VERSION_LESS "0.1.32")
  message(STATUS "building darktable-cmstest with colord support. nice.")
  target_link_libraries(darktable-cmstest ${COLORD_LIBRARIES})
  include_directories(SYSTEM ${COLORD_INCLUDE_DIRS})
  add_definitions(${COLORD_DEFINITIONS})
  add_definitions("-DHAVE_COLORD")
else(COLORD_FOUND AND NOT ${COLORD_VERSION} VERSION_LESS "0.1.32")
  message(STATUS "no colord support for darktable-cmstest. not cool.")
endif(COLORD_FOUND AND NOT ${COLORD_VERSION} VERSION_LESS "0.1.32")


set_target_properties(darktable-cmstest PROPERTIES LINKER_LANGUAGE C)

if (WIN32)
  _detach_debuginfo (darktable-cmstest bin)
endif(WIN32)

install(TARGETS darktable-cmstest DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DTApplication)
