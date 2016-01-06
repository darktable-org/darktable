include_directories(${CMAKE_CURRENT_BINARY_DIR}/..)
add_executable(darktable-chart main.c colorchart.c deltaE.c pfm.c cairo.c common.c thinplate.c tonecurve.c)

target_link_libraries(darktable-chart ${LCMS2_LIBRARIES})
add_definitions(${LCMS2_DEFINITIONS})

target_link_libraries(darktable-chart ${GTK3_LIBRARIES})
add_definitions(${GTK3_DEFINITIONS})

target_link_libraries(darktable-chart "-lm")

target_link_libraries(darktable-chart lib_darktable)

set_target_properties(darktable-chart PROPERTIES LINKER_LANGUAGE C)
set_target_properties(darktable-chart PROPERTIES CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set_target_properties(darktable-chart PROPERTIES CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)
if(APPLE)
  set_target_properties(darktable-chart PROPERTIES INSTALL_RPATH @loader_path/../${CMAKE_INSTALL_LIBDIR}/darktable)
else(APPLE)
  set_target_properties(darktable-chart PROPERTIES INSTALL_RPATH $ORIGIN/../${CMAKE_INSTALL_LIBDIR}/darktable)
endif(APPLE)

install(TARGETS darktable-chart DESTINATION ${CMAKE_INSTALL_BINDIR})
