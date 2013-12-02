include_directories(${CMAKE_CURRENT_BINARY_DIR}/..)
add_executable(darktable-cmstest main.c)


target_link_libraries(darktable-cmstest ${Glib_LIBRARIES})
add_definitions(${Glib_DEFINITIONS})

target_link_libraries(darktable-cmstest ${LCMS2_LIBRARIES})
add_definitions(${LCMS2_DEFINITIONS})

# TODO: Add a proper XRandR test, too
find_package(X11)
if(X11_FOUND)
  include_directories(${X11_INCLUDE_DIRS})
  target_link_libraries(darktable-cmstest ${X11_LIBRARIES} Xrandr)
  add_definitions(${X11_DEFINITIONS})
  add_definitions("-DHAVE_X11")
endif(X11_FOUND)

if(COLORD_FOUND)
  target_link_libraries(darktable-cmstest ${COLORD_LIBRARIES})
  add_definitions("-DHAVE_COLORD")
endif(COLORD_FOUND)


set_target_properties(darktable-cmstest PROPERTIES CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set_target_properties(darktable-cmstest PROPERTIES CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)
set_target_properties(darktable-cmstest PROPERTIES INSTALL_RPATH $ORIGIN/../${LIB_INSTALL}/darktable)
set_target_properties(darktable-cmstest PROPERTIES LINKER_LANGUAGE C)

install(TARGETS darktable-cmstest DESTINATION bin)
