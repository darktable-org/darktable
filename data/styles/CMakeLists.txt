#
# Install default styles
#
FILE(GLOB STYLE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/darktable*.dtstyle ${CMAKE_CURRENT_SOURCE_DIR}/examples*.dtstyle)
install(FILES ${STYLE_FILES} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable/styles COMPONENT DTApplication)
# Need to copy to have in build dir so we can run from there and have darktable find the
# files like after installing
file(COPY ${STYLE_FILES} DESTINATION ${DARKTABLE_DATADIR}/styles)
