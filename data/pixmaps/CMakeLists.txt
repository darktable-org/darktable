#
# Install fixed sized icons
#
set(FIXED_SIZES "16x16" "22x22" "24x24" "32x32" "48x48" "64x64" "256x256" "scalable")
foreach(pixmap_size ${FIXED_SIZES})
	# install apps icons
	FILE(GLOB PIXMAP_FILES ${CMAKE_CURRENT_SOURCE_DIR}/${pixmap_size}/*.png ${CMAKE_CURRENT_SOURCE_DIR}/${pixmap_size}/*.svg)
    install(FILES ${PIXMAP_FILES} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/${THEME}/${pixmap_size}/apps COMPONENT DTApplication)
endforeach(pixmap_size)

#
# Install plugin pixmaps
#
FILE(GLOB DARKROOM_PLUGIN_PIXMAPS "plugins/darkroom/*.png" "plugins/darkroom/*.svg")
install(FILES ${DARKROOM_PLUGIN_PIXMAPS} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable/pixmaps/plugins/darkroom COMPONENT DTApplication)

FILE(GLOB OTHER_PIXMAPS "*.png" "*.svg")
install(FILES ${OTHER_PIXMAPS} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable/pixmaps COMPONENT DTApplication)
