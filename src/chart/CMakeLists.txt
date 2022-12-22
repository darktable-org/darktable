add_executable(darktable-chart
               main.c
               colorchart.c
               deltaE.c
               pfm.c
               dtcairo.c
               common.c
               thinplate.c
               tonecurve.c)

target_include_directories(darktable-chart
                           PRIVATE
                              ${CMAKE_CURRENT_BINARY_DIR}/..)

target_compile_options(darktable-chart
                       PRIVATE
                           ${LCMS2_DEFINITIONS}
                           ${GTK3_DEFINITIONS})

target_link_libraries(darktable-chart
                      PRIVATE
                        ${LCMS2_LIBRARIES}
                        ${GTK3_LIBRARIES}
                        lib_darktable
                        m)
if (WIN32)
  set_target_properties(darktable-chart PROPERTIES LINK_FLAGS "-mwindows -Wl,-subsystem,windows")
  _detach_debuginfo (darktable-chart bin)
else()
    set_target_properties(darktable-chart
                          PROPERTIES
                          INSTALL_RPATH ${RPATH_ORIGIN}/${REL_BIN_TO_LIBDIR})
endif(WIN32)

install(TARGETS darktable-chart DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DTApplication)
