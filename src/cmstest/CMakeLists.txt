include_directories(${CMAKE_CURRENT_BINARY_DIR}/..)
add_executable(darktable-cmstest main.c)

target_link_libraries(darktable-cmstest ${LCMS2_LIBRARIES})
add_definitions(${LCMS2_DEFINITIONS})

target_link_libraries(darktable-cmstest ${Glib_LIBRARIES})
add_definitions(${Glib_DEFINITIONS})

find_package(GObject REQUIRED)
include_directories(SYSTEM ${GObject_INCLUDE_DIRS})
target_link_libraries(darktable-cmstest ${GObject_LIBRARIES})
add_definitions(${GObject_DEFINITIONS})

# TODO: Add a proper XRandR test, too?
find_package(X11)
if(X11_FOUND)
  include_directories(SYSTEM ${X11_INCLUDE_DIRS})
  target_link_libraries(darktable-cmstest ${X11_LIBRARIES} Xrandr)
  add_definitions(${X11_DEFINITIONS})
  add_definitions("-DHAVE_X11")
endif(X11_FOUND)

if(COLORD_FOUND AND NOT ${COLORD_VERSION} VERSION_LESS "0.1.32")
  message(STATUS "building darktable-cmstest with colord support. nice.")
#   include_directories(SYSTEM ${COLORD_INCLUDE_DIRS}) # only needed if we want to ignore USE_COLORD
  target_link_libraries(darktable-cmstest ${COLORD_LIBRARIES})
  add_definitions("-DHAVE_COLORD")
else(COLORD_FOUND AND NOT ${COLORD_VERSION} VERSION_LESS "0.1.32")
  message(STATUS "no colord support for darktable-cmstest. not cool.")
endif(COLORD_FOUND AND NOT ${COLORD_VERSION} VERSION_LESS "0.1.32")


set_target_properties(darktable-cmstest PROPERTIES LINKER_LANGUAGE C)

install(TARGETS darktable-cmstest DESTINATION bin)
