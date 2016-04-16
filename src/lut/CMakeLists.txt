include_directories(${CMAKE_CURRENT_BINARY_DIR}/..)
add_executable(darktable-lut main.c colorchart.c deltaE.c pfm.c cairo.c common.c thinplate.c tonecurve.c)

target_link_libraries(darktable-lut ${LCMS2_LIBRARIES})
add_definitions(${LCMS2_DEFINITIONS})

target_link_libraries(darktable-lut ${GTK3_LIBRARIES})
add_definitions(${GTK3_DEFINITIONS})

target_link_libraries(darktable-lut "-lm")

target_link_libraries(darktable-lut lib_darktable)

set_target_properties(darktable-lut PROPERTIES LINKER_LANGUAGE C)
set_target_properties(darktable-lut PROPERTIES CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set_target_properties(darktable-lut PROPERTIES CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)
if(APPLE)
  set_target_properties(darktable-lut PROPERTIES INSTALL_RPATH @loader_path/../${LIB_INSTALL}/darktable)
else(APPLE)
  set_target_properties(darktable-lut PROPERTIES INSTALL_RPATH $ORIGIN/../${LIB_INSTALL}/darktable)
endif(APPLE)

install(TARGETS darktable-lut DESTINATION bin)
