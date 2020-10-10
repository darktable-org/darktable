#
# Install fixed sized icons
#
set(FIXED_SIZES "16x16" "22x22" "24x24" "32x32" "48x48" "64x64" "256x256" "scalable")
set(THEME hicolor)
foreach(pixmap_size ${FIXED_SIZES})
	# install apps icons
	FILE(GLOB PIXMAP_FILES ${CMAKE_CURRENT_SOURCE_DIR}/${pixmap_size}/*.png ${CMAKE_CURRENT_SOURCE_DIR}/${pixmap_size}/*.svg)
	install(FILES ${PIXMAP_FILES} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/${THEME}/${pixmap_size}/apps COMPONENT DTApplication)
	# Need to copy to have themes, icons, etc all in one path like after installing
	FILE(MAKE_DIRECTORY ${DARKTABLE_DATADIR}/icons/${THEME}/${pixmap_size})
	FILE(COPY ${PIXMAP_FILES} DESTINATION ${DARKTABLE_DATADIR}/icons/${THEME}/${pixmap_size}/apps)
endforeach(pixmap_size)

#
# Install plugin pixmaps
#
FILE(GLOB DARKROOM_PLUGIN_PIXMAPS "plugins/darkroom/*.png" "plugins/darkroom/*.svg")
install(FILES ${DARKROOM_PLUGIN_PIXMAPS} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable/pixmaps/plugins/darkroom COMPONENT DTApplication)
# Need to copy to have themes, icons, etc all in one path like after installing
file(COPY ${DARKROOM_PLUGIN_PIXMAPS} DESTINATION ${DARKTABLE_DATADIR}/pixmaps/plugins/darkroom)

FILE(GLOB OTHER_PIXMAPS "*.png" "*.svg")
install(FILES ${OTHER_PIXMAPS} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable/pixmaps COMPONENT DTApplication)
# Need to copy to have themes, icons, etc all in one path like after installing
file(COPY ${OTHER_PIXMAPS} DESTINATION ${DARKTABLE_DATADIR}/pixmaps)
